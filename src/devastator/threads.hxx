////////////////////////////////////////////////////////////////////////////////
// forward declarations

#ifndef _37b7d24adb2d4adea562b808318a0461
#define _37b7d24adb2d4adea562b808318a0461

#include <devastator/utility.hxx>
#include <upcxx/utility.hpp>

#include <cstdint>

namespace deva {
namespace threads {
  constexpr int thread_n = DEVA_THREAD_N;
  constexpr int log2_thread_n = log_up(thread_n, 2);

  int const& thread_me();

  std::uint64_t epoch_low64();
  std::uint64_t epoch_mod3();
  
  struct epoch_transition {
    static epoch_transition *all_head_;
    epoch_transition *all_next_;

    epoch_transition() {
      this->all_next_ = all_head_;
      all_head_ = this;
    }

    virtual void transition(std::uint64_t epoch_low64, int epoch_mod3) = 0;
  };
  
  template<typename Fn>
  void send(int thread, Fn &&fn);
  
  bool progress(bool deaf=false);
  void progress_epoch();
  
  void barrier(bool deaf=false);
  
  void run(upcxx::detail::function_ref<void()> fn);
  
  template<typename Fn>
  void bcast_peers(Fn fn);
}}
#endif

////////////////////////////////////////////////////////////////////////////////
// definitions/implementation

#ifndef _aa2e908bfc6640b9b4d0cd6090738760
#define _aa2e908bfc6640b9b4d0cd6090738760

#include <devastator/threads/barrier_state.hxx>
#include <devastator/threads/message.hxx>

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

  inline std::uint64_t epoch_low64() {
    return epoch_barrier_l_.epoch64();
  }
  inline std::uint64_t epoch_mod3() {
    return epoch_mod3_;
  }
  
  template<typename Fn>
  void send(int thread, Fn &&fn) {
    send_am(ams_w[thread_me_], thread, static_cast<Fn&&>(fn));
  }
  
  template<typename Fn>
  void bcast_peers(Fn fn) {
    for(int t=0; t < thread_n; t++) {
      if(t != thread_me_)
        send_am(ams_w[thread_me_], t, fn);
    }
  }
}}
#endif
