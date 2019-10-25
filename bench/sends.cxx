#include <devastator/diagnostic.hxx>
#include <devastator/world.hxx>
#include <devastator/os_env.hxx>

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

thread_local chrono::steady_clock::time_point wall_begin;
thread_local chrono::steady_clock::duration wall_cut;
thread_local int64_t tot_send_n = 0;
thread_local int64_t tot_recv_n = 0;
thread_local bool terminating = false;

void bounce(rng_state rng) {
  if(terminating)
    return;
  
  int dst = rng() % deva::rank_n;
  tot_send_n += 1;
  deva::send(dst, [=]() {
    tot_recv_n += 1;
    bounce(rng);
  });
}

int main() {
  auto doit = [&]() {
    int ray_per_rank = deva::os_env<int>("ray_per_rank", 100);
    double duration = deva::os_env<double>("wall_secs", 10);

    deva::barrier();
    
    wall_begin = chrono::steady_clock::now();
    wall_cut = chrono::duration_cast<chrono::steady_clock::duration>(chrono::duration<double>(duration));
    
    rng_state rng1(deva::rank_me());
    for(int r=0; r < ray_per_rank; r++) {
      rng_state rng2(rng1());
      bounce(rng2);
    }

    while(!terminating) {
      static __thread int skips = 0;

      if(++skips == 100) {
        skips = 0;
        auto dt = std::chrono::steady_clock::now() - wall_begin;
        if(dt >= wall_cut)
          terminating = true;
      }
      
      deva::progress();
    }

    //deva::say()<<"local terminated";
    
    while(!deva::reduce_and(terminating))
      deva::progress();

    //deva::say()<<"global terminated";
    
    while(0 != deva::reduce_sum(tot_send_n - tot_recv_n))
      deva::progress();
    
    //deva::say()<<"global quiesced";

    auto wall_end = std::chrono::steady_clock::now();
    
    double wall_secs = std::chrono::duration<double>(wall_end - wall_begin).count();
    wall_secs = deva::reduce_min(wall_secs);
    tot_send_n = deva::reduce_sum(tot_send_n);
    
    if(deva::rank_me()==0) {
      std::cout<<"sample('send_per_rank_per_sec', "<<tot_send_n/wall_secs/rank_n<<")\n";
    }
  };

  deva::run(doit);
  return 0;
}
