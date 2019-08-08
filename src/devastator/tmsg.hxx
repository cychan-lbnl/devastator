#ifndef _185047ccc3634e778407efc12d5bea5d
#define _185047ccc3634e778407efc12d5bea5d

#include <devastator/diagnostic.hxx>
#include <devastator/opnew_fwd.hxx>
#include <devastator/utility.hxx>

#include <upcxx/utility.hpp>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <new>
#include <utility>

#ifndef THREAD_N
#  error "-DTHREAD_N=<num> required"
#endif

namespace deva {
namespace tmsg {
  struct hot_slot {
    std::uint32_t ix, delta, old;
  };
  
  template<int n>
  struct slot_list {
    union alignas(n > 1 ? 64 : 1) {
      std::atomic<std::uint32_t> live[n];
    };
    union alignas(n > 1 ? 64 : 1) {
      std::uint32_t shadow[n];
    };

    slot_list() {
      for(int i=0; i < n; i++) {
        live[i].store(0, std::memory_order_relaxed);
        shadow[i] = 0;
      }
    }
    
    int reap(hot_slot hot[n]);
    
    bool quiet() const;
  };

  struct message {
    message *next = reinterpret_cast<message*>(0xdeadbeef);
  };
  
  template<int n>
  class channels_w;

  template<int n>
  class channels_r {
    template<int m>
    friend struct channels_w;
    
    struct each {
      message *recv_last;
      std::atomic<std::uint32_t> *ack_slot;
    } r_[n];
    
    slot_list<n> slots_;
    std::atomic<int> slot_next_{0};
    
  public:
    template<typename Rcv>
    bool receive(Rcv rcv);
    template<typename Rcv, typename Batch>
    bool receive_batch(Rcv rcv, Batch batch);
    
    bool quiet() const { return slots_.quiet(); }
    
  private:
    void prefetch(int hot_n, hot_slot hot[]);
  };
  
  template<int n>
  class channels_w {
    struct each {
      message *sent_last;
      message *ack_head;
      std::atomic<std::uint32_t> *recv_slot;
      std::uint32_t recv_bump = 0; 
    } w_[n];
    
    slot_list<n> slots_;

  public:
    channels_w() = default;
    
    void destroy();
    
    template<int m>
    void connect(int id, channels_r<m> &rs);

    void send(int id, message *m);

    bool cleanup();
    bool quiet() const { return slots_.quiet(); }
  };

  //////////////////////////////////////////////////////////////////////////////

  template<int n>
  void channels_w<n>::destroy() {
    for(int i=0; i < n; i++) {
      while(w_[i].ack_head != w_[i].sent_last)
        cleanup();
      
      ::operator delete(w_[i].ack_head);
    }
  }
    
  template<int n>
  template<int m>
  void channels_w<n>::connect(int w_id, channels_r<m> &rs) {
    message *dummy = new(operator new(sizeof(message))) message;
    int r_id = rs.slot_next_.fetch_add(1);
    
    rs.r_[r_id].recv_last = dummy;
    rs.r_[r_id].ack_slot = &slots_.live[w_id];
    
    w_[w_id].sent_last = dummy;
    w_[w_id].ack_head = dummy;
    w_[w_id].recv_slot = &rs.slots_.live[r_id];
  }

  template<int n>
  void channels_w<n>::send(int id, message *m) {
    w_[id].sent_last->next = m;
    w_[id].sent_last = m;
    w_[id].recv_slot->store(++w_[id].recv_bump, std::memory_order_release);
    //say()<<"wchan "<<id<<" of "<<n<<" bumped "<<w_[id].recv_bump-1<<" -> "<<w_[id].recv_bump;
  }
  
  template<int n>
  int slot_list<n>::reap(hot_slot hot[n]) {
    int hot_n = 0;
    std::uint32_t fresh[n];

    for(int i=0; i < n; i++)
      fresh[i] = live[i].load(std::memory_order_relaxed);

    // software fence: prevents load/stores from moving before or after.
    // this is for performance, we want to scan the live counters with
    // dense loads into a temporary "fresh" buffer, and do comparison
    // processing afterwards.
    std::atomic_signal_fence(std::memory_order_acq_rel);
    
    for(int i=0; i < n; i++) {
      if(fresh[i] != shadow[i]) {
        hot[hot_n].ix = i;
        hot[hot_n].delta = fresh[i] - shadow[i];
        hot[hot_n].old = shadow[i];
        hot_n += 1;
        
        shadow[i] = fresh[i];
      }
    }
    
    if(hot_n != 0)
      std::atomic_thread_fence(std::memory_order_acquire);
    
    return hot_n;
  }

  template<int n>
  bool slot_list<n>::quiet() const {
    bool yep = true;
    for(int i=0; i < n; i++)
      yep &= shadow[i] == live[i].load(std::memory_order_relaxed);
    return yep;
  }
  
