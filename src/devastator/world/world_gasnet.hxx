// The forwarded API this header is implementing.
#include <devastator/world.hxx>

#ifndef _86d347eb52d247a290fdf21fe440bce0
#define _86d347eb52d247a290fdf21fe440bce0

#ifndef DEVA_PROCESS_N
  #error "-DDEVA_PROCESS_N=<num> required"
#endif
#ifndef DEVA_WORKER_N
  #error "-DDEVA_WORKER_N=<num> required"
#endif

#include <devastator/threads.hxx>
#include <devastator/utility.hxx>

#include <upcxx/bind.hpp>
#include <upcxx/command.hpp>
#include <upcxx/serialization.hpp>

#include <cstdint>
#include <functional>
#include <utility>

namespace deva {
  constexpr int process_n = DEVA_PROCESS_N;
  constexpr int worker_n = DEVA_WORKER_N;
  
  constexpr int rank_n = process_n * worker_n;
  constexpr int log2up_rank_n = log2up(rank_n);
  
  extern threads::channels_r<threads::thread_n> remote_send_chan_r;
  extern threads::channels_w<1> remote_send_chan_w[threads::thread_n];
  extern threads::channels_r<1> remote_recv_chan_r[threads::thread_n];
  extern threads::channels_w<threads::thread_n> remote_recv_chan_w;

  extern __thread int rank_me_;
  extern int process_me_;
  extern int process_rank_lo_, process_rank_hi_;
  
  void run(upcxx::detail::function_ref<void()> fn);

  inline void run_and_die(upcxx::detail::function_ref<void()> fn) {
    run(fn);
    std::exit(0);
  }
  
  inline int rank_me() { return rank_me_; }
  inline int rank_me_local() { return rank_me() - process_rank_lo_; }

  inline bool rank_is_local(int rank) {
    return process_rank_lo_ <= rank && rank < process_rank_hi_;
  }
  
  inline int process_me() { return process_me_; }
  constexpr int process_rank_lo(int proc) { return proc*worker_n; }
  constexpr int process_rank_hi(int proc) { return (proc+1)*worker_n; }

  void progress(bool spinning, bool deaf);

  void barrier(bool deaf);

  struct alignas(8) remote_out_message: threads::message {
    std::int32_t rank, size8;
    remote_out_message *bundle_next;

    template<typename Fn, typename Ub>
    static remote_out_message* make_help(Fn const &fn, Ub ub, std::false_type ub_valid) {
      typename std::aligned_storage<512,64>::type tmp;
      upcxx::detail::serialization_writer<false> w(&tmp, 512);
      w.template place_new<remote_out_message>();
      upcxx::detail::command::serialize(w, ub.size, fn);
      w.place(0,8);
      DEVA_ASSERT(w.align() <= 8);
      std::size_t w_size = w.size();
      void *buf = operator new(w_size);
      w.compact_and_invalidate(buf);
      auto *rm = new(buf) remote_out_message;
      rm->size8 = (w_size - sizeof(remote_out_message))/8;
      return rm;
    }
    
    template<typename Fn, typename Ub>
    static remote_out_message* make_help(Fn const &fn, Ub ub, std::true_type ub_valid) {
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
    static remote_out_message* make(int rank, Fn const &fn) {
      auto ub = upcxx::detail::command::ubound(
          upcxx::template storage_size_of<remote_out_message>(),
          fn
        );
      auto *rm = make_help(fn, ub, std::integral_constant<bool, ub.is_valid>());
      DEVA_ASSERT(rm->size8 > 0);
      rm->rank = rank;
      return rm;
    }

    static remote_out_message* make(int rank, void const *cmd, std::size_t cmd_size) {
      size_t buf_size =
        upcxx::template storage_size_of<remote_out_message>()
        .cat(cmd_size, 8)
        .size_aligned(8);
      void *buf = operator new(buf_size);
      auto *rm = new(buf) remote_out_message;
      rm->rank = rank;
      rm->size8 = (cmd_size + 7)/8;
      std::memcpy((void*)(rm+1), cmd, cmd_size);
      return rm;
    }
  };

  using upcxx::bind;

  template<typename Fn, typename ...Arg>
  void send_local(int rank, Fn &&fn, Arg &&...arg) {
    DEVA_ASSERT(rank == ~process_me_ || (process_rank_lo_ <= rank && rank < process_rank_hi_));
    threads::send(
      rank < 0 ? 0 : 1 + rank-process_rank_lo_,
      upcxx::bind(static_cast<Fn&&>(fn), static_cast<Arg&&>(arg)...)
    );
  }

  template<typename Fn, typename ...Arg>
  void send_remote(int rank, Fn &&fn, Arg &&...arg) {
    auto *m = remote_out_message::make(rank,
      upcxx::bind(
        [](auto const &fn_on_args, void const *cmd, std::size_t cmd_size) {
          fn_on_args();
        },
        upcxx::bind(static_cast<Fn&&>(fn), static_cast<Arg&&>(arg)...)
      )
    );
    //say()<<"send_remote to "<<rank<<" size "<<m->size8;
    remote_send_chan_w[threads::thread_me()].send(0, m);
  }

  template<typename Fn, typename ...Arg>
  void send(int rank, Fn &&fn, Arg &&...arg) {
    if(rank_is_local(rank))
      send_local(rank, static_cast<Fn&&>(fn), static_cast<Arg&&>(arg)...);
    else
      send_remote(rank, static_cast<Fn&&>(fn), static_cast<Arg&&>(arg)...);
  }

  template<typename Fn, typename ...Arg>
  void send(int rank, ctrue3_t local, Fn &&fn, Arg &&...arg) {
    send_local(rank, static_cast<Fn&&>(fn), static_cast<Arg&&>(arg)...);
  }
  template<typename Fn, typename ...Arg>
  void send(int rank, cfalse3_t local, Fn fn, Arg ...arg) {
    send_remote(rank, static_cast<Fn&&>(fn), static_cast<Arg&&>(arg)...);
  }
  template<typename Fn, typename ...Arg>
  void send(int rank, cmaybe3_t local, Fn fn, Arg ...arg) {
    send(rank, static_cast<Fn&&>(fn), static_cast<Arg&&>(arg)...);
  }
  
  void bcast_remote_sends_(int proc_root, void const *cmd, std::size_t cmd_size);
  
  template<typename ProcFn>
  void bcast_procs(ProcFn &&proc_fn) {
    std::int32_t proc_root = process_me_;
    
    auto relay_fn = deva::bind(
      [=](ProcFn const &proc_fn, void const *cmd, std::size_t cmd_size) {
        deva::bcast_remote_sends_(proc_root, cmd, cmd_size);
        proc_fn();
      },
      static_cast<ProcFn&&>(proc_fn)
    );
    
    threads::send(0, [relay_fn(std::move(relay_fn))]() {
      auto *rm = remote_out_message::make(0xdeadbeef, relay_fn);
      relay_fn((void*)(rm+1), 8*std::size_t(rm->size8));
      operator delete((void*)rm);
    });
  }
}

#define SERIALIZED_FIELDS UPCXX_SERIALIZED_FIELDS

#endif
