#include "opnew.hxx"
#include "diagnostic.hxx"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdlib>
#include <sys/mman.h>

#if OPNEW_ENABLED // contains whole file

#define ASAN_POISON(base, lo, hi) \
  { if((lo)<(hi)) ASAN_POISON_MEMORY_REGION((base)+(lo), (const char*)((base)+(hi))-(const char*)((base)+(lo))); }

#define ASAN_UNPOISON(base, lo, hi) \
  { if((lo)<(hi)) ASAN_UNPOISON_MEMORY_REGION((base)+(lo), (const char*)((base)+(hi))-(const char*)((base)+(lo))); }

using namespace std;
using namespace opnew;

void* operator new(std::size_t size) {
  return opnew::operator_new(size);
}

void operator delete(void *o) {
  opnew::operator_delete(o);
}

static_assert(sizeof(arena) % page_size != 0, "Crap, arena is page aligned");

namespace {
  arena* arena_create();
  void arena_destroy(arena *a);
  arena* arena_best(int pn);

  void arena_hole_changed(arena *a, int p, int pn);
  
  void* arena_alloc_avail(arena *a, int pn);
  void arena_dealloc_blob(arena *a, void *o);

  void arena_hold_pooled(arena *a, frobj *o, intru_heap<pool> &held_pools);

  void arena_dealloc_remote(arena *a, frobj *o);
  
  int arena_pool_init(arena *a, void *b, int bin, frobj **out_head, frobj **out_tail);
  
  thread_local arena *my_arenas = nullptr;

  constexpr int arena_heap_n = 1 + log2up((huge_size-1 + page_size-1)/page_size);
  thread_local intru_heap<arena> arena_heaps[arena_heap_n];

  struct remote_thread_bins {
    static constexpr int max_bin_n = 5;
    int8_t bin_n = 0;
    int8_t bin[max_bin_n];
    uint8_t popn_minus_one[max_bin_n];
    frobj *head[max_bin_n], *tail[max_bin_n];
    frobj *rest_head = nullptr;
  };
  
  thread_local uintptr_t remote_thread_mask[(tmsg::thread_n + 8*sizeof(uintptr_t)-1)/sizeof(uintptr_t)] = {/*0...*/};
  thread_local remote_thread_bins remote_bins[tmsg::thread_n] {};

  constexpr size_t pool_waste(int bin, int pn) {
    #define bin_sz (size_of_bin(bin))
    #define bin_al ((bin_sz & -bin_sz) < 64 ? (bin_sz & -bin_sz) : 64)
    #define pool_pad ((sizeof(pool) + bin_al-1) & -bin_al)
    return (pool_pad + (page_size*pn - pool_pad)%bin_sz)/pn;
    #undef bin_sz
    #undef bin_al
    #undef pool_pad
  }
  
  constexpr int calc_pool_best_pages(int bin, int pn=1, int pn_best=1) {
    return size_of_bin(bin) % page_size == 0
      ? -1
      : pn == 15 || pool_waste(bin, pn) < 64
        ? pn
        : calc_pool_best_pages(
          bin, pn+1,
          pool_waste(bin,pn) < pool_waste(bin,pn_best) ? pn : pn_best
        );
  }

  template<int ...bin>
  constexpr array<int8_t, bin_n> make_pool_best_pages(opnew::index_sequence<bin...>) {
    return {{calc_pool_best_pages(bin)...}};
  }
  
  constexpr array<int8_t, bin_n> pool_best_pages = make_pool_best_pages(opnew::make_index_sequence<bin_n>());
}

thread_local uint64_t opnew::bins_occupied_mask = 0;
thread_local uint64_t opnew::opcalls = 0;
thread_local bin_state opnew::bins[bin_n];

