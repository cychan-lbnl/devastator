#ifndef PHOLD_BS_STATE_HXX
#define PHOLD_BS_STATE_HXX

#include <cstdint>
#include <tuple>

struct rng_state
{
  uint64_t a, b;
  rng_state (int seed=0); 
  uint64_t operator() ();
};

struct actor
{
  int a;
  rng_state state_cur;
  uint64_t check;

  actor (int a=0);
  std::tuple<int, uint64_t, rng_state, uint64_t> execute (int ray);
  void unexecute (rng_state state_prev, uint64_t check_prev);
  uint64_t checksum () const;
};

#endif // PHOLD_BS_STATE_HXX
