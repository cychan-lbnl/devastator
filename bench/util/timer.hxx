#ifndef _6359bcd812d04986a143a692cf25b8a6
#define _6359bcd812d04986a143a692cf25b8a6

#include <chrono>

namespace deva {
namespace bench {
  class timer {
    using clock = std::chrono::steady_clock;
    using time_point = clock::time_point;
    time_point t0;
    
  public:
    timer() {
      t0 = clock::now();
    }
    
    double elapsed() const {
      time_point t1 = clock::now();
      return std::chrono::duration<double>(t1 - t0).count();
    }
    
    double reset() {
      time_point t1 = clock::now();
      double ans = std::chrono::duration<double>(t1 - t0).count();
      t0 = t1;
      return ans;
    }
  };
}}
#endif
