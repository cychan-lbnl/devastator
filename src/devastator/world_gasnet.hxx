#ifndef _e9ac220a_a23a_4309_927b_e4bca6cc4634
#define _e9ac220a_a23a_4309_927b_e4bca6cc4634

#ifndef PROCESS_N
#  error "-DPROCESS_N=<num> required"
#endif
#ifndef WORKER_N
  #error "-DWORKER_N=<num> required"
#endif

#define THREAD_N (WORKER_N)+1
#include "tmsg.hxx"

#include <upcxx/bind.hpp>
#include <upcxx/command.hpp>
#include <upcxx/packing.hpp>

#include <functional>
#include <utility>

namespace world {
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

  extern thread_local int rank_me_;
  extern int process_rank_lo_, process_rank_hi_;
  
  inline int rank_me() { return rank_me_; }

  inline bool rank_is_local(int rank) {
    return process_rank_lo_ <= rank && rank < process_rank_hi_;
  }
  
  template<typename Fn>
  void send_local(int rank, Fn fn) {
    tmsg::send(1 + rank-process_rank_lo_, std::move(fn));
  }

  struct alignas(8) remote_out_message: tmsg::message {
    int rank, size8;
    remote_out_message *bundle_next;
    
    template<typename Fn>
    static remote_out_message* create(int rank, Fn fn) {
      upcxx::parcel_layout ub;
      ub.add_trivial_aligned<remote_out_message>();
      ub.add_bytes(0, 8);
      upcxx::command_size_ubound(ub, fn);
      
      void *buf = operator new(ub.size());
      upcxx::parcel_writer w{buf};
      remote_out_message *rm = w.put_trivial_aligned<remote_out_message>({});
      rm->rank = rank;
      rm->size8 = (ub.size_aligned() - sizeof(remote_out_message))/8;
      
      upcxx::command_pack(w, ub.size(), fn);

      ASSERT(w.alignment() <= 8);
      
      return rm;
    }
  };

  template<typename Fn>
  void send_remote(int rank, Fn fn) {
    auto *m = remote_out_message::create(rank, std::move(fn));
    //say()<<"send_remote to "<<rank<<" size "<<m->size8;
    remote_send_chan_w[rank_me_ - process_rank_lo_].send(0, m);
  }

  template<typename Fn>
  void send(int rank, Fn fn) {
    if(rank_is_local(rank))
      send_local(rank, std::move(fn));
    else
      send_remote(rank, std::move(fn));
  }

  void progress();

  void barrier();
  
  void run_and_die(const std::function<void()> &fn);

  using upcxx::bind;
}
#endif
