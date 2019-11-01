#ifndef _51ac0f50d1944c92bd0a16bfc7e4de62
#define _51ac0f50d1944c92bd0a16bfc7e4de62

#include <cstdlib>
#include <iostream>
#include <sstream>
#include <string>

namespace deva {
  template<typename T>
  T os_env(const char *name, T deft) {
    char *s = getenv(name);
    if(s && *s != 0) {
      std::stringstream ss(s);
      T val;
      ss >> val;
      return val;
    }
    else
      return deft;
  }

  template<typename T>
  T os_env(const char *name) {
    char *s = std::getenv(name);
    if(s && *s != 0) {
      std::stringstream ss(s);
      T val;
      ss >> val;
      return val;
    }
    else {
      std::cerr<<"Fatal: env var not set "<<name<<'\n';
      std::abort();
    }
  }

  template<>
  inline std::string os_env<std::string>(const char *name, std::string deft) {
    char *s = std::getenv(name);
    if(s && *s != 0)
      return std::string(s);
    else
      return deft;
  }

  template<>
  inline std::string os_env<std::string>(const char *name) {
    char *s = std::getenv(name);
    if(s && *s != 0)
      return std::string(s);
    else {
      std::cerr<<"Fatal: env var not set "<<name<<'\n';
      std::abort();
    }
  }
}
#endif
