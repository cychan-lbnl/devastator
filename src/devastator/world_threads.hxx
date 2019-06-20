#ifndef _ea1a72b1274c4ccbbd173e0dbd75abec
#define _ea1a72b1274c4ccbbd173e0dbd75abec

#ifndef RANK_N
  #error "-DRANK_N=<num> required"
#endif

#define THREAD_N RANK_N
#include <devastator/tmsg.hxx>
#include <devastator/utility.hxx>

#include <functional>
#include <tuple>

namespace deva {
  constexpr int rank_n = tmsg::thread_n;
  constexpr int process_n = 1;
  constexpr int worker_n = rank_n;
  constexpr int log2up_rank_n = log2up(rank_n);

  inline int rank_me() {
    return tmsg::thread_me();
  }
  inline int rank_me_local() {
    return rank_me();
  }
  
  constexpr bool rank_is_local(int rank) {
    return true;
  }
  
  constexpr int process_me() {
    return 0;
  }
  
  void progress(bool spinning=false, bool deaf=false);

  inline void barrier(bool quiesced=false) {
    tmsg::barrier(quiesced);
  }
  
  inline void run(upcxx::detail::function_ref<void()> fn) {
    tmsg::run(fn);
  }

  inline void run_and_die(upcxx::detail::function_ref<void()> fn) {
    run(fn);
    std::exit(0);
  }

  #if 0
    using std::bind;
  #else
    template<typename Fn, typename ...B>
    struct bound {
      Fn fn_;
      mutable std::tuple<B...> b_;

      template<typename Me, std::size_t ...bi, typename ...Arg>
      static auto apply(Me &&me, std::index_sequence<bi...>, Arg &&...a)
        -> decltype(me.fn_(std::get<bi>(me.b_)..., std::forward<Arg>(a)...)) {
        return me.fn_(std::get<bi>(me.b_)..., std::forward<Arg>(a)...);
      }
      
      using b_ixseq = std::make_index_sequence<sizeof...(B)>;
      
      template<typename ...Arg>
      auto operator()(Arg &&...a)
        -> decltype(apply(*this, b_ixseq(), std::forward<Arg>(a)...)) {
        return apply(*this, b_ixseq(), std::forward<Arg>(a)...);
      }
      template<typename ...Arg>
      auto operator()(Arg &&...a) const
        -> decltype(apply(*this, b_ixseq(), std::forward<Arg>(a)...)) {
        return apply(*this, b_ixseq(), std::forward<Arg>(a)...);
      }
    };
    
    template<typename Fn, typename ...B>
    auto bind(Fn fn, B ...b)
      -> decltype(bound<Fn,B...>{std::move(fn), std::tuple<B...>{std::move(b)...}}) {
      return bound<Fn,B...>{std::move(fn), std::tuple<B...>{std::move(b)...}};
    }
    
    template<typename Fn>
    Fn&& bind(Fn &&fn) {
      return static_cast<Fn&&>(fn);
    }
  #endif
  
  template<typename Fn, typename ...Arg>
  void send(int rank, Fn fn, Arg ...arg) {
    tmsg::send(rank, deva::bind(std::move(fn), std::move(arg)...));
  }
  template<bool3 local, typename Fn, typename ...Arg>
  void send(int rank, cbool3<local>, Fn fn, Arg ...arg) {
    tmsg::send(rank, deva::bind(std::move(fn), std::move(arg)...));
  }

  template<typename Fn, typename ...Arg>
  void send_local(int rank, Fn fn, Arg ...arg) {
    tmsg::send(rank, deva::bind(std::move(fn), std::move(arg)...));
  }
  
  template<typename Fn, typename ...Arg>
  void send_remote(int rank, Fn fn, Arg ...arg) {
    DEVA_ASSERT(0);
  }
}

#define SERIALIZED_FIELDS(...) /*nothing*/

#endif
