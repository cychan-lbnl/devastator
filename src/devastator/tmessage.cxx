#include "world.hxx"
#include "tmessage.hxx"
#include "opnew.hxx"

#include <atomic>
#include <thread>

#include <sched.h>

thread_local int tmsg::thread_me_ = 0;

tmsg::active_channels_r<tmsg::thread_n> tmsg::ams_r[thread_n];
tmsg::active_channels_w<tmsg::thread_n> tmsg::ams_w[thread_n];

bool tmsg::progress_noyield() {
  opnew::progress();

  bool did_something;
  did_something =  ams_w[thread_me_].cleanup();
  did_something |= ams_r[thread_me_].receive();

  return did_something;
}

void tmsg::progress() {
  bool did_something = progress_noyield();
  
  static thread_local int consecutive_nothings = 0;

  if(did_something)
    consecutive_nothings = 0;
  else if(++consecutive_nothings == 10) {
    consecutive_nothings = 0;
    sched_yield();
  }
}

void tmsg::barrier() {
  static std::atomic<int> c[2]{{0}, {0}};
  static thread_local unsigned epoch = 0;

  int end = epoch & 2 ? 0 : thread_n;
  int bump = epoch & 2 ? -1 : 1;
  
  if((c[epoch & 1] += bump) != end) {
    while(c[epoch & 1].load(std::memory_order_acquire) != end)
      progress();
  }
  
  epoch += 1;
}

void tmsg::run_and_die(const std::function<void()> &fn) {
  auto tmain = [=](int me) {
    thread_me_ = me;
    opnew::thread_me_initialized();
    
    for(int r=0; r < thread_n; r++)
      ams_w[me].connect(r, ams_r[r]);
    
    fn();
  };
  
  std::thread *threads[thread_n];
  
  for(int t=1; t < thread_n; t++)
    threads[t] = new std::thread(tmain, t);

  tmain(0);

  for(int t=1; t < thread_n; t++) {
    threads[t]->join();
    delete threads[t];
  }
  
  std::exit(0);
}
