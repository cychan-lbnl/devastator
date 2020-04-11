#ifndef _33b82d57a3f346d59a519f14e65d9323
#define _33b82d57a3f346d59a519f14e65d9323

#include <devastator/datarow.hxx>

#include <iostream>
#include <sstream>

namespace deva {
  extern const char *const git_version;
  
  void assert_failed(const char *file, int line, const char *msg=nullptr);
  void dbgbrk(bool *aborting=nullptr);
}

#if DEBUG
  #define DEVA_DEBUG_ONLY(...) __VA_ARGS__
#else
  #define DEVA_DEBUG_ONLY(...)
#endif

#define DEVA_ASSERT_1(ok) \
  (!!(ok) || (::deva::assert_failed(__FILE__, __LINE__, "Failed condition: " #ok), 0))

#define DEVA_ASSERT_2(ok, ios_msg) \
  if(!(ok)) { \
    ::std::stringstream ss; \
    ss << ios_msg; \
    ::deva::assert_failed(__FILE__, __LINE__, ss.str().c_str()); \
  };

#define DEVA_ASSERT_DISPATCH(_1, _2, NAME, ...) NAME

// Assert that will only happen in debug-mode.
#if DEBUG
  #define DEVA_ASSERT(...) DEVA_ASSERT_DISPATCH(__VA_ARGS__, DEVA_ASSERT_2, DEVA_ASSERT_1, _DUMMY)(__VA_ARGS__)
#else
  #define DEVA_ASSERT(...) ((void)0)
#endif

// Assert that happens regardless of debug-mode.
#define DEVA_ASSERT_ALWAYS(...) DEVA_ASSERT_DISPATCH(__VA_ARGS__, DEVA_ASSERT_2, DEVA_ASSERT_1, _DUMMY)(__VA_ARGS__)

namespace deva {
  // Return key/value map dessribing configuration of devastator runtime.
  datarow describe();
  
  struct say {
    std::stringstream ss;
    
    say();
    say(say const&) = delete;
    ~say();

    template<typename T>
    say& operator<<(T &&x) {
      ss << x;
      return *this;
    }
  };
}

#endif
