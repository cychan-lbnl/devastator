#include <devastator/threads/message.hxx>

#ifndef _22a7222317264148b00b1c730d390bbd
#define _22a7222317264148b00b1c730d390bbd

#include <devastator/opnew.hxx>
#include <devastator/threads/signal_slots.hxx>

#include <atomic>
#include <cstdint>
#include <new>

#if DEVA_THREADS_ALLOC_EPOCH
  #error "talloc=epoch not supported with tmsg=spsc"
#endif

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
    #define DEVA_THREADS_SPSC_BITS 8
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
    #if DEVA_THREADS_ALLOC_OPNEW_SYM
      std::atomic<uint_signal_t> *ack_slot;
    #endif
    } r_[rn];
    
    signal_slots<uint_signal_t, rn> recv_slots_;
    std::atomic<int> slot_next_{0};
    
  public:
    template<typename Rcv>
    bool receive(Rcv &&rcv, bool epoch_bumped, std::uint64_t old_epoch);
    template<typename Rcv, typename Batch>
    bool receive_batch(Rcv &&rcv, Batch &&batch, bool epoch_bumped, std::uint64_t old_epoch);

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
        #if DEVA_THREADS_ALLOC_OPNEW_ASYM
          uint_signal_t epoch_ack_bump[4] = {0,0,0,0};
        #endif
      #endif
    } w_[wn];

    #if DEVA_THREADS_ALLOC_OPNEW_SYM
      signal_slots<uint_signal_t, wn> ack_slots_;
    #endif
    
  public:
    channels_w() = default;
    
    void connect();
    void destroy();
    void send(int id, message *m);
    void reclaim(threads::progress_state &ps);
  };

  //////////////////////////////////////////////////////////////////////////////
  
  template<int wn, int rn, channels_r<rn>(*chan_r)[wn]>
  void channels_w<wn,rn,chan_r>::destroy() {
    for(int i=0; i < wn; i++) {
      #if DEVA_THREADS_ALLOC_OPNEW_SYM
        while(w_[i].ack_head != w_[i].sent_last) {
          threads::progress_state ps;
          reclaim(ps);
        }
        threads::dealloc_message(w_[i].sent_last);
      #elif DEVA_THREADS_ALLOC_OPNEW_ASYM
        threads::dealloc_message(w_[i].sent_last);
      #endif
    }
  }
    
  template<int wn, int rn, channels_r<rn>(*chan_r)[wn]>
  void channels_w<wn,rn,chan_r>::connect() {
    for(int w_id=0; w_id < wn; w_id++) {
      message *dummy = ::new(threads::alloc_message(sizeof(message), alignof(message))) message;
      channels_r<rn> *rs = &(*chan_r)[w_id];
      int r_id = rs->slot_next_.fetch_add(1);
      
      rs->r_[r_id].recv_last = dummy;
      w_[w_id].sent_last = dummy;
      
      #if DEVA_THREADS_ALLOC_OPNEW_SYM
        rs->r_[r_id].ack_slot = &ack_slots_.live.atom[w_id];
        w_[w_id].ack_head = dummy;
      #endif
      
      w_[w_id].recv_slot = &rs->recv_slots_.live.atom[r_id];
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
  bool channels_r<rn>::receive(Rcv &&rcv, bool epoch_bumped, std::uint64_t old_epoch) {
    #if DEVA_THREADS_SPSC_ORDER_DFS
      hot_slot<uint_signal_t> hot[rn];
      int hot_n = this->recv_slots_.reap(hot);
      bool did_something = hot_n != 0;
      
      prefetch(hot_n, hot);
      
      for(int i=0; i < hot_n; i++) {
        channels_r::each *ch = &this->r_[hot[i].ix];
        std::uint32_t msg_n = hot[i].delta;
        message *m = ch->recv_last;
        
        //say()<<"rchan "<<hot[i].ix<<" of "<<rn<<" bumped "<<hot[i].old<<" -> "<<hot[i].old+hot[i].delta;
        
        do {
          message *m1 = m->next;
          #if DEVA_THREADS_ALLOC_OPNEW_ASYM
            threads::dealloc_message(m);
          #endif
          rcv(m1);
          m = m1;
        } while(--msg_n != 0);

        #if DEVA_THREADS_ALLOC_OPNEW_SYM
          ch->ack_slot->store(hot[i].old + hot[i].delta, std::memory_order_release);
        #endif
        ch->recv_last = m;
      }
      
    #elif DEVA_THREADS_SPSC_ORDER_BFS

      hot_slot<uint_signal_t> hot[rn];
      int hot_head = this->recv_slots_.reap_circular(hot);
      bool did_something = hot_head != -1;

      if(did_something) {
        int i_prev = hot_head;
        int i = hot[hot_head].next;
        
        while(true) {
          channels_r::each *ch = &this->r_[i];
          message *m = ch->recv_last->next;
          #if DEVA_THREADS_ALLOC_OPNEW_ASYM
            threads::dealloc_message(ch->recv_last);
          #endif
          ch->recv_last = m;
          rcv(m);
          
          if(0 == --hot[i].delta) {
            #if DEVA_THREADS_ALLOC_OPNEW_SYM
              ch->ack_slot->store(hot[i].now, std::memory_order_release);
            #endif
            if(i == hot[i].next)
              break;
            i = hot[i].next;
            hot[i_prev].next = i;
          }
          else {
            i_prev = i;
            i = hot[i].next;
          }
        }
      }
    #endif
    
    return did_something;
  }

  template<int rn>
  template<typename Rcv, typename Batch>
  bool channels_r<rn>::receive_batch(Rcv &&rcv, Batch &&batch, bool epoch_bumped, std::uint64_t old_epoch) {
    hot_slot<uint_signal_t> hot[rn];
    int hot_n = this->recv_slots_.reap(hot);
    
    prefetch(hot_n, hot);
    
    for(int i=0; i < hot_n; i++) {
      channels_r::each *ch = &this->r_[hot[i].ix];
      std::uint32_t msg_n = hot[i].delta;
      message *m = ch->recv_last;
      
      do {
        message *m1 = m->next;
        #if DEVA_THREADS_ALLOC_OPNEW_ASYM
          threads::dealloc_message(m);
        #endif
        rcv(m1);
        m = m1;
      } while(--msg_n != 0);
      
      ch->recv_last = m;
    }

    batch();
    
    #if DEVA_THREADS_ALLOC_OPNEW_SYM
      for(int i=0; i < hot_n; i++) {
        channels_r::each *ch = &this->r_[hot[i].ix];
        ch->ack_slot->store(hot[i].old + hot[i].delta, std::memory_order_release);
      }
    #endif
    
    return hot_n != 0; // did something
  }

  template<int wn, int rn, channels_r<rn>(*chan_r)[wn]>
  void channels_w<wn,rn,chan_r>::reclaim(threads::progress_state &ps) {
  #if DEVA_THREADS_ALLOC_OPNEW_ASYM
    #if DEVA_THREADS_SPSC_BITS >= 32
      // nop
    #else
      if(ps.epoch_bumped) {
        bool backlogged = false;
        //std::stringstream ss_acks;
        
        for(int i=0; i < wn; i++) {
          channels_w::each *w = &this->w_[i];
          std::uint32_t acks = w->epoch_ack_bump[0];

          //ss_acks<<"creds="<<(u32_less_eq(w->recv_bump_wall, w->recv_bump) ? -int(w->recv_bump - w->recv_bump_wall+1) : int(w->recv_bump_wall-1 - w->recv_bump));
          //ss_acks<<" [";
          //for(int e=0; e < 4; e++)
          //  ss_acks<<int(w->epoch_ack_bump[e])<<' ';
          //ss_acks<<"] ";

          std::uint32_t held = 0;
          for(int e=0; e < 3; e++) {
            held += w->epoch_ack_bump[e+1];
            w->epoch_ack_bump[e] = w->epoch_ack_bump[e+1];
          }

          std::int32_t backlog;
          if(u32_less_eq(w->recv_bump_wall + acks, w->recv_bump)) {
            w->epoch_ack_bump[3] = ((1<<signal_bits)-1) - held;
            backlog = w->recv_bump - (w->recv_bump_wall + acks);
          }
          else {
            w->epoch_ack_bump[3] = ((1<<signal_bits)-1) - held - (w->recv_bump_wall+acks-1 - w->recv_bump);
            backlog = -((w->recv_bump_wall + acks) - w->recv_bump);
          }

          backlogged |= backlog >= 1<<signal_bits;

          //DEVA_ASSERT_ALWAYS(
          //  w->epoch_ack_bump[3] + w->epoch_ack_bump[2] + w->epoch_ack_bump[1] + w->epoch_ack_bump[0] <= (1<<signal_bits)-1,
          //  "held="<<0+w->epoch_ack_bump[0]<<','<<0+w->epoch_ack_bump[1]<<','<<0+w->epoch_ack_bump[2]<<','<<0+w->epoch_ack_bump[3]
          //);
          
          if(u32_less_eq(w->recv_bump_wall + acks-1, w->recv_bump))
            w->recv_slot->store(w->recv_bump_wall + acks-1, std::memory_order_release);
          else if(u32_less_eq(w->recv_bump_wall, w->recv_bump))
            w->recv_slot->store(w->recv_bump, std::memory_order_release);
          
          w->recv_bump_wall += acks;

          DEVA_ASSERT(u32_less_eq(w->recv_bump_wall-(1<<signal_bits), w->recv_bump));
        }
        
        //if(threads::epoch()%1000 == 0) {
        //  deva::say()<<"epoch="<<threads::epoch()<<" backlogged="<<backlogged<<" acks="<<ss_acks.str();
        //}
        
        ps.backlogged |= backlogged;
      }
    #endif
  #else
    hot_slot<uint_signal_t> hot[wn];
    int hot_n = this->ack_slots_.reap(hot);
    
    for(int i=0; i < hot_n; i++) {
      channels_w::each *w = &this->w_[hot[i].ix];
      std::uint32_t acks = hot[i].delta;
      
      #if DEVA_THREADS_SPSC_BITS < 32
        if(u32_less_eq(w->recv_bump_wall + acks-1, w->recv_bump))
          w->recv_slot->store(w->recv_bump_wall + acks-1, std::memory_order_release);
        else if(u32_less_eq(w->recv_bump_wall, w->recv_bump))
          w->recv_slot->store(w->recv_bump, std::memory_order_release);
        
        w->recv_bump_wall += acks;
      #endif
      
      message *m = w->ack_head;
      do {
        message *m1 = m->next;
        threads::dealloc_message(m);
        m = m1;
      } while(--acks != 0);
      
      w->ack_head = m;
    }

    ps.did_something |= hot_n != 0;
  #endif
  }
}}
#endif
