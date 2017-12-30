#ifndef _7949681d_8a89_4f83_afb9_de702bf1a46b
#define _7949681d_8a89_4f83_afb9_de702bf1a46b

#include <sstream>

namespace upcxx {
  void dbgbrk();
  void assert_failed(const char *file, int line, const char *msg=nullptr);
}

#define UPCXX_ASSERT_1(ok) \
  do { \
    if(!(ok)) \
      ::upcxx::assert_failed(__FILE__, __LINE__); \
  } while(0);

#define UPCXX_ASSERT_2(ok, ios_msg) \
  do { \
    if(!(ok)) { \
      ::std::stringstream ss; \
      ss << ios_msg; \
      ::upcxx::assert_failed(__FILE__, __LINE__, ss.str().c_str()); \
    } \
  } while(0);

#define UPCXX_ASSERT_DISPATCH(_1, _2, NAME, ...) NAME

// Assert that will only happen in debug-mode. For now we just
// always enable it.
#define UPCXX_ASSERT(...) UPCXX_ASSERT_DISPATCH(__VA_ARGS__, UPCXX_ASSERT_2, UPCXX_ASSERT_1, _DUMMY)(__VA_ARGS__)

// Assert that happens regardless of debug-mode.
#define UPCXX_ASSERT_ALWAYS(...) UPCXX_ASSERT_DISPATCH(__VA_ARGS__, UPCXX_ASSERT_2, UPCXX_ASSERT_1, _DUMMY)(__VA_ARGS__)

// In debug mode this will abort. In non-debug this is a nop.
#define UPCXX_INVOKE_UB() ::upcxx::assert_failed(__FILE__, __LINE__)

namespace upcxx {
  // ostream-like class which will print to standard error with as
  // much atomicity as possible. Incluces current rank and trailing
  // newline.
  // usage:
  //   upcxx::say() << "hello world";
  // prints:
  //   [0] hello world \n
  class say {
    std::stringstream ss;
  public:
    say();
    ~say();
    
    template<typename T>
    say& operator<<(T const &that) {
      ss << that;
      return *this;
    }
  };
}

#endif
