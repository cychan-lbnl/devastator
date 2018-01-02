#include <devastator/gvt.hxx>

#include <random>
#include <queue>

using namespace std;

thread_local uint64_t lvt = 0;

void orbit(uint64_t t) {
  ASSERT(gvt::epoch_gvt() <= t);

  const uint64_t t_end = 100;

  say() << "t="<<t<<" landed on "<<world::rank_me();
  lvt = t+1;
  
  world::send(
    world::rank_me(),
    [=]() {
      if(t+1 < t_end + world::rank_n) {
        gvt::send(
          (world::rank_me()+1) % world::rank_n,
          t+1,
          [=]() { orbit(t+1); }
        );
      }
      lvt = ~uint64_t(0);
    }
  );
}

void tmain() {
  gvt::init({});
  gvt::epoch_begin(0, {});

  if(world::rank_me() == 0)
    orbit(0);
  
  while(true) {
    world::progress();
    gvt::advance();

    //say()<<"gvt "<<gvt::epoch_gvt();
    
    if(~gvt::epoch_gvt() == 0)
      break;
    
    if(gvt::epoch_ended()) {
      //say()<<"gvt="<<gvt::epoch_gvt();
      gvt::epoch_begin(lvt, {});
    }
  }

  world::barrier();
}

int main() {
  world::run_and_die(tmain);
}
