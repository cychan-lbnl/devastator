#include <devastator/diagnostic.hxx>
#include <devastator/world.hxx>
#include <devastator/pdes.hxx>
#include <devastator/os_env.hxx>

#include "util/report.hxx"
#include "util/timer.hxx"

#include <chrono>
#include <cmath>
#include <cstdint>
#include <memory>
#include <vector>

using namespace std;

namespace pdes = deva::pdes;

using deva::rank_n;
using deva::rank_me;

struct rng_state {
  uint64_t a, b;
  
  rng_state(int seed=0) {
    a = 0x1234567812345678ull*(1+seed);
    b = 0xdeadbeefdeadbeefull*(10+seed);
    // mix it up
    this->operator()();
    this->operator()();
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

int lp_per_rank;
int ray_per_lp;
double fraction_remote;

thread_local unique_ptr<rng_state[]> state_cur;

thread_local deva::bench::timer begun;
double cutoff;

struct bounce {
  int ray;
  int lp;
  
  struct reverse {
    rng_state state_prev;
    
    void unexecute(pdes::event_context&, bounce &me) {
      //say() << "unexecute "<<me.lp;
      int cd = me.lp % lp_per_rank;
      state_cur[cd] = state_prev;
    }

    void commit(pdes::event_context&, bounce&) {
      // nop
    }
  };
  
  reverse execute(pdes::execute_context &cxt) {
    int cd = this->lp % lp_per_rank;

    rng_state &rng = state_cur[cd];
    rng_state state_prev = rng;

    static __thread int skips = 0;
    if(skips < 0)
      return reverse{state_prev};
    else if(++skips == 100) {
      skips = 0;
      if(begun.elapsed() >= cutoff) {
        skips = -1;
        return reverse{state_prev};
      }
    }

    constexpr double lambda = 10000;
    #if 0
      uint64_t dt = lambda;
    #else
      uint64_t dt = (uint64_t)(-lambda * std::log(1.0 - double(rng())/double(-1ull)));
    #endif
    dt += 1;
    
    int lp_to;
    if(rng() < uint64_t(fraction_remote*double(-1ull)))
      lp_to = int(rng() % (lp_per_rank*rank_n));
    else
      lp_to = lp;
    
    cxt.send(
      /*rank=*/lp_to/lp_per_rank,
      /*cd=*/lp_to%lp_per_rank,
      /*time=*/cxt.time + dt,
      bounce{ray, lp_to}
    );
    
    // return the unexecute lambda
    return reverse{state_prev};
  }
};

int main() {
  double duration;
  
  auto doit = [&]() {
    if(deva::rank_me_local() == 0) {
      lp_per_rank = deva::os_env<int>("lp_per_rank", 1000);
      ray_per_lp = deva::os_env<int>("ray_per_lp", 2);
      fraction_remote = deva::os_env<double>("fraction_remote", .5);
      cutoff = deva::os_env<double>("wall_secs", 10);
    }

    deva::barrier();

    pdes::chitter_secs = -1;
    pdes::init(lp_per_rank);
    
    state_cur.reset(new rng_state[lp_per_rank]);
    
    for(int cd=0; cd < lp_per_rank; cd++) {
      int lp = rank_me()*lp_per_rank + cd;
      state_cur[cd] = rng_state{/*seed=*/lp};
      
      pdes::register_state(cd, &state_cur[cd]);

      for(int lp_ray=0; lp_ray < ray_per_lp; lp_ray++) {
        int ray = lp*ray_per_lp + lp_ray;
        pdes::root_event(cd, ray, bounce{ray, lp});
      }
    }

    begun.reset();
    
    pdes::drain();
    
    auto wall_end = std::chrono::steady_clock::now();
    pdes::finalize();
    
    double wall_secs = deva::reduce_min(begun.elapsed());
    pdes::statistics stats = deva::reduce_sum(pdes::local_stats());
    
    if(deva::rank_me()==0) {
      deva::bench::report rep(__FILE__);
      rep.emit(
        deva::datarow::x("lp_per_rank", lp_per_rank) &
        deva::datarow::x("ray_per_lp", ray_per_lp) &
        deva::datarow::x("frac_remote", fraction_remote) &
        
        deva::datarow::y("execute_per_rank_per_sec", stats.executed_n/wall_secs/rank_n) &
        deva::datarow::y("commit_per_rank_per_sec", stats.committed_n/wall_secs/rank_n) &
        deva::datarow::y("deterministic", stats.deterministic)
      );
    }
  };

  deva::run(doit);
  return 0;
}
