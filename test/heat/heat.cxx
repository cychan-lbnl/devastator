#include <devastator/diagnostic.hxx>
#include <devastator/world.hxx>
#include <devastator/pdes.hxx>
#include <devastator/os_env.hxx>

#include <cstdint>
#include <cmath>
#include <memory>
#include <vector>
#include <unordered_map>

#include "print.hxx"

using namespace std;

namespace pdes = deva::pdes;

using deva::process_n;
using deva::rank_n;
using deva::rank_me;
using deva::rank_me_local;
using deva::rank_is_local;

using Time = unsigned;

constexpr int priority_bit_n = 1;
uint64_t get_timestamp (Time t, unsigned priority)
{
  DEVA_ASSERT(priority < 1 << priority_bit_n);
  return static_cast<uint64_t>(t << priority_bit_n | priority);
}

int get_time (uint64_t timestamp)
{
  return static_cast<int>(timestamp >> priority_bit_n);
}

///////////////////////
// Model Actor: Cell //
///////////////////////

// model parameters
const int cell_n = deva::os_env<int>("cells", 100);
const int cell_per_process = (cell_n + process_n-1)/process_n;
const int cell_per_rank = (cell_n + rank_n-1)/rank_n;
const double alpha = deva::os_env<double>("alpha", 0.01);
const double value_threshold = deva::os_env<double>("value_thresh", 0.1);

// local integrator time step
const Time local_delta_t = deva::os_env<unsigned>("local_delta_t", 1);
// force advance at least this many seconds
const Time max_advance_delta_t = deva::os_env<unsigned>("max_advance_delta_t", 10);
// simulation ends after this many seconds
const Time sim_end_time = deva::os_env<unsigned>("sim_end_time", 10);

class Cell
{
public:

  struct State
  {
    double val = 0;
  };

  struct Neighbor
  {
    State state;
    Time effective = 0;
  };

  Cell () = default;
  Cell (int i, unordered_map<int, Neighbor> nbrs) : id(i), neighbors(std::move(nbrs)) {}

  // helper functions
  void set_source (double x) { source = x; }
  void set_last_sent (Neighbor nbr) { last_sent = nbr; }
  void set_next_advance_time (Time t) { next_advance_time = t; }
  const State & get_state () const { return state; }

private:

  int id = 0;        // my ID
  Time time = 0;     // current time
  State state;       // current state
  double source = 0; // source term
  unordered_map<int, Neighbor> neighbors; // neighbor states

  // main local integrator function
  // integrates from cur_state at cur_time (assuming fixed neighbor states)
  // stops when cur_time reaches end_time or when f(cur_state) is true
  template <class F>
  void local_integrator (Time & cur_time, Time end_time,
                         State & cur_state, const F & f) const;

  // overload with no f function
  void local_integrator (Time & cur_time, Time end_time, State & cur_state) const {
    local_integrator(cur_time, end_time, cur_state, [] (Time t, const State & s) { return false; });
  }

  // last sent value to neighbors
  Neighbor last_sent;

  // next scheduled advance
  Time next_advance_time = 0;

  // helper functions
  bool check_need_update (Time t, const State & s) const;
  const unordered_map<int, Neighbor> get_neighbors () const { return neighbors; }
  int neighbor_n () const { return neighbors.size(); }
  pair<Time, State> estimate_next_send_time () const;

public:

  ///////////////////
  // Advance event //
  ///////////////////

  struct AdvanceInfo
  {
    Time  saved_time = 0;
    State saved_state;
  };

  // event that advance's a cell's state to a given time
  struct Advance
  {
    Time event_time = 0;
    int cell_id = 0;

    // include all object data fields in this macro
    SERIALIZED_FIELDS(event_time, cell_id);
    
    // helper struct to unexecute or commit event
    struct UC
    {
      // saved data to restore state
      AdvanceInfo adv_info;
      Neighbor saved_last_sent;
      Time saved_next_advance_time;

      void unexecute (pdes::event_context & cxt, Advance & me);
      void commit (pdes::event_context & cxt, Advance & me) {} // does nothing
    };

    // main event execute function
    UC execute (pdes::execute_context & cxt);
  };

  ///////////////////////
  // ShareState event //
  ///////////////////////

  // event that sets a destination Cell's neighbor value
  struct ShareState
  {
    Time event_time = 0;
    int src_cell_id = 0;
    int dst_cell_id = 0;
    Neighbor nbr;

    // include all object data fields in this macro
    SERIALIZED_FIELDS(event_time, src_cell_id, dst_cell_id, nbr);
    
    // helper struct to unexecute or commit event
    struct UC
    {
      AdvanceInfo adv_info;
      Neighbor old_nbr;
      Time saved_next_advance_time;

      void unexecute (pdes::event_context & cxt, ShareState & me);
      void commit (pdes::event_context & cxt, ShareState & me) {} // does nothing
    };

    // main event execute function
    UC execute (pdes::execute_context & cxt);
  };

