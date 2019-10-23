#include <devastator/diagnostic.hxx>
#include <devastator/world.hxx>
#include <devastator/pdes.hxx>
#include <devastator/os_env.hxx>

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

int lp_n = 1000;
int ray_n = 2*lp_n;
double percent_remote = .5; // .5

int lp_per_rank;

thread_local std::unique_ptr<rng_state[]> state_cur;

thread_local std::chrono::steady_clock::time_point wall_begin;
thread_local std::chrono::steady_clock::duration wall_cut;

struct bounce {
  int ray;
  int lp;
  
  struct reverse {
    int a;
    rng_state state_prev;
    
    void unexecute(pdes::event_context&, bounce &me) {
      //say() << "unexecute "<<me.lp;
      int a = me.lp % lp_per_rank;
      state_cur[a] = state_prev;
    }

    void commit(pdes::event_context&, bounce&) {
      // nop
    }
  };
  
  reverse execute(pdes::execute_context &cxt) {
    int a = this->lp % lp_per_rank;

    rng_state &rng = state_cur[a];
    rng_state state_prev = rng;

    static __thread int skips = 0;
    if(skips < 0)
      return reverse{a, state_prev};
    else if(++skips == 100) {
      skips = 0;
      auto dt = std::chrono::steady_clock::now() - wall_begin;
      if(dt >= wall_cut) {
        skips = -1;
        return reverse{a, state_prev};
      }
    }

    constexpr double lambda = 100;
    #if 0
      uint64_t dt = lambda;
    #else
      uint64_t dt = (uint64_t)(-lambda * std::log(1.0 - double(rng())/double(-1ull)));
    #endif
    dt += 1;
    
    int lp_to;
    if(rng() < uint64_t(percent_remote*double(-1ull)))
      lp_to = int(rng() % lp_n);
    else
      lp_to = lp;
    
    cxt.send(
      /*rank=*/lp_to/lp_per_rank,
      /*cd=*/lp_to%lp_per_rank,
      /*time=*/cxt.time + dt,
      bounce{ray, lp_to}
    );
    
    // return the unexecute lambda
    return reverse{a, state_prev};
  }
};

int main() {
  auto doit = [&]() {
    if(deva::rank_me_local() == 0) {
      lp_n = deva::os_env<int>("lp_n", 1000);
      ray_n = deva::os_env<int>("ray_n", 2*lp_n);
      percent_remote = deva::os_env<double>("percent_remote", .5);
      
      lp_per_rank = (lp_n + rank_n-1)/rank_n;
    }

    deva::barrier();

    pdes::chitter_secs = -1;
    pdes::init(lp_per_rank);
    
    state_cur.reset(new rng_state[lp_per_rank]);
    
    int lp_lb = rank_me()*lp_per_rank;
    int lp_ub = std::min(lp_n, (rank_me()+1)*lp_per_rank);

    for(int lp=lp_lb; lp < lp_ub; lp++) {
      int cd = lp - lp_lb;
      state_cur[cd] = rng_state{/*seed=*/lp};
      
      pdes::register_state(cd, &state_cur[cd]);
    }
    
    for(int ray=0; ray < ray_n; ray++) {
      int lp = ray % lp_n;
      if(lp_lb <= lp && lp < lp_ub) {
        int cd = lp - lp_lb;
        pdes::root_event(cd, ray, bounce{ray, lp});
      }
    }

    wall_begin = std::chrono::steady_clock::now();
    wall_cut = std::chrono::seconds(10);
    
    pdes::drain();
    
    auto wall_end = std::chrono::steady_clock::now();
    pdes::finalize();
    
    double wall_secs = std::chrono::duration<double>(wall_end - wall_begin).count();
    wall_secs = deva::reduce_min(wall_secs);
    pdes::statistics stats = deva::reduce_sum(pdes::local_stats());
    
    if(deva::rank_me()==0) {
      std::cout<<"event/sec = "<<stats.executed_n/wall_secs<<'\n'
               <<"commit/sec = "<<stats.committed_n/wall_secs<<'\n'
               <<"deterministic = "<<stats.deterministic<<'\n';
    }
  };

  deva::run(doit);
  return 0;
}
