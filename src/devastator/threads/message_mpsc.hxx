#include <devastator/threads/message.hxx>

#ifndef _f49ebd2f7c9341ad9a3d9f865d565837
#define _f49ebd2f7c9341ad9a3d9f865d565837

#include <devastator/opnew.hxx>

#include <atomic>
#include <cstdint>

// enable to test "other" platform's algorithm (all algorithms are correct on all platforms)
#define DEVA_ARCH_TSO_FLIP 0

#ifdef __x86_64__
  #define DEVA_ARCH_TSO (1^DEVA_ARCH_TSO_FLIP)
#else
  #define DEVA_ARCH_TSO (0^DEVA_ARCH_TSO_FLIP)
#endif

namespace deva {
namespace threads {
  struct message {
    union {
      message *next = nullptr; // the name by which world_gasnet uses this field
      std::atomic<message*> r_next; // next in reader's list (the actual queue)
    };
    
  #if DEVA_THREADS_ALLOC_OPNEW_SYM
    message *w_next;
  #endif
  };

  template<int rn>
  struct channels_r_base;
  
  template<int rn>
  class channels_r_base {
  protected:
    template<int wn, int rn1, channels_r<rn1>(*)[wn]>
    friend struct channels_w;
    
    static constexpr int rail_n = rn == 1 ? 1 : DEVA_THREADS_MPSC_RAIL_N;
    
    struct alignas(64) rail_tail_t {
      std::atomic<message*> head[2]{{nullptr},{reinterpret_cast<message*>(0x1)}};
      std::atomic<std::atomic<message*>*> tailp{&head[0]};
    } tails_[rail_n];

    alignas(64)
    std::atomic<message*> *headp_[rail_n];
    #if DEVA_THREADS_ALLOC_OPNEW_ASYM
      message *headp_owner_[rail_n];
    #endif
    
    channels_r_base();
    
  public:
    void enqueue(message *m) {
      m->r_next.store(nullptr, std::memory_order_relaxed);

      if(rn > 1) {
        auto *q = &this->tails_[threads::thread_me() % rail_n];
        auto *old_tailp = q->tailp.exchange(&m->r_next, std::memory_order_acq_rel);
        old_tailp->store(m, std::memory_order_relaxed);
      }
      else { // we are only possible writer, do exchange non-atomically
        auto *q = &this->tails_[0];
        auto *old_tailp = q->tailp.load(std::memory_order_relaxed);
        q->tailp.store(&m->r_next, std::memory_order_release);
        old_tailp->store(m, std::memory_order_relaxed);
      }
    }
  };

  #if DEVA_THREADS_ALLOC_EPOCH
  template<>
  class channels_r_base</*rn=*/1> {
  protected:
    template<int wn, int rn1, channels_r<rn1>(*)[wn]>
    friend struct channels_w;
    
    static constexpr int rail_n = 3;
    
    struct rail_tail_t {
      std::atomic<message*> head[1]{{nullptr}};
      std::atomic<std::atomic<message*>*> tailp{&head[0]};
    };

    alignas(64)
    rail_tail_t tails_[/*rail_n=*/3];
    
    alignas(64)
    std::atomic<message*> *headp_[/*rail_n=*/3]{&tails_[0].head[0], &tails_[1].head[0], &tails_[2].head[0]};

  public:
    void enqueue(message *m) {
      m->r_next.store(nullptr, std::memory_order_relaxed);
      int e3 = threads::epoch_mod3();
      
      // we are only possible writer, do exchange non-atomically
      auto *q = &this->tails_[e3].tailp;
      auto *tailp_old = q->load(std::memory_order_relaxed);
      q->store(&m->r_next, std::memory_order_release);
      tailp_old->store(m, std::memory_order_relaxed);
    }
  };
  #endif
  
  template<int rn>
  class channels_r: public channels_r_base<rn> {
    template<int wn, int rn1, channels_r<rn1>(*)[wn]>
    friend struct channels_w;
    
  public:
    template<typename Rcv>
    bool receive(Rcv &&rcv, bool epoch_bumped, std::uint64_t epoch_old);
    template<typename Rcv, typename Batch>
    bool receive_batch(Rcv &&rcv, Batch &&batch, bool epoch_bumped, std::uint64_t epoch_old);
  };
  
  template<int wn, int rn, channels_r<rn>(*chan_r)[wn]>
  class channels_w {
    #if DEVA_THREADS_ALLOC_OPNEW_SYM
      message *sent_head_ = nullptr;
      message **sent_tailp_ = &sent_head_;
    #endif
  public:
    void connect();
    void destroy();
    void send(int w, message *m);
    void reclaim(threads::progress_state &ps);
  };

