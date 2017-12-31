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

constexpr int actor_n = 4;
constexpr int actor_per_rank = (actor_n + rank_n-1)/rank_n;
constexpr double percent_remote = .5;
constexpr double lambda = 100;
constexpr uint64_t end_time = uint64_t(20*lambda);

thread_local rng_state state_cur[actor_per_rank];
thread_local uint64_t check[actor_per_rank];

struct event {
  int ray;
  int actor;
  rng_state state_prev;
  uint64_t check_prev, check_next;

  std::unique_ptr<void**[]> poo;
  
  event(int ray, int actor):
    ray{ray}, actor(actor) {

    poo.reset(new void**[1<<12]);
    for(int i=0; i < 1<<12; i++) {
      poo[i] = new void*;
      *poo[i] = poo[i];
    }
  }

  void kill_poo() {
    if(poo) {
      for(int i=0; i < 1<<12; i++) {
        ASSERT(poo[i] == *poo[i]);
        delete poo[i];
      }
      poo.reset();
    }
  }
  
  event(event&&) = default;
  ~event() {
    kill_poo();
  }
  
  void execute(pdes::execute_context &cxt) {
    //say() << "execute "<<actor;
    kill_poo();
    
    int a = this->actor % actor_per_rank;
    
    rng_state &rng = state_cur[a];
    this->state_prev = rng;
    
    this->check_prev = check[a];
    check[a] ^= check[a]>>31;
    check[a] *= 0xdeadbeef;
    check[a] += ray;
    this->check_next = check[a];
    
    uint64_t dt = (uint64_t)(-lambda * std::log(1.0 - double(rng())/double(-1ull)));
    
    int actor_to;
    if(rng() < uint64_t(percent_remote*double(-1ull)))
      actor_to = int(rng() % actor_n);
    else
      actor_to = actor;
    
    if(cxt.time + 1 + dt < end_time) {
      cxt.send(
        /*rank=*/actor_to/actor_per_rank,
        /*cd=*/actor_to%actor_per_rank,
        /*time=*/cxt.time + 1 + dt,
        /*id=*/cxt.id,
        std::move(event{ray, actor_to})
      );
    }
  }

  void unexecute() {
    //say() << "unexecute "<<actor;
    int a = actor % actor_per_rank;
    ASSERT(check_next == check[a]);
    state_cur[a] = this->state_prev;
    check[a] = this->check_prev;
  }

  void commit() {
    //say() << "commit "<<actor;
  }
};

uint64_t checksum() {
  int a_lb = rank_me()*actor_per_rank;
  int a_ub = std::min(actor_n, (rank_me()+1)*actor_per_rank);

  uint64_t lacc = 0;
  for(int a=a_lb; a < a_ub; a++) {
    lacc ^= check[a - a_lb];
  }
  
  static atomic<uint64_t> gacc{0};
  
  gacc.fetch_xor(lacc);
  world::barrier();
  
  if(rank_me() == 0)
    say() << "checksum = "<< gacc.load();
}

int main() {
  world::run_and_die([]() {
    pdes::init(actor_per_rank);
    
    int actor_lb = rank_me()*actor_per_rank;
    int actor_ub = std::min(actor_n, (rank_me()+1)*actor_per_rank);
    
    for(int actor=actor_lb; actor < actor_ub; actor++) {
      int cd = actor - actor_lb;
      state_cur[cd] = rng_state{/*seed=*/actor};
      check[cd] = actor;
      pdes::root_event(cd, 1, actor, event{actor, actor});
    }
    
    pdes::drain();
    checksum();
  });
}
