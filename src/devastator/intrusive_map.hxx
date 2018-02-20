#ifndef _b9691292_f5c6_4d37_9e44_127508d173dd
#define _b9691292_f5c6_4d37_9e44_127508d173dd

#include <algorithm>
#include <array>
#include <cstdint>
#include <functional>
#include <utility>

template<typename T, typename Key,
         T*&(&next_of)(T*),
         Key(&key_of)(T*),
         std::size_t(&hash_of)(Key const&)>
class intrusive_map {
  unsigned popn_;
  int hbit_;
  union {
    T *bkt0_;  // if hbit_ == 0
    T **bkts_; // if hbit_ != 0
  };

public:
  intrusive_map();
  ~intrusive_map();
  
  intrusive_map(const intrusive_map&) = delete;
  intrusive_map& operator=(const intrusive_map&) = delete;
  
  intrusive_map(intrusive_map&&);
  intrusive_map& operator=(intrusive_map&&);
  
  std::intptr_t size() const { return std::intptr_t(popn_); }
  
  void insert(T *o);
  T* remove(Key const&);
  T* remove(T*);
  
  template<typename Vtor>
  T* visit(Key const &key, Vtor &&visitor);
  
  template<typename F>
  void for_each(F &&f);
  
  template<typename F>
  void clear(F &&f);

private:
  static int bucket_of(int hbit, const Key &key) {
    std::size_t h = hash_of(key);
    h ^= h >> 8*sizeof(std::size_t)/2;
    h *= 0x9e3779b97f4a7c15u;
    return hbit == 0 ? 0 : h >> (8*sizeof(std::size_t) - hbit);
  }
  
  template<int delta=0>
  void popn_changed();
  void resize(int hbits1);
};


template<typename T, typename Key,
         T*&(&next_of)(T*),
         Key(&key_of)(T*),
         std::size_t(&hash_of)(Key const&)>
intrusive_map<T,Key,next_of,key_of,hash_of>::intrusive_map() {
  popn_ = 0;
  hbit_ = 0;
  bkt0_ = nullptr;
}

template<typename T, typename Key,
         T*&(&next_of)(T*),
         Key(&key_of)(T*),
         std::size_t(&hash_of)(Key const&)>
intrusive_map<T,Key,next_of,key_of,hash_of>::~intrusive_map() {
  if(hbit_ != 0)
    delete[] bkts_;
}

template<typename T, typename Key,
         T*&(&next_of)(T*),
         Key(&key_of)(T*),
         std::size_t(&hash_of)(Key const&)>
intrusive_map<T,Key,next_of,key_of,hash_of>::intrusive_map(intrusive_map &&that) {
  this->popn_ = that.popn_;
  this->hbit_ = that.hbit_;
  this->bkts_ = that.bkts_;
  that.popn_ = 0;
  that.hbit_ = 0;
  that.bkt0_ = nullptr;
}

template<typename T, typename Key,
         T*&(&next_of)(T*),
         Key(&key_of)(T*),
         std::size_t(&hash_of)(Key const&)>
intrusive_map<T,Key,next_of,key_of,hash_of>&
intrusive_map<T,Key,next_of,key_of,hash_of>::operator=(intrusive_map &&that) {
  std::swap(this->popn_, that.popn_);
  std::swap(this->hbit_, that.hbit_);
  std::swap(this->bkts_, that.bkts_);
  return *this;
}

template<typename T, typename Key,
         T*&(&next_of)(T*),
         Key(&key_of)(T*),
         std::size_t(&hash_of)(Key const&)>
void intrusive_map<T,Key,next_of,key_of,hash_of>::insert(T *o) {
  std::size_t h = hash_of(key_of(o));

  int b = bucket_of(hbit_, key_of(o));
  T **p = hbit_ == 0 ? &bkt0_ : &bkts_[b];
  
  next_of(o) = *p;
  *p = o;
  
  popn_ += 1;
  this->popn_changed<1>();
}

template<typename T, typename Key,
         T*&(&next_of)(T*),
         Key(&key_of)(T*),
         std::size_t(&hash_of)(Key const&)>
T* intrusive_map<T,Key,next_of,key_of,hash_of>::remove(Key const &key) {
  int b = bucket_of(hbit_, key);
  T **p = hbit_ == 0 ? &bkt0_ : &bkts_[b];
  
  while(*p) {
    if(key_of(*p) != key)
      p = &next_of(*p);
    else {
      T *o = *p;
      *p = next_of(o);
      popn_ -= 1;
      this->popn_changed<-1>();
      return o;
    }
  }

  return nullptr;
}

template<typename T, typename Key,
         T*&(&next_of)(T*),
         Key(&key_of)(T*),
         std::size_t(&hash_of)(Key const&)>
