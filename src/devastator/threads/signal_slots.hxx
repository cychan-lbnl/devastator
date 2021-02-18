#ifndef _64127753ef6b44c0beff7c6a5dc22174
#define _64127753ef6b44c0beff7c6a5dc22174

#include <devastator/utility.hxx>

#include <atomic>
#include <cstring>

#ifndef DEVA_THREADS_SIGNAL_REAP_ATOM
  #define DEVA_THREADS_SIGNAL_REAP_ATOM 0
#endif
#ifndef DEVA_THREADS_SIGNAL_REAP_MEMCPY
  #define DEVA_THREADS_SIGNAL_REAP_MEMCPY 0
#endif
#ifndef DEVA_THREADS_SIGNAL_REAP_SIMD
  #define DEVA_THREADS_SIGNAL_REAP_SIMD 0
#endif

namespace deva {
namespace threads {
  template<typename Uint>
  struct hot_slot {
    union {
      int ix_xor_i; // returned by reap()
      int next; // returned by reap_circular()
    };
    Uint delta;
    union {
      Uint old; // returned by reap()
      Uint now; // returned by reap_circular()
    };
  };
  
  template<typename Uint, int n>
  struct signal_slots {
    static constexpr int word_n = (n*sizeof(Uint) + sizeof(std::uintptr_t)-1)/sizeof(std::uintptr_t);
    
    #if DEVA_THREADS_SIGNAL_REAP_SIMD
      #if __AVX512__
        static constexpr int native_simd_size = 64;
      #elif __AVX__
        static constexpr int native_simd_size = 32;
      #elif __SSE__
        static constexpr int native_simd_size = 16;
      #else
        static constexpr int native_simd_size = 8;
      #endif

      static constexpr int needed_size = 1<<log_up(n*sizeof(Uint), 2);
      
      typedef Uint simd_t __attribute__((
        vector_size(needed_size < native_simd_size ? needed_size : native_simd_size)
      ));
      
      static constexpr int simd_n = (n*sizeof(Uint) + sizeof(simd_t)-1)/sizeof(simd_t);
    #endif

    union alignas(n > 1 ? 64 : 1) slots_t {
      std::atomic<Uint> atom[n];
      Uint non_atom[n];
      std::uintptr_t word[word_n];
      #if DEVA_THREADS_SIGNAL_REAP_SIMD
        simd_t simd[simd_n];
      #endif
    } live, shadow;
    
    signal_slots() {
      std::memset(&live, 0, sizeof(live));
      std::memset(&shadow, 0, sizeof(shadow));
    }

    // Looks for slots with notifications in them. Fills in provided array with the
    // found "n" slots, "n" returned.
    template<bool acquire=true, bool peek=false>
    int reap(hot_slot<Uint> hot[n]);
    
    int reap_circular(hot_slot<Uint> hot[n]);
  };

  template<typename Uint, int n>
  template<bool acquire, bool peek>
  __attribute__((noinline))
  int signal_slots<Uint,n>::reap(hot_slot<Uint> hot[n]) {
    int hot_n = 0;
    slots_t fresh;

    std::atomic_signal_fence(std::memory_order_acq_rel);

    #if DEVA_THREADS_SIGNAL_REAP_ATOM
      for(int i=0; i < n; i++)
        fresh.non_atom[i] = live.atom[i].load(std::memory_order_relaxed);
    #elif DEVA_THREADS_SIGNAL_REAP_MEMCPY
      std::memcpy(&fresh, &live, sizeof(live));
    #elif DEVA_THREADS_SIGNAL_REAP_SIMD
      for(int i=0; i < simd_n; i++)
        fresh.simd[i] = live.simd[i];
    #endif
    
    std::atomic_signal_fence(std::memory_order_acq_rel);
    
    for(int i=0; i < n; i++) {
      if(fresh.non_atom[i] != shadow.non_atom[i]) {
        hot[hot_n].ix_xor_i = i ^ hot_n;
        hot[hot_n].delta = fresh.non_atom[i] - shadow.non_atom[i];
        hot[hot_n].old = shadow.non_atom[i];
        hot_n += 1;
        
        if(!peek) shadow.non_atom[i] = fresh.non_atom[i];
      }
    }
    
    if(acquire && hot_n != 0)
      std::atomic_thread_fence(std::memory_order_acquire);
    
    return hot_n;
  }

  template<typename Uint, int n>
  __attribute__((noinline))
  int signal_slots<Uint,n>::reap_circular(hot_slot<Uint> hot[n]) {
    slots_t fresh;

    std::atomic_signal_fence(std::memory_order_acq_rel);

    #if DEVA_THREADS_SIGNAL_REAP_ATOM
      for(int i=0; i < n; i++)
        fresh.non_atom[i] = live.atom[i].load(std::memory_order_relaxed);
    #elif DEVA_THREADS_SIGNAL_REAP_MEMCPY
      std::memcpy(&fresh, &live, sizeof(live));
    #elif DEVA_THREADS_SIGNAL_REAP_SIMD
      for(int i=0; i < simd_n; i++)
        fresh.simd[i] = live.simd[i];
    #endif
    
    std::atomic_signal_fence(std::memory_order_acq_rel);
    
    int head = -1, tail = -1;
    
    for(int i=0; i < n; i++) {
      if(fresh.non_atom[i] != shadow.non_atom[i]) {
        hot[i].next = head;
        if(head == -1) tail = i;
        head = i;
        
        hot[i].delta = fresh.non_atom[i] - shadow.non_atom[i];
        hot[i].now = fresh.non_atom[i];
        
        shadow.non_atom[i] = fresh.non_atom[i];
      }
    }
    
    if(head != -1) {
      hot[tail].next = head; // circularize
      std::atomic_thread_fence(std::memory_order_acquire);
    }
    
    return head;
  }
}}
#endif
