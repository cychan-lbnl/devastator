#include <devastator/os_env.hxx>

#include "qss.hxx"

#ifndef QSS_ORDER
#define QSS_ORDER 1
#endif

// only QSS order 0 and 1 currently implemented
static_assert(QSS_ORDER == 0 || QSS_ORDER == 1);

using namespace std;

namespace pdes = deva::pdes;

using deva::rank_me;

//////////////////////
// Model Parameters //
//////////////////////

// model parameters
const int cell_n = deva::os_env<int>("cells", 100);
const double alpha = deva::os_env<double>("alpha", 0.01);
const double value_threshold = deva::os_env<double>("value_thresh", 0.1);
const string sim_conf = deva::os_env<string>("sim_conf", "source");

// local integrator time step
const Time local_delta_t = deva::os_env<unsigned>("local_delta_t", 1);
// skip over at most this many time steps
const Time max_advance_delta_t = deva::os_env<unsigned>("max_advance_delta_t", sim_end_time);


///////////////
// CellState //
///////////////

struct CellState
{
  struct NeighborState
  {
    Poly<double, QSS_ORDER> poly;

    // include all object data fields in this macro
    SERIALIZED_FIELDS(poly);
  };

  double val = 0;    // value
  double source = 0; // source term

  // include all object data fields in this macro
  SERIALIZED_FIELDS(val, source);

  // QSSActor API functions

  // advance state from prev_time to cur_time
  void advance (Time prev_time, Time cur_time,
                const unordered_map<int, NeighborState> & neighbors);

  // returns whether the state differs enough from the last sent state to require an update
  bool check_need_update (Time time, const NeighborState & last_sent) const;

  // estimate when we would need to update neighbors
  Time estimate_next_send_time (Time time,
                                const unordered_map<int, NeighborState> & neighbors,
                                const NeighborState & last_sent) const;

  // generate a NeighborState object to share with neighbors
  NeighborState gen_neighbor_state (Time time, const unordered_map<int, NeighborState> & neighbors) const;

private:

  // helper function to evaluate derivative
  double time_derivative (Time time, const unordered_map<int, NeighborState> & neighbors) const;

  // local integrator function
  // integrates from time to end_time (assuming fixed neighbor states)
  // stops early when f(t, state) is true
  // returns time actually integrated to
  template <class F>
  Time local_integrator (Time time, Time end_time,
                         const unordered_map<int, NeighborState> & neighbors,
                         const F & f);
};

///////////////////////////////////////////
// CellState QSSActor API Implementation //
///////////////////////////////////////////

void CellState::advance (Time prev_time, Time cur_time, 
                         const unordered_map<int, NeighborState> & neighbors)
{
  auto always_false = [] (Time t, const CellState & s) { return false; };
  local_integrator(prev_time, cur_time, neighbors, always_false);
}

// need to update neighbors when estimated value has deviated from estimate by threshold
bool CellState::check_need_update (Time time, const NeighborState & last_sent) const
{
  return std::abs(val - last_sent.poly.eval(time)) >= value_threshold;
}

// estimate when we would need to update neighbors via local integration
Time CellState::estimate_next_send_time (Time time,
                                         const unordered_map<int, NeighborState> & neighbors,
                                         const NeighborState & last_sent) const
{
  CellState est_state = *this;
  auto checker = [&last_sent] (Time t, const CellState & s) {
    return s.check_need_update(t, last_sent);
  };
  return est_state.local_integrator(time, time + max_advance_delta_t, neighbors, checker);
}

typename CellState::NeighborState
CellState::gen_neighbor_state (Time time, const unordered_map<int, NeighborState> & neighbors) const
{
#if QSS_ORDER == 0
  // order 0 polynomial approximation
  Poly<double, 0> poly{{val}, time};
#elif QSS_ORDER == 1
  // order 1 polynomial approximation
  Poly<double, 1> poly{{val, time_derivative(time, neighbors)}, time};
#endif
  return NeighborState {poly};
}

ostream & operator<< (ostream & os, const CellState & x) {
  os << x.val;
  return os;
}

ostream & operator<< (ostream & os, const CellState::NeighborState & x) {
  os << x.poly;
  return os;
}

//////////////////////////////////
// CellState Internal Functions //
//////////////////////////////////

double CellState::time_derivative (Time time, const unordered_map<int, NeighborState> & neighbors) const
{
  int nbr_n = neighbors.size();
  double nbr_sum = 0;
  for (const auto & p : neighbors) {
    nbr_sum += p.second.poly.eval(time);
  }
  return source + alpha * (nbr_sum - nbr_n * val);
}

// approximate diffusion using estimated neighbor values
// integrates from time until end_time OR f() evaluates true
// returns time integrated to
// this can be made more efficient
// NOTE: check the math
template <class F>
Time CellState::local_integrator (const Time start_time, const Time end_time,
                                  const unordered_map<int, NeighborState> & neighbors,
                                  const F & f)
{
  // integrate until end time or f() evaluates to true
  Time time = start_time;
  while (time < end_time && !f(time, *this)) {
    Time next_time = std::min(time + local_delta_t, end_time);
    Time step = next_time - time;
    double slope = time_derivative(time, neighbors);
    val += slope * step;
    time = next_time;
  }

  return time;
}

/////////////////////
// Cell ActorSpace //
/////////////////////

using Cell = QSSActor<CellState>;

class CellSpace : public BasicActorSpace<Cell>
{
public:
  void configure () override
  {
    // initialize cells owned by this rank
    for (int actor_id : rank_to_actor_ids(rank_me())) {
      // set left/right neighbors
      unordered_map<int, CellState::NeighborState> neighbors;
      neighbors.insert({actor_id-1, CellState::NeighborState()});
      neighbors.insert({actor_id+1, CellState::NeighborState()});
      if (sim_conf == "diri" && actor_id == 0) {
        // Dirichlet boundary condition at cell -1 with fixed value 100
        // First coeff is 100, rest are value-initialized to 0
        neighbors.at(-1).poly = Poly<double, QSS_ORDER> {{100}, 0};
      }
      get_actor(actor_id).set_neighbors(std::move(neighbors));

      // set source term at midpoint
      if (sim_conf == "source" && actor_id == cell_n / 2) {
        get_actor(actor_id).get_state().source = 0.2;
      }
    }
  }

  void root_events () const override
  {
    // insert a root advance event for each actor owned by this rank
    for (int actor_id : rank_to_actor_ids(deva::rank_me())) {
      int actor_idx = actor_id_to_idx(actor_id);
      using Advance = typename QSSActor<CellState>::Advance;
      deva::pdes::root_event(actor_idx, // use index as causality domain
                             get_timestamp(0, 0), // event time stamp
                             Advance{0, actor_id}); // advance event
    }
  }
};

CellSpace cs;

template <>
BasicActorSpace<Cell> * get_actor_space<Cell> ()
{
  return static_cast<BasicActorSpace<Cell> *>(&cs);
}

/////////////////////
// Main Simulation //
/////////////////////

// this function runs in parallel on every rank
void rank_main ()
{
  cs.init(cell_n);
  cs.configure();

  Print() << "Using QSS order " << QSS_ORDER << endl;
  Print() << "Initial state:" << endl;
  cs.print();

  pdes::init(cs.actor_per_rank());
  cs.root_events();
  pdes::drain();

  Print() << "Final state:" << endl;
  cs.print();

  pdes::finalize();
}

// program main
int main ()
{
  deva::run(rank_main);
  return 0;
}
