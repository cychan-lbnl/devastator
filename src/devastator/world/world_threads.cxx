#include <devastator/world//world_threads.hxx>

#include <sched.h>

void deva::progress(bool spinning, bool deaf) {
  bool did_something = threads::progress(deaf);
  
  static __thread int nothings = 0;
  
  if(!spinning || did_something)
    nothings = 0;
  else if(++nothings == 10) {
    nothings = 0;
    sched_yield();
  }
}
