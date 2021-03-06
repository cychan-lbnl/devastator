#include <devastator/diagnostic.hxx>
#include <devastator/world.hxx>
#include <devastator/pdes.hxx>

#include <atomic>
#include <vector>
#include <cmath>
#include <cstdint>
#include <memory>
#include <sched.h>

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

constexpr int actor_n = 1000; // 1000
constexpr int ray_n = 2*actor_n; // 2*actor_n
constexpr double percent_remote = .5; // .5
constexpr double lambda = 100; //100
constexpr uint64_t end_time = uint64_t(100*lambda);

constexpr int actor_per_rank = (actor_n + rank_n-1)/rank_n;

thread_local rng_state state_cur[actor_per_rank];
thread_local uint64_t check[actor_per_rank];

struct event {
  int ray;
  int actor;
  std::vector<int> devil;
  
  event() = default;
  event(int ray, int actor):
    ray{ray}, actor(actor) {
    devil.resize(3);
    devil[0]= 6;
    devil[1]= 6;
    devil[2]= 6;
  }
  
  SERIALIZED_FIELDS(ray, actor, devil);

  void sane(const char *file, int line) {
    bool ok = true;
    ok &= (devil.size()==3);
    ok &= (devil[0]==6 && devil[1]==6 && devil[2]==6);
    if(!ok) deva::assert_failed(file, line);
  }

  // no subtime() means use magic determinacy bits
  //uint64_t subtime() const { return ray; }
  //uint64_t subtime() const { return 0; } // if non-determinism detected will fail checksum test

  struct reverse {
    int a;
    rng_state state_prev;
    uint64_t check_prev;
    
    void unexecute(pdes::event_context&, event &me) {
      //say() << "unexecute "<<me.actor;
      int a = me.actor % actor_per_rank;
      state_cur[a] = state_prev;
      check[a] = check_prev;  
    }

    void commit(pdes::event_context&cxt, event&me) {
      // nop
    }
  };
  
  reverse execute(pdes::execute_context &cxt) {
    //deva::say() << "execute "<<ray;
    sane(__FILE__,__LINE__);
    
    int a = this->actor % actor_per_rank;

    //deva::say()<<"ray="<<ray<<" time="<<cxt.time;
    
    rng_state &rng = state_cur[a];
    rng_state state_prev = rng;

    auto check_prev = check[a];
    check[a] ^= check[a]>>31;
    check[a] *= 0xdeadbeef;
    check[a] += ray ^ 0xdeadbeef;

    #if 0
      uint64_t dt = lambda;
    #else
      uint64_t dt = (uint64_t)(-lambda * std::log(1.0 - double(rng())/double(-1ull)));
    #endif
    dt += 1;
    
    int actor_to;
    if(rng() < uint64_t(percent_remote*double(-1ull)))
      actor_to = int(rng() % actor_n);
    else
      actor_to = actor;
    
    if(cxt.time + dt < end_time) {
      cxt.send(
        /*rank=*/actor_to/actor_per_rank,
        /*cd=*/actor_to%actor_per_rank,
        /*time=*/cxt.time + dt,
        event{ray, actor_to}
      );
    }
    
    // return the unexecute lambda
    return reverse{a,state_prev,check_prev};
  }
};

uint64_t checksum() {
  int a_lb = rank_me()*actor_per_rank;
  int a_ub = std::min(actor_n, (rank_me()+1)*actor_per_rank);

  uint64_t lacc = 0;
  for(int a=a_lb; a < a_ub; a++) {
    lacc ^= check[a - a_lb];
  }

  return deva::reduce_xor(lacc);
}

int main() {
  int iter = 0;
  
  auto doit = [&]() {
    pdes::init(actor_per_rank);
    
    int actor_lb = rank_me()*actor_per_rank;
    int actor_ub = std::min(actor_n, (rank_me()+1)*actor_per_rank);
    
    for(int actor=actor_lb; actor < actor_ub; actor++) {
      int cd = actor - actor_lb;
      state_cur[cd] = rng_state{/*seed=*/actor};
      check[cd] = actor;

      pdes::register_state(cd, &state_cur[cd]);
      pdes::register_state(cd, &check[cd]);

      pdes::register_checksum_if_debug(cd, [=]()->uint64_t {
        return state_cur[cd].a ^ state_cur[cd].b;
      });
    }
    
    for(int ray=0; ray < ray_n; ray++) {
      int actor = ray % actor_n;
      if(actor_lb <= actor && actor < actor_ub) {
        int cd = actor - actor_lb;
        pdes::root_event(cd, ray, event{ray, actor});
      }
    }

    if(iter % 2 == 0) {
      uint64_t t0 = 0;
      uint64_t dt = end_time/3;
      while(true) {
        uint64_t t1 = t0 + dt;
        //deva::say()<<"drain("<<t1<<")";
        pdes::drain(/*end=*/t1, /*rewindable=*/true);
        //deva::say()<<"rewind(true)";
        pdes::rewind(true);
        //deva::say()<<"drain("<<t1<<")";
        if(1 == -pdes::drain(t1, true)) {
          pdes::rewind(false);
          break;
        }
        //deva::say()<<"rewind(false)";
        pdes::rewind(false);
        t0 = t1;
      }
    }
    else
      pdes::drain();

    pdes::finalize();

    pdes::statistics stats = deva::reduce_sum(pdes::local_stats());
    if(deva::rank_me()==0) {
      std::cout<<"rewind enabled = "<<(iter%2 == 0)<<'\n'
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
  };

  for(iter=0; iter < 4; iter++)
    deva::run(doit);
  
  if(deva::process_me() == 0)
    std::cout<<"Looks good!"<<std::endl;
  return 0;
}
