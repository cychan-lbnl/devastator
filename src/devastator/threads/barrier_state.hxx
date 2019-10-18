#ifndef _6b85c1e4a172458397274fb041231337
#define _6b85c1e4a172458397274fb041231337

#include <devastator/utility.hxx>

#include <atomic>
#include <cstdint>

namespace deva {
namespace threads {
  template<int thread_n>
  class barrier_state_global {
    template<int>
    friend class barrier_state_local;
    
    static constexpr int log2_thread_n = thread_n == 1 ? 1 : log_up(thread_n, 2);

    struct phase_t {
      char slot[log2_thread_n];
    };
    struct alignas(64) phases_t {
      phase_t phase[2];
    };
    
    phases_t hot[thread_n];

  public:
    constexpr barrier_state_global():
      hot{/*zeros...*/} {
    }
  };

  template<int thread_n>
  class barrier_state_local {
    int i;
    char or_acc[2];
    std::uint64_t e64;

  public:
    constexpr barrier_state_local():
      i(0), or_acc{0, 0}, e64(0) {
    }
    
    std::uint64_t epoch64() const { return e64; }
    bool or_result() const { return 0 != or_acc[1-(e64 & 1)]; }
    
    void begin(barrier_state_global<thread_n> &g, int me, bool or_in=false);
    // returns true on barrier completion
    bool try_end(barrier_state_global<thread_n> &g, int me);
    
  private:
    bool advance(barrier_state_global<thread_n> &g, int me);
  };

  template<int thread_n>
  void barrier_state_local<thread_n>::begin(
      barrier_state_global<thread_n> &g, int me, bool or_in
    ) {
    std::atomic_thread_fence(std::memory_order_release);
    
    int ph = this->e64 & 1;
    int peer = me + 1;
    if(peer == thread_n)
      peer = 0;
    g.hot[peer].phase[ph].slot[0] = 0x1 | (or_in ? 0x2 : 0x0);
    
    this->i = 0;
    this->or_acc[ph] = 0x1 | (or_in ? 0x2 : 0x0);
    this->advance(g, me);
  }

  template<int thread_n>
  bool barrier_state_local<thread_n>::try_end(
      barrier_state_global<thread_n> &g, int me
    ) {
    if(this->advance(g, me)) {
      std::atomic_thread_fence(std::memory_order_acquire);
      
      int ph = this->e64 & 1;
      g.hot[me].phase[ph] = {/*zeros...*/};
      this->or_acc[ph] ^= 0x1; // clear notify bit
      this->e64 += 1;
      return true;
    }
    else
      return false;
  }

  template<int thread_n>
  bool barrier_state_local<thread_n>::advance(
      barrier_state_global<thread_n> &g, int me
    ) {
    
    if(thread_n == 1)
      return true;
    
    int ph = this->e64 & 1;
    auto hot = g.hot[me].phase[ph];
    
    std::atomic_signal_fence(std::memory_order_acq_rel);

    constexpr int log2_thread_n = barrier_state_global<thread_n>::log2_thread_n;
    
    for(; i < log2_thread_n-1; i++) {
      if(hot.slot[i] != 0) {
        or_acc[ph] |= hot.slot[i];
        int peer = me + (1<<(i+1));
        if(peer >= thread_n)
          peer -= thread_n;
        g.hot[peer].phase[ph].slot[i+1] = or_acc[ph];
      }
      else
        return false;
    }

    or_acc[ph] |= hot.slot[log2_thread_n-1];
    return hot.slot[log2_thread_n-1] != 0;
  }
}}
#endif