  //////////////////////////////////////////////////////////////////////////////

  template<int wn, int rn, channels_r<rn>(*chan_r)[wn]>
  void channels_w<wn,rn,chan_r>::connect() {
    // nop
  }

  template<int wn, int rn, channels_r<rn>(*chan_r)[wn]>
  void channels_w<wn,rn,chan_r>::destroy() {
  #if DEVA_THREADS_ALLOC_OPNEW_SYM
    while(sent_head_ != nullptr) {
      message *m = sent_head_;
      sent_head_ = m->w_next;
      ::operator delete(m);
    }
  #endif
  }
  
  template<int wn, int rn, channels_r<rn>(*chan_r)[wn]>
  void channels_w<wn,rn,chan_r>::reclaim(threads::progress_state &ps) {
  #if !DEVA_THREADS_ALLOC_OPNEW_SYM
    // nop
  #else
    message **mp = &this->sent_head_;
    int keeps = 0;
    bool did_something = false;

    #if DEVA_ARCH_TSO
      while(*mp != nullptr) {
        message *m = *mp;        
        if(m->r_next.load(std::memory_order_acquire) == reinterpret_cast<message*>(0x1)) { 
          *mp = m->w_next;
          did_something = true;
          dealloc_message((void*)m);
        }
        else if(++keeps < 4)
          mp = &m->w_next;
        else
          break;
      }
    #else
      message *del_list = nullptr;
      
      while(*mp != nullptr) {
        message *m = *mp;
        if(m->r_next.load(std::memory_order_relaxed) == reinterpret_cast<message*>(0x1)) { 
          *mp = m->w_next;
          did_something = true;
          m->w_next = del_list;
          del_list = m;
        }
        else if(++keeps < 4)
          mp = &m->w_next;
        else
          break;
      }

      if(del_list != nullptr)
        std::atomic_thread_fence(std::memory_order_acquire);
      
      while(del_list != nullptr) {
        message *next = del_list->w_next;
        dealloc_message(del_list);
        del_list = next;
      }
    #endif
    
    this->sent_tailp_ = mp;
    ps.did_something |= did_something;
  #endif
  }

  template<int wn, int rn, channels_r<rn>(*chan_r)[wn]>
  void channels_w<wn,rn,chan_r>::send(int w, message *m) {
    (*chan_r)[w].enqueue(m);
    
    #if DEVA_THREADS_ALLOC_OPNEW_SYM
      m->w_next = nullptr;
      *this->sent_tailp_ = m;
      this->sent_tailp_ = &m->w_next;
    #endif
  }

  //////////////////////////////////////////////////////////////////////////////

  template<int rn>
  channels_r_base<rn>::channels_r_base() {
    for(int rail=0; rail < rail_n; rail++) {
      headp_[rail] = &tails_[rail].head[0];
      #if DEVA_THREADS_ALLOC_OPNEW_ASYM
        headp_owner_[rail] = nullptr;
      #endif
    }
  }

