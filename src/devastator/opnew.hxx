#ifndef _1fa94734_0f88_4819_abb4_43776c659c8a
#define _1fa94734_0f88_4819_abb4_43776c659c8a

#define OPNEW_ENABLED 1
#define OPNEW_DEBUG 0

#if OPNEW_DEBUG
  #define OPNEW_ASSERT(ok) ASSERT(ok)
#else
  #define OPNEW_ASSERT(ok) ((void)0)
#endif

#if !OPNEW_ENABLED
namespace opnew {
  inline void progress() {}
}
#else

#include "diagnostic.hxx"

#include <cstdint>
#include <new>
#include <type_traits>

#include <sanitizer/asan_interface.h>

#if __has_feature(address_sanitizer) || defined(__SANITIZE_ADDRESS__)
 #define ASAN_ENABLED 1
#else
 #define ASAN_ENABLED 0
#endif

namespace opnew {
  template<int...>
  struct index_sequence {};

  template<int n, int ...s>
  struct make_index_sequence_: make_index_sequence_<n-1, n-1, s...> {};

  template<int ...s>
  struct make_index_sequence_<0, s...> {
    typedef index_sequence<s...> type;
  };
  
  template<int n>
  using make_index_sequence = typename make_index_sequence_<n>::type;

  //////////////////////////////////////////////////////////////////////
  
  constexpr int log2dn(unsigned int x) {
    return 8*sizeof(unsigned int)-1 - __builtin_clz(x);
  }
  constexpr int log2dn(unsigned long x) {
    return 8*sizeof(unsigned long)-1 - __builtin_clzl(x);
  }
  constexpr int log2dn(unsigned long long x) {
    return 8*sizeof(unsigned long long)-1 - __builtin_clzll(x);
  }
  template<typename T>
  constexpr int log2dn(T x) {
    return log2dn(typename std::make_unsigned<T>::type(x));
  }
  
  constexpr int log2up(unsigned int x) {
    return 8*sizeof(unsigned int)-1 - __builtin_clz(x|1) + (x&(x-1) ? 1 : 0);
  }
  constexpr int log2up(unsigned long x) {
    return 8*sizeof(unsigned long)-1 - __builtin_clzl(x|1) + (x&(x-1) ? 1 : 0);
  }
  constexpr int log2up(unsigned long long x) {
    return 8*sizeof(unsigned long long)-1 - __builtin_clzll(x|1) + (x&(x-1) ? 1 : 0);
  }
  template<typename T>
  constexpr int log2up(T x) {
    return log2up(typename std::make_unsigned<T>::type(x));
  }
  
  template<typename T>
  constexpr int log2dn(T x, int when_zero) {
    return x == 0 ? when_zero : log2dn(x);
  }
  template<typename T>
  constexpr int log2up(T x, int when_zero) {
    return x == 0 ? when_zero : log2up(x);
  }

  constexpr int bitffs(unsigned int x) {
    return __builtin_ffs(x);
  }
  constexpr int bitffs(unsigned long x) {
    return __builtin_ffsl(x);
  }
  constexpr int bitffs(unsigned long long x) {
    return __builtin_ffsll(x);
  }
  template<typename T>
  constexpr int bitffs(T x) {
    return bitffs(typename std::make_unsigned<T>::type(x));
  }
  
  
  //////////////////////////////////////////////////////////////////////
  
  constexpr std::size_t size_of_bin(int bin) {
    return sizeof(void*)*(
      bin < 7
        ? 1+bin
        : bin < 39 // 39 -> 1K
          ? (8|((bin-7)&7))<<((bin-7)>>3)
          : (4|((bin-39)&3))<<(((bin-39)>>2) + 5)
    );
  }

  constexpr int bin_of_size_help2(int nw, int log2dn_nw) {
    return nw < 8
      ? nw - 1
      : nw < 128
        ? (nw >> (log2dn_nw-3))-0x8 + (nw&((1<<(log2dn_nw-3))-1)?1:0) + (log2dn_nw<<3) + 7 - (3<<3)
        : (nw >> (log2dn_nw-2))-0x4 + (nw&((1<<(log2dn_nw-2))-1)?1:0) + (log2dn_nw<<2) + 39 - (7<<2);
  }

