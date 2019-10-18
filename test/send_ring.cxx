#include <devastator/diagnostic.hxx>
#include <devastator/world.hxx>

#include <cstdint>
#include <iostream>

using namespace std;

using deva::rank_n;
using deva::rank_me;

__thread bool done = false;

void bounce(int round) {
  //deva::say()<<"bounce "<<round;
  
  if(round >= 50)
    done = true;
    
  if(round < 50 + rank_n) {
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
    
    //deva::say()<<"leaving";

    for(int i=0; i < 4; i++)
      deva::barrier(true);
  };
  
  deva::run(doit);
  std::cout<<"SUCCESS\n";
  return 0;
}
