#include <devastator/diagnostic.hxx>
#include <devastator/world.hxx>
#include <devastator/os_env.hxx>

#include "util/report.hxx"
#include "util/timer.hxx"

#include <chrono>
#include <cstdint>
#include <memory>

using namespace std;

using deva::rank_n;
using deva::rank_me;

struct rng_state {
  uint64_t a, b;
  
  rng_state(int64_t seed=0) {
    a = 0x1234567812345678ull*(1+seed);
    b = 0xdeadbeefdeadbeefull*(10+seed);
  }
  
  uint64_t operator()() {
    uint64_t x = a;
    uint64_t y = b;
    a = y;
    x ^= x << 23; // a
    b = x ^ y ^ (x >> 17) ^ (y >> 26); // b, c
    return b + y;
  }
};

thread_local deva::bench::timer begun;
double cutoff;
thread_local int64_t tot_send_n = 0;
thread_local int64_t tot_recv_n = 0;
thread_local bool terminating = false;

thread_local std::unique_ptr<uint8_t[]> state;
std::unique_ptr<uint8_t[]> hugeness;
int kbs_per_rank;
int false_misses;

void bounce(rng_state rng) {
  if(terminating)
    return;

  uint8_t x = rng() % 256;
  for(int i=0; i < kbs_per_rank<<10; i++)
    state[i] ^= x + state[i^x];

  int dst = rng() % deva::rank_n;
  tot_send_n += 1;
  deva::send(dst, [=]() {
    tot_recv_n += 1;
    bounce(rng);
  });
}

int main() {
  auto doit = [&]() {
    int msg_per_rank = deva::os_env<int>("msg_per_rank", 100);

    if(deva::rank_me() == 0) {
      kbs_per_rank = deva::os_env<int>("kbs_per_rank", 1);
      false_misses = deva::os_env<int>("false_misses", 0);
      cutoff = deva::os_env<double>("wall_secs", 10);

      hugeness.reset(new uint8_t[128<<10]());
    }
    
    deva::barrier();

    state.reset(new uint8_t[kbs_per_rank<<10]());
    begun.reset();
    
    deva::barrier();

    rng_state rng1(deva::rank_me());
    for(int r=0; r < msg_per_rank; r++) {
      rng_state rng2(rng1());
      bounce(rng2);
    }

    while(!terminating) {
      static __thread int skips = 0;

      if(++skips == 100) {
        skips = 0;
        if(begun.elapsed() >= cutoff)
          terminating = true;
      }
      
      deva::progress();

      uint8_t x = rng1() % 256;
      for(int i=0; i < false_misses; i++) {
        int j = rng1() % (128<<10);
        x ^= hugeness[j];
        hugeness[j] = x;
      }
    }

    //deva::say()<<"local terminated";
    
    while(!deva::reduce_and(terminating))
      deva::progress();

    //deva::say()<<"global terminated";
    
    while(0 != deva::reduce_sum(tot_send_n - tot_recv_n))
      deva::progress();
    
    //deva::say()<<"global quiesced";

    double wall_secs = deva::reduce_min(begun.elapsed());
    tot_send_n = deva::reduce_sum(tot_send_n);
    
    if(deva::rank_me()==0) {
      deva::bench::report rep(__FILE__);
      rep.emit(
        deva::datarow::x("msg_per_rank", msg_per_rank) &
        deva::datarow::x("kbs_per_rank", kbs_per_rank) &
        deva::datarow::x("false_misses", false_misses) &
        deva::datarow::y("send_per_rank_per_sec", tot_send_n/wall_secs/rank_n)
      );
    }
  };

  deva::run(doit);
  return 0;
}