void* opnew::operator_new_slow(size_t size) {
  int bin_id = bin_of_size(size);
  bin_state *bin = &bins[bin_id];
  
  if(bin_id != -1) {
    // bin is empty!
    OPNEW_ASSERT(bin->popn == 0);
    OPNEW_ASSERT(bin->head == &bin->tail);
    OPNEW_ASSERT(bin->tail.next_xor_prev == 0);
    int pn = pool_best_pages[bin_id];
    
    if(pn != -1) {
      frobj *head, *tail;
      int popn;
      
      if(bin->held_pools.top != nullptr) {
        pool *poo = bin->held_pools.pop_top(&pool::heap_link);
        OPNEW_ASSERT(poo->deadbeef == 0xdeadbeef);
        
        head = poo->hold_head;
        tail = poo->hold_tail;
        popn = poo->popn - poo->popn_not_held;
        poo->hold_head = nullptr;
        poo->hold_tail = nullptr;
        poo->popn_not_held = poo->popn;

        #if OPNEW_DEBUG
          frobj *x = head,*y=nullptr;
          for(int n=0; n < popn; n++) {
            frobj *z = x->next(y);
            y = x;
            x = z;
          }
          OPNEW_ASSERT(x==nullptr && y==tail);
        #endif
      }
      else {
        arena *a = arena_best(pn);
        void *b = arena_alloc_avail(a, pn);
        popn = arena_pool_init(a, b, bin_id, &head, &tail);
      }
      tail->change_link(nullptr, &bin->tail);
      bin->tail.set_links(tail, nullptr);
      bin->head = head->next(nullptr);
      bin->head->change_link(head, nullptr);
      bin->popn = popn - 1;
      bin->sane();
      return (void*)head;
    }
    else {
      pn = (size_of_bin(bin_id) + page_size-1)/page_size;
      arena *a = arena_best(pn);
      return arena_alloc_avail(a, pn);
    }
  }
  else if(size < huge_size) {
    int pn = (size + page_size-1)/page_size;
    arena *a = arena_best(pn);
    return arena_alloc_avail(a, pn);
  }
  else {
    void *ans;
    int ok = posix_memalign(&ans, huge_align, size);
    if(ok != 0) throw std::bad_alloc();
    return ans;
  }
}

void opnew::operator_delete_slow(void *obj) {
  arena *a = arena_of(obj);
  
  if(a != nullptr) {
    OPNEW_ASSERT(a->pmap_is_blob(((char*)obj - (char*)(a+1))/page_size));
    
    if(a->owner_id == tmsg::thread_me())
      arena_dealloc_blob(a, obj);
    else
      arena_dealloc_remote(a, (frobj*)obj);
  }
  else
    std::free(obj);
}

void opnew::gc_bins() {
  uint64_t m = bins_occupied_mask;
  bins_occupied_mask = 0;

  while(m != 0) {
    int bin_id = bitffs(m) - 1;
    m &= m-1;

    bin_state *bin = &bins[bin_id];
    bin->sane();
    int best_pn = pool_best_pages[bin_id];
    
    if(bin->popn_least != 0) {
      uintptr_t n = (3*bin->popn_least)/4;
      OPNEW_ASSERT(n <= bin->popn);
      
      frobj *o = bin->tail.next(nullptr); // tail->prev
      frobj *oprev = &bin->tail;
      bin->popn -= n;
      
      while(n--) {
        frobj *onext = o->next(oprev);
        arena *a = arena_of_nonhuge(o);
        
        if(best_pn != -1)
          arena_hold_pooled(a, o, bin->held_pools);
        else
          arena_dealloc_blob(a, o);
        
        oprev = o;
        o = onext;
      }

      bin->tail.set_links(o, nullptr);
      if(o != nullptr)
        o->change_link(oprev, &bin->tail);
      else
        bin->head = &bin->tail;
    }
    
    bin->popn_least = bin->popn;
    bin->sane();
    bins_occupied_mask |= uint64_t(bin->popn ? 1 : 0) << bin_id;
  }
}

void opnew::flush_remote() {
  constexpr int B = 8*sizeof(uintptr_t);
  
  for(int mi=0; mi < (tmsg::thread_n + B-1)/B; mi++) {
    uintptr_t m = remote_thread_mask[mi];
    remote_thread_mask[mi] = 0;
    
    while(m != 0) {
      int t = mi*B - 1 + bitffs(m);
      m &= m-1;

      remote_thread_bins rbins = remote_bins[t];
      remote_bins[t] = {};
      
      tmsg::send(t, [=]() {
        for(int i=0; i < rbins.bin_n; i++) {
          bin_state *bin = &bins[rbins.bin[i]];
          rbins.tail[i]->change_link(nullptr, bin->head);
          bin->head->change_link(nullptr, rbins.tail[i]);
          bin->head = rbins.head[i];
          bin->popn += int(rbins.popn_minus_one[i]) + 1;
          bin->sane();
        }
        
        frobj *o = rbins.rest_head;
        while(o != nullptr) {
          frobj *o1 = o->next(nullptr);
          opnew::opcalls -= 1;
          opnew::operator_delete</*known_size=*/0, /*known_local*/true>(o);
          o = o1;
        }
      });
    }
  }
}

