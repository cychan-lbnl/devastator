#include <devastator/gvt.hxx>

#include <random>
#include <set>
#include <random>

using namespace std;

namespace gvt = deva::gvt;

thread_local uint64_t landed_n = 0;
thread_local std::multiset<std::uint64_t> lvts;
thread_local std::default_random_engine rng;

constexpr uint64_t t_end = 100000 + deva::rank_n;
constexpr int per_rank = 100;

void send_someday(uint64_t t, int delta);

void orbit(uint64_t t, int delta) {
  DEVA_ASSERT(gvt::epoch_gvt() <= t);

  landed_n += 1;
  
  if(t+1 < t_end)
    send_someday(t, delta);
}

void send_someday(uint64_t t, int delta) {
  DEVA_ASSERT(gvt::epoch_gvt() <= t+1);
  
  if(rng() % 4 == 0) {
    gvt::send(
      (deva::rank_me() + deva::rank_n + delta) % deva::rank_n,
      t+1,
      [=]() { orbit(t+1, delta); }
    );
  }
  else {
    lvts.insert(t+1);
    deva::send(deva::rank_me(),
      [=]() {
        lvts.erase(t+1);
        send_someday(t, delta);
      }
    );
  }
}

void tmain() {
  rng.seed(0xdeadbeefull*deva::rank_me());
  
  gvt::init({});
  gvt::epoch_begin(0, {});

  for(int i=0; i < per_rank; i++) {
    int delta = 1 + deva::rank_me();
    delta *= delta;
    delta += i;
    delta %= 0x1eef;
    delta *= delta;
    orbit(0, delta);
  }
  
  while(true) {
    deva::progress();
    gvt::advance();

    //say()<<"gvt "<<gvt::epoch_gvt();
    
    if(~gvt::epoch_gvt() == 0)
      break;
    
    if(gvt::epoch_ended()) {
      //say()<<"gvt="<<gvt::epoch_gvt();
      uint64_t lvt = lvts.empty() ? uint64_t(-1) : *lvts.begin();
      gvt::epoch_begin(lvt, {});
    }
  }

  deva::barrier();
  
  uint64_t landed = deva::reduce_sum(landed_n);
  bool success = landed == deva::rank_n*per_rank*t_end;
  
  if(deva::rank_me() == 0)
    std::cout<<(success?"SUCCESS":"FAILURE")<<'\n';
}

int main() {
  deva::run_and_die(tmain);
}
