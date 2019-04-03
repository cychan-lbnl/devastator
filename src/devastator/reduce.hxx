#ifndef _665ed957_fc08_4e29_97d3_ecb6b268efd7
#define _665ed957_fc08_4e29_97d3_ecb6b268efd7

#include <devastator/world.hxx>

namespace deva {
  extern thread_local int rdxn_incoming;
  extern thread_local void *rdxn_acc;
  extern thread_local void *rdxn_ans;
  
  template<typename T>
  void reduce_down_(int to_ub, T ans) {
    int rank_me = deva::rank_me();
    
    while(true) {
      int mid = rank_me + (to_ub - rank_me)/2;
      if(mid == rank_me) break;

      deva::send(mid, [=](T &ans) {
          reduce_down_(to_ub, std::move(ans));
        },
        ans
      );
      
      to_ub = mid;
    }
    
    rdxn_ans = new T(std::move(ans));
  }
  
  template<typename T, typename Op>
  void reduce_up_(T val, Op op) {
    if(rdxn_incoming == 0) {
      while(true) {
        int kid = deva::rank_me() | (1<<rdxn_incoming);
        if(kid == deva::rank_me() || deva::rank_n <= kid)
          break;
        rdxn_incoming += 1;
      }
      rdxn_incoming += 1; // add one for self
      rdxn_acc = new T(std::move(val));
    }
    else
      op(*(T*)rdxn_acc, val);
    
    if(0 == --rdxn_incoming) {
      val = std::move(*(T*)rdxn_acc);
      delete (T*)rdxn_acc;
      
      if(0 == deva::rank_me()) {
        reduce_down_(deva::rank_n, std::move(val));
      }
      else {
        int parent = deva::rank_me();
        parent &= parent-1;
        deva::send(parent, [=](T &val) {
            reduce_up_(std::move(val), op);
          },
          std::move(val)
        );
      }
    }
  }

  //////////////////////////////////////////////////////////////////////
  
  template<typename T, typename Op>
  T reduce(T val, Op op) {
    rdxn_ans = nullptr;
    reduce_up_(val, op);
    
    while(rdxn_ans == nullptr)
      deva::progress();

    T ans = std::move(*(T*)rdxn_ans);
    delete (T*)rdxn_ans;
    return ans;
  }

  template<typename T>
  T reduce_sum(T val) {
    return reduce(val, [](T &acc, T x) { acc += x; });
  }
  
  template<typename T>
  T reduce_xor(T val) {
    return reduce(val, [](T &acc, T x) { acc ^= x; });
  }

  template<typename T>
  T reduce_min(T val) {
    return reduce(val, [](T &acc, T x) { acc = std::min(acc, x); });
  }

  template<typename T>
  T reduce_max(T val) {
    return reduce(val, [](T &acc, T x) { acc = std::max(acc, x); });
  }
}
#endif
