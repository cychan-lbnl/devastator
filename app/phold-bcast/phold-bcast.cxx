#include <devastator/diagnostic.hxx>
#include <devastator/world.hxx>
#include <devastator/pdes.hxx>

#include <cmath>

namespace pdes = deva::pdes;

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

constexpr int cd_per_rank = 10;
constexpr int ray_n = 100;
constexpr double percent_remote = .5; // .5
constexpr double lambda = 100; //100
constexpr uint64_t end_time = uint64_t(100*lambda);

thread_local rng_state state_cur[cd_per_rank];
thread_local uint64_t check[cd_per_rank];

struct event {
  int ray, center;
  
  struct reverse {
    rng_state state_prev;
    uint64_t check_prev;
    
    void unexecute(pdes::event_context &cxt, event &me) {
      //say() << "unexecute "<<me.actor;
      state_cur[cxt.cd] = state_prev;
      check[cxt.cd] = check_prev;  
    }

    void commit(pdes::event_context&cxt, event&me) {
      // nop
    }
  };

  reverse execute(pdes::execute_context &cxt) {
    int ray = this->ray;
    rng_state &rng = state_cur[cxt.cd];
    rng_state state_prev = rng;
    
    auto check_prev = check[cxt.cd];
    check[cxt.cd] ^= check[cxt.cd]>>31;
    check[cxt.cd] *= 0xdeadbeef;
    check[cxt.cd] += ray ^ 0xdeadbeef;

    #if 0
      uint64_t dt = lambda;
    #else
      uint64_t dt = (uint64_t)(-lambda * std::log(1.0 - double(rng())/double(-1ull)));
    #endif
    dt += 1;
    uint64_t t_new = cxt.time + dt;

    if(deva::rank_me()*cd_per_rank + cxt.cd == center) {
      int center_new;
      if(rng() < uint64_t(percent_remote*double(-1ull)))
        center_new = int(rng() % (cd_per_rank*deva::rank_n));
      else
        center_new = center;
      
      if(t_new < end_time) {
        cxt.bcast_procs(
          t_new,
          cd_per_rank*deva::rank_n,
          [=](auto const &run_at_rank) {
            for(int r=deva::process_rank_lo(); r < deva::process_rank_hi(); r++) {
              run_at_rank(r, cd_per_rank,
                [=](auto const &insert) {
                  for(int cd=0; cd < cd_per_rank; cd++)
                    insert(cd, t_new, event{ray,center_new});
                }
              );
            }
          }
        );
      }
    }
    
    // return the unexecute lambda
    return reverse{state_prev,check_prev};
  }
};

uint64_t checksum() {
  uint64_t lacc = 0;
  for(int cd=0; cd < cd_per_rank; cd++)
    lacc ^= check[cd];
  return deva::reduce_xor(lacc);
}

int main() {
  for(int iter=0; iter < 4; iter++ ) {
    deva::run([&]() {
      pdes::init(cd_per_rank);
      
      for(int cd=0; cd < cd_per_rank; cd++) {
        int a = deva::rank_me()*cd_per_rank + cd;
        state_cur[cd] = rng_state{/*seed=*/a};
        check[cd] = a;
      }

      if(deva::rank_me() == 0) {
        for(int ray=0; ray < ray_n; ray++) {
          pdes::root_event(ray % cd_per_rank, ray, event{ray, ray % cd_per_rank});
        }
      }
      
      pdes::drain();
      pdes::finalize();

      pdes::statistics stats = deva::reduce_sum(pdes::local_stats());
      if(deva::rank_me()==0) {
        std::cout<<"iter = "<<iter<<'\n'
                 <<"  events = "<<stats.executed_n<<'\n'
                 <<"  commits = "<<stats.committed_n<<'\n'
                 <<"  deterministic = "<<stats.deterministic<<'\n';
      }
      
      uint64_t chk = checksum();
      thread_local uint64_t chkprev = 666;

      DEVA_ASSERT_ALWAYS(chkprev == 666 || chk == chkprev);
      chkprev = chk;
      if(deva::rank_me() == 0)
        std::cout<<"  checksum = "<<chk<<std::endl;
    });
  }
  return 0;
}