void opnew::thread_me_initialized() {
  for(arena *a = my_arenas; a != nullptr; a = a->owner_next)
    a->owner_id = tmsg::thread_me();
}

namespace {
  arena* arena_create() {
    void *m = mmap(nullptr, 2*arena_size, PROT_READ|PROT_WRITE, MAP_PRIVATE|MAP_ANONYMOUS, -1, 0);
    uintptr_t u0 = reinterpret_cast<uintptr_t>(m);
    uintptr_t u1 = (u0 + arena_size-1) & -arena_size;
    
    if(u1 != u0)
      munmap(m, u1-u0);
    
    if(u1 + arena_size != u0 + 2*arena_size)
      munmap(reinterpret_cast<void*>(u1 + arena_size), u0 + arena_size - u1);

    arena *a = reinterpret_cast<arena*>(u1);
    a->owner_id = tmsg::thread_me();
    a->owner_next = my_arenas;
    my_arenas = a;
    
    a->pmap[0] = page_per_arena;
    ASAN_POISON(a->pmap, 1, page_per_arena-1);
    a->pmap[page_per_arena-1] = page_per_arena;

    ASAN_POISON(a->pbin, 0, page_per_arena);
    
    for(int t=0; t < sizeof(arena::holes)/sizeof(arena::holes[0]); t++)
      a->holes[t] = 0;

    arena_hole_changed(a, 0, page_per_arena);
    
    arena_heaps[arena_heap_n-1].insert(&arena::heap_link, a);
    
    return a;
  }

  /*void arena_destroy(arena *a) {
    munmap((void*)a, arena_size);
  }*/
  
  arena* arena_best(int pn) {
    int hp = std::min(arena_heap_n-1, log2up(pn));
    uintptr_t best = uintptr_t(-1);
    
    while(hp < arena_heap_n) {
      uintptr_t a = -1 + reinterpret_cast<uintptr_t>(arena_heaps[hp].top);
      if(a < best)
        best = a;
      hp += 1;
    }
    
    if(best+1 != 0x0)
      return reinterpret_cast<arena*>(best+1);
    else
      return arena_create();
  }

  void arena_hole_changed(arena *a, int p, int pn) {
    int p0 = p & -2;
    int t = (1<<(arena::hole_lev_n-1))-1 + (p>>1);
    a->holes[t] = pn;
    while(true) {
      int t1 = ((t+1) ^ 1)-1;
      pn = std::max(pn, (int)a->holes[t1]);
      t = (t-1)/2;
      int pn0 = a->holes[t];
      a->holes[t] = pn;
      if(t == 0 || pn == pn0) break;
    }
  }
  
  void* arena_alloc_avail(arena *a, int pn) {
    int t = 0;
    int lev = 1;
    while(lev < arena::hole_lev_n) {
      t = 2*t + 1;
      t += a->holes[t] < pn ? 1 : 0;
      lev += 1;
    }
    
    int hp = 2*(t - ((1<<(arena::hole_lev_n-1))-1));
    hp += a->pmap_is_hole(hp) ? 0 : 1;
    OPNEW_ASSERT(hp < page_per_arena);
    int hpn = a->pmap_hole_length(hp);
    
    int big_old = a->holes[0];

    ASAN_UNPOISON(&a->pmap[hp + pn-1], 0, 1);
    a->pmap[hp + pn-1] = -hp - 1;
    a->pmap[hp] = -pn - 16*K;

    ASAN_UNPOISON(&a->pbin[hp], 0, 1);
    a->pbin[hp] = -1;
    
    arena_hole_changed(a, hp, 0);
    
    if(hpn != pn) {
      ASAN_UNPOISON(&a->pmap[hp + pn], 0, 1);
      a->pmap[hp + pn] = hpn-pn;
      a->pmap[hp + hpn-1] = hpn-pn;
      arena_hole_changed(a, hp+pn, hpn-pn);
    }
    
    int big_new = a->holes[0];
    
    // translate to heap ordinals
    big_old = std::min(arena_heap_n-1, log2dn(big_old));
    big_new = std::min(arena_heap_n-1, log2dn(big_new, -1));

    if(big_new != big_old) {
      arena_heaps[big_old].remove(&arena::heap_link, a);
      
      if(big_new != -1)
        arena_heaps[big_new].insert(&arena::heap_link, a);
    }
    
    return (char*)(a+1) + hp*page_size;
  }

