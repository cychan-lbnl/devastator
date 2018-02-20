#include <devastator/diagnostic.hxx>
#include <devastator/world.hxx>

#include <vector>
#include <cstdint>
#include <iostream>

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

thread_local int unacked = 0;
thread_local size_t sent = 0;

struct message {
  int origin;
  vector<char> hunk;

  void operator()() {
    for(char x: hunk)
      ASSERT_ALWAYS(x == 'x');
    sent += hunk.size();
    world::send(origin, []() {
      unacked -= 1;
    });
  }

  template<typename Re>
  friend void reflect(Re &re, message &me) {
    re(me.origin);
    re(me.hunk);
  }
};

int main() {
  auto doit = []() {
    rng_state rng{rank_me()};
    
    sent = 0;
    
    for(int i=0; i < 1000; i++) {
      unacked += 1;
      world::send(i % rank_n, message{rank_me(), vector<char>(1<<(rng()%20), 'x')});
      world::progress();
    }

    while(unacked != 0)
      world::progress();

    world::barrier();
    
    size_t sent_sum = world::reduce_sum(sent);
    if(rank_me() == 0)
      std::cout << "total bytes = "<<sent_sum<<'\n';
  };
   
  world::run(doit);
  world::run(doit);
  world::run(doit);
  world::run(doit);
}
