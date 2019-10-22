#ifndef _64127753ef6b44c0beff7c6a5dc22174
#define _64127753ef6b44c0beff7c6a5dc22174

#include <atomic>

namespace deva {
namespace threads {
  template<typename Uint>
  struct hot_slot {
    int ix;
    Uint delta, old;
  };
  
  template<typename Uint, int n>
  struct signal_slots {
    union alignas(n > 1 ? 64 : 1) {
      std::atomic<Uint> live[n];
    };
    union alignas(n > 1 ? 64 : 1) {
      Uint shadow[n];
    };

    signal_slots() {
      for(int i=0; i < n; i++) {
        live[i].store(0, std::memory_order_relaxed);
        shadow[i] = 0;
      }
    }

    // Looks for slots with notifications in them. Fills in provided array with the
    // found "n" slots, "n" returned.
    int reap(hot_slot<Uint> hot[n]);
    
    bool possibly_quiesced() const;
  };

  template<typename Uint, int n>
  int signal_slots<Uint,n>::reap(hot_slot<Uint> hot[n]) {
    int hot_n = 0;
    Uint fresh[n];

    for(int i=0; i < n; i++)
      fresh[i] = live[i].load(std::memory_order_relaxed);

    // software fence: prevents load/stores from moving before or after.
    // this is for performance, we want to scan the live counters with
    // dense loads into a temporary "fresh" buffer, and do comparison
    // processing afterwards.
    std::atomic_signal_fence(std::memory_order_acq_rel);
    
    for(int i=0; i < n; i++) {
      if(fresh[i] != shadow[i]) {
        hot[hot_n].ix = i;
        hot[hot_n].delta = fresh[i] - shadow[i];
        hot[hot_n].old = shadow[i];
        hot_n += 1;
        
        shadow[i] = fresh[i];
      }
    }
    
    if(hot_n != 0)
      std::atomic_thread_fence(std::memory_order_acquire);
    
    return hot_n;
  }

  template<typename Uint, int n>
  bool signal_slots<Uint,n>::possibly_quiesced() const {
    bool yep = true;
    for(int i=0; i < n; i++)
      yep &= shadow[i] == live[i].load(std::memory_order_relaxed);
    return yep;
  }
}}
#endif