  void arena_dealloc_blob(arena *a, void *o) {
    int p = ((char*)o - (char*)(a+1))/page_size;
    int pn = a->pmap_blob_head_length(p);
    
    bool lhole = p > 0 && a->pmap_is_hole(p-1);
    int lp = lhole ? p-1 - (a->pmap[p-1]-1) : p;
    int lpn = p - lp;
    
    int rp = p + pn;
    bool rhole = rp != page_per_arena && a->pmap_is_hole(rp);
    int rpn = rhole ? a->pmap_hole_length(rp) : 0;
    
    a->pmap[rp + rpn-1] = lpn + pn + rpn;
    a->pmap[lp] = lpn + pn + rpn;
    ASAN_POISON(a->pmap, lp+1, rp + rpn-1);
    ASAN_POISON(a->pbin, lp, rp + rpn);
    
    int big_old = a->holes[0];
    
    if(rhole)
      arena_hole_changed(a, rp, 0);
    
    arena_hole_changed(a, lp, lpn + pn + rpn);
    
    int big_new = a->holes[0];

    // translate to heap ordinals
    big_old = std::min(arena_heap_n-1, log2dn(big_old, -1));
    big_new = std::min(arena_heap_n-1, log2dn(big_new));
    
    if(big_new != big_old) {
      if(big_old != -1)
        arena_heaps[big_old].remove(&arena::heap_link, a);
      
      arena_heaps[big_new].insert(&arena::heap_link, a);
    }
  }
  
  int arena_pool_init(arena *a, void *b, int bin, frobj **out_head, frobj **out_tail) {
    int p0 = ((char*)b - (char*)(a+1))/page_size;
    int p1 = p0 + (-a->pmap[p0] - 16*K);

    pool *poo = (pool*)b;
    #if OPNEW_DEBUG
      poo->deadbeef = 0xdeadbeef;
    #endif
    
    ASAN_UNPOISON(a->pbin, p0, p1);
    for(int p=p0; p < p1; p++)
      a->pbin[p] = bin;

    ASAN_UNPOISON(a->pmap, p0+1, p1);
    for(int p=p0+1; p < p1; p++)
      a->pmap[p] = -p0 - 1;
    
    uintptr_t obj_size = size_of_bin(bin);
    uintptr_t obj_align = obj_size & -obj_size;
    obj_align = std::min<uintptr_t>(64, obj_align);
    
    uintptr_t ou0 = reinterpret_cast<uintptr_t>(poo + 1);
    ou0 = (ou0 + obj_align-1) & -obj_align;

    uintptr_t ou = ou0;
    uintptr_t ou1 = reinterpret_cast<uintptr_t>(a+1) + p1*page_size;

    frobj *head = nullptr, *tail = nullptr;
    int popn = 0;

    // first element
    head = reinterpret_cast<frobj*>(ou);
    head->set_links(nullptr, nullptr);
    tail = head;
    ou += obj_size;
    popn += 1;

    // remaining elements
    frobj *tail_prev = nullptr;
    while(ou0 < ou + obj_size && ou + obj_size <= ou1) {
      if((ou & (huge_align-1)) != 0) {
        frobj *o = reinterpret_cast<frobj*>(ou);
        tail->set_links(tail_prev, o);
        tail_prev = tail;
        tail = o;
        ou += obj_size;
        popn += 1;
      }
      else
        ou += obj_align;
    }
    tail->set_links(tail_prev, nullptr);
    
    poo->popn = popn;
    poo->popn_not_held = popn;
    poo->hold_head = nullptr;
    poo->hold_tail = nullptr;
    
    *out_head = tail; // reversed for temporal locality
    *out_tail = head;
    return popn;
  }
  
