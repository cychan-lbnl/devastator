#include "world.hxx"
#include "tmessage.hxx"
#include "opnew.hxx"

#include <atomic>
#include <thread>

#include <sched.h>

thread_local int tmsg::thread_me_ = 0;

tmsg::active_channels_r<tmsg::thread_n> tmsg::ams_r[thread_n];
tmsg::active_channels_w<tmsg::thread_n> tmsg::ams_w[thread_n];

void tmsg::progress() {
  opnew::progress();
  
  bool did_something;
  did_something =  ams_w[thread_me_].cleanup();
  did_something |= ams_r[thread_me_].receive();

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

void tmsg::run(const std::function<void()> &fn) {
  static bool first_call = true;
  static std::thread *threads[thread_n];

  static thread_local bool running = true;
  static thread_local std::function<void()> user_fn{};
  
  if(first_call) {
    first_call = false;
    
    auto setup = [=](int me) {
      thread_me_ = me;
      
      for(int r=0; r < thread_n; r++)
        ams_w[me].connect(r, ams_r[r]);
    };
    
    auto worker = [=](int me) {
      setup(me);

      while(running) {
        progress();
        
        if(user_fn) {
          user_fn();
          user_fn = {};
          barrier();
        }
      }
    };

    for(int t=1; t < thread_n; t++)
      threads[t] = new std::thread(worker, t);

    setup(0);

    std::atexit([]() {
      for(int t=1; t < thread_n; t++) {
        ams_w[0].send(t, [=]() { running = false; });
        threads[t]->join();
        delete threads[t];
      }
    });
  }

  for(int t=1; t < thread_n; t++) {
    ams_w[0].send(t, [=]() {
      user_fn = fn;
    });
  }
  
  fn();
  barrier();
}
