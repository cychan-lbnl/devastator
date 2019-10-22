#include <devastator/threads/message.hxx>

#ifndef _f49ebd2f7c9341ad9a3d9f865d565837
#define _f49ebd2f7c9341ad9a3d9f865d565837

#include <devastator/opnew.hxx>

#include <atomic>
#include <cstdint>

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
    
    while(*mp != nullptr) {
      message *m = *mp;
      if(m->r_next.load(std::memory_order_relaxed) == reinterpret_cast<message*>(0x1)) { 
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
    
    sent_tailp_ = &sent_head_;
    return did_something;
  }

  template<int wn, int rn, channels_r<rn>(*chan_r)[wn]>
  void channels_w<wn,rn,chan_r>::send(int w, message *m) {
    m->r_next.store(nullptr, std::memory_order_relaxed);

    if(rn > 1) {
      auto *q = &(*chan_r)[w].tails_[(threads::thread_me() + w) % channels_r<rn>::rail_n];
      std::atomic<message*> *got = q->tailp_.exchange(&m->r_next, std::memory_order_relaxed);
      got->store(m, std::memory_order_release);
    }
    else { // we are only possible writer, do exchange non-atomically
      auto *q = &(*chan_r)[w].tails_[0];
      std::atomic<message*> *got = q->tailp_.load(std::memory_order_relaxed);
      q->tailp_.store(&m->r_next, std::memory_order_relaxed);
      got->store(m, std::memory_order_release);
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

    for(int rail=0; rail < rail_n; rail++)
      tailp[rail] = tails_[rail].tailp_.load(std::memory_order_relaxed);
    
    #if DEVA_THREADS_MESSAGE_RAIL_N > 1
    if(rail_n > 1) {
      std::atomic<message*> *mp[rail_n];
      message *m[rail_n];

      for(int rail=0; rail < rail_n; rail++) {
        mp[rail] = headp_[rail];
        m[rail] = mp[rail]->load(std::memory_order_relaxed);
      }

      while(true) {
        for(int rail=0; rail < rail_n; rail++)
          if(!(mp[rail] != tailp[rail] && m[rail] != nullptr))
            goto finish_parallel_rails;

        std::atomic_thread_fence(std::memory_order_acquire);
        did_something = true;

        for(int rail=0; rail < rail_n; rail++)
          rcv(m[rail]);

        std::atomic_signal_fence(std::memory_order_acq_rel);
        
        for(int rail=0; rail < rail_n; rail++) {
          mp[rail]->store(reinterpret_cast<message*>(0x1), std::memory_order_release);
          mp[rail] = &m[rail]->r_next;
          m[rail] = mp[rail]->load(std::memory_order_relaxed);
        }

        std::atomic_signal_fence(std::memory_order_acq_rel);
      }

    finish_parallel_rails:
      for(int rail=0; rail < rail_n; rail++)
        headp_[rail] = mp[rail];
    }
    #endif

    for(int rail=0; rail < rail_n; rail++) {
      std::atomic<message*> *mp = headp_[rail];
      message *m = mp->load(std::memory_order_relaxed);
      
      while(mp != tailp[rail] && m != nullptr) {
        std::atomic_thread_fence(std::memory_order_acquire);
        did_something = true;
        rcv(m);
        mp->store(reinterpret_cast<message*>(0x1), std::memory_order_release);
        mp = &m->r_next;
        m = mp->load(std::memory_order_relaxed);
      }

      headp_[rail] = mp;
    }
    
    return did_something;
  }
  
  template<int rn>
  template<typename Rcv, typename Batch>
  bool channels_r<rn>::receive_batch(Rcv &&rcv, Batch &&batch) {
    bool did_something = false;
    std::atomic<message*> *tailp[rail_n];
    std::atomic<message*> *mp[rail_n];
    message *m[rail_n];
    std::intptr_t n_par = 0;
    std::intptr_t n_seq[rail_n];
    
    for(int rail=0; rail < rail_n; rail++) {
      tailp[rail] = tails_[rail].tailp_.load(std::memory_order_relaxed);
      mp[rail] = headp_[rail];
      m[rail] = mp[rail]->load(std::memory_order_relaxed);
    }

    #if DEVA_THREADS_MESSAGE_RAIL_N > 1
    if(rail_n > 1) {
      while(true) {
        for(int rail=0; rail < rail_n; rail++) {
          if(!(mp[rail] != tailp[rail] && m[rail] != nullptr))
            goto finish_parallel_rcv;
        }

        std::atomic_thread_fence(std::memory_order_acquire);

        n_par += 1;
        for(int rail=0; rail < rail_n; rail++)
          rcv(m[rail]);

        std::atomic_signal_fence(std::memory_order_acq_rel);

        for(int rail=0; rail < rail_n; rail++) {
          mp[rail] = &m[rail]->r_next;
          m[rail] = mp[rail]->load(std::memory_order_relaxed);
        }

        std::atomic_signal_fence(std::memory_order_acq_rel);
      }

    finish_parallel_rcv:
      did_something |= n_par != 0;
    }
    #endif

    for(int rail=0; rail < rail_n; rail++) {
      std::intptr_t n = 0;
      
      while(mp[rail] != tailp[rail] && m[rail] != nullptr) {
        n += 1;
        std::atomic_thread_fence(std::memory_order_acquire);
        rcv(m[rail]);
        mp[rail] = &m[rail]->r_next;
        m[rail] = mp[rail]->load(std::memory_order_relaxed);
      }

      n_seq[rail] = n;
      did_something |= n != 0;
    }

    batch();
    
    if(did_something) {
      std::atomic_thread_fence(std::memory_order_release);

      for(int rail=0; rail < rail_n; rail++)
        mp[rail] = headp_[rail];
      
      #if DEVA_THREADS_MESSAGE_RAIL_N > 1
      if(rail_n > 1) {
        while(n_par--) {
          for(int rail=0; rail < rail_n; rail++) {
            m[rail] = mp[rail]->load(std::memory_order_relaxed);
            mp[rail]->store(reinterpret_cast<message*>(0x1), std::memory_order_relaxed);
            mp[rail] = &m[rail]->r_next;
          }
        }

        std::atomic_signal_fence(std::memory_order_acq_rel);
      }
      #endif
      
      for(int rail=0; rail < rail_n; rail++) {
        std::intptr_t n = n_seq[rail];
        while(n--) {
          m[rail] = mp[rail]->load(std::memory_order_relaxed);
          mp[rail]->store(reinterpret_cast<message*>(0x1), std::memory_order_relaxed);
          mp[rail] = &m[rail]->r_next;
        }
        headp_[rail] = mp[rail];
      }
    }
    
    return did_something;
  }
}}
#endif
