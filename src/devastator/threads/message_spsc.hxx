#include <devastator/threads/message.hxx>

#ifndef _22a7222317264148b00b1c730d390bbd
#define _22a7222317264148b00b1c730d390bbd

#include <devastator/opnew.hxx>
#include <devastator/threads/signal_slots.hxx>

#include <atomic>
#include <cstdint>
#include <new>

#if 0 && DEVA_THREADS_ALLOC_EPOCH
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
    #if DEVA_THREADS_ALLOC_OPNEW_SYM || DEVA_THREADS_ALLOC_OPNEW_ASYM
      message *recv_last;
      #if DEVA_THREADS_ALLOC_OPNEW_SYM
        std::atomic<uint_signal_t> *ack_slot;
      #endif
    #elif DEVA_THREADS_ALLOC_EPOCH
      message *head[3]{reinterpret_cast<message*>(0xbeefbeef),reinterpret_cast<message*>(0xbeefbeef),reinterpret_cast<message*>(0xbeefbeef)};
      std::atomic<message**> tailp_live[3]{{&head[0]},{&head[1]},{&head[2]}};
      message **tailp_shadow[3]{&head[0],&head[1],&head[2]};
    #endif
    } r_[rn];
    
    signal_slots<uint_signal_t, rn> recv_slots_;
    std::atomic<int> slot_next_{0};
    
  public:
    template<typename Rcv>
    void receive(Rcv &&rcv, threads::progress_state &st);
    template<typename Rcv, typename Batch>
    void receive_batch(Rcv &&rcv, Batch &&batch, threads::progress_state &st);

  private:
    void prefetch(int hot_n, hot_slot<uint_signal_t> hot[]);
  };
  
  template<int wn, int rn, channels_r<rn>(*chan_r)[wn]>
  class channels_w {
    struct each {
      #if DEVA_THREADS_ALLOC_OPNEW_SYM || DEVA_THREADS_ALLOC_OPNEW_ASYM
        message *sent_last;
        #if DEVA_THREADS_ALLOC_OPNEW_SYM
          message *ack_head;
        #endif
      #elif DEVA_THREADS_ALLOC_EPOCH
        message **tailp;
      #endif
      
      std::int32_t recv_slot;
      std::uint32_t recv_bump = 0;
      #if DEVA_THREADS_SPSC_BITS < 32
        #if DEVA_THREADS_ALLOC_OPNEW_SYM || DEVA_THREADS_ALLOC_OPNEW_ASYM
          std::uint32_t recv_bump_wall = 1<<signal_bits;
          #if !DEVA_THREADS_ALLOC_OPNEW_SYM
            uint_signal_t epoch_ack_bump[4] = {0,0,0,0};
          #endif
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
      #if DEVA_THREADS_ALLOC_OPNEW_SYM || DEVA_THREADS_ALLOC_OPNEW_ASYM
        #if DEVA_THREADS_ALLOC_OPNEW_SYM
          while(w_[i].ack_head != w_[i].sent_last) {
            threads::progress_state ps;
            reclaim(ps);
          }
        #endif
        threads::dealloc_message(w_[i].sent_last);
      #endif
    }
  }
    
  template<int wn, int rn, channels_r<rn>(*chan_r)[wn]>
  void channels_w<wn,rn,chan_r>::connect() {
    for(int w_id=0; w_id < wn; w_id++) {
      message *dummy = ::new(threads::alloc_message(sizeof(message), alignof(message))) message;
      channels_r<rn> *rs = &(*chan_r)[w_id];
      int slot = rs->slot_next_.fetch_add(1);
      w_[w_id].recv_slot = slot;
      
      #if DEVA_THREADS_ALLOC_OPNEW_SYM || DEVA_THREADS_ALLOC_OPNEW_ASYM
        rs->r_[slot].recv_last = dummy;
        w_[w_id].sent_last = dummy;
        #if DEVA_THREADS_ALLOC_OPNEW_SYM
          rs->r_[slot].ack_slot = &ack_slots_.live.atom[w_id];
          w_[w_id].ack_head = dummy;
        #endif
      #elif DEVA_THREADS_ALLOC_EPOCH
        w_[w_id].tailp = &rs->r_[slot].head[0];
      #endif
    }
  }

  template<int wn, int rn, channels_r<rn>(*chan_r)[wn]>
  void channels_w<wn,rn,chan_r>::send(int id, message *m) {
    auto *w = &this->w_[id];
    w->recv_bump += 1;
    #if DEVA_THREADS_ALLOC_OPNEW_SYM || DEVA_THREADS_ALLOC_OPNEW_ASYM
      w->sent_last->next = m;
      w->sent_last = m;
    #else
      const int e3 = threads::epoch_mod3();
      *w->tailp = m;
      w->tailp = &m->next;
      (*chan_r)[id].r_[w->recv_slot].tailp_live[e3].store(&m->next, std::memory_order_release);
    #endif

    constexpr auto bump_mo = DEVA_THREADS_ALLOC_EPOCH ? std::memory_order_relaxed : std::memory_order_release;

    #if !DEVA_THREADS_ALLOC_EPOCH && DEVA_THREADS_SPSC_BITS < 32
      if(u32_less(w->recv_bump, w->recv_bump_wall))
        (*chan_r)[id].recv_slots_.live.atom[w->recv_slot].store(w->recv_bump, bump_mo);
      //else
      //  deva::say()<<"BACKPRESSURED";
    #else
      (*chan_r)[id].recv_slots_.live.atom[w->recv_slot].store(w->recv_bump, bump_mo);
    #endif
  }

  template<int wn, int rn, channels_r<rn>(*chan_r)[wn]>
  void channels_w<wn,rn,chan_r>::reclaim(threads::progress_state &ps) {
  #if DEVA_THREADS_ALLOC_OPNEW_SYM
    hot_slot<uint_signal_t> hot[wn];
    int hot_n = this->ack_slots_.reap(hot);
    
    for(int i=0; i < hot_n; i++) {
      int w_id = hot[i].ix_xor_i ^ i;
      channels_w::each *w = &this->w_[w_id];
      std::uint32_t acks = hot[i].delta;
      
      #if DEVA_THREADS_SPSC_BITS < 32
        auto *slot = &(*chan_r)[w_id].recv_slots_.live.atom[w->recv_slot];
        if(u32_less_eq(w->recv_bump_wall + acks-1, w->recv_bump))
          slot->store(w->recv_bump_wall + acks-1, std::memory_order_release);
        else if(u32_less_eq(w->recv_bump_wall, w->recv_bump))
          slot->store(w->recv_bump, std::memory_order_release);
        
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

  #elif DEVA_THREADS_ALLOC_OPNEW_ASYM
  
    if(ps.epoch_bumped) {
      bool backlogged = false;
      
      for(int i=0; i < wn; i++) {
        channels_w::each *w = &this->w_[i];
        
        #if DEVA_THREADS_SPSC_BITS < 32
          std::uint32_t acks = w->epoch_ack_bump[0];
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

          auto *slot = &(*chan_r)[i].recv_slots_.live.atom[w->recv_slot];
          if(u32_less_eq(w->recv_bump_wall + acks-1, w->recv_bump))
            slot->store(w->recv_bump_wall + acks-1, std::memory_order_release);
          else if(u32_less_eq(w->recv_bump_wall, w->recv_bump))
            slot->store(w->recv_bump, std::memory_order_release);
          
          w->recv_bump_wall += acks;

          DEVA_ASSERT(u32_less_eq(w->recv_bump_wall-(1<<signal_bits), w->recv_bump));
        #endif
      }
      
      //if(threads::epoch()%1000 == 0) {
      //  deva::say()<<"epoch="<<threads::epoch()<<" backlogged="<<backlogged<<" acks="<<ss_acks.str();
      //}
      
      ps.backlogged |= backlogged;
    }
    
  #elif DEVA_THREADS_ALLOC_EPOCH
  
    if(ps.epoch_bumped) {
      int e3 = threads::epoch_mod3();
      for(int i=0; i < wn; i++) {
        channels_w::each *w = &this->w_[i];
        w->tailp = &(*chan_r)[i].r_[w->recv_slot].head[e3];
      }
    }

  #endif
  }

  //////////////////////////////////////////////////////////////////////////////
  
  template<int rn>
  __attribute__((noinline))
  void channels_r<rn>::prefetch(int hot_n, hot_slot<uint_signal_t> hot[]) {
    #if DEVA_THREADS_SPSC_PREFETCH >= 1
      #if DEVA_THREADS_ALLOC_EPOCH
        #error "Can't prefetch with talloc=epoch"
      #endif
      
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
  void channels_r<rn>::receive(Rcv &&rcv, threads::progress_state &ps) {
    hot_slot<uint_signal_t> hot[rn];
    int hot_n;
    int hot_stride_mask; // 0 or -1
    
    #if DEVA_THREADS_ALLOC_EPOCH
    int e3_q; // quiesced epoch
    if(ps.epoch_bumped) {
      hot_n = rn;
      hot_stride_mask = 0;
      hot[0].ix_xor_i = 0;
      e3_q = threads::template epoch3_inc<-1>(ps.epoch3_old);
    }
    else
    #endif
    {
      hot_n = this->recv_slots_.template reap</*acquire=*/!DEVA_THREADS_ALLOC_EPOCH, /*peek=*/DEVA_THREADS_ALLOC_EPOCH>(hot);
      hot_stride_mask = -1;
    }
    
    bool did_something = hot_n != 0;
    
    prefetch(hot_n, hot);
    
    for(int i=0; i < hot_n; i++) {
      channels_r::each *ch = &this->r_[hot[i & hot_stride_mask].ix_xor_i ^ i];
      
      #if DEVA_THREADS_ALLOC_OPNEW_ASYM || DEVA_THREADS_ALLOC_OPNEW_SYM
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
        
      #elif DEVA_THREADS_ALLOC_EPOCH

        message **mp[3], **mp_live[3];
        did_something = false;
        
        for(int e3=0; e3 < 3; e3++) {
          mp[e3] = ch->tailp_shadow[e3];
          mp_live[e3] = ch->tailp_live[e3].load(std::memory_order_relaxed);
          ch->tailp_shadow[e3] = mp_live[e3];
          did_something |= mp[e3] != mp_live[e3];
        }

        if(ps.epoch_bumped) {
          ch->tailp_live[e3_q].store(&ch->head[e3_q], std::memory_order_relaxed);
          ch->tailp_shadow[e3_q] = &ch->head[e3_q];
        }

        if(did_something) {
          std::atomic_thread_fence(std::memory_order_acquire);
          
          int n = 0;
          for(int e3=0; e3 < 3; e3++) {
            while(mp[e3] != mp_live[e3]) {
              n += 1;
              message *m = *mp[e3];
              rcv(m);
              mp[e3] = &m->next;
            }
          }
                    
          this->recv_slots_.shadow.non_atom[hot[i & hot_stride_mask].ix_xor_i ^ i] += n;

          std::atomic_signal_fence(std::memory_order_acq_rel);
        }
      #endif
    }
    
    ps.did_something |= did_something;
  }

  template<int rn>
  template<typename Rcv, typename Batch>
  void channels_r<rn>::receive_batch(Rcv &&rcv, Batch &&batch, threads::progress_state &ps) {
  #if DEVA_THREADS_ALLOC_EPOCH
    this->receive(static_cast<Rcv&&>(rcv), ps);
    static_cast<Batch&&>(batch)();
  #else
    hot_slot<uint_signal_t> hot[rn];
    int hot_n = this->recv_slots_.template reap</*acquire=*/!DEVA_THREADS_ALLOC_EPOCH>(hot);
    constexpr int hot_stride_mask = -1;
    
    bool did_something = hot_n != 0;
    
    prefetch(hot_n, hot);

    for(int i=0; i < hot_n; i++) {
      channels_r::each *ch = &this->r_[hot[i & hot_stride_mask].ix_xor_i ^ i];
      std::uint32_t msg_n = hot[i].delta;
      message *m = ch->recv_last;
      
      do {
        message *m1 = m->next;
        rcv(m1);
        m = m1;
      } while(--msg_n != 0);
      
      #if !DEVA_THREADS_ALLOC_OPNEW_ASYM
        ch->recv_last = m;
      #endif
    }
    
    batch();
    
    #if DEVA_THREADS_ALLOC_OPNEW_SYM
      for(int i=0; i < hot_n; i++) {
        channels_r::each *ch = &this->r_[hot[i & hot_stride_mask].ix_xor_i ^ i];
        ch->ack_slot->store(hot[i].old + hot[i].delta, std::memory_order_release);
      }
    #elif DEVA_THREADS_ALLOC_OPNEW_ASYM
      for(int i=0; i < hot_n; i++) {
        channels_r::each *ch = &this->r_[hot[i & hot_stride_mask].ix_xor_i ^ i];
        std::uint32_t msg_n = hot[i].delta;
        message *m = ch->recv_last;
        
        do {
          message *m1 = m->next;
          threads::dealloc_message(m);
          m = m1;
        } while(--msg_n != 0);
        
        ch->recv_last = m;
      }
    #endif
    
    ps.did_something |= did_something;
  #endif
  }
}}
#endif
