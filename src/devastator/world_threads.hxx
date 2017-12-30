#ifndef _ea1a72b1274c4ccbbd173e0dbd75abec
#define _ea1a72b1274c4ccbbd173e0dbd75abec

#ifndef RANK_N
  #error "-DRANK_N=<num> required"
#endif

#define THREAD_N RANK_N
#include "tmessage.hxx"

namespace world {
  inline constexpr int log2up(int x) {
    return x <= 0 ? -1 :
           x == 1 ? 0 :
           x == 3 ? 2 :
           1 + log2up((x/2) | (x%2));
  }
  
  constexpr int rank_n = tmsg::thread_n;
  constexpr int log2up_rank_n = log2up(rank_n);

  inline int rank_me() {
    return tmsg::thread_me();
  }
  
  template<typename Fn>
  void send(int rank, Fn fn) {
    tmsg::send(rank, std::move(fn));
  }
  
  inline void progress() {
    tmsg::progress();
  }

  inline void barrier() {
    tmsg::barrier();
  }
  
  inline void run(const std::function<void()> &fn) {
    tmsg::run(fn);
  }
}

#endif
