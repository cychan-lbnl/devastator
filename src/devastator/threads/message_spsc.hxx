#include <devastator/threads/message.hxx>

#ifndef _22a7222317264148b00b1c730d390bbd
#define _22a7222317264148b00b1c730d390bbd

#include <devastator/opnew.hxx>
#include <devastator/threads/signal_slots.hxx>

#include <atomic>
#include <cstdint>
#include <new>

namespace deva {
namespace threads {
  #if 1
    #define DEVA_CAT3_1(a,b,c) a##b##c
    #define DEVA_CAT3(a,b,c) DEVA_CAT3_1(a,b,c)
    using uint_signal_t = DEVA_CAT3(std::uint, DEVA_THREADS_SPSC_BITS, _t);
    #undef DEVA_CAT3
    #undef DEVA_CAT3_1
  #else // really low credit mode to test for backpressure correctness
    #undef DEVA_THREADS_SPSC_BITS
    #define DEVA_THREADS_SPSC_BITS 2
    using uint_signal_t = std::uint8_t;
  #endif
  
  static constexpr int signal_bits = DEVA_THREADS_SPSC_BITS;

  inline bool u32_less(std::uint32_t a, std::uint32_t b) {
    return b-a - 1 < ~(~std::uint32_t(0)>>1); // a < b
  }
  inline bool u32_less_eq(std::uint32_t a, std::uint32_t b) {
    return b-a < ~(~std::uint32_t(0)>>1); // a <= b
  }
  
  struct message {
    message *next = reinterpret_cast<message*>(0xdeadbeef);
  };
  
  template<int rn>
  class channels_r {
    template<int wn, int rn1, channels_r<rn1>(*)[wn]>
    friend class channels_w;
  
    struct each {
      message *recv_last;
      std::atomic<uint_signal_t> *ack_slot;
    } r_[rn];
    
    signal_slots<uint_signal_t, rn> slots_;
    std::atomic<int> slot_next_{0};
    
  public:
    template<typename Rcv>
    bool receive(Rcv &&rcv);
    template<typename Rcv, typename Batch>
    bool receive_batch(Rcv &&rcv, Batch &&batch);

  private:
    void prefetch(int hot_n, hot_slot<uint_signal_t> hot[]);
  };
  
  template<int wn, int rn, channels_r<rn>(*chan_r)[wn]>
  class channels_w {
    struct each {
      message *sent_last;
      message *ack_head;
      std::atomic<uint_signal_t> *recv_slot;
      std::uint32_t recv_bump = 0;
      #if DEVA_THREADS_SPSC_BITS < 32
        std::uint32_t recv_bump_wall = 1<<signal_bits;
      #endif
    } w_[wn];
    
    signal_slots<uint_signal_t, wn> slots_;

  public:
    channels_w() = default;
    
    void connect();
    void destroy();
    void send(int id, message *m);
    bool steward();
  };

  //////////////////////////////////////////////////////////////////////////////

  template<int wn, int rn, channels_r<rn>(*chan_r)[wn]>
  void channels_w<wn,rn,chan_r>::destroy() {
    for(int i=0; i < wn; i++) {
      while(w_[i].ack_head != w_[i].sent_last)
        steward();
      
      ::operator delete(w_[i].ack_head);
    }
  }
    
  template<int wn, int rn, channels_r<rn>(*chan_r)[wn]>
  void channels_w<wn,rn,chan_r>::connect() {
    for(int w_id=0; w_id < wn; w_id++) {
      message *dummy = new(operator new(sizeof(message))) message;
      channels_r<rn> *rs = &(*chan_r)[w_id];
      int r_id = rs->slot_next_.fetch_add(1);
      
      rs->r_[r_id].recv_last = dummy;
      rs->r_[r_id].ack_slot = &slots_.live.atom[w_id];
      
      w_[w_id].sent_last = dummy;
      w_[w_id].ack_head = dummy;
      w_[w_id].recv_slot = &rs->slots_.live.atom[r_id];
    }
  }