  template<int chan_n>
  void channels_r<chan_n>::prefetch(int hot_n, hot_slot hot[]) {
  #if 0
    message *mp[chan_n];
    std::uint32_t mn[chan_n];
    
    for(int i=0; i < hot_n; i++) {
      mp[i] = r_[hot[i].ix].recv_last;
      mn[i] = hot[i].delta;
    }
    
    while(hot_n >= 4) {
      if(chan_n < 4) __builtin_unreachable();
      
      int r = 0;
      int w = 0;
      
      while(r + 4 <= hot_n) {
        message *mp4[4];
        std::uint32_t mn4_min = ~std::uint32_t(0);
        
        for(int i=0; i < 4; i++) {
          mp4[i] = mp[r+i];
          mn4_min = std::min(mn4_min, mn[r+i]);
        }

        for(int j=0; j < (int)mn4_min; j++) {
          for(int i=0; i < 4; i++)
            mp4[i] = *(message*volatile*)&mp4[i]->next;
        }
        
        for(int i=0; i < 4; i++) {
          mp[w] = mp4[i];
          mn[w] = mn[r+i] - mn4_min;
          w += mn[w] != 0 ? 1 : 0;
        }
        r += 4;
      }
      
      while(r < hot_n) {
        mp[w] = mp[r];
        mn[w] = mn[r];
        r++; w++;
      }
      
      hot_n = w;
    }
    
    while(hot_n > 1) {
      int r = 0;
      int w = r;
      while(r < hot_n) {
        mp[w] = *(message*volatile*)&mp[r]->next;
        mn[w] = mn[r] - 1;
        
        r += 1;
        w += 0 != mn[w] ? 1 : 0;
      }
      hot_n = w;
    }
  #endif
  }
  
  template<int chan_n>
  template<typename Rcv>
  bool channels_r<chan_n>::receive(Rcv rcv) {
    hot_slot hot[chan_n];
    int hot_n = this->slots_.reap(hot);
    
    prefetch(hot_n, hot);
    
    for(int i=0; i < hot_n; i++) {
      channels_r::each *ch = &this->r_[hot[i].ix];
      std::uint32_t msg_n = hot[i].delta;
      message *m = ch->recv_last;
      
      //say()<<"rchan "<<hot[i].ix<<" of "<<chan_n<<" bumped "<<hot[i].old<<" -> "<<hot[i].old+hot[i].delta;
      
      do {
        m = m->next;
        rcv(m);
      } while(--msg_n != 0);

      ch->ack_slot->store(hot[i].old + hot[i].delta, std::memory_order_release);
      ch->recv_last = m;
    }
    
    return hot_n != 0; // did something
  }

  template<int chan_n>
  template<typename Rcv, typename Batch>
  bool channels_r<chan_n>::receive_batch(Rcv rcv, Batch batch) {
    hot_slot hot[chan_n];
    int hot_n = this->slots_.reap(hot);
    
    prefetch(hot_n, hot);
    
    for(int i=0; i < hot_n; i++) {
      channels_r::each *ch = &this->r_[hot[i].ix];
      std::uint32_t msg_n = hot[i].delta;
      message *m = ch->recv_last;
      
      do {
        m = m->next;
        rcv(m);
      } while(--msg_n != 0);
      
      ch->recv_last = m;
    }

    batch();
    
    for(int i=0; i < hot_n; i++) {
      channels_r::each *ch = &this->r_[hot[i].ix];
      ch->ack_slot->store(hot[i].old + hot[i].delta, std::memory_order_release);
    }
    
    return hot_n != 0; // did something
  }

  template<int chan_n>
  bool channels_w<chan_n>::cleanup() {
    hot_slot hot[chan_n];
    int hot_n = this->slots_.reap(hot);
    
    for(int i=0; i < hot_n; i++) {
      channels_w::each *ch = &this->w_[hot[i].ix];
      std::uint32_t msg_n = hot[i].delta;
      message *m = ch->ack_head;
      
      do {
        message *m1 = m->next;
        #if OPNEW_ENABLED
          opnew::template operator_delete</*known_size=*/0, /*known_local=*/true>(m);
        #else
          ::operator delete(m);
        #endif
        m = m1;
      } while(--msg_n != 0);
      
      ch->ack_head = m;
    }

    return hot_n != 0; // did something
  }

  //////////////////////////////////////////////////////////////////////////////
  
  struct active_message: message {
    void(*execute_and_destruct)(active_message*);
  };

  template<typename Fn>
  struct active_message_impl final: active_message {
    Fn fn;

    static void the_execute_and_destruct(active_message *m) {
      auto *me = static_cast<active_message_impl<Fn>*>(m);
      me->fn();
      me->fn.~Fn();
    }
    
    active_message_impl(Fn fn):
      fn{std::move(fn)} {
      this->execute_and_destruct = the_execute_and_destruct;
    }
  };

  template<int n>
  struct active_channels_w: channels_w<n> {
    template<typename Fn>
    void send(int id, Fn fn) {
      auto *m = new(
          ::operator new(sizeof(active_message_impl<Fn>))
        ) active_message_impl<Fn>{std::move(fn)};

      channels_w<n>::send(id, m);
    }
  };

