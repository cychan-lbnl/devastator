////////////////////////////////////////////////////////////////////////////////
// forward declarations

#ifndef _37b7d24adb2d4adea562b808318a0461
#define _37b7d24adb2d4adea562b808318a0461

#ifndef DEVA_THREAD_N
  #define DEVA_THREAD_N 1
#endif

#ifndef DEVA_THREADS_ALLOC_OPNEW_SYM
  #define DEVA_THREADS_ALLOC_OPNEW_SYM 0
#endif
#ifndef DEVA_THREADS_ALLOC_OPNEW_ASYM
  #define DEVA_THREADS_ALLOC_OPNEW_ASYM 0
#endif
#ifndef DEVA_THREADS_ALLOC_EPOCH
  #define DEVA_THREADS_ALLOC_EPOCH 0
#endif

#include <devastator/utility.hxx>
#include <upcxx/utility.hpp>

#include <cstdint>

namespace deva {
namespace threads {
  constexpr int thread_n = DEVA_THREAD_N;
  constexpr int log2_thread_n = log_up(thread_n, 2);

  int const& thread_me();

  std::uint64_t epoch();
  std::uint64_t epoch_mod3();
  
  template<typename Fn>
  void send(int thread, Fn &&fn);

  struct progress_state {
    bool did_something = false;
    bool backlogged = false;
    bool epoch_bumped;
    std::uint64_t epoch_old;
  };

  void progress_begin(progress_state &ps);
  void progress_end(progress_state ps);
  
  void barrier(void(*progress_work)(threads::progress_state&));
  
  void run(upcxx::detail::function_ref<void()> fn);
  
  template<typename Fn>
  void bcast_peers(Fn fn);
}}

////////////////////////////////////////////////////////////////////////////////
// internal declarations

#if DEVA_THREADS_ALLOC_EPOCH
  #include <devastator/threads/epoch_allocator.hxx>

  namespace deva {
  namespace threads {
    extern __thread char *msg_arena_base_;
    constexpr int msg_arena_epochs = 5;
    extern __thread epoch_allocator<msg_arena_epochs> msg_arena_;
  }}
#endif

namespace deva {
namespace threads {
  void* alloc_message(std::size_t size, std::size_t align);
  void dealloc_message(void *m);
}}

#endif

////////////////////////////////////////////////////////////////////////////////
// definitions/implementation

#ifndef _aa2e908bfc6640b9b4d0cd6090738760
#define _aa2e908bfc6640b9b4d0cd6090738760

#include <devastator/threads/barrier_state.hxx>
#include <devastator/threads/message.hxx>
#include <devastator/opnew.hxx>

namespace deva {
namespace threads {
  extern channels_r<thread_n> ams_r[thread_n];
  extern channels_w<thread_n, thread_n, &ams_r> ams_w[thread_n];
  
  extern __thread int thread_me_;
  extern __thread int epoch_mod3_;
  extern __thread barrier_state_local<thread_n> barrier_l_;
  extern __thread barrier_state_local<thread_n> epoch_barrier_l_;

  inline int const& thread_me() {
    return thread_me_;
  }

  inline std::uint64_t epoch() {
    return epoch_barrier_l_.epoch();
  }
  inline std::uint64_t epoch_mod3() {
    return epoch_mod3_;
  }

  inline void* alloc_message(std::size_t size, std::size_t align) {
    #if 1 && DEVA_THREADS_ALLOC_EPOCH
      return msg_arena_.allocate(size, align);
    #else
      return ::operator new(size);
    #endif
  }

  inline void dealloc_message(void *m) {
    #if 1 && DEVA_THREADS_ALLOC_EPOCH
      // nop
    #else
      #if DEVA_OPNEW
        opnew::template operator_delete</*known_size=*/0, /*known_local=*/DEVA_THREADS_ALLOC_OPNEW_SYM>(m);
      #else
        ::operator delete(m);
      #endif
    #endif
  }

  struct active_message: message {
    void(*execute_and_destruct)(active_message*);
  };

  template<typename Fn>
  struct active_message_impl final: active_message {
    Fn fn;

    static void the_execute_and_destruct(active_message *m) {
      auto *me = static_cast<active_message_impl<Fn>*>(m);
      static_cast<Fn&&>(me->fn)();
      me->fn.~Fn();
    }
    
    active_message_impl(Fn &&fn):
      fn(static_cast<Fn&&>(fn)) {
      this->execute_and_destruct = the_execute_and_destruct;
    }
  };

  template<typename Fn1>
  void send(int thread, Fn1 &&fn) {
    using Fn = typename std::decay<Fn1>::type;
    using Msg = active_message_impl<Fn>;
    void *m = alloc_message(sizeof(Msg), alignof(Msg));
    ams_w[thread_me_].send(thread, ::new(m) Msg{static_cast<Fn1&&>(fn)});
  }
  
  template<typename Fn>
  void bcast_peers(Fn fn) {
    for(int t=0; t < thread_n; t++) {
      if(t != thread_me_)
        send(t, fn);
    }
  }
}}
#endif
