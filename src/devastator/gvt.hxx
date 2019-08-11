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

    template<typename ProcFn>
    void bcast_procs(std::uint64_t t_lb, std::int32_t credit_n, ProcFn const &proc_fn);
    
    void advance();

    void coll_begin(std::uint64_t lvt, reducibles rxs);
    bool coll_ended();
    reducibles coll_reducibles();

    bool coll_was_epoch();
    std::uint64_t epoch_gvt();
  }

  namespace gvt {
    enum class coll_status_e  {
      reducing,
      quiesced,
      non_quiesced,
    };

    extern __thread coll_status_e coll_status_[2];
    extern __thread reducibles coll_rxs_[2];
    extern __thread std::uint64_t epoch_gvt_[2];
    
    extern __thread unsigned epoch_;
    extern __thread std::uint64_t epoch_lvt_[2];
    extern __thread std::uint64_t epoch_lsend_[2];
    extern __thread std::uint64_t epoch_lrecv_[3];
  }

  inline void gvt::advance() {
    coll_status_[0] = coll_status_[1];
    coll_rxs_[0] = coll_rxs_[1];
    epoch_gvt_[0] = epoch_gvt_[1];
  }
  
  inline bool gvt::coll_ended() {
    return coll_status_[0] != coll_status_e::reducing;
  }
  
  inline gvt::reducibles gvt::coll_reducibles() {
    return coll_rxs_[0];
  }

  inline bool gvt::coll_was_epoch() {
    return coll_status_[0] == coll_status_e::quiesced;
  }
  
  inline std::uint64_t gvt::epoch_gvt() {
    return epoch_gvt_[0];
  }

  template<typename Fn, typename ...Arg>
  void gvt::send(int rank, std::uint64_t t, Fn fn, Arg ...arg) {
    gvt::template send<maybe3>(rank, t, std::move(fn), std::move(arg)...);
  }
  
  template<bool3 local, typename Fn, typename ...Arg>
  void gvt::send(int rank, cbool3<local> local1, std::uint64_t t, Fn fn, Arg ...arg) {
    DEVA_ASSERT(epoch_gvt_[0] <= t);
    
    unsigned e = epoch_ + 1;
    epoch_lsend_[1] += 1;
    epoch_lvt_[1] = std::min(epoch_lvt_[1], t);
    
    deva::send(rank, local1,
      [=](Fn &fn, Arg &...arg) {
        int i = e >= epoch_ ? int(e - epoch_) : -int(epoch_ - e);
        DEVA_ASSERT(0 <= i && i < 3);
        DEVA_ASSERT(epoch_gvt_[0] <= t);
        
        epoch_lrecv_[i] += 1;
        
        fn(arg...);
      },
      std::move(fn), std::move(arg)...
    );
  }

  template<typename ProcFn>
  void gvt::bcast_procs(std::uint64_t t_lb, std::int32_t credits, ProcFn const &proc_fn) {
    DEVA_ASSERT(epoch_gvt_[0] <= t_lb);
    
    unsigned e = epoch_ + 1;
    epoch_lsend_[1] += credits;
    epoch_lvt_[1] = std::min(epoch_lvt_[1], t_lb);

    deva::bcast_procs(
      deva::bind(
        [=](ProcFn const &proc_fn1) {
          proc_fn1(/*run_at_rank*/[&](int rank, auto fn) {
            deva::send_local(rank,
              [=, fn1(std::move(fn))]() {
                int i = e >= epoch_ ? int(e - epoch_) : -int(epoch_ - e);
                DEVA_ASSERT(0 <= i && i < 3);
                DEVA_ASSERT(epoch_gvt_[0] <= t_lb);
                
                std::int32_t credits = fn1();
                epoch_lrecv_[i] += credits;
              }
            );
          });
        },
        proc_fn
      )
    );
  }
} // namespace deva
#endif