  constexpr int bin_of_size_help1(std::size_t size) {
    return bin_of_size_help2(
      (size + sizeof(void*)-1)/sizeof(void*),
      log2dn((size + sizeof(void*)-1)/sizeof(void*))
    );
  }
  
  constexpr int bin_n = 51;
  constexpr std::size_t bin_size_max = size_of_bin(bin_n-1);
  constexpr int bin_size_max_words = (bin_size_max + sizeof(void*)-1)/sizeof(void*);
  
  struct bin_of_size_table {
    alignas(64) std::int8_t a[bin_size_max_words];
  };
  
  template<int ...i>
  constexpr bin_of_size_table make_bin_of_size_small(index_sequence<i...>) {
    return {{bin_of_size_help1(i*sizeof(void*))...}};
  }

  constexpr bin_of_size_table bin_of_size_small = make_bin_of_size_small(make_index_sequence<bin_size_max_words>());

  constexpr int bin_of_size(std::size_t size) {
    return (size + sizeof(void*)-1)/sizeof(void*) < bin_size_max_words
      ? bin_of_size_small.a[(size + sizeof(void*)-1)/sizeof(void*)]
      : -1;
  }
  
  //////////////////////////////////////////////////////////////////////

  constexpr std::intptr_t K = 1<<10;
  constexpr std::intptr_t arena_size = 4*K*K;
  constexpr std::intptr_t page_size = 2*K;
  constexpr std::intptr_t huge_size = 256*K;
  constexpr std::intptr_t huge_align = 16*K;
  
  struct frobj {
    std::uintptr_t next_xor_prev;

    frobj* next(frobj *prev) const {
      return reinterpret_cast<frobj*>(
        reinterpret_cast<std::uintptr_t>(prev) ^ next_xor_prev
      );
    }

    void set_links(frobj *prev, frobj *next) {
      next_xor_prev = reinterpret_cast<std::uintptr_t>(prev) ^
                      reinterpret_cast<std::uintptr_t>(next);
    }
    void change_link(frobj *old, frobj *next) {
      next_xor_prev ^= reinterpret_cast<std::uintptr_t>(old) ^
                       reinterpret_cast<std::uintptr_t>(next);
    }
  };

  template<typename T>
  struct intru_heap_link {
    std::intptr_t ix;
    T *kid[2];
  };
  template<typename T>
  struct intru_heap {
    T *top = nullptr;
    std::intptr_t n = 0;
    
    void insert(intru_heap_link<T> T::*link_of, T *x);
    T* pop_top(intru_heap_link<T> T::*link_of);
    void remove(intru_heap_link<T> T::*link_of, T *x);
    
    void sane(intru_heap_link<T> T::*link_of);
  };

  struct pool {
    #if OPNEW_DEBUG
      int deadbeef;
    #endif
    int popn, popn_not_held;
    frobj *hold_head;
    frobj *hold_tail;
    intru_heap_link<pool> heap_link;
  };

  template<int page_per_arena>
  struct arena_form {
    using arena = arena_form<page_per_arena>;
    
    alignas(64)
    int owner;
    
    alignas(64)
    intru_heap_link<arena> heap_link;

    #if OPNEW_DEBUG && ASAN_ENABLED
      // use 8-byte types so we accurately track our own poisoning
      std::int64_t pbin[page_per_arena];
      std::int64_t pmap[page_per_arena];
    #else
      // bin id of pool for any page belonging to a pool, otherwise
      // for non-pool blobs head page has -1 and body pages are undefined.
      std::int8_t pbin[page_per_arena];

      // x < -16k: blob head, -x-16k is length
      // x < 0: -x-1 is blob head
      // x >= 0: hole of length x+1
      std::int16_t pmap[page_per_arena];
    #endif
    
