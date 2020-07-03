////////////////////////////////////////////////////////////////////////////////
// forward declarations

#ifndef _b562d0d1055e42598549ddbccfb1b879
#define _b562d0d1055e42598549ddbccfb1b879

#ifndef DEVA_OPNEW_DEVA
  #define DEVA_OPNEW_DEVA 0
#endif
#ifndef DEVA_OPNEW_LIBC
  #define DEVA_OPNEW_LIBC 0
#endif
#ifndef DEVA_OPNEW_JEMALLOC
  #define DEVA_OPNEW_JEMALLOC 0
#endif

#ifndef DEVA_OPNEW_DEBUG
  #define DEVA_OPNEW_DEBUG 0
#endif

#include <new>

#if DEVA_OPNEW_LIBC || DEVA_OPNEW_JEMALLOC
  #if DEVA_OPNEW_JEMALLOC
    #include <external/jemalloc.h>
  #endif
  namespace deva {
  namespace opnew {
    inline void progress() {}
    inline void thread_me_initialized() {}
    
    inline void* operator_new(std::size_t size) {
      return ::operator new(size);
    }
    
    template<std::size_t known_size=0, bool known_local=false>
    void operator_delete(void *obj) noexcept {
      ::operator delete(obj);
    }
  }}
#elif DEVA_OPNEW_DEVA
  namespace deva {
  namespace opnew {
    void* operator_new(std::size_t);
    
    template<std::size_t known_size=0, bool known_local=false>
    void operator_delete(void *obj) noexcept;
  }}
#endif

#endif

////////////////////////////////////////////////////////////////////////////////
// definitions

#ifndef _1fa947340f884819abb443776c659c8a
#define _1fa947340f884819abb443776c659c8a

#if DEVA_OPNEW_DEVA

#include <devastator/diagnostic.hxx>
#include <devastator/threads.hxx>
#include <devastator/utility.hxx>

#include <cstdint>
#include <new>
#include <type_traits>

#if DEVA_OPNEW_DEBUG
  #define DEVA_OPNEW_ASSERT(ok) (!!(ok) || (::deva::assert_failed(__FILE__,__LINE__), 0))
#else
  #define DEVA_OPNEW_ASSERT(ok) ((void)0)
#endif

