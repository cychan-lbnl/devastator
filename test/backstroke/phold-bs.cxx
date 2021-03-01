#include <devastator/diagnostic.hxx>
#include <devastator/world.hxx>
#include <devastator/pdes.hxx>

#include <atomic>
#include <vector>
#include <cmath>
#include <cstdint>
#include <memory>
#include <sched.h>

#include "phold-bs-params.hxx"
#include "backstroke_phold-bs-state.hxx"

#include "backstroke/rtss.h"

using namespace std;

namespace pdes = deva::pdes;

using deva::rank_n;
using deva::rank_me;
using deva::process_n;
using deva::process_me;

constexpr int actor_per_rank = (actor_n + rank_n-1)/rank_n;
static_assert(rank_n % process_n == 0);
constexpr int rank_per_proc = rank_n / process_n;
constexpr int actor_per_proc = actor_per_rank * rank_per_proc;

// must not use thread_local for backstroke
actor proc_actors[actor_per_proc];

actor & get_actor(int actor_id)
{
  int idx = actor_id - actor_per_proc * process_me();
  DEVA_ASSERT(idx >= 0 && idx < actor_per_proc);
  return proc_actors[idx];
}

struct event {
  int ray;
  int a;
  std::vector<int> devil;
  
  event() = default;
  event(int ray, int a):
    ray{ray}, a(a) {
    devil.resize(3);
    devil[0]= 6;
    devil[1]= 6;
    devil[2]= 6;
  }
  
  SERIALIZED_FIELDS(ray, a, devil);

  void sane(const char *file, int line) {
    bool ok = true;
    ok &= (devil.size()==3);
    ok &= (devil[0]==6 && devil[1]==6 && devil[2]==6);
    if(!ok) deva::assert_failed(file, line);
  }

  // no subtime() means use magic determinacy bits
  //uint64_t subtime() const { return ray; }
  //uint64_t subtime() const { return 0; } // if non-determinism detected will fail checksum test

  struct reverse {
    rng_state state_prev;
    uint64_t check_prev;
    
    void unexecute(pdes::event_context&, event &me) {
      actor & target = get_actor(me.a);
//    target.unexecute(state_prev, check_prev);
      xpdes::reverseEvent();
    }

    void commit(pdes::event_context&cxt, event&me) {
      xpdes::commitEvent();
    }
  };
  
  reverse execute(pdes::execute_context &cxt) {
    //deva::say() << "execute "<<ray;
    sane(__FILE__,__LINE__);
    
    actor & target = get_actor(this->a);

    //deva::say()<<"ray="<<ray<<" time="<<cxt.time;

    int actor_to;
    uint64_t dt;
    rng_state state_prev;
    uint64_t check_prev;

    xpdes::beginForwardEvent();
    tie(actor_to, dt, state_prev, check_prev) = target.execute(ray);
    xpdes::endForwardEvent();
    
    if(cxt.time + dt < end_time) {
      cxt.send(
        /*rank=*/actor_to/actor_per_rank,
        /*cd=*/actor_to%actor_per_rank,
        /*time=*/cxt.time + dt,
        event{ray, actor_to}
      );
    }
    
    // return the unexecute lambda
    return reverse{state_prev,check_prev};
  }
};

uint64_t checksum() {
  int a_lb = rank_me()*actor_per_rank;
  int a_ub = std::min(actor_n, (rank_me()+1)*actor_per_rank);

  uint64_t lacc = 0;
  for(int a=a_lb; a < a_ub; a++) {
    lacc ^= get_actor(a).check;
  }

  return deva::reduce_xor(lacc);
}

bool rtss_inited = false;

int main() {
  int iter = 0;

  auto doit = [&]() {
    if (!rtss_inited) {
      if (deva::rank_me_local() == 0) {
        cout << "Process " << deva::process_me() << " running initializeRTSS() ..." << endl;
        xpdes::initializeRTSS();
      }
      deva::barrier();
      rtss_inited = true;
    }
  
    cout << "Rank " << deva::rank_me() << " doit() ..." << endl;
    pdes::init(actor_per_rank);
    
    int actor_lb = rank_me()*actor_per_rank;
    int actor_ub = std::min(actor_n, (rank_me()+1)*actor_per_rank);
    
    for(int a=actor_lb; a < actor_ub; a++) {
      auto & cur_actor = get_actor(a);
      int cd = a - actor_lb;
      cur_actor = actor(a);

      pdes::register_state(cd, &cur_actor.state_cur);
      pdes::register_state(cd, &cur_actor.check);

      pdes::register_checksum_if_debug(cd, [=]()->uint64_t {
        return cur_actor.checksum();
      });
    }
    
    for(int ray=0; ray < ray_n; ray++) {
      int a = ray % actor_n;
      if(actor_lb <= a && a < actor_ub) {
        int cd = a - actor_lb;
        pdes::root_event(cd, ray, event{ray, a});
      }
    }

    bool flag_rewind = (iter % 2 == 1);
    if(flag_rewind) {
      uint64_t t0 = 0;
      uint64_t dt = end_time/3;
      while(true) {
        uint64_t t1 = t0 + dt;
        //deva::say()<<"drain("<<t1<<")";
        pdes::drain(/*end=*/t1, /*rewindable=*/true);
        //deva::say()<<"rewind(true)";
        pdes::rewind(true);
        //deva::say()<<"drain("<<t1<<")";
        if(1 == -pdes::drain(t1, true)) {
          pdes::rewind(false);
          break;
        }
        //deva::say()<<"rewind(false)";
        pdes::rewind(false);
        t0 = t1;
      }
    }
    else
      pdes::drain();

    pdes::finalize();

    pdes::statistics stats = deva::reduce_sum(pdes::local_stats());
    if(deva::rank_me()==0) {
      std::cout<<"rewind enabled = "<<flag_rewind<<'\n'
               <<"  events = "<<stats.executed_n<<'\n'
               <<"  commits = "<<stats.committed_n<<'\n'
               <<"  deterministic = "<<stats.deterministic<<'\n';
    }
    
    uint64_t chk = checksum();
    thread_local uint64_t chkprev = 666;

    DEVA_ASSERT_ALWAYS(chkprev == 666 || chk == chkprev);
    chkprev = chk;
    if(deva::rank_me() == 0)
      std::cout<<"  checksum = "<<chk<<std::endl;
  };

  for(iter=0; iter < 4; iter++)
    deva::run(doit);
  
  if(deva::process_me() == 0)
    std::cout<<"Looks good!"<<std::endl;
  return 0;
}
