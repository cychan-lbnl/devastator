#ifndef _cfe11b2db23c404181869953e95007f3
#define _cfe11b2db23c404181869953e95007f3

namespace deva {
  // ternary logic value
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
