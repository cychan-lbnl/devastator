#include <cstdint>
#include <cmath>

#include "phold-bs-params.hxx"
#include "phold-bs-state.hxx"

rng_state::rng_state (int seed)
{
  a = 0x1234567812345678ull*(1+seed);
  b = 0xdeadbeefdeadbeefull*(10+seed);
  // mix it up
  this->operator()();
  this->operator()();
}

uint64_t rng_state::operator() ()
{
  uint64_t x = a;
  uint64_t y = b;
  a = y;
  x ^= x << 23; // a
  b = x ^ y ^ (x >> 17) ^ (y >> 26); // b, c
  return b + y;
}

actor::actor (int a) : a(a), state_cur(a), check(a) { }

std::tuple<int, uint64_t, rng_state, uint64_t> actor::execute (int ray)
{
  rng_state &rng = state_cur;
  rng_state state_prev = rng;

  auto check_prev = check;
  check ^= check>>31;
  check *= 0xdeadbeef;
  check += ray ^ 0xdeadbeef;

  uint64_t dt = (uint64_t)(-lambda * std::log(1.0 - double(rng())/double(-1ull)));
  dt += 1;
  
  int actor_to;
  if(rng() < uint64_t(percent_remote*double(-1ull)))
    actor_to = int(rng() % actor_n);
  else
    actor_to = a;
  
  return std::make_tuple(actor_to, dt, state_prev, check_prev);
}

#pragma reversible map forward=original
void actor::unexecute (rng_state state_prev, uint64_t check_prev)
{
  state_cur = state_prev;
  check = check_prev;  
}

uint64_t actor::checksum () const
{
  return state_cur.a ^ state_cur.b;
}
