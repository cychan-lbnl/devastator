#include <devastator/threads/message.hxx>

#ifndef _f49ebd2f7c9341ad9a3d9f865d565837
#define _f49ebd2f7c9341ad9a3d9f865d565837

#include <devastator/opnew.hxx>

#include <atomic>
#include <cstdint>

// enable to test "other" platform's algorithm (all algorithms are correct on all platforms)
#define DEVA_ARCH_TSO_FLIP 1

#ifdef __x86_64__
  #define DEVA_ARCH_TSO (1^DEVA_ARCH_TSO_FLIP)
#else
  #define DEVA_ARCH_TSO (0^DEVA_ARCH_TSO_FLIP)
#endif

namespace deva {
namespace threads {
  struct message {
    std::atomic<message*> r_next; // next in reader's list (the actual queue)
    union {
      message *w_next; // the name by which we use the field (next in writer's list)
      message *next; // the name by which world_gasnet uses this field
    };
  };

  template<int rn>
  class channels_r {
    template<int wn, int rn1, channels_r<rn1>(*)[wn]>
    friend struct channels_w;
    
    static constexpr int rail_n = rn == 1 ? 1 : DEVA_THREADS_MESSAGE_RAIL_N;
    
    struct alignas(64) rail_tail_t {
      std::atomic<message*> head0_{nullptr};
      std::atomic<std::atomic<message*>*> tailp_{&head0_};
    } tails_[rail_n];

    alignas(64)
    std::atomic<message*> *headp_[rail_n];
    
  public:
    channels_r();
    
    template<typename Rcv>
    bool receive(Rcv &&rcv);
    template<typename Rcv, typename Batch>
    bool receive_batch(Rcv &&rcv, Batch &&batch);
  };
  
  template<int wn, int rn, channels_r<rn>(*chan_r)[wn]>
  class channels_w {
    message *sent_head_ = nullptr;
    message **sent_tailp_ = &sent_head_;
    
  public:
    void connect();
    void destroy();
    void send(int w, message *m);
    bool steward();
  };

  //////////////////////////////////////////////////////////////////////////////

  template<int wn, int rn, channels_r<rn>(*chan_r)[wn]>
  void channels_w<wn,rn,chan_r>::connect() {
    // nop
  }

  template<int wn, int rn, channels_r<rn>(*chan_r)[wn]>
  void channels_w<wn,rn,chan_r>::destroy() {
    while(sent_head_ != nullptr) {
      message *m = sent_head_;
      sent_head_ = m->w_next;
      ::operator delete(m);
    }
  }
  
  template<int wn, int rn, channels_r<rn>(*chan_r)[wn]>
  bool channels_w<wn,rn,chan_r>::steward() {
    message **mp = &sent_head_;
    int keeps = 0;
    bool did_something = false;

    #if DEVA_ARCH_TSO
      while(*mp != nullptr) {
        message *m = *mp;
        if(m->r_next.load(std::memory_order_acquire) == reinterpret_cast<message*>(0x1)) { 
          *mp = m->w_next;
          did_something = true;
          
          #if DEVA_OPNEW
            opnew::template operator_delete</*known_size=*/0, /*known_local=*/true>(m);
          #else
            ::operator delete(m);
          #endif
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
        #if DEVA_OPNEW
          opnew::template operator_delete</*known_size=*/0, /*known_local=*/true>(del_list);
        #else
          ::operator delete(del_list);
        #endif
        del_list = next;
      }
    #endif
    
    sent_tailp_ = &sent_head_;
    return did_something;
  }

  template<int wn, int rn, channels_r<rn>(*chan_r)[wn]>
  void channels_w<wn,rn,chan_r>::send(int w, message *m) {
    m->r_next.store(nullptr, std::memory_order_relaxed);

    if(rn > 1) {
      auto *q = &(*chan_r)[w].tails_[threads::thread_me() % channels_r<rn>::rail_n];
      std::atomic<message*> *old_tailp = q->tailp_.exchange(&m->r_next, std::memory_order_release);
      old_tailp->store(m, std::memory_order_relaxed);
    }
    else { // we are only possible writer, do exchange non-atomically
      auto *q = &(*chan_r)[w].tails_[0];
      std::atomic<message*> *old_tailp = q->tailp_.load(std::memory_order_relaxed);
      q->tailp_.store(&m->r_next, std::memory_order_release);
      old_tailp->store(m, std::memory_order_relaxed);
    }
    
    m->w_next = nullptr;
    *this->sent_tailp_ = m;
    this->sent_tailp_ = &m->w_next;
  }

  //////////////////////////////////////////////////////////////////////////////

  template<int rn>
  channels_r<rn>::channels_r() {
    for(int rail=0; rail < rail_n; rail++)
      headp_[rail] = &tails_[rail].head0_;
  }
  
  template<int rn>
  template<typename Rcv>
  bool channels_r<rn>::receive(Rcv &&rcv) {
    bool did_something = false;
    std::atomic<message*> *tailp[rail_n];

    for(int rail=0; rail < rail_n; rail++) {
      tailp[rail] = tails_[rail].tailp_.load(std::memory_order_relaxed);
      did_something |= tailp[rail] != headp_[rail];
    }

    if(!did_something)
      return false;
    
    std::atomic_thread_fence(std::memory_order_acquire);
    
    #if DEVA_THREADS_MESSAGE_RAIL_N > 1
    if(rail_n > 1) {
      std::atomic<message*> *mp[rail_n];

      for(int rail=0; rail < rail_n; rail++)
        mp[rail] = headp_[rail];

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
          mp[rail]->store(reinterpret_cast<message*>(0x1), std::memory_order_release);
          mp[rail] = &m[rail]->r_next;
        }
      }
      
      for(int rail=0; rail < rail_n; rail++)
        headp_[rail] = mp[rail];
    }
    #endif

