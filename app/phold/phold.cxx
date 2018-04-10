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

using world::rank_n;
using world::rank_me;

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
constexpr int actor_per_rank = (actor_n + rank_n-1)/rank_n;
constexpr double percent_remote = .5;
constexpr double lambda = 100;
constexpr uint64_t end_time = uint64_t(2000*lambda); // 2000

thread_local rng_state state_cur[actor_per_rank];
thread_local uint64_t check[actor_per_rank];

struct event {
  int ray;
  int actor;
  std::vector<int> devil;
  
  event(int ray, int actor):
    ray{ray}, actor(actor) {
    devil.resize(3);
    devil[0]= 6;
    devil[1]= 6;
    devil[2]= 6;
  }

  REFLECTED(ray, actor, devil);

  void sane(const char *file, int line) {
    bool ok = true;
    ok &= (devil.size()==3);
    ok &= (devil[0]==6 && devil[1]==6 && devil[2]==6);
    if(!ok) deva::assert_failed(file, line);
  }
  
  // only member is execute, unexecute is now a lambda returned by execute
  auto execute(pdes::execute_context &cxt) {
    //say() << "execute "<<actor;
    sane(__FILE__,__LINE__);
    
    int a = this->actor % actor_per_rank;
    
    rng_state &rng = state_cur[a];
    rng_state state_prev = rng;

    auto check_prev = check[a];
    check[a] ^= check[a]>>31;
    check[a] *= 0xdeadbeef;
    check[a] += ray;

    #if 0
      uint64_t dt = lambda;
    #else
      uint64_t dt = 1 + (uint64_t)(-lambda * std::log(1.0 - double(rng())/double(-1ull)));
    #endif
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
        /*digest=*/cxt.digest,
        event{ray, actor_to}
      );
    }
    
    // return the unexecute lambda
    return [=](event &me) {
      //say() << "unexecute "<<me.actor;
      int a = me.actor % actor_per_rank;
      state_cur[a] = state_prev;
      check[a] = check_prev;  
    };
  }

  void commit() {}
};

uint64_t checksum() {
  int a_lb = rank_me()*actor_per_rank;
  int a_ub = std::min(actor_n, (rank_me()+1)*actor_per_rank);

  uint64_t lacc = 0;
  for(int a=a_lb; a < a_ub; a++) {
    lacc ^= check[a - a_lb];
  }

  return world::reduce_xor(lacc);
}

int main() {
  auto doit = []() {
    pdes::init(actor_per_rank);
    
    int actor_lb = rank_me()*actor_per_rank;
    int actor_ub = std::min(actor_n, (rank_me()+1)*actor_per_rank);
    
    for(int actor=actor_lb; actor < actor_ub; actor++) {
      int cd = actor - actor_lb;
      state_cur[cd] = rng_state{/*seed=*/actor};
      check[cd] = actor;
      pdes::root_event(cd, actor, actor, event{actor, actor});
    }
    
    pdes::drain();

    uint64_t chk = checksum();
    thread_local uint64_t chkprev = 666;

    ASSERT_ALWAYS(chkprev == 666 || chk == chkprev);
    chkprev = chk;
    
    if(rank_me() == 0)
      say() << "checksum = "<< chk;
  };

  for(int i=0; i < 10; i++)
    world::run(doit);
}