  AdvanceInfo advance (Time t);
  void unadvance (AdvanceInfo info);
  Neighbor set_neighbor (int nid, Neighbor nbr);
  Neighbor send_share_state_events (pdes::execute_context & cxt); // send state update to neighbors
  Time reschedule (pdes::execute_context & cxt); // self-schedule an advance
};

//////////////////////
// Cell Actor Space //
//////////////////////

class CellSpace
{
public:

  void initialize ()
  {
    if (rank_me_local() == 0) {
      for (int cell_id = 0; cell_id < cell_n; ++cell_id) {
        if (rank_is_local(cell_id_to_rank(cell_id))) {
          get_shared_cell(cell_id) = Cell();
        }
      }
    }

    deva::barrier();

    // initialize cells owned by this rank
    for (int cell_id : rank_to_cell_ids(rank_me())) {
      unordered_map<int, Cell::Neighbor> neighbors;
      // insert left/right neighbors
      if (cell_id == 0) {
        // Dirichlet boundary condition
        neighbors.insert({cell_id-1, Cell::Neighbor{100, 0}});
      } else {
        neighbors.insert({cell_id-1, Cell::Neighbor()});
      }
      neighbors.insert({cell_id+1, Cell::Neighbor()});
      get_cell(cell_id) = Cell(cell_id, std::move(neighbors));
    }
  }

  void root_events ()
  {
    // insert a root advance event for each cell owned by this rank
    for (int cell_id : rank_to_cell_ids(rank_me())) {
      int cell_idx = cell_id_to_idx(cell_id);
      pdes::root_event(cell_idx,            // use index as causality domain
                       get_timestamp(0, 0), // event time stamp
                       Cell::Advance{0, cell_id}); // advance event
    }
  }

  bool has_actor (int cell_id) const
  {
    return cell_id >= 0 && cell_id < cell_n;
  }

  // which rank a cell is assigned to
  int cell_id_to_rank (int cell_id)
  {
    DEVA_ASSERT(has_actor(cell_id));
    return cell_id / cell_per_rank;
  }

  // returns cell's index within rank
  int cell_id_to_idx (int cell_id)
  {
    DEVA_ASSERT(has_actor(cell_id));
    return cell_id % cell_per_rank;
  }

  // which cells are assigned to a rank
  vector<int> rank_to_cell_ids (int rank)
  {
    int lb = std::min(cell_n,  rank_me()      * cell_per_rank);
    int ub = std::min(cell_n, (rank_me() + 1) * cell_per_rank);
    
    vector<int> result(ub - lb);
    for (int idx = 0; idx < ub - lb; idx++) {
      result[idx] = lb + idx;
    }
    return result;
  }

  // return Cell from local storage
  Cell & get_cell (int cell_id)
  {
    DEVA_ASSERT(cell_id_to_rank(cell_id) == rank_me());
    return cells[cell_id];
  }

  // return Cell from local storage (any cell on process)
  Cell & get_shared_cell (int cell_id)
  {
    DEVA_ASSERT(rank_is_local(cell_id_to_rank(cell_id)));
    return cells[cell_id];
  }

  void print () const
  {
    if (rank_me() == 0) {
      vector<int> cell_ids;
      for (const auto & p : cells) {
        cell_ids.push_back(p.first);
      }
      std::sort(cell_ids.begin(), cell_ids.end());
      AllPrint() << "Cell values: [";
      bool flag_first = true;
      for (int cell_id : cell_ids) {
        if (!flag_first) {
          AllPrint() << ',';
        }
        AllPrint() << cells.at(cell_id).get_state().val;
        flag_first = false;
      }
      AllPrint() << ']' << endl;
    }
  }

private:

  // here's where the cell actors are stored
  unordered_map<int, Cell> cells;
};

CellSpace cs;

///////////////////////////////
// Cell Class Implementation //
///////////////////////////////

Cell::Neighbor Cell::set_neighbor (int nid, Cell::Neighbor nbr)
{
  AllPrint() << "Time " << time << ": Cell " << id << ": set_neighbor() cell " << nid << " to " << nbr.state.val << endl;
  Neighbor old_nbr = neighbors[nid];
  neighbors[nid] = nbr;
  return old_nbr;
}

// advance cell state to time t
Cell::AdvanceInfo Cell::advance (Time t)
{
  // save old state in case of rollback
  AdvanceInfo info {time, state};
  local_integrator(time, t, state);
  AllPrint() << "Time " << t << ": Cell " << id << ": advance() from time " << info.saved_time << " with value: " << info.saved_state.val << " -> " << state.val << endl;
  DEVA_ASSERT(time == t);
  return info;
}

void Cell::unadvance (AdvanceInfo info)
{
  time  = info.saved_time;
  state = info.saved_state;
}

// approximate diffusion for fixed neighbor values
// integrates from cur_time until end_time OR f() evaluates true
// this can be made more efficient
// NOTE: check the math
template <class F>
void Cell::local_integrator (Time & cur_time, Time end_time,
                             State & cur_state, const F & f) const
{
  // these are fixed for all steps
  int nbr_n = neighbor_n();
  double nbr_sum = 0;
  for (const auto & p : neighbors) {
    nbr_sum += p.second.state.val;
  }

  // integrate until end time or f() evaluates to true
  while (cur_time < end_time && !f(cur_time, cur_state)) {
    Time next_time = std::min(cur_time + local_delta_t, end_time);
    Time step = next_time - cur_time;
    double rhs = source + alpha * (nbr_sum - nbr_n * cur_state.val);
    cur_state.val += rhs * step;
    cur_time = next_time;
  }
}

