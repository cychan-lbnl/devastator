#ifndef _185047ccc3634e778407efc12d5bea5d
#define _185047ccc3634e778407efc12d5bea5d

#include <devastator/diagnostic.hxx>
#include <devastator/opnew_fwd.hxx>

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
  };

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
    asm volatile("": : :"memory");
    
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

  template<int chan_n>
  template<typename Rcv>
  bool channels_r<chan_n>::receive(Rcv rcv) {
    hot_slot hot[chan_n];
    int hot_n = this->slots_.reap(hot);
    
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

  //////////////////////////////////////////////////////////////////////
  
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

  //////////////////////////////////////////////////////////////////////

  constexpr int thread_n = THREAD_N;
  
  extern active_channels_r<thread_n> ams_r[thread_n];
  extern tmsg::active_channels_w<thread_n> ams_w[thread_n];
  
  extern __thread int thread_me_;

  inline int const& thread_me() {
    return thread_me_;
  }

  template<typename Fn>
  void send(int thread, Fn fn) {
    ams_w[thread_me_].send(thread, std::move(fn));
  }
  
  void progress();
  bool progress_noyield();
  
  void barrier(bool do_progress=true);
  
  void run(upcxx::function_ref<void()> fn);
}
}
#endif
