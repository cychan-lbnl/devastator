#ifndef _cfe11b2db23c404181869953e95007f3
#define _cfe11b2db23c404181869953e95007f3

namespace deva {
  constexpr int log_up(int x, int base, int up=0) {
    return x == 0 ? -1 :
           x < base ? (up != 0 ? 1 : 0) :
           1 + log_up(x/base, base, up | (x%base));
  }

  constexpr int log2dn(unsigned int x) {
    return 8*sizeof(unsigned int)-1 - __builtin_clz(x);
  }
  constexpr int log2dn(unsigned long x) {
    return 8*sizeof(unsigned long)-1 - __builtin_clzl(x);
  }
  constexpr int log2dn(unsigned long long x) {
    return 8*sizeof(unsigned long long)-1 - __builtin_clzll(x);
  }
  template<typename T,
           typename = typename std::enable_if<std::is_signed<T>::value>::type>
  constexpr int log2dn(T x) {
    return log2dn(typename std::make_unsigned<T>::type(x));
  }
  
  constexpr int log2up(unsigned int x) {
    return 8*sizeof(unsigned int)-1 - __builtin_clz(x|1) + (x&(x-1) ? 1 : 0);
  }
  constexpr int log2up(unsigned long x) {
    return 8*sizeof(unsigned long)-1 - __builtin_clzl(x|1) + (x&(x-1) ? 1 : 0);
  }
  constexpr int log2up(unsigned long long x) {
    return 8*sizeof(unsigned long long)-1 - __builtin_clzll(x|1) + (x&(x-1) ? 1 : 0);
  }
  template<typename T,
           typename = typename std::enable_if<std::is_signed<T>::value>::type>
  constexpr int log2up(T x) {
    return log2up(typename std::make_unsigned<T>::type(x));
  }
  
  template<typename T>
  constexpr int log2dn(T x, int when_zero) {
    return x == 0 ? when_zero : log2dn(x);
  }
  template<typename T>
  constexpr int log2up(T x, int when_zero) {
    return x == 0 ? when_zero : log2up(x);
  }

  constexpr int bitffs(unsigned int x) {
    return __builtin_ffs(x);
  }
  constexpr int bitffs(unsigned long x) {
    return __builtin_ffsl(x);
  }
  constexpr int bitffs(unsigned long long x) {
    return __builtin_ffsll(x);
  }
  template<typename T,
           typename = typename std::enable_if<std::is_signed<T>::value>::type>
  constexpr int bitffs(T x) {
    return bitffs(typename std::make_unsigned<T>::type(x));
  }

  //////////////////////////////////////////////////////////////////////////////
  // ternary logic

  enum bool3 {
    false3, true3, maybe3
  };

  // family of singleton types foreach ternary value
  template<bool3 val>
  struct cbool3 {
    static constexpr bool3 value = val;
  };

  using cfalse3_t = cbool3<false3>;
  using ctrue3_t = cbool3<true3>;
  using cmaybe3_t = cbool3<maybe3>;
  
  constexpr cfalse3_t cfalse3 = {};
  constexpr ctrue3_t ctrue3 = {};
  constexpr cmaybe3_t cmaybe3 = {};
}

#endif