T* intrusive_map<T,Key,next_of,key_of,hash_of>::remove(T *x) {
  int b = bucket_of(hbit_, key_of(x));
  T **p = hbit_ == 0 ? &bkt0_ : &bkts_[b];
  
  while(*p) {
    if(*p != x)
      p = &next_of(*p);
    else {
      *p = next_of(x);
      popn_ -= 1;
      this->popn_changed<-1>();
      return x;
    }
  }

  return nullptr;
}

template<typename T, typename Key,
         T*&(&next_of)(T*),
         Key(&key_of)(T*),
         std::size_t(&hash_of)(Key const&)>
template<typename Vtor>
T* intrusive_map<T,Key,next_of,key_of,hash_of>::visit(
    Key const &key,
    Vtor &&visitor
  ) {
  int b = bucket_of(hbit_, key);
  T **p = hbit_ == 0 ? &bkt0_ : &bkts_[b];
  
  while(*p) {
    T *o = *p;
    if(key == key_of(o))
      break;
    else
      p = &next_of(o);
  }
  
  T *o0 = *p;
  T *next = o0 ? next_of(o0) : nullptr;
  T *o1 = visitor(o0); // not worrying about reentrance but should
  
  if(o0 == o1) // no change
    return o0;
  
  if(o0) {
    if(o1) { // replaced
      next_of(o1) = next;
      *p = o1;
    }
    else { // removed
      *p = next;
      popn_ -= 1;
      this->template popn_changed<-1>();
    }
    return o0;
  }
  else { // added
    *p = o1;
    next_of(o1) = nullptr;
    popn_ += 1;
    this->template popn_changed<1>();
    return o1;
  }
}

template<typename T, typename Key,
         T*&(&next_of)(T*),
         Key(&key_of)(T*),
         std::size_t(&hash_of)(Key const&)>
template<typename F>
void intrusive_map<T,Key,next_of,key_of,hash_of>::for_each(F &&f) {
  T **bkts = hbit_ == 0 ? &bkt0_ : bkts_;
  for(int b=0; b < 1<<hbit_; b++) {
    T *o = bkts[b];
    while(o) {
      T *o_next = next_of(o);
      f(o);
      o = o_next;
    }
  }
}

template<typename T, typename Key,
         T*&(&next_of)(T*),
         Key(&key_of)(T*),
         std::size_t(&hash_of)(Key const&)>
template<typename F>
void intrusive_map<T,Key,next_of,key_of,hash_of>::clear(F &&f) {
  this->for_each(std::forward<F>(f));
  
  if(hbit_ != 0)
    delete[] bkts_;
  
  popn_ = 0;
  hbit_ = 0;
  bkts_ = nullptr;
}

namespace intrusive_map_help {
  template<std::size_t ...hb>
  constexpr std::array<unsigned,31> calc_bump_sizes(std::index_sequence<hb...>) {
    return {{0u, 1u, unsigned((4 + hb)<<hb)..., ~0u}};
  }

  static constexpr auto bump_sizes = calc_bump_sizes(std::make_index_sequence<28>());
}

template<typename T, typename Key,
         T*&(&next_of)(T*),
         Key(&key_of)(T*),
         std::size_t(&hash_of)(Key const&)>
template<int delta>
void intrusive_map<T,Key,next_of,key_of,hash_of>::popn_changed() {
  if(delta == 1) {
    if(popn_ > intrusive_map_help::bump_sizes[hbit_ + 2])
      resize(hbit_ + 1);
  }
  else {
    if(popn_ < intrusive_map_help::bump_sizes[hbit_])
      resize(hbit_ - 1);
  }
}

template<typename T, typename Key,
         T*&(&next_of)(T*),
         Key(&key_of)(T*),
         std::size_t(&hash_of)(Key const&)>
void intrusive_map<T,Key,next_of,key_of,hash_of>::resize(int hbit_new) {
  int hbit_old = this->hbit_;
  
  T *bkt0_new = nullptr;
  T **bkts_new;
  
  if(hbit_new == 0)
    bkts_new = &bkt0_new;
  else
    bkts_new = new T*[1<<hbit_new]{/*nullptr...*/};
  
  for(int b0=0; b0 < 1<<hbit_old; b0++) {
    T *o = hbit_ == 0 ? bkt0_ : bkts_[b0];
    while(o) {
      T *o_next = next_of(o);
      int b1 = bucket_of(hbit_new, key_of(o));
      next_of(o) = bkts_new[b1];
      bkts_new[b1] = o;
      o = o_next;
    }
  }
  
  if(hbit_old != 0)
    delete[] bkts_;
  
  hbit_ = hbit_new;
  if(hbit_new == 0)
    bkt0_ = bkt0_new;
  else
    bkts_ = bkts_new;
}

#endif