  template<int rn>
  template<typename Rcv>
  bool channels_r<rn>::receive(Rcv &&rcv, bool epoch_bumped, std::uint64_t epoch_old) {
    static constexpr int rail_n = channels_r<rn>::rail_n;

    std::uint64_t epoch_new = epoch_old + (epoch_bumped ? 1 : 0);
    bool did_something = false;
    std::atomic<message*> *headp[rail_n];
    std::atomic<message*> *tailp[rail_n];
    
    if(DEVA_THREADS_ALLOC_EPOCH && rn > 1 && epoch_bumped) {
      for(int rail=0; rail < rail_n; rail++) {
        headp[rail] = this->headp_[rail];
        this->headp_[rail] = &this->tails_[rail].head[epoch_new%2];
        this->headp_[rail]->store(nullptr, std::memory_order_relaxed);
        tailp[rail] = this->tails_[rail].tailp.exchange(this->headp_[rail], std::memory_order_release);
        did_something |= tailp[rail] != headp[rail];
      }
    }
    else {
      for(int rail=0; rail < rail_n; rail++) {
        headp[rail] = this->headp_[rail];
        tailp[rail] = this->tails_[rail].tailp.load(std::memory_order_relaxed);
        this->headp_[rail] = tailp[rail];
        did_something |= tailp[rail] != headp[rail];
      }
    }

    if(did_something) {
      std::atomic_thread_fence(std::memory_order_acquire);
      
      #if DEVA_THREADS_MPSC_RAIL_N > 1
      if(rail_n > 1) {
        std::atomic<message*> *mp[rail_n];

        for(int rail=0; rail < rail_n; rail++)
          mp[rail] = headp[rail];

        while(true) {
          bool any_depleted = false;
          for(int rail=0; rail < rail_n; rail++)
            any_depleted |= mp[rail] == tailp[rail];
          if(any_depleted) break;
          
          message *m[rail_n];
          while(true) {
            bool any_null = false;

            std::atomic_signal_fence(std::memory_order_acq_rel);
            for(int rail=0; rail < rail_n; rail++) {
              m[rail] = mp[rail]->load(std::memory_order_relaxed);
              any_null |= m[rail] == nullptr;
            }
            std::atomic_signal_fence(std::memory_order_acq_rel);
            
            if(!any_null) break;
          }
          
          for(int rail=0; rail < rail_n; rail++)
            rcv(m[rail]);
          
          for(int rail=0; rail < rail_n; rail++) {
            #if DEVA_THREADS_ALLOC_OPNEW_SYM
              mp[rail]->store(reinterpret_cast<message*>(0x1), std::memory_order_release);
            #elif DEVA_THREADS_ALLOC_OPNEW_ASYM
              threads::dealloc_message(headp_owner_[rail]);
              headp_owner_[rail] = m[rail];
            #endif
            mp[rail] = &m[rail]->r_next;
          }
        }
        
        for(int rail=0; rail < rail_n; rail++)
          headp[rail] = mp[rail];
      }
      #endif

      for(int rail=0; rail < rail_n; rail++) {
        std::atomic<message*> *mp = headp[rail];

        while(mp != tailp[rail]) {
          message *m;
          do m = mp->load(std::memory_order_relaxed);
          while(m == nullptr);
          DEVA_ASSERT(m != reinterpret_cast<message*>(0x1));
          #if DEVA_ARCH_TSO
            #if DEVA_THREADS_ALLOC_OPNEW_SYM
              mp->store(reinterpret_cast<message*>(0x1), std::memory_order_release);
            #elif DEVA_THREADS_ALLOC_OPNEW_ASYM
              threads::dealloc_message(this->headp_owner_[rail]);
              this->headp_owner_[rail] = m;
            #endif
          #endif
          rcv(m);
          mp = &m->r_next;
        }
        
        #if !DEVA_ARCH_TSO && (DEVA_THREADS_ALLOC_OPNEW_SYM || DEVA_THREADS_ALLOC_OPNEW_ASYM)
          std::atomic_thread_fence(std::memory_order_release); // keep following stores from preceding
          mp = headp[rail];
          
          while(mp != tailp[rail]) {
            message *m = mp->load(std::memory_order_relaxed);
            #if DEVA_THREADS_ALLOC_OPNEW_SYM
              mp->store(reinterpret_cast<message*>(0x1), std::memory_order_relaxed);
            #else
              threads::dealloc_message(this->headp_owner_[rail]);
              this->headp_owner_[rail] = m;
            #endif
            mp = &m->r_next;
          }
        #endif
      }
    }

    if(DEVA_THREADS_ALLOC_EPOCH && rn == 1 && epoch_bumped) {
      DEVA_ASSERT_ALWAYS(rail_n == 3);
      int e3_dead = threads::epoch_mod3() + 1;
      if(e3_dead == 3) e3_dead = 0;
      this->tails_[e3_dead].head[0].store(nullptr, std::memory_order_relaxed);
      this->tails_[e3_dead].tailp.store(&this->tails_[e3_dead].head[0], std::memory_order_relaxed);
      this->headp_[e3_dead] = &this->tails_[e3_dead].head[0];
    }
    
    return did_something;
  }

