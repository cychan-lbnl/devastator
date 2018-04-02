#include "diagnostic.hxx"
#include "world.hxx"

#include <csignal>
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

  void dbgbrk(bool &aborting) {
    gasnett_freezeForDebuggerNow(&dbgflag, "dbgflag");
  }
#else
  void dbgbrk(bool &aborting) {
    std::raise(SIGINT);
  }
#endif

void assert_failed(const char *file, int line) {
  lock_.lock();
  fflush(stdout);
  fprintf(stderr, "ASSERT FAILED %s@%d\n", file, line);
  lock_.unlock();

  bool aborting = true;
  #if DEBUG
    dbgbrk(aborting);
  #endif
  
  if(aborting) std::abort();
}

say::say() {
  lock_.lock();
  fprintf(stderr, "[%d] ", world::rank_me());
}

say::~say() {
  fprintf(stderr, "\n");
  lock_.unlock();
}
