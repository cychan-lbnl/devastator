#ifndef _d4018a12_dacc_4669_a157_0e6389ee87fa
#define _d4018a12_dacc_4669_a157_0e6389ee87fa

#include <new>

#ifndef OPNEW_ENABLED
  #define OPNEW_ENABLED 1
#endif
#define OPNEW_DEBUG 0

#if !OPNEW_ENABLED
namespace opnew {
  inline void progress() {}
  inline void thread_me_initialized() {}
  
  inline void* operator_new(std::size_t size) {
    return ::operator new(size);
  }
  
  template<std::size_t known_size=0, bool known_local=false>
  void operator_delete(void *obj) {
    ::operator delete(obj);
  }
}
#else
namespace opnew {
  void* operator_new(std::size_t);
  
  template<std::size_t known_size=0, bool known_local=false>
  void operator_delete(void *obj);
}
#endif
#endif
