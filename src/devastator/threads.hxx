#ifndef _aa2e908bfc6640b9b4d0cd6090738760
#define _aa2e908bfc6640b9b4d0cd6090738760

#ifndef DEVA_THREAD_N
#  error "-DDEVA_THREAD_N=<num> required"
#endif

#include <devastator/utility.hxx>
#include <upcxx/utility.hpp>

namespace deva {
namespace threads {
  //////////////////////////////////////////////////////////////////////////////
  // Public API
  
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

#if DEVA_THREADS_SPSC
  #include <devastator/threads/threads_spsc.hxx>
#endif

#endif
