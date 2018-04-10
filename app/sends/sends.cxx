#include <devastator/diagnostic.hxx>
#include <devastator/world.hxx>

#include <vector>
#include <cstdint>
#include <iostream>

using namespace std;

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

thread_local int sent_n, recv_n, ack_n;
thread_local uint64_t sent_sz, recv_sz;
thread_local int epoch = 0;

struct message {
  int origin;
  int epoch;
  vector<char> hunk;

  void operator()() {
    if(::epoch != this->epoch) {
      deva::say()<<this->epoch<<" landed on "<<::epoch;
      DEVA_ASSERT_ALWAYS(0);
    }
    
    for(char x: hunk)
      DEVA_ASSERT_ALWAYS(x == 'x');

    recv_n += 1;
    recv_sz += hunk.size();
    deva::send(origin, []() {
      ack_n += 1;
    });
  }

  REFLECTED(origin, epoch, hunk);
};

int main() {
  auto doit = []() {
    epoch += 1;
    if(rank_me() == 0)
      std::cout<<"round "<<epoch<<'\n';
    
    rng_state rng{rank_me()};

    sent_n = 0;
    recv_n = 0;
    ack_n = 0;
    sent_sz = 0;
    recv_sz = 0;

    for(int i=0; i < 1000; i++) {
      int len = 1<<(rng()%20);
      //int len = 1;
      sent_n += 1;
      sent_sz += len;
      deva::send(i % rank_n, message{rank_me(), epoch, vector<char>(len, 'x')});
      deva::progress();
    }

    while(ack_n != sent_n)
      deva::progress();

    deva::barrier();

    auto sum2 = [](const char *name, uint64_t a, uint64_t b) {
      uint64_t a1 = deva::reduce_sum(a);
      uint64_t b1 = deva::reduce_sum(b);
      if(rank_me() == 0 || a1 != b1) {
        std::cout<<name<<" = "<<a1<<" "<<b1<<'\n';
      }
      deva::barrier();
      DEVA_ASSERT_ALWAYS(a1 == b1);
    };
    
    sum2("send/recv n", sent_n, recv_n);
    sum2("sent/recv sz", sent_sz, recv_sz);
    if(rank_me() == 0) std::cout<<'\n';
  };
   
  deva::run(doit);
  deva::run(doit);
  deva::run(doit);
  deva::run(doit);
}
