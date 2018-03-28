#ifndef _a0009246_4028_4372_88b9_4bbf7c6096f9
#define _a0009246_4028_4372_88b9_4bbf7c6096f9

#include "diagnostic.hxx"
#include "world.hxx"

#include <array>
#include <algorithm>
#include <cstdint>

namespace gvt {
  struct reducibles {
    std::uint64_t sum1, sum2;
    
    void reduce_with(reducibles const &that)  {
      sum1 += that.sum1;
      sum2 += that.sum2;
    }
  };
  
  void init(reducibles rxs0);
  
  template<typename Fn>
  void send(int rank, std::uint64_t t, Fn fn);
  template<typename Fn>
  void send_local(int rank, std::uint64_t t, Fn fn);
  template<typename Fn>
  void send_remote(int rank, std::uint64_t t, Fn fn);

  void advance();

  void epoch_begin(std::uint64_t lvt, reducibles rxs);
  bool epoch_ended();
  std::uint64_t epoch_gvt();
  reducibles epoch_reducibles();
}

namespace gvt {
  extern __thread bool epoch_ended_;
  extern __thread std::uint64_t epoch_gvt_;
  extern __thread reducibles epoch_rxs_;
  
  extern __thread unsigned round_;
  extern __thread std::uint64_t round_lvt_[2];
  extern __thread std::uint64_t round_lsend_[2];
  extern __thread std::uint64_t round_lrecv_[3];
}

inline bool gvt::epoch_ended() {
  return gvt::epoch_ended_;
}

inline std::uint64_t gvt::epoch_gvt() {
  return gvt::epoch_gvt_;
}

inline gvt::reducibles gvt::epoch_reducibles() {
  return gvt::epoch_rxs_;
}

template<typename Fn>
void gvt::send(int rank, std::uint64_t t, Fn fn) {
  ASSERT(gvt::epoch_gvt_ <= t);
  
  unsigned r = gvt::round_ + 1;
  gvt::round_lsend_[1] += 1;
  gvt::round_lvt_[1] = std::min(gvt::round_lvt_[1], t);
  
  world::send(rank,
    world::bind(
      [=](Fn const &fn) {
        int i = r >= gvt::round_ ? int(r - gvt::round_) : -int(gvt::round_ - r);
        ASSERT(0 <= i && i < 3);
        ASSERT(gvt::epoch_gvt_ <= t);
        
        gvt::round_lrecv_[i] += 1;
        
        fn();
      },
      std::move(fn)
    )
  );
}

template<typename Fn>
void gvt::send_local(int rank, std::uint64_t t, Fn fn) {
  ASSERT(gvt::epoch_gvt_ <= t);
  
  unsigned r = gvt::round_ + 1;
  gvt::round_lsend_[1] += 1;
  gvt::round_lvt_[1] = std::min(gvt::round_lvt_[1], t);
  
  world::send_local(rank,
    world::bind(
      [=](Fn const &fn) {
        int i = r >= gvt::round_ ? int(r - gvt::round_) : -int(gvt::round_ - r);
        ASSERT(0 <= i && i < 3);
        ASSERT(gvt::epoch_gvt_ <= t);
        
        gvt::round_lrecv_[i] += 1;
        
        fn();
      },
      std::move(fn)
    )
  );
}

template<typename Fn>
void gvt::send_remote(int rank, std::uint64_t t, Fn fn) {
  ASSERT(gvt::epoch_gvt_ <= t);
  
  unsigned r = gvt::round_ + 1;
  gvt::round_lsend_[1] += 1;
  gvt::round_lvt_[1] = std::min(gvt::round_lvt_[1], t);
  
  world::send_remote(rank,
    world::bind(
      [=](Fn const &fn) {
        int i = r >= gvt::round_ ? int(r - gvt::round_) : -int(gvt::round_ - r);
        ASSERT(0 <= i && i < 3);
        ASSERT(gvt::epoch_gvt_ <= t);
        
        gvt::round_lrecv_[i] += 1;
        
        fn();
      },
      std::move(fn)
    )
  );
}
#endif
