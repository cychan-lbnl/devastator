#ifndef _6b7f08b314dd408d8be166b187df8b89
#define _6b7f08b314dd408d8be166b187df8b89

namespace deva {
#if 0
  #include <deque>
  template<typename T>
  class queue {
    std::deque<T> d;
  public:
    int size() { return (int)d.size(); }
    T at_forwards(int i) const {
      return d[i];
    }
    T at_backwards(int i) const {
      return d[d.size()-1-i];
    }

    T front_or(T otherwise) const {
      return d.empty() ? otherwise : d.front();
    }
    T back_or(T otherwise) const {
      return d.empty() ? otherwise : d.back();
    }

    void reserve_more() {}

    void push_back(T x) {
      d.push_back(x);
    }
    void push_back_reserved(T x) {
      d.push_back(x);
    }
    T pop_back() {
      T ans = d.back();
      d.pop_back();
      return ans;
    }

    void push_front(T x) {
      d.push_front(x);
    }
    void push_front_reserved(T x) {
      d.push_front(x);
    }
    T pop_front() {
      T ans = d.front();
      d.pop_front();
      return ans;
    }

    void chop_back(int n) {
      while(n--)
        d.pop_back();
    }
    void chop_front(int n) {
      while(n--)
        d.pop_front();
    }
  };

#else

  template<typename T>
  class queue {
    int n_ = 0;
    int cap_ = 0;
    int beg_ = 0;
    T *buf_ = nullptr;
    
  public:
    queue() = default;
    queue(queue const&) = delete;
    queue(queue &&that) {
      this->n_ = that.n_;
      this->cap_ = that.cap_;
      this->beg_ = that.beg_;
      this->buf_ = that.buf_;
      that.n_ = 0;
      that.cap_ = 0;
      that.beg_ = 0;
      that.buf_ = nullptr;
    }
    ~queue() {
      if(buf_)
        delete[] buf_;
    }

  private:
    void resize(int cap1);

  public:
    int size() const { return n_; }

    T at_forwards(int i) const {
      return buf_[(beg_ + i) & (cap_-1)];
    }
    T at_backwards(int i) const {
      return buf_[(beg_ + n_-1 - i) & (cap_-1)];
    }

    T front_or(T otherwise) const {
      return n_ == 0 ? otherwise : buf_[beg_ & (cap_-1)];
    }
    T back_or(T otherwise) const {
      return n_ == 0 ? otherwise : buf_[(beg_ + n_-1) & (cap_-1)];
    }
    
    void reserve_more(int n) {
      int cap = cap_;
      while(n_ + n > cap)
        cap = cap == 0 ? 1 : cap<<1;
      if(cap != cap_)
        resize(cap);
    }
    
    void push_back(T x) {
      if(n_ == cap_)
        resize(cap_ == 0 ? 1 : cap_<<1);
      buf_[(beg_ + n_++) & (cap_-1)] = x;
    }

    void push_back_reserved(T x) {
      buf_[(beg_ + n_++) & (cap_-1)] = x;
    }

    T pop_back() {
      T ans = buf_[(beg_ + --n_) & (cap_-1)];
      if(n_ <= cap_>>3 && cap_ >= 16)
        resize(cap_>>1);
      return ans;
    }

    void chop_back(int n) {
      n_ -= n;
      int cap = cap_;
      while(n_ <= cap>>3 && cap >= 16)
        cap >>= 1;
      if(cap != cap_)
        resize(cap);
    }

    void push_front(T x) {
      if(n_ == cap_)
        resize(cap_ == 0 ? 1 : cap_<<1);
      beg_ = (beg_-1) & (cap_-1);
      buf_[beg_] = x;
      n_ += 1;
    }
    
    void push_front_reserved(T x) {
      beg_ = (beg_-1) & (cap_-1);
      buf_[beg_] = x;
      n_ += 1;
    }

    T pop_front() {
      T ans = buf_[beg_];
      beg_ = (beg_+1) & (cap_-1);
      n_ -= 1;
      if(n_ <= cap_>>3 && cap_ >= 16)
        resize(cap_>>1);
      return ans;
    }

    void chop_front(int n) {
      beg_ = (beg_ + n) & (cap_-1);
      n_ -= n;
      int cap = cap_;
      while(n_ <= cap>>3 && cap >= 16)
        cap >>= 1;
      if(cap != cap_)
        resize(cap);
    }
  };

  template<typename T>
  void queue<T>::resize(int cap1) {
    T *buf1 = cap1 ? new T[cap1] : nullptr;
    for(int i=0; i < n_; i++)
      buf1[i] = buf_[(i + beg_) & (cap_-1)];
    if(buf_)
      delete[] buf_;
    buf_ = buf1;
    cap_ = cap1;
    beg_ = 0;
  }
#endif
} // namespace deva

#endif
