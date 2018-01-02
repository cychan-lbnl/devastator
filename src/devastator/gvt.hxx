#ifndef _a0009246_4028_4372_88b9_4bbf7c6096f9
#define _a0009246_4028_4372_88b9_4bbf7c6096f9

#include "diagnostic.hxx"
#include "world.hxx"

#include <array>
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
  extern thread_local bool epoch_ended_;
  extern thread_local std::uint64_t epoch_gvt_;
  extern thread_local reducibles epoch_rxs_;
  
  extern thread_local unsigned round_;
  extern thread_local std::uint64_t round_lvt_;
  extern thread_local std::uint64_t round_lsend_;
  extern thread_local std::uint64_t round_lrecv_[3];
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
  std::uint8_t r = gvt::round_ + 1;
  gvt::round_lsend_ += 1;
  gvt::round_lvt_ = std::min(gvt::round_lvt_, t);
  
  world::send(rank,
    world::bind(
      [=](Fn const &fn) {
        int i = (unsigned(r) - gvt::round_) & 0xff;
        gvt::round_lrecv_[i] += 1;
        
        fn();
      },
      std::move(fn)
    )
  );
}

template<typename Fn>
void gvt::send_local(int rank, std::uint64_t t, Fn fn) {
  std::uint8_t r = gvt::round_ + 1;
  gvt::round_lsend_ += 1;
  gvt::round_lvt_ = std::min(gvt::round_lvt_, t);
  
  world::send_local(rank,
    world::bind(
      [=](Fn const &fn) {
        int i = (unsigned(r) - gvt::round_) & 0xff;
        gvt::round_lrecv_[i] += 1;
        
        fn();
      },
      std::move(fn)
    )
  );
}

template<typename Fn>
void gvt::send_remote(int rank, std::uint64_t t, Fn fn) {
  std::uint8_t r = gvt::round_ + 1;
  gvt::round_lsend_ += 1;
  gvt::round_lvt_ = std::min(gvt::round_lvt_, t);
  
  world::send_remote(rank,
    world::bind(
      [=](Fn const &fn) {
        int i = (unsigned(r) - gvt::round_) & 0xff;
        gvt::round_lrecv_[i] += 1;
        
        fn();
      },
      std::move(fn)
    )
  );
}
#endif
