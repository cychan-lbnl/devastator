#include <devastator/world.hxx>
#include <devastator/tmsg.hxx>
#include <devastator/opnew.hxx>

#include <atomic>

#include <pthread.h>
#include <sched.h>

namespace opnew = deva::opnew;
namespace tmsg = deva::tmsg;

__thread int tmsg::thread_me_ = -1;
__thread int tmsg::epoch_mod3_ = 0;
__thread tmsg::barrier_state_local<tmsg::thread_n> tmsg::barrier_l_;
__thread tmsg::barrier_state_local<tmsg::thread_n> tmsg::epoch_barrier_l_;

tmsg::active_channels_r<tmsg::thread_n> tmsg::ams_r[thread_n];
tmsg::active_channels_w<tmsg::thread_n> tmsg::ams_w[thread_n];

tmsg::epoch_transition* tmsg::epoch_transition::all_head = nullptr;

namespace {
  tmsg::barrier_state_global<tmsg::thread_n> barrier_g_;
  tmsg::barrier_state_global<tmsg::thread_n> epoch_barrier_g_;
}

void tmsg::progress_epoch() {
  if(epoch_barrier_l_.try_end(epoch_barrier_g_, thread_me_)) {
    // previous epoch values
    std::uint64_t e64 = epoch_barrier_l_.epoch64()-1; // incremented by successful advance()
    int e3 = epoch_mod3_;
    
    epoch_mod3_ += 1;
    if(epoch_mod3_ == 3)
      epoch_mod3_ = 0;
    
    for(epoch_transition *et = epoch_transition::all_head; et != nullptr; et = et->all_next)
      et->transition(e64, e3);

    epoch_barrier_l_.begin(epoch_barrier_g_, thread_me_);
  }
}

bool tmsg::progress(bool deaf) {
  int const me = thread_me_;
  
  tmsg::progress_epoch();
  
  opnew::progress();

  bool did_something = ams_w[me].cleanup();
  if(!deaf)
    did_something |= ams_r[me].receive();
  
  return did_something;
}

void tmsg::barrier(bool quiesced) {
  barrier_l_.begin(barrier_g_, thread_me_);

  int spun = 0;
  while(!barrier_l_.try_end(barrier_g_, thread_me_)) {
    tmsg::progress(/*deaf=*/quiesced);
    
    if(++spun == 100) {
      spun = 0;
      sched_yield();
    }
  }

  if(quiesced) {
    DEVA_ASSERT_ALWAYS(ams_r[thread_me_].quiet());
    
    barrier_l_.begin(barrier_g_, thread_me_);
    
    while(!barrier_l_.try_end(barrier_g_, thread_me_)) {
      tmsg::progress(/*deaf=*/quiesced);
      
      if(++spun == 100) {
        spun = 0;
        sched_yield();
      }
    }

    DEVA_ASSERT_ALWAYS(ams_w[thread_me_].quiet());
  }
}

namespace {
  pthread_t threads[tmsg::thread_n];
  pthread_mutex_t lock;
  pthread_cond_t wake;
  unsigned run_epoch = 0;
  bool shutdown = false;
  upcxx::detail::function_ref<void()> run_fn;
  
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

      tmsg::epoch_barrier_l_.begin(epoch_barrier_g_, me);
    }

    bool running;
    unsigned run_epoch_prev = 0;
    do {
      run_fn();
      tmsg::barrier(/*quiesced=*/true);

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
    DEVA_ASSERT_ALWAYS(tmsg::thread_me() == -1);
    
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

void tmsg::run(upcxx::detail::function_ref<void()> fn) {
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
