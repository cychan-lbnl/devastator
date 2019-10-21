// The forwarded API this header is implementing
#include <devastator/world.hxx>

#ifndef _ea1a72b1274c4ccbbd173e0dbd75abec
#define _ea1a72b1274c4ccbbd173e0dbd75abec

#ifndef DEVA_THREAD_N
  #error "-DDEVA_THREAD_N=<num> required"
#endif

#include <devastator/threads.hxx>

#include <devastator/utility.hxx>

#include <functional>
#include <tuple>

namespace deva {
  constexpr int rank_n = threads::thread_n;
  constexpr int process_n = 1;
  constexpr int worker_n = rank_n;
  constexpr int log2up_rank_n = log_up(rank_n, 2);

  #define SERIALIZED_FIELDS(...) /*nothing*/
  
  inline void run(upcxx::detail::function_ref<void()> fn) {
    threads::run(fn);
  }

  inline void run_and_die(upcxx::detail::function_ref<void()> fn) {
    run(fn);
    std::exit(0);
  }

  inline int rank_me() {
    return threads::thread_me();
  }
  inline int rank_me_local() {
    return rank_me();
  }
  
  inline bool rank_is_local(int rank) {
    return true;
  }
  
  inline int process_me() { return 0; }
  constexpr int process_rank_lo(int proc) { return 0; }
  constexpr int process_rank_hi(int proc) { return rank_n; }
  
  inline void barrier(bool deaf) {
    threads::barrier(/*deaf=*/deaf);
  }
  
  template<typename Fn, typename ...B>
  struct bound {
    Fn fn_;
    mutable std::tuple<B...> b_;

    template<typename Me, std::size_t ...bi, typename ...Arg>
    static auto apply(Me &&me, std::index_sequence<bi...>, Arg &&...a)
      -> decltype(static_cast<Me&&>(me).fn_(std::get<bi>(static_cast<Me&&>(me).b_)..., std::forward<Arg>(a)...)) {
      return static_cast<Me&&>(me).fn_(std::get<bi>(static_cast<Me&&>(me).b_)..., std::forward<Arg>(a)...);
    }
    
    using b_ixseq = std::make_index_sequence<sizeof...(B)>;
    
    template<typename ...Arg>
    auto operator()(Arg &&...a) const &
      -> decltype(apply(*this, b_ixseq(), static_cast<Arg&&>(a)...)) {
      return apply(*this, b_ixseq(), static_cast<Arg&&>(a)...);
    }
    template<typename ...Arg>
    auto operator()(Arg &&...a) &
      -> decltype(apply(*this, b_ixseq(), static_cast<Arg&&>(a)...)) {
      return apply(*this, b_ixseq(), static_cast<Arg&&>(a)...);
    }
    template<typename ...Arg>
    auto operator()(Arg &&...a) &&
      -> decltype(apply(static_cast<bound&&>(*this), b_ixseq(), static_cast<Arg&&>(a)...)) {
      return apply(static_cast<bound&&>(*this), b_ixseq(), static_cast<Arg&&>(a)...);
    }
  };
  
  template<typename Fn, typename ...B>
  bound<Fn,B...> bind(Fn fn, B ...b) {
    return bound<Fn,B...>{static_cast<Fn&&>(fn), std::tuple<B...>(static_cast<B&&>(b)...)};
  }
  
  template<typename Fn>
  Fn&& bind(Fn &&fn) {
    return static_cast<Fn&&>(fn);
  }
  
  template<typename Fn, typename ...Arg>
  void send(int rank, Fn &&fn, Arg &&...arg) {
    threads::send(rank, deva::bind(static_cast<Fn&&>(fn), static_cast<Arg&&>(arg)...));
  }
  template<bool3 local, typename Fn, typename ...Arg>
  void send(int rank, cbool3<local>, Fn &&fn, Arg &&...arg) {
    threads::send(rank, deva::bind(static_cast<Fn&&>(fn), static_cast<Arg&&>(arg)...));
  }

  template<typename Fn, typename ...Arg>
  void send_local(int rank, Fn &&fn, Arg &&...arg) {
    threads::send(rank, deva::bind(static_cast<Fn&&>(fn), static_cast<Arg&&>(arg)...));
  }
  
  template<typename Fn, typename ...Arg>
  void send_remote(int rank, Fn &&fn, Arg &&...arg) {
    DEVA_ASSERT(0);
  }

  template<typename ProcFn>
  void bcast_procs(ProcFn &&proc_fn) {
    static_cast<ProcFn&&>(proc_fn)();
  }
}

#endif