  void arena_hold_pooled(arena *a, frobj *o, intru_heap<pool> &held_pools) {
    int p = ((char*)o - (char*)(a+1))/page_size;
    OPNEW_ASSERT(a->pmap_is_blob(p));
    p = a->pmap_blob_any_to_head(p);
    
    pool *poo = (pool*)((char*)(a+1) + p*page_size);
    OPNEW_ASSERT(poo->deadbeef == 0xdeadbeef);
    poo->popn_not_held -= 1;
    OPNEW_ASSERT(poo->popn_not_held >= 0);
    
    if(poo->popn_not_held != 0) {
      o->set_links(nullptr, poo->hold_head);
      
      if(poo->hold_head == nullptr) {
        poo->hold_tail = o;
        held_pools.insert(&pool::heap_link, poo);
      }
      else
        poo->hold_head->change_link(nullptr, o);
      
      poo->hold_head = o;
    }
    else {
      held_pools.remove(&pool::heap_link, poo);
      #if OPNEW_DEBUG
        poo->deadbeef = 666;
      #endif
      arena_dealloc_blob(a, poo);
    }
  }

  void arena_dealloc_remote(arena *a, frobj *o) {
    int t = a->owner_id;
    auto rbin = &remote_bins[t];
    
    int bin = bin_of(a, o);

    constexpr int B = 8*sizeof(uintptr_t);
    remote_thread_mask[t/B] |= uintptr_t(1)<<(t%B);
    
    if(bin != -1) {
      for(int i=0; i < rbin->bin_n; i++) {
        if(rbin->bin[i] == bin && rbin->popn_minus_one[i] != 255) {
          o->set_links(nullptr, rbin->head[i]);
          rbin->head[i]->change_link(nullptr, o);
          rbin->head[i] = o;
          rbin->popn_minus_one[i] += 1;
          return;
        }
      }
      
      if(rbin->bin_n < remote_thread_bins::max_bin_n) {
        int i = rbin->bin_n++;
        rbin->bin[i] = bin;
        rbin->popn_minus_one[i] = 0;
        rbin->head[i] = o;
        rbin->tail[i] = o;
        o->set_links(nullptr, nullptr);
        return;
      }
    }
    
    o->set_links(nullptr, rbin->rest_head);
    rbin->rest_head = o;
  }
}

template<typename T>
void opnew::intru_heap<T>::insert(intru_heap_link<T> T::*link_of, T *a) {
  auto key_of = [](T *o) {
    return reinterpret_cast<uintptr_t>(o);
  };
    
  T **px = &this->top;
  intptr_t ix0 = this->n++;
  intptr_t ix = ix0;
  
  while(ix != 0) {
    T *x = *px;
    OPNEW_ASSERT(x != a);
    if(key_of(a) < key_of(x)) {
      a->*link_of = x->*link_of;
      *px = a;
      std::swap(a, x);
    }
    ix -= 1;
    px = &(x->*link_of).kid[ix & 1];
    ix >>= 1;
  }

  OPNEW_ASSERT(*px == nullptr);
  *px = a;
  (a->*link_of).ix = ix0;
  (a->*link_of).kid[0] = nullptr;
  (a->*link_of).kid[1] = nullptr;

  this->sane(link_of);
}

template<typename T>
T* opnew::intru_heap<T>::pop_top(intru_heap_link<T> T::*link_of) {
  auto key_of = [](T *o) {
    return reinterpret_cast<uintptr_t>(o);
  };

  T *ans = this->top;
  
  // pop last item off heap
  T **px = &this->top;
  intptr_t ix0 = --this->n;
  intptr_t ix = ix0;
  
  while(ix != 0) {
    ix -= 1;
    px = &((*px)->*link_of).kid[ix & 1];
    ix >>= 1;
  }
  
  T *last = *px;
  *px = nullptr;

  if(ans == last) {
    OPNEW_ASSERT((ans->*link_of).ix == ix0);
    this->sane(link_of);
    return ans;
  }
  
  // replace top with last
  this->top = last;
  last->*link_of = ans->*link_of;
  px = &this->top;
  T *x = last;
  
  { // sift down to leaves
    uintptr_t x_key = key_of(x);
    
    while(true) {
      T *kid[2] = {(x->*link_of).kid[0], (x->*link_of).kid[1]};
      int k;
      
      if(kid[1] && (key_of(kid[0]) < x_key || key_of(kid[1]) < x_key))
        k = key_of(kid[0]) < key_of(kid[1]) ? 0 : 1;
      else if(kid[0] && key_of(kid[0]) < x_key)
        k = 0;
      else
        break;
      
      *px = kid[k];
      std::swap(kid[k]->*link_of, x->*link_of);
      px = &(kid[k]->*link_of).kid[k];
      *px = x;
    }
  }

  this->sane(link_of);

  return ans;
}

