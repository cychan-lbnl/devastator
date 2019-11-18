#include <devastator/diagnostic.hxx>
#include <devastator/world.hxx>

#include <cstdint>
#include <iostream>

using namespace std;

using deva::rank_n;
using deva::rank_me;

__thread bool done = false;

constexpr int round_end = 500;

void bounce(int round) {
  //if(round%100 == 0) deva::say()<<"bounce "<<round;
  
  if(round >= round_end)
    done = true;
    
  if(round < round_end + rank_n) {
    deva::send((rank_me()+1)%rank_n,
      [=]() {
        bounce(round+1);
      }
    );
  }
}

int main() {
  auto doit = []() {
    if(rank_me() == 0)
      bounce(0);

    while(!done)
      deva::progress();
    
    deva::say()<<"leaving";

    for(int i=0; i < 4; i++)
      deva::barrier(true);
  };
  
  deva::run(doit);
  std::cout<<"SUCCESS\n";
  return 0;
}
