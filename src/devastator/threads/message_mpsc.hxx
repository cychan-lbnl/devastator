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

  template<int>
  class channels_r {
    template<int wn, typename Chans_r, Chans_r(*)[wn]>
    friend struct channels_w;
    
    alignas(64)
    std::atomic<message*> head0_{nullptr};
    std::atomic<std::atomic<message*>*> tailp_{&head0_};
    alignas(64)
    std::atomic<message*> *headp_ = &head0_;
    
  public:
    template<typename Rcv>
    bool receive(Rcv &&rcv);
    template<typename Rcv, typename Batch>
    bool receive_batch(Rcv &&rcv, Batch &&batch);
  };
  
  template<int wn, typename Chans_r, Chans_r(*chan_r)[wn]>
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

  template<int wn, typename Chans_r, Chans_r(*chan_r)[wn]>
  void channels_w<wn,Chans_r,chan_r>::connect() {
    // nop
  }

  template<int wn, typename Chans_r, Chans_r(*chan_r)[wn]>
  void channels_w<wn,Chans_r,chan_r>::destroy() {
    steward();
  }
  
  template<int wn, typename Chans_r, Chans_r(*chan_r)[wn]>
  bool channels_w<wn,Chans_r,chan_r>::steward() {
    bool did_something = false;
    
    while(sent_head_ != nullptr) {
      message *m = sent_head_;
      if(m->r_next.load(std::memory_order_relaxed) != reinterpret_cast<message*>(0x1))
        break;
      sent_head_ = m->w_next;
      #if 1 && DEVA_OPNEW
        opnew::template operator_delete</*known_size=*/0, /*known_local=*/true>(m);
      #else
        ::operator delete(m);
      #endif
      did_something = true;
    }
    
    sent_tailp_ = &sent_head_;
    return did_something;
  }

  template<int wn, typename Chans_r, Chans_r(*chan_r)[wn]>
  void channels_w<wn,Chans_r,chan_r>::send(int w, message *m) {
    Chans_r *q = &(*chan_r)[w];
    
    m->r_next.store(nullptr, std::memory_order_relaxed);
    std::atomic<message*> *got = q->tailp_.exchange(&m->r_next, std::memory_order_relaxed);
    got->store(m, std::memory_order_release);

    m->w_next = nullptr;
    *this->sent_tailp_ = m;
    this->sent_tailp_ = &m->w_next;
  }

  //////////////////////////////////////////////////////////////////////////////

  template<int rn>
  template<typename Rcv>
  bool channels_r<rn>::receive(Rcv &&rcv) {
    bool something = false;
    std::atomic<message*> *tailp = tailp_.load(std::memory_order_relaxed);
    std::atomic<message*> *mp = headp_;
    message *m = mp->load(std::memory_order_relaxed);
    
    while(mp != tailp && m != nullptr) {
      std::atomic_thread_fence(std::memory_order_acquire);
      something = true;
      rcv(m);
      mp->store(reinterpret_cast<message*>(0x1), std::memory_order_release);
      mp = &m->r_next;
      m = mp->load(std::memory_order_relaxed);
    }

    headp_ = mp;
    return something;
  }
  
  template<int rn>
  template<typename Rcv, typename Batch>
  bool channels_r<rn>::receive_batch(Rcv &&rcv, Batch &&batch) {
    std::atomic<message*> *tailp = tailp_.load(std::memory_order_relaxed);
    std::atomic<message*> *mp = headp_;
    message *m = mp->load(std::memory_order_relaxed);
    std::intptr_t n = 0;
    
    while(mp != tailp && m != nullptr) {
      n += 1;
      std::atomic_thread_fence(std::memory_order_acquire);
      rcv(m);
      mp = &m->r_next;
      m = mp->load(std::memory_order_relaxed);
    }

    batch();
    
    bool something = n != 0;

    if(something) {
      std::atomic_thread_fence(std::memory_order_release);
      
      mp = headp_;
      while(n--) {
        m = mp->load(std::memory_order_relaxed);
        mp->store(reinterpret_cast<message*>(0x1), std::memory_order_relaxed);
        mp = &m->r_next;
      }
      headp_ = mp;
    }
    
    return something;
  }
}}
#endif