  template<int rn>
  template<typename Rcv, typename Batch>
  bool channels_r<rn>::receive_batch(Rcv &&rcv, Batch &&batch, bool epoch_bumped, std::uint64_t epoch_old) {
    static constexpr int rail_n = channels_r<rn>::rail_n;
    
    std::uint64_t epoch_new = epoch_old + (epoch_bumped ? 1 : 0);
    bool did_something = false;
    std::atomic<message*> *headp[rail_n];
    std::atomic<message*> *tailp[rail_n];
    std::intptr_t n_par = 0;
    std::intptr_t n_seq[rail_n];

    if(DEVA_THREADS_ALLOC_EPOCH && rn > 1 && epoch_bumped) {
      for(int rail=0; rail < rail_n; rail++) {
        headp[rail] = this->headp_[rail];
        this->headp_[rail] = &this->tails_[rail].head[epoch_new%2];
        this->headp_[rail]->store(nullptr, std::memory_order_relaxed);
        tailp[rail] = this->tails_[rail].tailp.exchange(this->headp_[rail], std::memory_order_release);
        did_something |= tailp[rail] != headp[rail];
      }
    }
    else {
      for(int rail=0; rail < rail_n; rail++) {
        headp[rail] = this->headp_[rail];
        tailp[rail] = this->tails_[rail].tailp.load(std::memory_order_relaxed);
        this->headp_[rail] = tailp[rail];
        did_something |= tailp[rail] != headp[rail];
      }
    }

    if(did_something) {
      std::atomic_thread_fence(std::memory_order_acquire);

      std::atomic<message*> *mp[rail_n];
      for(int rail=0; rail < rail_n; rail++)
        mp[rail] = headp[rail];

      #if DEVA_THREADS_MPSC_RAIL_N > 1
      if(rail_n > 1) {
        while(true) {
          bool any_depleted = false;
          for(int rail=0; rail < rail_n; rail++)
            any_depleted |= mp[rail] == tailp[rail];
          if(any_depleted) break;
          
          n_par += 1;

          message *m[rail_n];
          while(true) {
            bool any_null = false;

            std::atomic_signal_fence(std::memory_order_acq_rel);
            for(int rail=0; rail < rail_n; rail++) {
              m[rail] = mp[rail]->load(std::memory_order_relaxed);
              any_null |= m[rail] == nullptr;
            }
            std::atomic_signal_fence(std::memory_order_acq_rel);
            
            if(!any_null) break;
          }
          
          for(int rail=0; rail < rail_n; rail++)
            rcv(m[rail]);
          
          for(int rail=0; rail < rail_n; rail++)
            mp[rail] = &m[rail]->r_next;
        }
      }
      #endif

      for(int rail=0; rail < rail_n; rail++) {
        std::intptr_t n = 0;
        
        while(mp[rail] != tailp[rail]) {
          n += 1;
          message *m;
          do m = mp[rail]->load(std::memory_order_relaxed);
          while(m == nullptr);
          rcv(m);
          mp[rail] = &m->r_next;
        }
        
        n_seq[rail] = n;
      }
    }
    
    batch();
    
    #if DEVA_THREADS_ALLOC_OPNEW_SYM || DEVA_THREADS_ALLOC_OPNEW_ASYM
    if(did_something) {
      std::atomic_thread_fence(std::memory_order_release);

      std::atomic<message*> *mp[rail_n];
      for(int rail=0; rail < rail_n; rail++)
        mp[rail] = headp[rail];
      
      #if DEVA_THREADS_MPSC_RAIL_N > 1
      if(rail_n > 1) {
        while(n_par--) {
          for(int rail=0; rail < rail_n; rail++) {
            message *m = mp[rail]->load(std::memory_order_relaxed);
            #if DEVA_THREADS_ALLOC_OPNEW_SYM
              mp[rail]->store(reinterpret_cast<message*>(0x1), std::memory_order_relaxed);
            #else
              threads::dealloc_message(this->headp_owner_[rail]);
              this->headp_owner_[rail] = m;
            #endif
            mp[rail] = &m->r_next;
          }
        }

        std::atomic_signal_fence(std::memory_order_acq_rel);
      }
      #endif
      
      for(int rail=0; rail < rail_n; rail++) {
        std::intptr_t n = n_seq[rail];
        while(n--) {
          message *m = mp[rail]->load(std::memory_order_relaxed);
          #if DEVA_THREADS_ALLOC_OPNEW_SYM
            mp[rail]->store(reinterpret_cast<message*>(0x1), std::memory_order_relaxed);
          #elif DEVA_THREADS_ALLOC_OPNEW_ASYM
            threads::dealloc_message(this->headp_owner_[rail]);
            this->headp_owner_[rail] = m;
          #endif
          mp[rail] = &m->r_next;
        }
      }
    }
    #endif

    if(DEVA_THREADS_ALLOC_EPOCH && rn == 1 && epoch_bumped) {
      DEVA_ASSERT_ALWAYS(rail_n == 3);
      int e3_dead = threads::epoch_mod3() + 1;
      if(e3_dead == 3) e3_dead = 0;
      this->tails_[e3_dead].head[0].store(nullptr, std::memory_order_relaxed);
      this->tails_[e3_dead].tailp.store(&this->tails_[e3_dead].head[0], std::memory_order_relaxed);
      this->headp_[e3_dead] = &this->tails_[e3_dead].head[0];
    }
    
    return did_something;
  }
}}
#endif
