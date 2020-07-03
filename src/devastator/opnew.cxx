#include <devastator/opnew.hxx>
#include <devastator/diagnostic.hxx>

#if DEVA_OPNEW_DEVA // contains whole file

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdlib>
#include <mutex>

#include <sys/mman.h>

namespace opnew = deva::opnew;
namespace threads = deva::threads;

using namespace std;
using namespace opnew;

void* operator new(std::size_t size) {
  return opnew::operator_new(size);
}

void operator delete(void *o) noexcept {
  opnew::operator_delete(o);
}

static_assert(sizeof(arena) % page_size != 0, "Crap, arena is page aligned");

namespace {
  arena* arena_create();
  void arena_destroy(arena *a);
  
  tuple<arena*,void*> arena_fit_and_alloc(int pn);
  void arena_hole_changed(arena *a, int p, int pn);
  void* arena_alloc_avail(arena *a, int pn);
  void arena_dealloc_blob(arena *a, void *o);

  void arena_hold_pooled(arena *a, frobj *o, intru_heap<pool> &held_pools);

  void arena_dealloc_remote(arena *a, frobj *o);
  
  int arena_pool_init(arena *a, void *b, int bin, frobj **out_head, frobj **out_tail);

  std::mutex outsider_lock;
  
  __thread arena *my_arenas = nullptr;
  thread_local arena::arena_holes my_holes;
  
  struct remote_thread_bins {
    static constexpr int max_bin_n = 5;
    int8_t bin_n; // = 0
    int8_t bin[max_bin_n];
    uint8_t popn_minus_one[max_bin_n];
    frobj *head[max_bin_n], *tail[max_bin_n];
    frobj *rest_head; // = nullptr
  };
  