  template<int n>
  struct active_channels_r: channels_r<n> {
    bool receive() {
      return channels_r<n>::receive(
        [](message *m) {
          auto *am = static_cast<active_message*>(m);
          am->execute_and_destruct(am);
        }
      );
    }
  };

  //////////////////////////////////////////////////////////////////////////////

  template<int thread_n>
  class barrier_state_global {
    template<int>
    friend class barrier_state_local;
    
    static constexpr int log2_thread_n = thread_n == 1 ? 1 : log_up(thread_n, 2);

    struct phase_t {
      char slot[log2_thread_n];
    };
    struct alignas(64) phases_t {
      phase_t phase[2];
    };
    
    phases_t hot[thread_n];

  public:
    constexpr barrier_state_global():
      hot{/*zeros...*/} {
    }
  };

  template<int thread_n>
  class barrier_state_local {
    int i;
    char or_acc[2];
    std::uint64_t e64;

  public:
    constexpr barrier_state_local():
      i(0), or_acc{0, 0}, e64(0) {
    }
    
    std::uint64_t epoch64() const { return e64; }
    bool or_result() const { return 0 != or_acc[1-(e64 & 1)]; }
    
    void begin(barrier_state_global<thread_n> &g, int me, bool or_in=false);
    // returns true on barrier completion
    bool try_end(barrier_state_global<thread_n> &g, int me);
    
  private:
    bool advance(barrier_state_global<thread_n> &g, int me);
  };

  template<int thread_n>
  void barrier_state_local<thread_n>::begin(
      barrier_state_global<thread_n> &g, int me, bool or_in
    ) {
    std::atomic_thread_fence(std::memory_order_release);
    
    int ph = this->e64 & 1;
    int peer = me + 1;
    if(peer == thread_n)
      peer = 0;
    g.hot[peer].phase[ph].slot[0] = 0x1 | (or_in ? 0x2 : 0x0);
    
    this->i = 0;
    this->or_acc[ph] = 0x1 | (or_in ? 0x2 : 0x0);
    this->advance(g, me);
  }

  template<int thread_n>
  bool barrier_state_local<thread_n>::try_end(
      barrier_state_global<thread_n> &g, int me
    ) {
    if(this->advance(g, me)) {
      std::atomic_thread_fence(std::memory_order_acquire);
      
      int ph = this->e64 & 1;
      g.hot[me].phase[ph] = {/*zeros...*/};
      this->or_acc[ph] ^= 0x1; // clear notify bit
      this->e64 += 1;
      return true;
    }
    else
      return false;
  }

  template<int thread_n>
  bool barrier_state_local<thread_n>::advance(
      barrier_state_global<thread_n> &g, int me
    ) {
    
    if(thread_n == 1)
      return true;
    
    int ph = this->e64 & 1;
    auto hot = g.hot[me].phase[ph];
    
    std::atomic_signal_fence(std::memory_order_acq_rel);

    constexpr int log2_thread_n = barrier_state_global<thread_n>::log2_thread_n;
    
    for(; i < log2_thread_n-1; i++) {
      if(hot.slot[i] != 0) {
        or_acc[ph] |= hot.slot[i];
        int peer = me + (1<<(i+1));
        if(peer >= thread_n)
          peer -= thread_n;
        g.hot[peer].phase[ph].slot[i+1] = or_acc[ph];
      }
      else
        return false;
    }

    or_acc[ph] |= hot.slot[log2_thread_n-1];
    return hot.slot[log2_thread_n-1] != 0;
  }

  //////////////////////////////////////////////////////////////////////////////
  // deva::tmsg public API
  
  constexpr int thread_n = THREAD_N;
  constexpr int log2_thread_n = log_up(thread_n, 2);
  
  extern active_channels_r<thread_n> ams_r[thread_n];
  extern tmsg::active_channels_w<thread_n> ams_w[thread_n];
  
  extern __thread int thread_me_;
  extern __thread int epoch_mod3_;
  extern __thread barrier_state_local<thread_n> barrier_l_;
  extern __thread barrier_state_local<thread_n> epoch_barrier_l_;
  
  inline int const& thread_me() {
    return thread_me_;
  }

  inline std::uint64_t epoch_low64() {
    return epoch_barrier_l_.epoch64();
  }
  inline std::uint64_t epoch_mod3() {
    return epoch_mod3_;
  }
  
  struct epoch_transition {
    static epoch_transition *all_head;
    epoch_transition *all_next;

    epoch_transition() {
      this->all_next = all_head;
      all_head = this;
    }

    virtual void transition(std::uint64_t epoch_low64, int epoch_mod3) = 0;
  };
  
  template<typename Fn>
  void send(int thread, Fn fn) {
    ams_w[thread_me_].send(thread, std::move(fn));
  }
  
  bool progress(bool deaf=false);
  void progress_epoch();
  
  void barrier(bool deaf=false);

  void run(upcxx::detail::function_ref<void()> fn);

  template<typename Fn>
  void bcast_peers(Fn fn) {
    for(int t=0; t < thread_n; t++) {
      if(t != thread_me_)
        ams_w[thread_me_].send(t, fn);
    }
  }
}
}
#endif
