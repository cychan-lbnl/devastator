#include <devastator/os_env.hxx>

#include "qss.hxx"

using namespace std;

namespace pdes = deva::pdes;

using deva::process_n;
using deva::rank_n;
using deva::rank_me;
using deva::rank_me_local;
using deva::rank_is_local;

//////////////////////
// Model Parameters //
//////////////////////

// model parameters
const int cell_n = deva::os_env<int>("cells", 100);
const double alpha = deva::os_env<double>("alpha", 0.01);
const double value_threshold = deva::os_env<double>("value_thresh", 0.1);

// local integrator time step
const Time local_delta_t = deva::os_env<unsigned>("local_delta_t", 1);

////////////////
// Cell State //
////////////////

struct CellState
{
public:

  struct NeighborState
  {
    double val = 0;
    Time effective = 0;

    // include all object data fields in this macro
    SERIALIZED_FIELDS(val, effective);

    friend ostream & operator<< (ostream & os, const NeighborState & x);
  };

  double val = 0;    // value
  double source = 0; // source term

  // include all object data fields in this macro
  SERIALIZED_FIELDS(val, source);

  // main local integrator function
  // cur_time and cur_state are in/out parameters
  // integrates from cur_state at cur_time (assuming fixed neighbor states)
  // stops when cur_time reaches end_time or when f(cur_state) is true
  template <class F>
  static void local_integrator (Time & cur_time, Time end_time,
                                CellState & cur_state,
                                const unordered_map<int, NeighborState> & neighbors,
                                const F & f);

  // returns whether the passed state differs enough from the last sent state to require an update
  static bool check_need_update (const NeighborState & last_sent, Time t, const CellState & s);

  // generate a NeighborState object to share with neighbors
  NeighborState gen_neighbor_state (Time cur_time) const;

  friend ostream & operator<< (ostream & os, const CellState & x);
};

//////////////////////////////
// CellState Implementation //
//////////////////////////////

// approximate diffusion for fixed neighbor values
// integrates from cur_time until end_time OR f() evaluates true
// this can be made more efficient
// NOTE: check the math
template <class F>
void CellState::local_integrator (Time & cur_time, Time end_time,
                                  CellState & cur_state,
                                  const unordered_map<int, NeighborState> & neighbors,
                                  const F & f)
{
  // these are fixed for all steps
  int nbr_n = neighbors.size();
  double nbr_sum = 0;
  for (const auto & p : neighbors) {
    nbr_sum += p.second.val;
  }

  // integrate until end time or f() evaluates to true
  while (cur_time < end_time && !f(cur_time, cur_state)) {
    Time next_time = std::min(cur_time + local_delta_t, end_time);
    Time step = next_time - cur_time;
    double rhs = cur_state.source + alpha * (nbr_sum - nbr_n * cur_state.val);
    cur_state.val += rhs * step;
    cur_time = next_time;
  }
}

// need to update neighbors when estimated value has changed by threshold
bool CellState::check_need_update (const NeighborState & last_sent, Time t, const CellState & s)
{
  return std::abs(s.val - last_sent.val) >= value_threshold;
}

typename CellState::NeighborState CellState::gen_neighbor_state (Time cur_time) const
{
  return {val, cur_time};
}

ostream & operator<< (ostream & os, const CellState & x) {
  os << x.val;
  return os;
}

ostream & operator<< (ostream & os, const CellState::NeighborState & x) {
  os << x.val;
  return os;
}

/////////////////////
// Cell ActorSpace //
/////////////////////

using Cell = Actor<CellState>;

class CellSpace : public ActorSpace<Cell>
{
public:
  void connect () override
  {
    // connect cells owned by this rank
    for (int actor_id : rank_to_actor_ids(rank_me())) {
      unordered_map<int, Cell::NeighborState> neighbors;
      // insert left/right neighbors
      if (actor_id == 0) {
        // Dirichlet boundary condition
        neighbors.insert({actor_id-1, Cell::NeighborState{100, 0}});
      } else {
        neighbors.insert({actor_id-1, Cell::NeighborState()});
      }
      neighbors.insert({actor_id+1, Cell::NeighborState()});
      get_actor(actor_id).set_neighbors(std::move(neighbors));
    }
  }
};

CellSpace cs;

template <>
ActorSpace<Cell> * get_actor_space<Cell> ()
{
  return static_cast<ActorSpace<Cell> *>(&cs);
}

/////////////////////
// Main Simulation //
/////////////////////

// this function runs in parallel on every rank
void rank_main ()
{
  cs.init(cell_n);
  cs.connect();

  cs.print();

  pdes::init(cs.actor_per_rank());
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
