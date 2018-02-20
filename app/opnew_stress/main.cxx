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

int main() {
  world::run_and_die([]() {
    rng_state rng{rank_me()};
    
    for(int i=0; i < 10*1000; i++) {
      int n = (rng()%500)<<(rng()%4 << rng()%3);

      char *blob = (char*)operator new(n);
      for(int j=0; j < n; j++)
        blob[j] = "ab"[j%2];

      int origin = rank_me();
      unacked += 1;
      world::send(rng() % rank_n,
        [=]() {
          sent += n;
          for(int j=0; j < n; j++)
            ASSERT(blob[j] == "ab"[j%2]);
          operator delete(blob);
          
          world::send(origin, []() { unacked -= 1; });
        }
      );

      if(rng() % 10 == 0)
        world::progress();
    }

    while(unacked != 0)
      world::progress();

    world::barrier();
    
    size_t sent_sum = world::reduce_sum(sent);
    if(rank_me() == 0)
      std::cout << "total bytes = "<<sent_sum<<'\n';
  });
}