    for(int rail=0; rail < rail_n; rail++) {
      std::atomic<message*> *mp = headp_[rail];

      #if DEVA_ARCH_TSO
        while(mp != tailp[rail]) {
          message *m;
          do m = mp->load(std::memory_order_relaxed);
          while(m == nullptr);
          rcv(m);
          mp->store(reinterpret_cast<message*>(0x1), std::memory_order_release);
          mp = &m->r_next;
        }
      #else
        while(mp != tailp[rail]) {
          message *m;
          do m = mp->load(std::memory_order_relaxed);
          while(m == nullptr);
          rcv(m);
          mp = &m->r_next;
        }

        std::atomic_thread_fence(std::memory_order_release);
        mp = headp_[rail];
        
        while(mp != tailp[rail]) {
          message *m = mp->load(std::memory_order_relaxed);
          mp->store(reinterpret_cast<message*>(0x1), std::memory_order_relaxed);
          mp = &m->r_next;
        }
      #endif
      
      headp_[rail] = mp;
    }
    
    return true; // did_something
  }
  
  template<int rn>
  template<typename Rcv, typename Batch>
  bool channels_r<rn>::receive_batch(Rcv &&rcv, Batch &&batch) {
    bool did_something = false;
    std::atomic<message*> *tailp[rail_n];
    std::intptr_t n_par = 0;
    std::intptr_t n_seq[rail_n];
    
    for(int rail=0; rail < rail_n; rail++) {
      tailp[rail] = tails_[rail].tailp_.load(std::memory_order_relaxed);
      DEVA_ASSERT(tailp[rail] != nullptr);
      did_something |= tailp[rail] != headp_[rail];
    }

    if(did_something) {
      std::atomic_thread_fence(std::memory_order_acquire);

      std::atomic<message*> *mp[rail_n];
      for(int rail=0; rail < rail_n; rail++)
        mp[rail] = headp_[rail];

      #if DEVA_THREADS_MESSAGE_RAIL_N > 1
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
    
    if(did_something) {
      std::atomic_thread_fence(std::memory_order_release);

      std::atomic<message*> *mp[rail_n];
      for(int rail=0; rail < rail_n; rail++)
        mp[rail] = headp_[rail];
      
      #if DEVA_THREADS_MESSAGE_RAIL_N > 1
      if(rail_n > 1) {
        while(n_par--) {
          for(int rail=0; rail < rail_n; rail++) {
            message *m = mp[rail]->load(std::memory_order_relaxed);
            mp[rail]->store(reinterpret_cast<message*>(0x1), std::memory_order_relaxed);
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
          mp[rail]->store(reinterpret_cast<message*>(0x1), std::memory_order_relaxed);
          mp[rail] = &m->r_next;
        }
        headp_[rail] = mp[rail];
      }
    }
    
    return did_something;
  }
}}
#endif
