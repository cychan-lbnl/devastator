#ifndef _665ed957_fc08_4e29_97d3_ecb6b268efd7
#define _665ed957_fc08_4e29_97d3_ecb6b268efd7

#include <devastator/world.hxx>

namespace deva {
  namespace detail {
    extern __thread int reduce_incoming;
    extern __thread void *reduce_acc;
    extern __thread void *reduce_ans;
    
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
      
      reduce_ans = new T(std::move(ans));
    }
    
    template<typename T, typename Op>
    void reduce_up_(T val, Op op) {
      if(reduce_incoming == 0) {
        while(true) {
          int kid = deva::rank_me() | (1<<reduce_incoming);
          if(kid == deva::rank_me() || deva::rank_n <= kid)
            break;
          reduce_incoming += 1;
        }
        reduce_incoming += 1; // add one for self
        reduce_acc = new T(std::move(val));
      }
      else
        op(*(T*)reduce_acc, std::move(val));
      
      if(0 == --reduce_incoming) {
        val = std::move(*(T*)reduce_acc);
        delete (T*)reduce_acc;
        
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
  }

  //////////////////////////////////////////////////////////////////////////////
  
  template<typename T, typename Op>
  T reduce(T val, Op op) {
    detail::reduce_ans = nullptr;
    detail::reduce_up_(std::move(val), op);
    
    while(detail::reduce_ans == nullptr)
      deva::progress(/*spinning=*/true);

    T ans = std::move(*(T*)detail::reduce_ans);
    delete (T*)detail::reduce_ans;
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

  //////////////////////////////////////////////////////////////////////////////

  namespace detail {
    extern __thread int scan_reduce_received;
    extern __thread void *scan_reduce_accs;
    extern __thread void *scan_reduce_ans;
    
    template<typename T, typename Op>
    void scan_reduce_down(T prefix, T total, Op op) {
      T prefix_old = prefix;
      T *accs = (T*)scan_reduce_accs;
      scan_reduce_accs = nullptr;
      
      int rank_me = deva::rank_me();
      
      for(int i=0; ; i++) {
        int kid = rank_me | (1<<i);
        if(kid == rank_me || deva::rank_n <= kid)
          break;

        if(i == 0 && rank_me == 0)
          prefix = std::move(accs[i]);
        else
          op(prefix, std::move(accs[i]));
        accs[i].~T();
        
        deva::send(kid,
          [=](T &pre, T &tot) {
            scan_reduce_down(std::move(pre), std::move(tot), op);
          },
          prefix, total
        );
      }

      scan_reduce_ans = new std::pair<T,T>(std::move(prefix_old), std::move(total));
      ::operator delete((void*)accs);
    }
    
    template<typename T, typename Op>
    void scan_reduce_up(int sender, T val, Op op) {
      int incoming = 0;
      int slot = 0;
      while(true) {
        int kid = deva::rank_me() | (1<<incoming);
        if(kid == deva::rank_me() || deva::rank_n <= kid)
          break;
        if(kid == sender)
          slot = incoming + 1;
        incoming += 1;
      }
      incoming += 1; // add one for self

      if(scan_reduce_accs == nullptr)
        scan_reduce_accs = ::operator new(incoming*sizeof(T));

      ::new((T*)scan_reduce_accs + slot) T(std::move(val));
      
      if(++scan_reduce_received == incoming) {
        scan_reduce_received = 0;
        
        T total = *(T*)scan_reduce_accs;
        for(int i=1; i < incoming; i++)
          op(total, ((T*)scan_reduce_accs)[i]);
        
        if(0 == deva::rank_me()) {
          scan_reduce_down(T(), std::move(total), op);
        }
        else {
          sender = deva::rank_me();
          int parent = sender & (sender-1);
          
          deva::send(parent,
            [=](T &total) {
              scan_reduce_up(sender, std::move(total), op);
            },
            std::move(total)
          );
        }
      }
    }
  }
  
  //////////////////////////////////////////////////////////////////////////////

  template<typename T, typename Op>
  std::pair<T/*prefix*/, T/*total*/> scan_reduce(T val, T zero, Op op) {
    detail::scan_reduce_ans = nullptr;
    detail::scan_reduce_up(deva::rank_me(), std::move(val), op);
    
    while(detail::scan_reduce_ans == nullptr)
      deva::progress(/*spinning=*/true);

    std::pair<T,T> ans = std::move(*(std::pair<T,T>*)detail::scan_reduce_ans);
    delete (std::pair<T,T>*)detail::scan_reduce_ans;
    
    return std::pair<T,T>(
      std::move(deva::rank_me() == 0 ? zero : ans.first),
      std::move(ans.second)
    );
  }

  template<typename T>
  std::pair<T/*prefix*/, T/*total*/> scan_reduce_sum(T val, T zero = 0) {
    return scan_reduce(val, zero, [](T &acc, T x) { acc += x; });
  }
  
  template<typename T>
  std::pair<T/*prefix*/, T/*total*/> scan_reduce_xor(T val, T zero = 0) {
    return scan_reduce(val, zero, [](T &acc, T x) { acc ^= x; });
  }

  template<typename T>
  std::pair<T/*prefix*/, T/*total*/> scan_reduce_min(T val, T max = std::numeric_limits<T>::max()) {
    return scan_reduce(val, max, [](T &acc, T x) { acc = std::min(acc, x); });
  }

  template<typename T>
  std::pair<T/*prefix*/, T/*total*/> scan_reduce_max(T val, T min = std::numeric_limits<T>::min()) {
    return scan_reduce(val, min, [](T &acc, T x) { acc = std::max(acc, x); });
  }
}
#endif
