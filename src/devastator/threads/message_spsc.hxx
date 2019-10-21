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
  struct message {
    message *next = reinterpret_cast<message*>(0xdeadbeef);
  };
  
  template<int rn>
  class channels_r {
    template<int wn, int rn, channels_r<rn>(*chan_r)[wn]>
    friend class channels_w;
  
    struct each {
      message *recv_last;
      std::atomic<std::uint32_t> *ack_slot;
    } r_[rn];
    
    signal_slots<std::uint32_t, rn> slots_;
    std::atomic<int> slot_next_{0};
    
  public:
    template<typename Rcv>
    bool receive(Rcv &&rcv);
    template<typename Rcv, typename Batch>
    bool receive_batch(Rcv &&rcv, Batch &&batch);
    
  private:
    void prefetch(int hot_n, hot_slot<std::uint32_t> hot[]);
  };
  
  template<int wn, int rn, channels_r<rn>(*chan_r)[wn]>
  class channels_w {
    struct each {
      message *sent_last;
      message *ack_head;
      std::atomic<std::uint32_t> *recv_slot;
      std::uint32_t recv_bump = 0; 
    } w_[wn];
    
    signal_slots<std::uint32_t, wn> slots_;

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
      rs->r_[r_id].ack_slot = &slots_.live[w_id];
      
      w_[w_id].sent_last = dummy;
      w_[w_id].ack_head = dummy;
      w_[w_id].recv_slot = &rs->slots_.live[r_id];
    }
  }

  template<int wn, int rn, channels_r<rn>(*chan_r)[wn]>
  void channels_w<wn,rn,chan_r>::send(int id, message *m) {
    w_[id].sent_last->next = m;
    w_[id].sent_last = m;
    w_[id].recv_slot->store(++w_[id].recv_bump, std::memory_order_release);
    //say()<<"wchan "<<id<<" of "<<n<<" bumped "<<w_[id].recv_bump-1<<" -> "<<w_[id].recv_bump;
  }
  
  template<int rn>
  void channels_r<rn>::prefetch(int hot_n, hot_slot<std::uint32_t> hot[]) {
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
  
  template<int rn>
  template<typename Rcv>
  bool channels_r<rn>::receive(Rcv &&rcv) {
    hot_slot<std::uint32_t> hot[rn];
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
    hot_slot<std::uint32_t> hot[rn];
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
    hot_slot<std::uint32_t> hot[wn];
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
}}
#endif
