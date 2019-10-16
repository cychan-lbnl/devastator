#ifndef _bbb850e8e4e2461988f43975411ccdf3
#define _bbb850e8e4e2461988f43975411ccdf3

// The API this file implements forward declared here
#include <devastator/threads.hxx>

#include <devastator/threads/barrier_state.hxx>
#include <devastator/threads/signal_slots.hxx>

#include <atomic>
#include <cstdint>

namespace deva {
namespace threads {
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
    
    signal_slots<std::uint32_t, n> slots_;
    std::atomic<int> slot_next_{0};
    
  public:
    template<typename Rcv>
    bool receive(Rcv &&rcv);
    template<typename Rcv, typename Batch>
    bool receive_batch(Rcv &&rcv, Batch &&batch);
    
    bool possibly_quiesced() const {
      return slots_.possibly_quiesced();
    }
    
  private:
    void prefetch(int hot_n, hot_slot<std::uint32_t> hot[]);
  };
  
  template<int n>
  class channels_w {
    struct each {
      message *sent_last;
      message *ack_head;
      std::atomic<std::uint32_t> *recv_slot;
      std::uint32_t recv_bump = 0; 
    } w_[n];
    
    signal_slots<std::uint32_t, n> slots_;

  public:
    channels_w() = default;
    
    void destroy();
    
    template<int m>
    void connect(int id, channels_r<m> &rs);

    void send(int id, message *m);

    bool cleanup();
    
    bool possibly_quiesced() const {
      return slots_.possibly_quiesced();
    }
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
  
  template<int chan_n>
  void channels_r<chan_n>::prefetch(int hot_n, hot_slot<std::uint32_t> hot[]) {
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
  bool channels_r<chan_n>::receive(Rcv &&rcv) {
    hot_slot<std::uint32_t> hot[chan_n];
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
  bool channels_r<chan_n>::receive_batch(Rcv &&rcv, Batch &&batch) {
    hot_slot<std::uint32_t> hot[chan_n];
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
    hot_slot<std::uint32_t> hot[chan_n];
    int hot_n = this->slots_.reap(hot);
    
    for(int i=0; i < hot_n; i++) {
      channels_w::each *ch = &this->w_[hot[i].ix];
      std::uint32_t msg_n = hot[i].delta;
      message *m = ch->ack_head;
      
      do {
        message *m1 = m->next;
        #if DEVA_OPNEW
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
    
    active_message_impl(Fn &&fn):
      fn{static_cast<Fn&&>(fn)} {
      this->execute_and_destruct = the_execute_and_destruct;
    }
  };

  template<int n>
  struct active_channels_w: channels_w<n> {
    template<typename Fn>
    void send(int id, Fn &&fn) {
      auto *m = new(
          ::operator new(sizeof(active_message_impl<Fn>))
        ) active_message_impl<Fn>{static_cast<Fn&&>(fn)};

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
  // deva::threads API implementation
  
  extern active_channels_r<thread_n> ams_r[thread_n];
  extern active_channels_w<thread_n> ams_w[thread_n];
  
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
  
  template<typename Fn>
  void send(int thread, Fn &&fn) {
    ams_w[thread_me_].send(thread, static_cast<Fn&&>(fn));
  }
  
  template<typename Fn>
  void bcast_peers(Fn fn) {
    for(int t=0; t < thread_n; t++) {
      if(t != thread_me_)
        ams_w[thread_me_].send(t, fn);
    }
  }
}}
#endif
