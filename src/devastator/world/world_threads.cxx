#include <devastator/world//world_threads.hxx>

#include <sched.h>

void deva::progress(bool spinning) {
  threads::progress_state ps;
  do {
    threads::progress_begin(ps);
    threads::progress_end(ps);
  } while(ps.backlogged);
  
  static __thread int nothings = 0;
  
  if(!spinning || ps.did_something)
    nothings = 0;
  else if(++nothings == 10) {
    nothings = 0;
    sched_yield();
  }
}
