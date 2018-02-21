#ifndef _e4fcf9d5_a849_498d_9f13_f98b13cdd923
#define _e4fcf9d5_a849_498d_9f13_f98b13cdd923

#include <algorithm>
#include <cstdint>

template<typename T, typename Key, int&(&ix_of)(T), Key(&key_of)(T)>
class intrusive_min_heap {
  int n_ = 0, cap_ = 0;
  T *buf_ = nullptr;
  
public:
  intrusive_min_heap() = default;
  intrusive_min_heap(intrusive_min_heap const&) = delete;
  intrusive_min_heap(intrusive_min_heap &&that) {
    this->n_ = that.n_;
    this->cap_ = that.cap_;
    this->buf_ = that.buf_;
    that.n_ = 0;
    that.cap_ = 0;
    that.buf_ = nullptr;
  }
  ~intrusive_min_heap() {
    if(buf_) delete[] buf_;
  }
  
private:
  void resize(int cap1);

  void push_back(T x) {
    if(n_ + 1 > cap_)
      resize(cap_ == 0 ? 1 : 2*cap_);
    ix_of(x) = n_;
    buf_[n_++] = x;
  }
  
  T pop_back() {
    T ans = buf_[--n_];
    if(n_ <= cap_/8 && cap_ >= 16)
      resize(cap_/2);
    return ans;
  }

  bool decreased(int ix, T x, Key key);
  void increased(int ix, T x, Key key);

public:
  int size() const { return this->n_; }

  Key least_key() const {
    return key_of(this->buf_[0]);
  }
  Key least_key_or(Key otherwise) const {
    return this->n_ == 0 ? otherwise : key_of(this->buf_[0]);
  }

  T peek_least() const {
    return this->buf_[0];
  }
  T peek_least_or(T otherwise) const {
    return this->n_ == 0 ? otherwise : this->buf_[0];
  }
  
  void insert(T x) {
    this->push_back(x);
    this->decreased(this->n_-1, x, key_of(x));
  }
  
  T pop_least() {
    T ans = this->buf_[0];
    ix_of(ans) = -1;
    T tmp = this->pop_back();
    this->buf_[0] = tmp;
    ix_of(tmp) = 0;
    this->increased(0, tmp, key_of(tmp));
    return ans;
  }

  void erase(T x) {
    T tmp1 = this->pop_back();
    
    if(&ix_of(x) != &ix_of(tmp1)) {
      int ix = ix_of(x);
      T tmp0 = this->buf_[ix];
      this->buf_[ix] = tmp1;
      ix_of(tmp1) = ix;
      
      if(key_of(tmp1) <= key_of(tmp0))
        this->decreased(ix, tmp1, key_of(tmp1));
      else
        this->increased(ix, tmp1, key_of(tmp1));
    }

    ix_of(x) = -1;
  }

  void increased(T x) {
    int ix = ix_of(x);
    this->buf_[ix] = x;
    this->increased(ix, x, key_of(x));
  }
  void increased(T x, Key key) {
    int ix = ix_of(x);
    this->buf_[ix] = x;
    this->increased(ix, x, key);
  }
  
  void decreased(T x) {
    int ix = ix_of(x);
    this->buf_[ix] = x;
    this->decreased(ix, x, key_of(x));
  }
  void decreased(T x, Key key) {
    int ix = ix_of(x);
    this->buf_[ix] = x;
    this->decreased(ix, x, key);
  }
  
  void changed(T x) {
    int ix = ix_of(x);
    this->buf_[ix] = x;
    Key key = key_of(x);
    if(!this->decreased(ix, x, key))
      this->increased(ix, x, key);
  }

  void clear() {
    n_ = 0;
    cap_ = 0;
    if(buf_) {
      delete[] buf_;
      buf_ = nullptr;
    }
  }
};

template<typename T, typename Key, int&(&ix_of)(T), Key(&key_of)(T)>
void intrusive_min_heap<T,Key,ix_of,key_of>::resize(int cap1) {
  T *buf1 = cap1 ? new T[cap1] : nullptr;
  for(int i=0; i < n_; i++)
    buf1[i] = buf_[i];
  if(buf_)
    delete[] buf_;
  buf_ = buf1;
  cap_ = cap1;
}

template<typename T, typename Key, int&(&ix_of)(T), Key(&key_of)(T)>
void intrusive_min_heap<T,Key,ix_of,key_of>::increased(int ix, T x, Key key) {
  // sift toward leaves
  T *buf = this->buf_;
  int n = this->n_;
  
  while(true) {
    int k0 = 2*ix + 1;
    int k1 = 2*ix + 2;
    int ix1;
    
    if(k1 < n && std::min(key_of(buf[k0]), key_of(buf[k1])) < key)
      ix1 = key_of(buf[k0]) < key_of(buf[k1]) ? k0 : k1;
    else if(k0 < n && key_of(buf[k0]) < key)
      ix1 = k0;
    else
      break;
    
    buf[ix] = buf[ix1];
    ix_of(buf[ix]) = ix;
    ix = ix1;
  }
  
  buf[ix] = x;
  ix_of(x) = ix;
}

template<typename T, typename Key, int&(&ix_of)(T), Key(&key_of)(T)>
bool intrusive_min_heap<T,Key,ix_of,key_of>::decreased(int ix, T x, Key key) {
  // sift toward root
  T *buf = this->buf_;
  bool changed = false;
  
  while(ix != 0) {
    int p = (ix-1)/2;
    
    if(key < key_of(buf[p])) {
      changed = true;
      buf[ix] = buf[p];
      ix_of(buf[ix]) = ix;
      ix = p;
    }
    else
      break;
  }
  
  buf[ix] = x;
  ix_of(buf[ix]) = ix;
  return changed;
}

#endif
