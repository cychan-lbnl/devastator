#include <devastator/diagnostic.hxx>
#include <devastator/world.hxx>

#include <iostream>

int main() {
  auto doit = []() {
    thread_local int got;

    for(int i=0; i < 20; i++) {
      got = ~deva::rank_me();
      deva::barrier();
      
      if(i%deva::rank_n == deva::rank_me()) {
        deva::bcast_procs([=]() {
          for(int r=deva::process_rank_lo(); r < deva::process_rank_hi(); r++) {
            deva::send_local(r, [=]() {
              DEVA_ASSERT_ALWAYS(got == ~deva::rank_me(), "alread got="<<got);
              got = i;
              //deva::say()<<"passed "<<i;
            });
          }
        });
      }
      
      while(got == ~deva::rank_me())
        deva::progress();
      //deva::say()<<"unspun "<<i;
      
      DEVA_ASSERT_ALWAYS(got == i);
      if(i%deva::rank_n == deva::rank_me()) {
        deva::say()<<"round "<<i;
      }
    }
  };
   
  deva::run(doit);
  std::cout<<"done\n";
}
