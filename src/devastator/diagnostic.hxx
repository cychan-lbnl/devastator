#ifndef _33b82d57a3f346d59a519f14e65d9323
#define _33b82d57a3f346d59a519f14e65d9323

#include <iostream>
#include <mutex>

#define ASSERT(ok) (!!(ok) || (assert_failed(__FILE__, __LINE__), 0))
#define ASSERT_ALWAYS(ok) (!!(ok) || (assert_failed(__FILE__, __LINE__), 0))

void assert_failed(const char *file, int line);

void dbgbrk();

struct say {
  say();
  say(say const&) = delete;
  ~say();

  template<typename T>
  say& operator<<(T &&x) {
    std::cerr << x;
    return *this;
  }
};

#endif
