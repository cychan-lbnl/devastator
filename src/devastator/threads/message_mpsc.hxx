#include <devastator/threads/message.hxx>

#ifndef _f49ebd2f7c9341ad9a3d9f865d565837
#define _f49ebd2f7c9341ad9a3d9f865d565837

#include <devastator/opnew.hxx>

#include <atomic>
#include <cstdint>

namespace deva {
namespace threads {
  static constexpr int port_n = DEVA_THREADS_MESSAGE_PORT_N;
  
  struct message {
    std::atomic<message*> r_next; // next in reader's list (the actual queue)
    union {
      message *w_next; // the name by which we use the field (next in writer's list)
      message *next; // the name by which world_gasnet uses this field
    };
  };

  template<int>
  class channels_r {
    template<int wn, int rn, channels_r<rn>(*)[wn]>
    friend struct channels_w;
    
    struct alignas(64) port_tail_t {
      std::atomic<message*> head0_{nullptr};
      std::atomic<std::atomic<message*>*> tailp_{&head0_};
    } tails_[port_n];
    
    alignas(64)
    std::atomic<message*> *headp_[port_n];
    
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
    auto *q = &(*chan_r)[w].tails_[(threads::thread_me() + w) % port_n];
    
    m->r_next.store(nullptr, std::memory_order_relaxed);
    std::atomic<message*> *got = q->tailp_.exchange(&m->r_next, std::memory_order_relaxed);
    got->store(m, std::memory_order_release);
    
    m->w_next = nullptr;
    *this->sent_tailp_ = m;
    this->sent_tailp_ = &m->w_next;
  }

  //////////////////////////////////////////////////////////////////////////////

  template<int rn>
  channels_r<rn>::channels_r() {
    for(int port=0; port < port_n; port++)
      headp_[port] = &tails_[port].head0_;
  }
  
  template<int rn>
  template<typename Rcv>
  bool channels_r<rn>::receive(Rcv &&rcv) {
    bool did_something = false;
    std::atomic<message*> *tailp[port_n];

    for(int port=0; port < port_n; port++)
      tailp[port] = tails_[port].tailp_.load(std::memory_order_relaxed);
    
    #if DEVA_THREADS_MESSAGE_PORT_N > 1
    {
      std::atomic<message*> *mp[port_n];
      message *m[port_n];

      for(int port=0; port < port_n; port++) {
        mp[port] = headp_[port];
        m[port] = mp[port]->load(std::memory_order_relaxed);
      }

      while(true) {
        for(int port=0; port < port_n; port++)
          if(!(mp[port] != tailp[port] && m[port] != nullptr))
            goto finish_parallel_ports;

        std::atomic_thread_fence(std::memory_order_acquire);
        did_something = true;

        for(int port=0; port < port_n; port++)
          rcv(m[port]);

        std::atomic_signal_fence(std::memory_order_acq_rel);
        
        for(int port=0; port < port_n; port++) {
          mp[port]->store(reinterpret_cast<message*>(0x1), std::memory_order_release);
          mp[port] = &m[port]->r_next;
          m[port] = mp[port]->load(std::memory_order_relaxed);
        }

        std::atomic_signal_fence(std::memory_order_acq_rel);
      }

    finish_parallel_ports:
      for(int port=0; port < port_n; port++)
        headp_[port] = mp[port];
    }
    #endif

    for(int port=0; port < port_n; port++) {
      std::atomic<message*> *mp = headp_[port];
      message *m = mp->load(std::memory_order_relaxed);
      
      while(mp != tailp[port] && m != nullptr) {
        std::atomic_thread_fence(std::memory_order_acquire);
        did_something = true;
        rcv(m);
        mp->store(reinterpret_cast<message*>(0x1), std::memory_order_release);
        mp = &m->r_next;
        m = mp->load(std::memory_order_relaxed);
      }

      headp_[port] = mp;
    }
    
    return did_something;
  }
  
  template<int rn>
  template<typename Rcv, typename Batch>
  bool channels_r<rn>::receive_batch(Rcv &&rcv, Batch &&batch) {
    bool did_something = false;
    std::atomic<message*> *tailp[port_n];
    std::atomic<message*> *mp[port_n];
    message *m[port_n];
    std::intptr_t n_par = 0;
    std::intptr_t n_seq[port_n];
    
    for(int port=0; port < port_n; port++) {
      tailp[port] = tails_[port].tailp_.load(std::memory_order_relaxed);
      mp[port] = headp_[port];
      m[port] = mp[port]->load(std::memory_order_relaxed);
    }

    #if DEVA_THREADS_MESSAGE_PORT_N > 1
    {
      while(true) {
        for(int port=0; port < port_n; port++) {
          if(!(mp[port] != tailp[port] && m[port] != nullptr))
            goto finish_parallel_rcv;
        }

        std::atomic_thread_fence(std::memory_order_acquire);

        n_par += 1;
        for(int port=0; port < port_n; port++)
          rcv(m[port]);

        std::atomic_signal_fence(std::memory_order_acq_rel);

        for(int port=0; port < port_n; port++) {
          mp[port] = &m[port]->r_next;
          m[port] = mp[port]->load(std::memory_order_relaxed);
        }

        std::atomic_signal_fence(std::memory_order_acq_rel);
      }

    finish_parallel_rcv:
      did_something |= n_par != 0;
    }
    #endif

    for(int port=0; port < port_n; port++) {
      std::intptr_t n = 0;
      
      while(mp[port] != tailp[port] && m[port] != nullptr) {
        n += 1;
        std::atomic_thread_fence(std::memory_order_acquire);
        rcv(m[port]);
        mp[port] = &m[port]->r_next;
        m[port] = mp[port]->load(std::memory_order_relaxed);
      }

      n_seq[port] = n;
      did_something |= n != 0;
    }

    batch();
    
    if(did_something) {
      std::atomic_thread_fence(std::memory_order_release);

      for(int port=0; port < port_n; port++)
        mp[port] = headp_[port];
      
      #if DEVA_THREADS_MESSAGE_PORT_N > 1
      while(n_par--) {
        for(int port=0; port < port_n; port++) {
          m[port] = mp[port]->load(std::memory_order_relaxed);
          mp[port]->store(reinterpret_cast<message*>(0x1), std::memory_order_relaxed);
          mp[port] = &m[port]->r_next;
        }
      }

      std::atomic_signal_fence(std::memory_order_acq_rel);
      #endif
      
      for(int port=0; port < port_n; port++) {
        std::intptr_t n = n_seq[port];
        while(n--) {
          m[port] = mp[port]->load(std::memory_order_relaxed);
          mp[port]->store(reinterpret_cast<message*>(0x1), std::memory_order_relaxed);
          mp[port] = &m[port]->r_next;
        }
        headp_[port] = mp[port];
      }
    }
    
    return did_something;
  }
}}
#endif
