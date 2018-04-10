#ifndef _9c3b2cb6_d978_4c8d_9b3e_a077c8926dfa
#define _9c3b2cb6_d978_4c8d_9b3e_a077c8926dfa

#include <upcxx/diagnostic.hpp>

#include <cstdint>
#include <cstring>
#include <functional>

namespace upcxx {
  //////////////////////////////////////////////////////////////////////////////
  // global_fnptr<Ret(Arg...)>: shippable pointer-to-function

  template<typename FnSig>
  class global_fnptr;
  
  namespace detail {
    void global_fnptr_basis();

    static constexpr std::uintptr_t global_fnptr_null = std::uintptr_t(-1)>>1;
    
    template<typename Fp>
    static std::uintptr_t fnptr_to_uintptr(Fp fp) {
      if(fp == nullptr)
        return global_fnptr_null;
      else {
        std::uintptr_t ans;
        std::memcpy(&ans, &fp, sizeof(Fp));
        return ans;
      }
    }
    
    template<typename Fp>
    static Fp fnptr_from_uintptr(std::uintptr_t u) {
      if(u == global_fnptr_null)
        return nullptr;
      else {
        Fp ans;
        std::memcpy(&ans, &u, sizeof(Fp));
        return ans;
      }
    }
  }

  template<typename Ret, typename ...Arg>
  class global_fnptr<Ret(Arg...)> {
    static_assert(
      sizeof(Ret(*)(Arg...)) == sizeof(std::uintptr_t),
      "Function pointers must be the same size as regular pointers."
    );

  public:
    using function_type = Ret(Arg...);

  private:
    std::uintptr_t u_;

    static std::uintptr_t encode(Ret(*fp)(Arg...)) {
      return fp == nullptr
        ? detail::global_fnptr_null
        : detail::fnptr_to_uintptr(fp) - detail::fnptr_to_uintptr(&detail::global_fnptr_basis);
    }
    
    static function_type* decode(std::uintptr_t u) {
      return u == detail::global_fnptr_null
        ? nullptr
        : detail::fnptr_from_uintptr<Ret(*)(Arg...)>(u + detail::fnptr_to_uintptr(&detail::global_fnptr_basis));
    }
    
    static function_type* decode_non_null(std::uintptr_t u) {
      UPCXX_ASSERT(u != detail::global_fnptr_null);
      return detail::fnptr_from_uintptr<Ret(*)(Arg...)>(u + detail::fnptr_to_uintptr(&detail::global_fnptr_basis));
    }
    
  public:
    constexpr global_fnptr(std::nullptr_t null = nullptr): u_{detail::global_fnptr_null} {}
    
    //global_fnptr(Ret(&fn)(Arg...)): u_{encode(&fn)} {}
    global_fnptr(Ret(*fp)(Arg...)): u_{encode(fp)} {}
    
    Ret operator()(Arg ...a) const {
      return decode_non_null(u_)(std::forward<Arg>(a)...);
    }

    constexpr operator bool() const { return u_ == detail::global_fnptr_null; }
    constexpr bool operator!() const { return u_ != detail::global_fnptr_null; }

    operator function_type*() const {
      return decode(u_);
    }

    function_type* fnptr_non_null() const {
      return decode_non_null(u_);
    }

    friend struct std::hash<global_fnptr<Ret(Arg...)>>;
    
    friend constexpr bool operator==(global_fnptr<Ret(Arg...)> a, global_fnptr<Ret(Arg...)> b) {
      return a.u_ == b.u_;
    }
    friend constexpr bool operator!=(global_fnptr<Ret(Arg...)> a, global_fnptr<Ret(Arg...)> b) {
      return a.u_ != b.u_;
    }
    friend constexpr bool operator<(global_fnptr<Ret(Arg...)> a, global_fnptr<Ret(Arg...)> b) {
      return a.u_ < b.u_;
    }
    friend constexpr bool operator<=(global_fnptr<Ret(Arg...)> a, global_fnptr<Ret(Arg...)> b) {
      return a.u_ <= b.u_;
    }
    friend constexpr bool operator>(global_fnptr<Ret(Arg...)> a, global_fnptr<Ret(Arg...)> b) {
      return a.u_ > b.u_;
    }
    friend constexpr bool operator>=(global_fnptr<Ret(Arg...)> a, global_fnptr<Ret(Arg...)> b) {
      return a.u_ >= b.u_;
    }
  };

  namespace detail {
    ////////////////////////////////////////////////////////////////////////////
    // detail::globalize_fnptr: Given a callable, return a global_fnptr if that
    // callable is a function pointer/reference, otherwise return the given
    // callable unaltered.
    
    template<typename Fn>
    Fn&& globalize_fnptr(Fn &&fn) {
      return std::forward<Fn>(fn);
    }
    template<typename Ret, typename ...Arg>
    global_fnptr<Ret(Arg...)> globalize_fnptr(Ret(*fn)(Arg...)) {
      return global_fnptr<Ret(Arg...)>(fn);
    }
  }
}

namespace std {
  template<typename Ret, typename ...Arg>
  struct less<upcxx::global_fnptr<Ret(Arg...)>> {
    constexpr bool operator()(upcxx::global_fnptr<Ret(Arg...)> lhs,
                              upcxx::global_fnptr<Ret(Arg...)> rhs) const {
      return lhs < rhs;
    }
  };
  template<typename Ret, typename ...Arg>
  struct less_equal<upcxx::global_fnptr<Ret(Arg...)>> {
    constexpr bool operator()(upcxx::global_fnptr<Ret(Arg...)> lhs,
                              upcxx::global_fnptr<Ret(Arg...)> rhs) const {
      return lhs <= rhs;
    }
  };
  template<typename Ret, typename ...Arg>
  struct greater<upcxx::global_fnptr<Ret(Arg...)>> {
    constexpr bool operator()(upcxx::global_fnptr<Ret(Arg...)> lhs,
                              upcxx::global_fnptr<Ret(Arg...)> rhs) const {
      return lhs > rhs;
    }
  };
  template<typename Ret, typename ...Arg>
  struct greater_equal<upcxx::global_fnptr<Ret(Arg...)>> {
    constexpr bool operator()(upcxx::global_fnptr<Ret(Arg...)> lhs,
                              upcxx::global_fnptr<Ret(Arg...)> rhs) const {
      return lhs >= rhs;
    }
  };
  
  template<typename Ret, typename ...Arg>
  struct hash<upcxx::global_fnptr<Ret(Arg...)>> {
    constexpr std::size_t operator()(upcxx::global_fnptr<Ret(Arg...)> x) const {
      return std::size_t(x.u_);
    }
  };
}
#endif
