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

///////////////
// CellState //
///////////////

struct CellState
{
  struct NeighborState
  {
    Poly<double, 1> poly;

    // include all object data fields in this macro
    SERIALIZED_FIELDS(poly);
  };

  double val = 0;    // value
  double source = 0; // source term

  // include all object data fields in this macro
  SERIALIZED_FIELDS(val, source);

  // main local integrator function
  // integrates from cur_time to end_time (assuming fixed neighbor states)
  // stops early when f(t, state) is true
  // returns time actually integrated to
  template <class F>
  Time local_integrator (Time cur_time, Time end_time,
                         const unordered_map<int, NeighborState> & neighbors,
                         const F & f);

  // returns whether the state differs enough from the last sent state to require an update
  bool check_need_update (Time time, const NeighborState & last_sent) const;

  // generate a NeighborState object to share with neighbors
  NeighborState gen_neighbor_state (Time cur_time, const unordered_map<int, NeighborState> & neighbors) const;

  // evaluate derivative
  double eval_deriv (Time cur_time, const unordered_map<int, NeighborState> & neighbors) const;
};

//////////////////////////////
// CellState Implementation //
//////////////////////////////

double CellState::eval_deriv (Time cur_time, const unordered_map<int, NeighborState> & neighbors) const
{
  int nbr_n = neighbors.size();
  double nbr_sum = 0;
  for (const auto & p : neighbors) {
    nbr_sum += p.second.poly.eval(cur_time);
  }
  return source + alpha * (nbr_sum - nbr_n * val);
}

// approximate diffusion using estimated neighbor values
// integrates from cur_time until end_time OR f() evaluates true
// this can be made more efficient
// NOTE: check the math
template <class F>
Time CellState::local_integrator (Time cur_time, Time end_time,
                                  const unordered_map<int, NeighborState> & neighbors,
                                  const F & f)
{
  // integrate until end time or f() evaluates to true
  while (cur_time < end_time && !f(cur_time, *this)) {
    Time next_time = std::min(cur_time + local_delta_t, end_time);
    Time step = next_time - cur_time;
    double slope = eval_deriv(cur_time, neighbors);
    val += slope * step;
    cur_time = next_time;
  }

  return cur_time;
}

// need to update neighbors when estimated value has deviated from linear estimate by threshold
bool CellState::check_need_update (Time time, const NeighborState & last_sent) const
{
  return std::abs(val - last_sent.poly.eval(time)) >= value_threshold;
}

typename CellState::NeighborState
CellState::gen_neighbor_state (Time cur_time, const unordered_map<int, NeighborState> & neighbors) const
{
  // generate order 1 polynomial approximation
  double slope = eval_deriv(cur_time, neighbors);
  return NeighborState {Poly<double, 1>{{val, slope}, cur_time}};
}

ostream & operator<< (ostream & os, const CellState & x) {
  os << x.val;
  return os;
}

ostream & operator<< (ostream & os, const CellState::NeighborState & x) {
  os << x.poly;
  return os;
}

/////////////////////
// Cell ActorSpace //
/////////////////////

using Cell = Actor<CellState>;

class CellSpace : public BasicActorSpace<Cell>
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
        neighbors.insert({actor_id-1, Cell::NeighborState{Poly<double, 1>{100, 0, 0}}});
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
  cs.connect();

  Print() << "Initial state:" << endl;
  cs.print();

  pdes::init(cs.actor_per_rank());
  cs.root_events();
  pdes::drain();

  Print() << "Final state:" << endl;
  cs.print();
}

// program main
int main ()
{
  deva::run(rank_main);
  return 0;
}