  template<int wn, int rn, channels_r<rn>(*chan_r)[wn]>
  void channels_w<wn,rn,chan_r>::send(int id, message *m) {
    auto *w = &this->w_[id];
    w->sent_last->next = m;
    w->sent_last = m;
    w->recv_bump += 1;
    #if DEVA_THREADS_SPSC_BITS < 32
      if(u32_less(w->recv_bump, w->recv_bump_wall))
        w->recv_slot->store(w->recv_bump, std::memory_order_release);
      //else
      //  deva::say()<<"BACKPRESSURED";
    #else
      w->recv_slot->store(w->recv_bump, std::memory_order_release);
    #endif
    //say()<<"wchan "<<id<<" of "<<n<<" bumped "<<w->recv_bump-1<<" -> "<<w->recv_bump;
  }
  
  template<int rn>
  __attribute__((noinline))
  void channels_r<rn>::prefetch(int hot_n, hot_slot<uint_signal_t> hot[]) {
    #if DEVA_THREADS_SPSC_PREFETCH >= 1
      std::atomic_signal_fence(std::memory_order_acq_rel);
      
      message **mp[rn];
      
      for(int i=0; i < hot_n; i++) {
        mp[i] = &r_[hot[i].ix].recv_last->next;
        #if DEVA_THREADS_SPSC_PREFETCH == 1
          __builtin_prefetch(mp[i]);
        #else
          __builtin_prefetch(*mp[i]);
        #endif
      }
      
      std::atomic_signal_fence(std::memory_order_acq_rel);
    #endif
  }
  
  template<int rn>
  template<typename Rcv>
  bool channels_r<rn>::receive(Rcv &&rcv) {
    hot_slot<uint_signal_t> hot[rn];
    int hot_n = this->slots_.reap(hot);
    
    prefetch(hot_n, hot);
    
    for(int i=0; i < hot_n; i++) {
      channels_r::each *ch = &this->r_[hot[i].ix];
      std::uint32_t msg_n = hot[i].delta;
      message *m = ch->recv_last;
      
      //say()<<"rchan "<<hot[i].ix<<" of "<<rn<<" bumped "<<hot[i].old<<" -> "<<hot[i].old+hot[i].delta;
      
      do {
        m = m->next;
        rcv(m);
      } while(--msg_n != 0);

      ch->ack_slot->store(hot[i].old + hot[i].delta, std::memory_order_release);
      ch->recv_last = m;
    }
    
    return hot_n != 0; // did something
  }

  template<int rn>
  template<typename Rcv, typename Batch>
  bool channels_r<rn>::receive_batch(Rcv &&rcv, Batch &&batch) {
    hot_slot<uint_signal_t> hot[rn];
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

  template<int wn, int rn, channels_r<rn>(*chan_r)[wn]>
  bool channels_w<wn,rn,chan_r>::steward() {
    hot_slot<uint_signal_t> hot[wn];
    int hot_n = this->slots_.reap(hot);
    
    for(int i=0; i < hot_n; i++) {
      channels_w::each *w = &this->w_[hot[i].ix];
      std::uint32_t msg_n = hot[i].delta;
      
      #if DEVA_THREADS_SPSC_BITS < 32
        if(u32_less_eq(w->recv_bump_wall + msg_n-1, w->recv_bump))
          w->recv_slot->store(w->recv_bump_wall + msg_n-1, std::memory_order_release);
        else if(u32_less_eq(w->recv_bump_wall, w->recv_bump))
          w->recv_slot->store(w->recv_bump, std::memory_order_release);
        
        w->recv_bump_wall += msg_n;
      #endif
      
      message *m = w->ack_head;
      do {
        message *m1 = m->next;
        #if DEVA_OPNEW
          opnew::template operator_delete</*known_size=*/0, /*known_local=*/true>(m);
        #else
          ::operator delete(m);
        #endif
        m = m1;
      } while(--msg_n != 0);
      
      w->ack_head = m;
    }

    return hot_n != 0; // did something
  }
}}
#endif