template<typename T>
void opnew::intru_heap<T>::remove(intru_heap_link<T> T::*link_of, T *a) {
  auto key_of = [](void *o) {
    return reinterpret_cast<uintptr_t>(o);
  };
  
  // pop last item off heap
  T **px = &this->top;
  intptr_t ix0 = --this->n;
  intptr_t ix = ix0;
  
  while(ix != 0) {
    ix -= 1;
    px = &((*px)->*link_of).kid[ix & 1];
    ix >>= 1;
  }
  T *last = *px;
  *px = nullptr;
  
  if(a == last) {
    OPNEW_ASSERT((a->*link_of).ix == ix0);
    this->sane(link_of);
    return;
  }
  
  // find `a` in heap, builds stack of parent "frames"
  int f = 0; // frame index
  uint64_t f_kid = 0; // path bits
  T *f_par[40];
  
  ix = (a->*link_of).ix;
  px = &this->top;
  
  while(ix != 0) {
    ix -= 1;
    f_par[f] = *px;
    f_kid = f_kid<<1 | (ix & 1);
    px = &((*px)->*link_of).kid[ix & 1];
    f += 1;
    ix >>= 1;
  }

  OPNEW_ASSERT(*px == a);
  
  // replace `a` with last
  T *x = last;
  *px = x;
  x->*link_of = a->*link_of;
  
  if(f != 0 && key_of(x) < key_of(f_par[f-1])) {
    // sift towards top
    do {
      std::swap(f_par[f-1]->*link_of, x->*link_of);
      (x->*link_of).kid[f_kid & 1] = f_par[f-1];
      f_kid >>= 1;
      f -= 1;
      if(f == 0) break;
    } while(key_of(x) < key_of(f_par[f-1]));

    if(f == 0)
      this->top = x;
    else
      (f_par[f-1]->*link_of).kid[f_kid & 1] = x;
    
    this->sane(link_of);
  }
  else {
    uintptr_t x_key = key_of(x);
    
    // sift down to leaves
    while(true) {
      T *kid[2] = {(x->*link_of).kid[0], (x->*link_of).kid[1]};
      int k;
      
      if(kid[1] && (key_of(kid[0]) < x_key || key_of(kid[1]) < x_key))
        k = key_of(kid[0]) < key_of(kid[1]) ? 0 : 1;
      else if(kid[0] && key_of(kid[0]) < x_key)
        k = 0;
      else
        break;
      
      *px = kid[k];
      std::swap(kid[k]->*link_of, x->*link_of);
      px = &(kid[k]->*link_of).kid[k];
      *px = x;
    }

    this->sane(link_of);
  }
}

template<typename T>
void opnew::intru_heap<T>::sane(intru_heap_link<T> T::*link_of) {
  #if OPNEW_DEBUG
    auto key_of = [](void *o) {
      return reinterpret_cast<uintptr_t>(o);
    };
    
    for(intptr_t ix0=0; ix0 < this->n; ix0++) {
      intptr_t ix = ix0;
      T *x = top;
      while(ix != 0) {
        T **kid = (x->*link_of).kid;
        ASSERT(!kid[0] || key_of(x) < key_of(kid[0]));
        ASSERT(!kid[1] || key_of(x) < key_of(kid[1]));
        ASSERT(!kid[1] || kid[0]);
        ix -= 1;
        x = (x->*link_of).kid[ix & 1];
        ix >>= 1;
      }
      ASSERT((x->*link_of).ix == ix0);
    }
  #endif
}
#endif