namespace deva {
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
    return nw == 0 ? 0 :
      nw < 8
        ? nw - 1
        : nw < 128
          ? (nw >> (log2dn_nw-3))-0x8 + (nw&((1<<(log2dn_nw-3))-1)?1:0) + (log2dn_nw<<3) + 7 - (3<<3)
          : (nw >> (log2dn_nw-2))-0x4 + (nw&((1<<(log2dn_nw-2))-1)?1:0) + (log2dn_nw<<2) + 39 - (7<<2);
  }

  constexpr int bin_of_size_help1(std::size_t size) {
    return bin_of_size_help2(
      (size + sizeof(void*)-1)/sizeof(void*),
      log_dn((size + sizeof(void*)-1)/sizeof(void*), 2)
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
    return {{(std::int8_t)bin_of_size_help1(i*sizeof(void*))...}};
  }

  constexpr bin_of_size_table bin_of_size_small = make_bin_of_size_small(make_index_sequence<bin_size_max_words>());

  constexpr int bin_of_size(std::size_t size) {
    return (size + sizeof(void*)-1)/sizeof(void*) < bin_size_max_words
      ? bin_of_size_small.a[(size + sizeof(void*)-1)/sizeof(void*)]
      : -1;
  }
  
  //////////////////////////////////////////////////////////////////////////////

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
    T *top; // = nullptr
    std::intptr_t n; // = 0
    
    void insert(intru_heap_link<T> T::*link_of, T *x);
    T* pop_top(intru_heap_link<T> T::*link_of);
    void remove(intru_heap_link<T> T::*link_of, T *x);
    
    #if 0
    void sane(intru_heap_link<T> T::*link_of);
    #else
    void sane(intru_heap_link<T> T::*link_of) {}
    #endif
  };

  struct pool {
    #if DEVA_OPNEW_DEBUG
      unsigned deadbeef;
    #endif
    int popn, popn_not_held;
    frobj *hold_head;
    frobj *hold_tail;
    intru_heap_link<pool> heap_link;
  };
  
  template<typename Arena, typename Size>
  struct arena_holes_link {
    Arena *kid[2];
    Size kid_max[2];
    int ix;
  };
  
  template<typename Arena, typename Size>
  struct arena_holes;
  
  struct thread_state;

  template<int page_per_arena>
  struct arena_form {
    using arena = arena_form<page_per_arena>;
    using arena_holes = opnew::arena_holes<arena, std::uint16_t>;
    
    alignas(64)
    thread_state *owner_ts; // &my_ts of owner
    int owner_id; // thread_me() of owner
    arena *owner_next;
    
    alignas(64)
    arena_holes_link<arena, std::uint16_t> holes_link;

    // bin id of pool for any page belonging to a pool, otherwise
    // for non-pool blobs head page has -1 and body pages are undefined.
    std::int8_t pbin[page_per_arena];

    // x < -16k: blob head, -x-16k is length
    // x < 0: -x-1 is blob head
    // x >= 0: hole of length x+1
    std::int16_t pmap[page_per_arena];
    
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

    static constexpr int hole_lev_n = 1 + log_up((page_per_arena+1)/2, 2);
    std::uint16_t holes[(2<<log_up((page_per_arena+1)/2, 2))-1];
    
    std::uint16_t hole_size_max() const {
      return holes[0];
    }
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

  //////////////////////////////////////////////////////////////////////////////

  template<typename Arena, typename Size>
  class arena_holes {
    Arena *root_ = nullptr;
    Size root_max_ = 0;
    int popn_ = 0;
    
  public:
    Size size_max() const {
      return root_max_;
    }
    
    void insert(Arena *a);
    
    template<typename Fn>
    auto fit_and_decrease(Size size, Fn &&fn)
      -> decltype(fn(std::declval<Arena*>()));
    
    void increased(Arena *a);

  private:
    void repair(Arena *p, Arena *p_up, std::uint64_t kpath);
  };

  //////////////////////////////////////////////////////////////////////////////
  
  struct bin_state {
    frobj tail; // = {0}
    std::uintptr_t head_xor_tail; // = 0
    std::uintptr_t popn, popn_least; // = 0,0
    intru_heap<pool> held_pools; // = {}

    // get head
    frobj* head() {
      return reinterpret_cast<frobj*>(head_xor_tail ^ reinterpret_cast<std::uintptr_t>(&tail));
    }
    // set head
    void head(frobj *o) {
      head_xor_tail = reinterpret_cast<std::uintptr_t>(&tail) ^ reinterpret_cast<std::uintptr_t>(o);
    }
    
    void sane() {
      held_pools.sane(&pool::heap_link);
      
      #if DEVA_OPNEW_DEBUG > 1
        frobj *x, *y;
        x = head();
        y = nullptr;
        for(int i=0; i < popn; i++) {
          frobj *z = x->next(y);
          y = x;
          x = z;
        }
        DEVA_OPNEW_ASSERT(x == &tail);
      #endif
    }
  };

  struct thread_state {
    std::uint64_t opcalls;
    frobj *outsider_frees;
    
    bin_state bins[bin_n];
  };
  
  extern __thread thread_state my_ts;
  
  //////////////////////////////////////////////////////////////////////

  void thread_me_initialized();
  void* operator_new_slow(std::size_t size);
  void operator_delete_slow(void *obj);
  void gc_bins();
  void flush_remote();
  
  //////////////////////////////////////////////////////////////////////
  // public API
  
  inline void progress() {
    my_ts.opcalls += 1;
    
    if(my_ts.opcalls >= 10000) {
      gc_bins();
      my_ts.opcalls = 0;
    }

    //flush_remote(); // done in gc_bins()
  }
  
  inline void* operator_new(std::size_t size) {
    //deva::say()<<"opnew";
    my_ts.opcalls += 1;
    int bin = bin_of_size(size);
    
    if(bin != -1 && my_ts.bins[bin].popn != 0) {
      bin_state *b = &my_ts.bins[bin];
      frobj *o = b->head();
      b->head(o->next(nullptr));
      b->head()->change_link(o, nullptr);
      b->popn -= 1;
      b->popn_least = std::min(b->popn_least, b->popn);
      
      b->sane();
      return (void*)o;
    }
    else
      return operator_new_slow(size);
  }

  template<std::size_t known_size, bool known_local>
  void operator_delete(void *obj) noexcept {
    my_ts.opcalls += 1;
    arena *a = opnew::template arena_of<known_size>(obj);
    
    if((known_size != 0 && known_size < huge_size) || a != nullptr) {
      int bin = opnew::template bin_of<known_size>(a, obj);

      DEVA_OPNEW_ASSERT(!known_local || a->owner_ts == &my_ts);
      
      if(bin != -1) {
        bin_state *b = &my_ts.bins[bin];
        frobj *o = new(obj) frobj;
        o->set_links(nullptr, b->head());
        b->head()->change_link(nullptr, o);
        b->head(o);
        b->popn += 1;
        
        b->sane();
        return;
      }
    }
    
    operator_delete_slow(obj);
  }
} // namespace opnew
} // namespace deva
#endif
#endif
