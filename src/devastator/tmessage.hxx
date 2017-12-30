#ifndef _185047ccc3634e778407efc12d5bea5d
#define _185047ccc3634e778407efc12d5bea5d

#include "diagnostic.hxx"

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <functional>
#include <new>
#include <utility>

#ifndef THREAD_N
#  error "-DTHREAD_N=<num> required"
#endif

namespace tmsg {
  struct hot_slot {
    std::uint32_t ix, delta, bump;
  };
  
  // TODO: specialize for n=1
  template<int n>
  class slot_list {
    static constexpr int block_n = (n + 15)/16;
    
    struct block {
      union alignas(64) {
        std::atomic<std::uint32_t> live[16];
      };
      union alignas(64) {
        std::uint32_t shadow[16];
      };
      
      block() {
        for(int i=0; i < 16; i++) {
          live[i].store(0, std::memory_order_relaxed);
          shadow[i] = 0;
        }
      }
    };

    block block_[block_n];

  public:
    std::atomic<uint32_t>& operator[](int i) {
      return block_[i/16].live[i%16];
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
    ~channels_w();
    
    template<int m>
    void connect(int id, channels_r<m> &rs);

    void send(int id, message *m);

    bool cleanup();
  };

  template<int n>
  channels_w<n>::~channels_w() {
    cleanup();
    
    for(int i=0; i < n; i++)
      ::operator delete(w_[i].ack_head);
  }
    
  template<int n>
  template<int m>
  void channels_w<n>::connect(int w_id, channels_r<m> &rs) {
    message *dummy = new(operator new(sizeof(message))) message;
    int r_id = rs.slot_next_.fetch_add(1);
    
    rs.r_[r_id].recv_last = dummy;
    rs.r_[r_id].ack_slot = &slots_[w_id];
    
    w_[w_id].sent_last = dummy;
    w_[w_id].ack_head = dummy;
    w_[w_id].recv_slot = &rs.slots_[r_id];
  }

  template<int n>
  void channels_w<n>::send(int id, message *m) {
    w_[id].sent_last->next = m;
    w_[id].sent_last = m;
    w_[id].recv_slot->store(++w_[id].recv_bump, std::memory_order_release);
  }
  
  template<int n>
  int slot_list<n>::reap(hot_slot hot[n]) {
    int hot_n = 0;
    
    for(int b=0; b < block_n; b++) {
      int m = std::min(n, 16*(b+1)) - 16*b;
      for(int s=0; s < m; s++) {
        std::uint32_t w1 = block_[b].live[s].load(std::memory_order_relaxed);
        std::uint32_t w0 = block_[b].shadow[s];
        
        if(w1 != w0) {
          hot[hot_n].ix = 16*b + s;
          hot[hot_n].delta = w1 - w0;
          hot[hot_n].bump = w0;
          hot_n += 1;
          
          block_[b].shadow[s] = w1;
        }
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
      
      do {
        m = m->next;
        rcv(m);
      } while(--msg_n != 0);
      
      ch->ack_slot->store(hot[i].bump + hot[i].delta, std::memory_order_release);
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
      ch->ack_slot->store(hot[i].bump + hot[i].delta, std::memory_order_release);
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
        ::operator delete(m);
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
  
  extern thread_local int thread_me_;

  inline int thread_me() { return thread_me_; }

  template<typename Fn>
  void send(int thread, Fn fn) {
    ams_w[thread_me_].send(thread, std::move(fn));
  }
  
  void progress();

  void barrier();
  
  void run(const std::function<void()> &fn);
}
#endif