    int pmap_is_hole(int p) {
      return pmap[p] > 0;
    }
    int pmap_hole_length(int p) {
      return pmap[p];
    }
    int pmap_hole_length_or_zero(int p) {
      return pmap[p] > 0 ? pmap[p] : 0;
    }
    bool pmap_is_blob(int p) {
      return pmap[p] < 0;
    }
    bool pmap_is_blob_head(int p) {
      return pmap[p] < -16*K;
    }
    int pmap_blob_body_to_head(int p) {
      return -pmap[p] - 1;
    }
    int pmap_blob_any_to_head(int p) {
      return pmap_is_blob_head(p) ? p : pmap_blob_body_to_head(p);
    }
    int pmap_blob_head_length(int p) {
      return -pmap[p] - 16*K;
    }

    static constexpr int hole_lev_n = 1 + log2up((page_per_arena+1)/2);
    std::uint16_t holes[(2<<log2up((page_per_arena+1)/2))-1];
  };

  constexpr int page_per_arena = (arena_size - sizeof(arena_form<arena_size/page_size>))/page_size;
  
  using arena = arena_form<page_per_arena>;

  inline arena* arena_of_nonhuge(void *o) {
    std::uintptr_t u = reinterpret_cast<std::uintptr_t>(o);
    return reinterpret_cast<arena*>(u & -arena_size);
  }

  template<std::size_t known_size=0>
  inline arena* arena_of(void *o) {
    std::uintptr_t u = reinterpret_cast<std::uintptr_t>(o);
    if((known_size != 0 && known_size < huge_size) || (u & (huge_align-1)))
      return reinterpret_cast<arena*>(u & -arena_size);
    else
      return nullptr;
  }

  template<std::size_t known_size=0>
  inline int bin_of(arena *a, void *o) {
    if(known_size != 0)
      return bin_of_size(known_size);
    else
      return a->pbin[((char*)o - (char*)(a+1))/page_size];
  }

  //////////////////////////////////////////////////////////////////////

  struct bin_state {
    frobj tail{0x0};
    frobj *head = &tail;
    std::uintptr_t popn = 0, popn_least = 0;
    intru_heap<pool> held_pools;

    void sane() {
      #if OPNEW_DEBUG
        frobj *x, *y;
        x = head;
        y = nullptr;
        for(int i=0; i < popn; i++) {
          frobj *z = x->next(y);
          y = x;
          x = z;
        }
        ASSERT(x == &tail);
      #endif
    }
  };

  extern thread_local std::uint64_t opcalls;
  extern thread_local std::uint64_t bins_occupied_mask;
  extern thread_local bin_state bins[bin_n];

  //////////////////////////////////////////////////////////////////////

  void* operator_new_slow(std::size_t size);
  void operator_delete_slow(void *obj);
  void gc_bins();
  void flush_remote();
  
  //////////////////////////////////////////////////////////////////////
  // public API
  
  inline void progress() {
    opcalls += 1;
    
    if(opcalls >= 10000) {
      gc_bins();
      opcalls = 0;
    }

    flush_remote();
  }
  
  inline void* operator_new(std::size_t size) {
    opcalls += 1;
    int bin = bin_of_size(size);
    
    if(bin != -1 && bins[bin].popn != 0) {
      bin_state *b = &bins[bin];
      frobj *o = b->head;
      b->head = o->next(nullptr);
      b->head->change_link(o, nullptr);
      b->popn -= 1;
      b->popn_least = std::min(b->popn_least, b->popn);
      
      b->sane();
      return (void*)o;
    }
    else
      return operator_new_slow(size);
  }

  template<std::size_t known_size=0, bool known_local=false>
  void operator_delete(void *obj) {
    opcalls += 1;
    arena *a = opnew::template arena_of<known_size>(obj);
    
    if((known_size != 0 && known_size < huge_size) || a != nullptr) {
      int bin = opnew::template bin_of<known_size>(a, obj);

      OPNEW_ASSERT(!known_local || a->owner == tmsg::thread_me());
      
      if((known_local || a->owner == tmsg::thread_me()) && bin != -1) {
        bin_state *b = &bins[bin];
        frobj *o = (frobj*)obj;
        o->set_links(nullptr, b->head);
        b->head->change_link(nullptr, o);
        b->head = o;
        b->popn += 1;
        bins_occupied_mask |= std::uint64_t(1)<<bin;
        
        b->sane();
        return;
      }
    }
    
    operator_delete_slow(obj);
  }
}

#endif
#endif