  __thread uintptr_t remote_thread_mask[(threads::thread_n + 8*sizeof(uintptr_t)-1)/sizeof(uintptr_t)] = {/*0...*/};
  __thread remote_thread_bins remote_bins[threads::thread_n] {/*{}...*/};

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

__thread thread_state opnew::my_ts {/*0...*/};

void* opnew::operator_new_slow(size_t size) {
  int bin_id = bin_of_size(size);
  bin_state *bin = &my_ts.bins[bin_id];
  
  if(bin_id != -1) {
    // bin is empty!
    DEVA_OPNEW_ASSERT(bin->popn == 0);
    DEVA_OPNEW_ASSERT(bin->head() == &bin->tail);
    DEVA_OPNEW_ASSERT(bin->tail.next_xor_prev == 0);
    int pn = pool_best_pages[bin_id];
    
    if(pn != -1) {
      frobj *head, *tail;
      int popn;
      
      if(bin->held_pools.top != nullptr) {
        pool *poo = bin->held_pools.pop_top(&pool::heap_link);
        DEVA_OPNEW_ASSERT(poo->deadbeef == 0xdeadbeef);
        
        head = poo->hold_head;
        tail = poo->hold_tail;
        popn = poo->popn - poo->popn_not_held;
        poo->hold_head = nullptr;
        poo->hold_tail = nullptr;
        poo->popn_not_held = poo->popn;

        #if DEVA_OPNEW_DEBUG > 1
          frobj *x = head,*y=nullptr;
          for(int n=0; n < popn; n++) {
            frobj *z = x->next(y);
            y = x;
            x = z;
          }
          DEVA_OPNEW_ASSERT(x==nullptr && y==tail);
        #endif
      }
      else {
        arena *a; void *b;
        std::tie(a,b) = arena_fit_and_alloc(pn);
        popn = arena_pool_init(a, b, bin_id, &head, &tail);
      }
      
      tail->change_link(nullptr, &bin->tail);
      bin->tail.set_links(tail, nullptr);
      bin->head(head->next(nullptr));
      bin->head()->change_link(head, nullptr);
      bin->popn = popn - 1;
      bin->sane();
      return (void*)head;
    }
    else {
      pn = (size_of_bin(bin_id) + page_size-1)/page_size;
      return std::get<1>(arena_fit_and_alloc(pn));
    }
  }
  else if(size < huge_size) {
    int pn = (size + page_size-1)/page_size;
    return std::get<1>(arena_fit_and_alloc(pn));
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
    DEVA_OPNEW_ASSERT(a->pmap_is_blob(((char*)obj - (char*)(a+1))/page_size));
    
    if(a->owner_ts == &my_ts)
      arena_dealloc_blob(a, obj);
    else
      arena_dealloc_remote(a, new(obj) frobj);
  }
  else
    std::free(obj);
}

void opnew::gc_bins() {
  for(int bin_id = 0; bin_id < bin_n; bin_id++) {
    bin_state *bin = &my_ts.bins[bin_id];
    bin->sane();
    int best_pn = pool_best_pages[bin_id];
    
    if(bin->popn_least != 0) {
      uintptr_t n = (3*bin->popn_least)/4;
      DEVA_OPNEW_ASSERT(n <= bin->popn);
      
      frobj *o = bin->tail.next(nullptr); // tail->prev
      frobj *oprev = &bin->tail;
      bin->popn -= n;
      
      while(n--) {
        frobj *onext = o->next(oprev);
        arena *a = arena_of_nonhuge(o);

        if(a->owner_ts != &my_ts)
          arena_dealloc_remote(a, o);
        else if(best_pn != -1)
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
        bin->head(&bin->tail);
    }
    
    bin->popn_least = bin->popn;
    bin->sane();
  }

  flush_remote();
}

void opnew::flush_remote() {
  constexpr int B = 8*sizeof(uintptr_t);

  if(my_ts.outsider_frees != nullptr) {
    frobj *o; {
      std::lock_guard<std::mutex> locked(outsider_lock);
      o = my_ts.outsider_frees;
      my_ts.outsider_frees = nullptr;
    }
    
    while(o != nullptr) {
      frobj *o1 = o->next(nullptr);
      operator_delete</*known_size=*/0, /*known_local=*/true>((void*)o);
      o = o1;
    }
  }
  
  for(int mi=0; mi < (threads::thread_n + B-1)/B; mi++) {
    uintptr_t m = remote_thread_mask[mi];
    remote_thread_mask[mi] = 0;
    
    while(m != 0) {
      int t = mi*B - 1 + bitffs(m);
      m &= m-1;

      remote_thread_bins rbins = remote_bins[t];
      remote_bins[t] = {};
      
      threads::send(t, [=]() {
        for(int i=0; i < rbins.bin_n; i++) {
          bin_state *bin = &my_ts.bins[rbins.bin[i]];
          rbins.tail[i]->change_link(nullptr, bin->head());
          bin->head()->change_link(nullptr, rbins.tail[i]);
          bin->head(rbins.head[i]);
          bin->popn += int(rbins.popn_minus_one[i]) + 1;
          bin->sane();
        }
        
        frobj *o = rbins.rest_head;
        while(o != nullptr) {
          frobj *o1 = o->next(nullptr);
          opnew::my_ts.opcalls -= 1;
          opnew::operator_delete</*known_size=*/0, /*known_local*/true>(o);
          o = o1;
        }
      });
    }
  }
}

void opnew::thread_me_initialized() {
  for(arena *a = my_arenas; a != nullptr; a = a->owner_next)
    a->owner_id = threads::thread_me();
}

namespace {
  mutex mm_lock;
  arena* mm_block = nullptr;
  int mm_id_bump = threads::thread_n;
  
  arena* arena_create() {
    arena *a;
    {
      lock_guard<mutex> mm_locked{mm_lock};
      int id = mm_id_bump++;
      
      if(id == threads::thread_n) {
        uintptr_t block_size = threads::thread_n*uintptr_t(arena_size);
        uintptr_t request_size = block_size + arena_size;
        
        void *m = mmap(nullptr, request_size, PROT_READ|PROT_WRITE, MAP_PRIVATE|MAP_ANONYMOUS, -1, 0);
        
        uintptr_t u0 = reinterpret_cast<uintptr_t>(m);
        uintptr_t u1 = (u0 + arena_size-1) & -arena_size;
        
        if(u1 != u0)
          munmap(m, u1-u0);
        
        if(u1 + block_size != u0 + request_size)
          munmap(reinterpret_cast<void*>(u1 + block_size), u0 + arena_size - u1);
        
        mm_block = reinterpret_cast<arena*>(u1);
        mm_id_bump = 1;
        id = 0;
      }
      
      a = new((char*)mm_block + id*uintptr_t(arena_size)) arena;
    }
    
    a->owner_ts = &my_ts;
    a->owner_id = threads::thread_me();
    a->owner_next = my_arenas;
    my_arenas = a;
    
    a->pmap[0] = page_per_arena;
    a->pmap[page_per_arena-1] = page_per_arena;
    
    for(int t=0; t < sizeof(arena::holes)/sizeof(arena::holes[0]); t++)
      a->holes[t] = 0;

    arena_hole_changed(a, 0, page_per_arena);
    
    my_holes.insert(a);
    
    return a;
  }

  /*void arena_destroy(arena *a) {
    munmap((void*)a, arena_size);
  }*/

  tuple<arena*,void*> arena_fit_and_alloc(int pn) {
    if(uint16_t(pn) <= my_holes.size_max()) {
      return my_holes.fit_and_decrease(pn,
        [=](arena *a) {
          return std::make_tuple(a, arena_alloc_avail(a, pn));
        }
      );
    }
    else {
      arena *a = arena_create();
      return std::make_tuple(a, arena_alloc_avail(a, pn));
    }
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
    DEVA_OPNEW_ASSERT(hp < page_per_arena);

    int hpn = a->pmap_hole_length(hp);
    
    a->pmap[hp + pn-1] = -hp - 1;
    a->pmap[hp] = -pn - 16*K;

    a->pbin[hp] = -1;
    
    arena_hole_changed(a, hp, 0);
    
    if(hpn != pn) {
      a->pmap[hp + pn] = hpn-pn;
      a->pmap[hp + hpn-1] = hpn-pn;
      arena_hole_changed(a, hp+pn, hpn-pn);
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
    
    if(rhole)
      arena_hole_changed(a, rp, 0);
    
    arena_hole_changed(a, lp, lpn + pn + rpn);
    
    my_holes.increased(a);
  }
  
  int arena_pool_init(arena *a, void *b, int bin, frobj **out_head, frobj **out_tail) {
    int p0 = ((char*)b - (char*)(a+1))/page_size;
    int p1 = p0 + (-a->pmap[p0] - 16*K);

    pool *poo = new(b) pool;
    #if DEVA_OPNEW_DEBUG
      poo->deadbeef = 0xdeadbeef;
    #endif
    
    for(int p=p0; p < p1; p++)
      a->pbin[p] = bin;

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
    head = new(reinterpret_cast<void*>(ou)) frobj;
    head->set_links(nullptr, nullptr);
    tail = head;
    ou += obj_size;
    popn += 1;

    // remaining elements
    frobj *tail_prev = nullptr;
    while(ou0 < ou + obj_size && ou + obj_size <= ou1) {
      if((ou & (huge_align-1)) != 0) {
        frobj *o = new(reinterpret_cast<void*>(ou)) frobj;
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
    DEVA_OPNEW_ASSERT(a->pmap_is_blob(p));
    p = a->pmap_blob_any_to_head(p);
    
    pool *poo = (pool*)((char*)(a+1) + p*page_size);
    DEVA_OPNEW_ASSERT(poo->deadbeef == 0xdeadbeef);
    poo->popn_not_held -= 1;
    DEVA_OPNEW_ASSERT(poo->popn_not_held >= 0);
    
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
      #if DEVA_OPNEW_DEBUG
        poo->deadbeef = 666;
      #endif
      arena_dealloc_blob(a, poo);
    }
  }

  void arena_dealloc_remote(arena *a, frobj *o) {
    int t = a->owner_id;

    if(t >= 0) {
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
    else {
      thread_state *ts = a->owner_ts;
      std::lock_guard<std::mutex> locked{outsider_lock};
      o->set_links(nullptr, ts->outsider_frees);
      ts->outsider_frees = o;
    }
  }
}

////////////////////////////////////////////////////////////////////////////////
// opnew::intru_heap

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
    DEVA_OPNEW_ASSERT(x != a);
    if(key_of(a) < key_of(x)) {
      a->*link_of = x->*link_of;
      *px = a;
      std::swap(a, x);
    }
    ix -= 1;
    px = &(x->*link_of).kid[ix & 1];
    ix >>= 1;
  }

  DEVA_OPNEW_ASSERT(*px == nullptr);
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

  DEVA_OPNEW_ASSERT(key_of(this->top) > 100);
  DEVA_OPNEW_ASSERT(this->n != 0);
  
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
    DEVA_OPNEW_ASSERT((ans->*link_of).ix == ix0);
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
    DEVA_OPNEW_ASSERT((a->*link_of).ix == ix0);
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

  DEVA_OPNEW_ASSERT(*px == a);
  
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

#if 0
template<typename T>
void opnew::intru_heap<T>::sane(intru_heap_link<T> T::*link_of) {
  auto key_of = [](void *o) {
    return reinterpret_cast<uintptr_t>(o);
  };
  
  #if DEVA_OPNEW_DEBUG > 1
    for(intptr_t ix0=0; ix0 < this->n; ix0++) {
      intptr_t ix = ix0;
      T *x = top;
      while(ix != 0) {
        T **kid = (x->*link_of).kid;
        DEVA_OPNEW_ASSERT(!kid[0] || key_of(x) < key_of(kid[0]));
        DEVA_OPNEW_ASSERT(!kid[1] || key_of(x) < key_of(kid[1]));
        DEVA_OPNEW_ASSERT(!kid[1] || kid[0]);
        ix -= 1;
        x = (x->*link_of).kid[ix & 1];
        ix >>= 1;
      }
      DEVA_OPNEW_ASSERT((x->*link_of).ix == ix0);
    }
  #else
    DEVA_OPNEW_ASSERT((key_of(this->top) & 1) == 0);
  #endif
}
#endif

////////////////////////////////////////////////////////////////////////////////
// opnew::arena_holes

template<typename Arena, typename Size>
void opnew::arena_holes<Arena,Size>::insert(Arena *a) {
  int ix = popn_++;
  a->holes_link.ix = ix;
  
  Arena **pkid = &root_;
  Size *pmax = &root_max_;
  Size a_max = a->hole_size_max();
  
  while(true) {
    if(ix == 0) {
      DEVA_OPNEW_ASSERT(*pkid == nullptr);
      DEVA_OPNEW_ASSERT(*pmax == 0);
      *pkid = a;
      *pmax = a_max;
      a->holes_link.kid[0] = nullptr;
      a->holes_link.kid_max[0] = 0;
      a->holes_link.kid[1] = nullptr;
      a->holes_link.kid_max[1] = 0;
      break;
    }
    else if((ix & (ix+1)) == 0) {
      DEVA_OPNEW_ASSERT(a != *pkid);
      a->holes_link.kid[0] = *pkid;
      a->holes_link.kid_max[0] = *pmax;
      a->holes_link.kid[1] = nullptr;
      a->holes_link.kid_max[1] = 0;
      *pkid = a;
      *pmax = std::max(*pmax, a_max);
      break;
    }
    else {
      *pmax = std::max(*pmax, a_max);
      pmax = &(*pkid)->holes_link.kid_max[1];
      pkid = &(*pkid)->holes_link.kid[1];
      ix ^= 1 << log2dn(ix);
    }
  }
}

template<typename Arena, typename Size>
template<typename Fn>
auto opnew::arena_holes<Arena,Size>::fit_and_decrease(Size size, Fn &&fn)
  -> decltype(fn(std::declval<Arena*>())) {
  
  DEVA_OPNEW_ASSERT(size <= root_max_);
  
  Arena *a = root_;
  Arena *a_up = nullptr;
  std::uint64_t kpath = 0;
  
  while(true) {
    int k;
    if(size <= a->holes_link.kid_max[0])
      k = 0;
    else if(size <= a->hole_size_max())
      break;
    else
      k = 1;
    
    Arena *kid = a->holes_link.kid[k];
    DEVA_OPNEW_ASSERT(kid != nullptr);
    a->holes_link.kid[k] = a_up;
    a_up = a;
    a = kid;
    kpath = (kpath<<1) | k;
  }
  
  auto ans = fn(a);
  repair(a, a_up, kpath);
  return ans;
}

template<typename Arena, typename Size>
void opnew::arena_holes<Arena,Size>::increased(Arena *a) {
  Size a_max = a->hole_size_max();
  int a_ix = a->holes_link.ix;
  
  Arena *p = root_;
  root_max_ = std::max(root_max_, a_max);
  
  while(a_ix != p->holes_link.ix) {
    int k = a_ix < p->holes_link.ix ? 0 : 1;
    p->holes_link.kid_max[k] = std::max(p->holes_link.kid_max[k], a_max);
    p = p->holes_link.kid[k];
  }
  
  DEVA_OPNEW_ASSERT(p == a);
}

template<typename Arena, typename Size>
void opnew::arena_holes<Arena,Size>::repair(Arena *p, Arena *p_up, std::uint64_t kpath) {
  Size max1;
  
  while(true) {
    max1 = std::max(
      p->hole_size_max(),
      std::max(p->holes_link.kid_max[0], p->holes_link.kid_max[1])
    );
    
    if(p_up == nullptr)
      break;
    
    Arena *p_up_up = p_up->holes_link.kid[kpath & 1];
    p_up->holes_link.kid_max[kpath & 1] = max1;
    p_up->holes_link.kid[kpath & 1] = p;
    p = p_up;
    p_up = p_up_up;
    kpath >>= 1;
  }
  
  root_max_ = max1;
}

#endif
