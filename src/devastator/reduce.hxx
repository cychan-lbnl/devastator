#ifndef _665ed957_fc08_4e29_97d3_ecb6b268efd7
#define _665ed957_fc08_4e29_97d3_ecb6b268efd7

#include "world.hxx"

namespace world {
  extern thread_local int rdxn_incoming;
  extern thread_local void *rdxn_acc;
  extern thread_local void *rdxn_ans;
  
  template<typename T>
  void reduce_down_(int to_ub, T ans) {
    int rank_me = world::rank_me();
    
    while(true) {
      int mid = rank_me + (to_ub - rank_me)/2;
      if(mid == rank_me) break;

      world::send(mid, [=]() {
        reduce_down_(to_ub, ans);
      });
      
      to_ub = mid;
    }
    
    rdxn_ans = new T{ans};
  }
  
  template<typename T, typename Op>
  void reduce_up_(T val, Op op) {
    if(rdxn_incoming == 0) {
      while(true) {
        int kid = world::rank_me() | (1<<rdxn_incoming);
        if(kid == world::rank_me() || world::rank_n <= kid)
          break;
        rdxn_incoming += 1;
      }
      rdxn_incoming += 1; // add one for self
      rdxn_acc = new T{val};
    }
    else {
      op(*(T*)rdxn_acc, val);
      val = *(T*)rdxn_acc;
    }
    
    if(0 == --rdxn_incoming) {
      delete (T*)rdxn_acc;
      
      if(0 == world::rank_me()) {
        reduce_down_(world::rank_n, val);
      }
      else {
        int parent = world::rank_me();
        parent &= parent-1;
        world::send(parent, [=]() {
          reduce_up_(val, op);
        });
      }
    }
  }

  //////////////////////////////////////////////////////////////////////
  
  template<typename T, typename Op>
  T reduce(T val, Op op) {
    rdxn_ans = nullptr;
    reduce_up_(val, op);
    
    while(rdxn_ans == nullptr)
      world::progress();

    T ans = *(T*)rdxn_ans;
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
}
#endif
