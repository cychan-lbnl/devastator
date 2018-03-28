#ifndef _33b82d57a3f346d59a519f14e65d9323
#define _33b82d57a3f346d59a519f14e65d9323

#include <cstdio>
#include <mutex>
#include <inttypes.h>

#if 0 || DEBUG
  #define ASSERT(ok) (!!(ok) || (assert_failed(__FILE__, __LINE__), 0))
#else
  //#define ASSERT(ok) (!!(ok) || (__builtin_unreachable(), 0))
  #define ASSERT(ok) ((void)0)
#endif

#define ASSERT_ALWAYS(ok) (!!(ok) || (assert_failed(__FILE__, __LINE__), 0))

/*[[noreturn]]*/ void assert_failed(const char *file, int line);

void dbgbrk();

struct say {
  say();
  say(say const&) = delete;
  ~say();

  #if 0
    template<typename T>
    say& operator<<(T &&x) {
      std::cerr << x;
      return *this;
    }
  #else
    say& operator<<(int x) {
      fprintf(stderr, "%d", x);
      return *this;
    }
    say& operator<<(std::int64_t x) {
      fprintf(stderr, "%" PRId64, x);
      return *this;
    }
    say& operator<<(std::uint64_t x) {
      fprintf(stderr, "%" PRIu64, x);
      return *this;
    }
    say& operator<<(const char *x) {
      fprintf(stderr, "%s", x);
      return *this;
    }
  #endif
};

#endif
