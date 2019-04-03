#ifndef _e9ac220a_a23a_4309_927b_e4bca6cc4634
#define _e9ac220a_a23a_4309_927b_e4bca6cc4634

#ifndef PROCESS_N
#  error "-DPROCESS_N=<num> required"
#endif
#ifndef WORKER_N
  #error "-DWORKER_N=<num> required"
#endif

#define THREAD_N (WORKER_N)+1
#include <devastator/tmsg.hxx>

#include <upcxx/bind.hpp>
#include <upcxx/command.hpp>
#include <upcxx/serialization.hpp>

#include <functional>
#include <utility>

namespace deva {
  inline constexpr int log2up(int x) {
    return x <= 0 ? -1 :
           x == 1 ? 0 :
           x == 3 ? 2 :
           1 + log2up((x/2) | (x%2));
  }

  constexpr int process_n = PROCESS_N;
  constexpr int worker_n = WORKER_N;
  
  constexpr int rank_n = process_n * worker_n;
  constexpr int log2up_rank_n = log2up(rank_n);
  
  extern tmsg::channels_r<worker_n> remote_send_chan_r;
  extern tmsg::channels_w<1> remote_send_chan_w[worker_n];
  extern tmsg::channels_r<1> remote_recv_chan_r[worker_n];
  extern tmsg::channels_w<worker_n> remote_recv_chan_w;

  extern __thread int rank_me_;
  extern int process_me_;
  extern int process_rank_lo_, process_rank_hi_;
  
  inline int rank_me() { return rank_me_; }
  inline int rank_me_local() { return rank_me() - process_rank_lo_; }

  inline bool rank_is_local(int rank) {
    return process_rank_lo_ <= rank && rank < process_rank_hi_;
  }
  
  inline int process_me() { return process_me_; }
  
  struct alignas(8) remote_out_message: tmsg::message {
    int rank, size8;
    remote_out_message *bundle_next;

    template<typename Fn, typename Ub>
    static remote_out_message* create_help(Fn fn, Ub ub, std::false_type ub_valid) {
      typename std::aligned_storage<512,64>::type tmp;
      upcxx::detail::serialization_writer<false> w(&tmp, 512);
      w.template place_new<remote_out_message>();
      upcxx::detail::command::serialize(w, ub.size, fn);
      w.place(0,8);
      DEVA_ASSERT(w.align() <= 8);
      void *buf = operator new(w.size());
      w.compact_and_invalidate(buf);
      auto *rm = new(buf) remote_out_message;
      rm->size8 = (w.size() - sizeof(remote_out_message))/8;
      return rm;
    }
    
    template<typename Fn, typename Ub>
    static remote_out_message* create_help(Fn fn, Ub ub, std::true_type ub_valid) {
      void *buf = operator new(ub.size_aligned(8));
      upcxx::detail::serialization_writer<true> w(buf);
      auto *rm = w.template place_new<remote_out_message>();
      upcxx::detail::command::serialize(w, ub.size, fn);
      DEVA_ASSERT(w.align() <= 8);
      w.place(0,8);
      rm->size8 = (w.size() - sizeof(remote_out_message))/8;
      return rm;
    }
    
    template<typename Fn>
    static remote_out_message* create(int rank, Fn fn) {
      auto ub = upcxx::detail::command::ubound(
          upcxx::storage_size_of<remote_out_message>()
            .cat(0,8),
          fn
        );
      auto *rm = create_help(std::move(fn), ub, std::integral_constant<bool, ub.is_valid>());
      rm->rank = rank;
      return rm;
    }
  };

  using upcxx::bind;

  template<typename Fn, typename ...Arg>
  void send_local(int rank, Fn fn, Arg ...arg) {
    tmsg::send(1 + rank-process_rank_lo_, upcxx::bind(std::move(fn), std::move(arg)...));
  }

  template<typename Fn, typename ...Arg>
  void send_remote(int rank, Fn fn, Arg ...arg) {
    auto *m = remote_out_message::create(rank, upcxx::bind(std::move(fn), std::move(arg)...));
    //say()<<"send_remote to "<<rank<<" size "<<m->size8;
    remote_send_chan_w[rank_me_ - process_rank_lo_].send(0, m);
  }

  template<typename Fn, typename ...Arg>
  void send(int rank, Fn fn, Arg ...arg) {
    auto bound = upcxx::bind(std::move(fn), std::move(arg)...);
    if(rank_is_local(rank))
      send_local(rank, std::move(bound));
    else
      send_remote(rank, std::move(bound));
  }

  void progress();

  void barrier(bool do_progress=true);
  
  void run(upcxx::detail::function_ref<void()> fn);

  inline void run_and_die(upcxx::detail::function_ref<void()> fn) {
    run(fn);
    std::exit(0);
  }
}

#define SERIALIZED_FIELDS UPCXX_SERIALIZED_FIELDS

#endif
