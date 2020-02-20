#include <devastator/threads.hxx>
#include <devastator/threads/message.hxx>
#include <devastator/opnew.hxx>
#include <devastator/os_env.hxx>

#include <atomic>

#include <external/pthread.h>
#include <sched.h>
#include <sys/mman.h>
#include <errno.h>

namespace opnew = deva::opnew;
namespace threads = deva::threads;

__thread int threads::thread_me_ = -1;
__thread int threads::epoch_mod3_ = 0;
__thread threads::barrier_state_local<threads::thread_n> threads::barrier_l_;
__thread threads::barrier_state_local<threads::thread_n> threads::epoch_barrier_l_;

#if DEVA_THREADS_ALLOC_EPOCH
__thread char *threads::msg_arena_base_;
__thread threads::epoch_allocator<threads::msg_arena_epochs> threads::msg_arena_;
#endif
  
threads::channels_r<threads::thread_n> threads::ams_r[thread_n];
threads::channels_w<threads::thread_n, threads::thread_n, &threads::ams_r> threads::ams_w[thread_n];

namespace {
  threads::barrier_state_global<threads::thread_n> barrier_g_;
  threads::barrier_state_global<threads::thread_n> epoch_barrier_g_;
}

void threads::progress_begin(threads::progress_state &ps) {
  int const me = thread_me_;

  ps.epoch_old = epoch_barrier_l_.epoch();
  ps.epoch_bumped = epoch_barrier_l_.try_end(epoch_barrier_g_, me);
  
  if(ps.epoch_bumped) {    
    epoch_mod3_ += 1;
    if(epoch_mod3_ == 3)
      epoch_mod3_ = 0;

    #if DEVA_THREADS_ALLOC_EPOCH
      msg_arena_.bump_epoch();
    #endif
    
    ps.backlogged = false;
  }
  
  opnew::progress();

  ams_w[me].reclaim(ps);

  ps.did_something |= ams_r[me].receive(
    [](message *m) {
      auto *am = static_cast<active_message*>(m);
      am->execute_and_destruct(am);
    },
    ps.epoch_bumped, ps.epoch_old
  );
}

void threads::progress_end(threads::progress_state ps) {
  if(ps.epoch_bumped) {
    epoch_barrier_l_.begin(epoch_barrier_g_, thread_me_);
  }
}

void threads::barrier(void(*progress_work)(threads::progress_state&)) {
  barrier_l_.begin(barrier_g_, thread_me_);

  int spun = 0;
  while(!barrier_l_.try_end(barrier_g_, thread_me_)) {
    if(progress_work != nullptr) {
      progress_state ps;
      threads::progress_begin(ps);
      progress_work(ps);
      threads::progress_end(ps);
    }
    
    if(++spun == 100) {
      spun = 0;
      sched_yield();
    }
  }
}

namespace {
  pthread_t thread_ids[threads::thread_n];
  pthread_mutex_t lock;
  pthread_cond_t wake;
  unsigned run_epoch = 0;
  bool shutdown = false;
  upcxx::detail::function_ref<void()> run_fn;

  #if DEVA_THREADS_ALLOC_EPOCH
    void *msg_arena_bases[threads::thread_n];
    std::size_t msg_arena_capacity;
  #endif
  
  void* tmain(void *me1) {
    int me = reinterpret_cast<std::intptr_t>(me1);
    static bool zero_inited = false;

    if(me != 0 || !zero_inited) {
      if(me == 0)
        zero_inited = true;
      
      threads::thread_me_ = me;

      #if DEVA_THREADS_ALLOC_EPOCH
        threads::msg_arena_base_ = (char*)msg_arena_bases[me];
        threads::msg_arena_.init(threads::msg_arena_base_, msg_arena_capacity);
      #endif
      
      opnew::thread_me_initialized();
      
      threads::ams_w[me].connect();
      
      threads::epoch_barrier_l_.begin(epoch_barrier_g_, me);
    }

    bool running;
    unsigned run_epoch_prev = 0;
    do {
      run_fn();
      threads::barrier(nullptr/*deaf*/);

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
    DEVA_ASSERT_ALWAYS(threads::thread_me() == -1);
    
    for(int t=0; t < threads::thread_n; t++) {
      pthread_join(thread_ids[t], nullptr);
    }
    
    for(int t=0;t < threads::thread_n; t++) {
      #if DEVA_THREADS_ALLOC_EPOCH
        munmap(msg_arena_bases[t], msg_arena_capacity);
      #endif
      
      threads::ams_w[t].destroy();
    }
    
    return nullptr;
  }

  void main_exited() {
    pthread_mutex_lock(&lock);
    shutdown = true;
    pthread_cond_signal(&wake);
    pthread_mutex_unlock(&lock);
    
    #if DEBUG
      pthread_exit(0);
    #endif
  }
}

void threads::run(upcxx::detail::function_ref<void()> fn) {
  static bool inited = false;

  run_fn = fn;
  
  if(!inited) {
    inited = true;

    (void)pthread_cond_init(&wake, nullptr);
    (void)pthread_mutex_init(&lock, nullptr);

    #if DEVA_THREADS_ALLOC_EPOCH
    {
      msg_arena_capacity = deva::os_env<std::size_t>("DEVA_TMSG_ARENA_MB", 1024) << 20;
      msg_arena_capacity = (msg_arena_capacity + 8192-1) & -8192;
      
      for(int t=0; t < thread_n; t++) {
        void *arena = mmap(nullptr, msg_arena_capacity, PROT_READ|PROT_WRITE, MAP_PRIVATE|MAP_ANONYMOUS|MAP_NORESERVE, -1, 0);
        DEVA_ASSERT_ALWAYS(arena != MAP_FAILED, "mmap of DEVA_TMSG_ARENA_MB="<<msg_arena_capacity<<" failed, errno="<<errno);
        msg_arena_bases[t] = arena;
      }
    }
    #endif
    
    thread_ids[0] = pthread_self();
    for(int t=1; t < thread_n; t++) {
      (void)pthread_create(&thread_ids[t], nullptr, tmain, reinterpret_cast<void*>(t));
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
