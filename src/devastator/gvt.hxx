#ifndef _a0009246_4028_4372_88b9_4bbf7c6096f9
#define _a0009246_4028_4372_88b9_4bbf7c6096f9

#include <devastator/diagnostic.hxx>
#include <devastator/world.hxx>

#include <array>
#include <algorithm>
#include <cstdint>

namespace deva {
  namespace gvt {
    struct reducibles {
      std::uint64_t sum1, sum2;
      
      void reduce_with(reducibles const &that)  {
        sum1 += that.sum1;
        sum2 += that.sum2;
      }
    };
    
    void init(std::uint64_t gvt, reducibles rxs0);
    
    template<bool3 local, typename Fn, typename ...Arg>
    void send(int rank, cbool3<local>, std::uint64_t t, Fn fn, Arg ...arg);

    template<typename Fn, typename ...Arg>
    void send(int rank, std::uint64_t t, Fn fn, Arg ...arg);
    
    void advance();

    void coll_begin(std::uint64_t lvt, reducibles rxs);
    bool coll_ended();
    reducibles coll_reducibles();

    bool epoch_ended();
    std::uint64_t epoch_gvt();
  }

  namespace gvt {
    extern __thread bool coll_ended_, epoch_ended_;
    extern __thread reducibles coll_rxs_;
    extern __thread std::uint64_t epoch_gvt_;
    
    extern __thread unsigned epoch_;
    extern __thread std::uint64_t epoch_lvt_[2];
    extern __thread std::uint64_t epoch_lsend_[2];
    extern __thread std::uint64_t epoch_lrecv_[3];
  }

  inline bool gvt::coll_ended() {
    return gvt::coll_ended_;
  }
  inline gvt::reducibles gvt::coll_reducibles() {
    return gvt::coll_rxs_;
  }

  inline bool gvt::epoch_ended() {
    return gvt::epoch_ended_;
  }
  inline std::uint64_t gvt::epoch_gvt() {
    return gvt::epoch_gvt_;
  }

  template<typename Fn, typename ...Arg>
  void send(int rank, std::uint64_t t, Fn fn, Arg ...arg) {
    gvt::template send<maybe3>(rank, t, std::move(fn), std::move(arg)...);
  }
  
  template<bool3 local, typename Fn, typename ...Arg>
  void gvt::send(int rank, cbool3<local> local1, std::uint64_t t, Fn fn, Arg ...arg) {
    DEVA_ASSERT(gvt::epoch_gvt_ <= t);
    
    unsigned e = gvt::epoch_ + 1;
    gvt::epoch_lsend_[1] += 1;
    gvt::epoch_lvt_[1] = std::min(gvt::epoch_lvt_[1], t);
    
    deva::send(rank, local1,
      [=](Fn &fn, Arg &...arg) {
        int i = e >= gvt::epoch_ ? int(e - gvt::epoch_) : -int(gvt::epoch_ - e);
        DEVA_ASSERT(0 <= i && i < 3);
        DEVA_ASSERT(gvt::epoch_gvt_ <= t);
        
        gvt::epoch_lrecv_[i] += 1;
        
        fn(arg...);
      },
      std::move(fn), std::move(arg)...
    );
  }
} // namespace deva
#endif
