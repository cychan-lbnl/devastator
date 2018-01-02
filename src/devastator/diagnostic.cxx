#include "diagnostic.hxx"
#include "world.hxx"

#include <cstdlib>
#include <mutex>

namespace {
  std::mutex lock_;
}

void dbgbrk() {}

void assert_failed(const char *file, int line) {
  lock_.lock();
  std::cout.flush();
  std::cerr<<"ASSERT FAILED "<<file<<"@"<<line<<"\n";
  lock_.unlock();
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