// need to update neighbors when estimated value has changed by threshold
bool Cell::check_need_update (Time t, const State & s) const
{
  return std::abs(s.val - last_sent.state.val) >= value_threshold;
}

// compute when to send updated value to neighbors
pair<Time, Cell::State> Cell::estimate_next_send_time () const
{
  Time est_time = time;
  State est_state = state;
  auto checker = [this] (Time t, const State & s) {
    return check_need_update(t, s);
  };

  // estimate when we would need to update neighbors via local integration
  local_integrator(est_time, time + max_advance_delta_t, est_state, checker);

  return {est_time, est_state};
}

Time Cell::reschedule (pdes::execute_context & cxt)
{
  Time saved_next_advance_time = next_advance_time;
  if (next_advance_time <= time) {
    next_advance_time = std::numeric_limits<Time>::max();
  }

  // estimate when value will change enough to send to neighbors
  Time est_time; State est_state;
  tie(est_time, est_state) = estimate_next_send_time();

  // only reschedule if est_time is earlier than currently scheduled advance 
  if (est_time > time && est_time < std::min(next_advance_time, sim_end_time)) {
    int rank = cs.cell_id_to_rank(id);
    int cd   = cs.cell_id_to_idx(id);
    AllPrint() << "Time " << time << ": Cell " << id << ": reschedule() for future time: " << est_time << endl;
    cxt.send(rank, cd, get_timestamp(est_time, 0), Advance {est_time, id});
    next_advance_time = est_time;
  }

  return saved_next_advance_time;
}

Cell::Neighbor Cell::send_share_state_events (pdes::execute_context & cxt)
{
  Neighbor saved_last_sent = last_sent;

  if (check_need_update(time, state)) {
    AllPrint() << "Time " << time << ": Cell " << id << ": send_share_state_events() with value: " << state.val << endl;
    Neighbor nbr{state, time};
    for (const auto & p : get_neighbors()) {
      int nbr_id = p.first;
      if (cs.has_actor(nbr_id)) {
        int nbr_rank = cs.cell_id_to_rank(nbr_id);
        int nbr_cd   = cs.cell_id_to_idx(nbr_id);
        ShareState event{time, id, nbr_id, nbr};
        cxt.send(nbr_rank, nbr_cd, get_timestamp(time, 1), event);
      }
    }
    last_sent = nbr;
  }

  return saved_last_sent;
}

/////////////////////////////////////
// ShareState Event Implementation //
/////////////////////////////////////

Cell::ShareState::UC Cell::ShareState::execute (pdes::execute_context & cxt)
{
  Cell & cell = cs.get_cell(dst_cell_id);
  // advance to given time using old neighbor values
  AdvanceInfo adv_info = cell.advance(event_time);
  // set the neighbor with new values from event
  Neighbor old_nbr = cell.set_neighbor(src_cell_id, nbr);
  DEVA_ASSERT(nbr.effective > old_nbr.effective);
  // reschedule advance given new neighbor values
  Time saved_next_advance_time = cell.reschedule(cxt);

  return ShareState::UC {adv_info, old_nbr, saved_next_advance_time};
}

void Cell::ShareState::UC::unexecute (pdes::event_context & cxt, Cell::ShareState & me)
{
  // restore the old state
  Cell & cell = cs.get_cell(me.dst_cell_id);
  cell.unadvance(std::move(adv_info));
  cell.set_neighbor(me.src_cell_id, old_nbr);
  cell.set_next_advance_time(saved_next_advance_time);
}

//////////////////////////////////
// Advance Event Implementation //
//////////////////////////////////

Cell::Advance::UC Cell::Advance::execute (pdes::execute_context & cxt)
{
  Cell & cell = cs.get_cell(cell_id);
  AdvanceInfo adv_info = cell.advance(event_time);
  // share state with neighbors
  Neighbor saved_last_sent = cell.send_share_state_events(cxt);
  // reschedule next advance
  Time saved_next_advance_time = cell.reschedule(cxt);
  return Advance::UC {std::move(adv_info), saved_last_sent, saved_next_advance_time};
}

void Cell::Advance::UC::unexecute (pdes::event_context & cxt, Cell::Advance & me) {
  // restore the old state
  Cell & cell = cs.get_cell(me.cell_id);
  cell.unadvance(std::move(adv_info));
  cell.set_last_sent(saved_last_sent);
  cell.set_next_advance_time(saved_next_advance_time);
};

////////////////
// Simulation //
////////////////

// this function runs in parallel on every rank
void rank_main ()
{
  pdes::init(cell_per_rank);
  cs.initialize();
  cs.print();
  cs.root_events();
  pdes::drain();
  cs.print();
}

// program main
int main ()
{
  deva::run(rank_main);
  return 0;
}
