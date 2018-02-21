#include "world.hxx"
#include "tmsg.hxx"
#include "opnew.hxx"

#include <atomic>

#include <pthread.h>
#include <sched.h>

__thread int tmsg::thread_me_ = -1;

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

void tmsg::barrier(bool do_progress) {
  static std::atomic<int> c[2]{{0}, {0}};
  static thread_local unsigned epoch = 0;

  int end = epoch & 2 ? 0 : tmsg::thread_n;
  int bump = epoch & 2 ? -1 : 1;
  
  if((c[epoch & 1] += bump) != end) {
    while(c[epoch & 1].load(std::memory_order_acquire) != end) {
      if(do_progress)
        tmsg::progress();
      else
        sched_yield();
    }
  }
  
  epoch += 1;
}

namespace {
  pthread_t threads[tmsg::thread_n];
  pthread_mutex_t lock;
  pthread_cond_t wake;
  unsigned run_epoch = 0;
  bool shutdown = false;
  upcxx::function_ref<void()> run_fn;
  
  void* tmain(void *me1) {
    int me = reinterpret_cast<std::intptr_t>(me1);
    static bool zero_inited = false;

    if(me != 0 || !zero_inited) {
      if(me == 0)
        zero_inited = true;
      
      tmsg::thread_me_ = me;
      opnew::thread_me_initialized();
      
      for(int r=0; r < tmsg::thread_n; r++)
        tmsg::ams_w[me].connect(r, tmsg::ams_r[r]);
    }

    bool running;
    unsigned run_epoch_prev = 0;
    do {
      run_fn();
      tmsg::barrier(/*do_progress=*/false);

      if(me == 0)
        running = false;
      else {
        pthread_mutex_lock(&lock);
        if(!shutdown && run_epoch == run_epoch_prev)
          pthread_cond_wait(&wake, &lock);
        running = !shutdown;
        run_epoch_prev = run_epoch;
        pthread_cond_signal(&wake);
        pthread_mutex_unlock(&lock);
      }
    } while(running);

    return nullptr;
  }

  void* finalizer_tmain(void*) {
    ASSERT_ALWAYS(tmsg::thread_me() == -1);
    
    for(int t=0; t < tmsg::thread_n; t++) {
      pthread_join(threads[t], nullptr);
      tmsg::ams_w[t].destroy();
    }

    return nullptr;
  }

  void main_exited() {
    pthread_mutex_lock(&lock);
    shutdown = true;
    pthread_cond_signal(&wake);
    pthread_mutex_unlock(&lock);
  }
}

void tmsg::run(upcxx::function_ref<void()> fn) {
  static bool inited = false;

  run_fn = fn;
  
  if(!inited) {
    inited = true;

    (void)pthread_cond_init(&wake, nullptr);
    (void)pthread_mutex_init(&lock, nullptr);
    
    threads[0] = pthread_self();
    for(int t=1; t < thread_n; t++) {
      (void)pthread_create(&threads[t], nullptr, tmain, reinterpret_cast<void*>(t));
    }

    #if DEBUG
      pthread_t the_finalizer;
      (void)pthread_create(&the_finalizer, nullptr, finalizer_tmain, nullptr);
    #endif

    std::atexit(main_exited);
  }
  else {
    pthread_mutex_lock(&lock);
    run_epoch += 1;
    pthread_cond_signal(&wake);
    pthread_mutex_unlock(&lock);
  }
  
  tmain(0);
}
