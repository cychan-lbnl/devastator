#include "diagnostic.hxx"
#include "world.hxx"

#include <cstdlib>
#include <mutex>

namespace {
  std::mutex lock_;
}

#if WORLD_GASNET
  #include <gasnetex.h>
  #include <gasnet_tools.h>

  extern "C" {
    volatile int dbgflag;
  }

  void dbgbrk() {
    gasnett_freezeForDebuggerNow(&dbgflag, "dbgflag");
  }
#else
  void dbgbrk() {}
#endif

void assert_failed(const char *file, int line) {
  lock_.lock();
  std::cout.flush();
  std::cerr<<"ASSERT FAILED "<<file<<"@"<<line<<"\n";
  lock_.unlock();

  #if DEBUG
    dbgbrk();
  #endif
  
  std::abort();
}

say::say() {
  lock_.lock();
  std::cerr << '['<<world::rank_me()<<"] ";
}

say::~say() {
  std::cerr << std::endl;
  lock_.unlock();
}
