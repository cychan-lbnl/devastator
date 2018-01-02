#ifndef _ddfd9ec8_a1f2_48b9_8797_fc579cedfb18
#define _ddfd9ec8_a1f2_48b9_8797_fc579cedfb18

#include "packing.hpp"
#include "utility.hpp"

#include <tuple>
#include <type_traits>
#include <utility>

namespace upcxx {
  template<typename Fn, typename ...B>
  struct bound_function {
    Fn fn_;
    mutable std::tuple<B...> b_;

    bound_function(bound_function&&) = default;
    
    template<typename Me, int ...bi, typename ...Arg>
    static auto apply(Me &&me, upcxx::index_sequence<bi...>, Arg &&...a)
      -> decltype(me.fn_(std::get<bi>(me.b_)..., std::forward<Arg>(a)...)) {
      return me.fn_(std::get<bi>(me.b_)..., std::forward<Arg>(a)...);
    }
    
    template<typename ...Arg>
    auto operator()(Arg &&...a)
      -> decltype(apply(*this, upcxx::make_index_sequence<sizeof...(B)>(), std::forward<Arg>(a)...)) {
      return apply(*this, upcxx::make_index_sequence<sizeof...(B)>(), std::forward<Arg>(a)...);
    }
    template<typename ...Arg>
    auto operator()(Arg &&...a) const
      -> decltype(apply(*this, upcxx::make_index_sequence<sizeof...(B)>(), std::forward<Arg>(a)...)) {
      return apply(*this, upcxx::make_index_sequence<sizeof...(B)>(), std::forward<Arg>(a)...);
    }
  };
  
  // make `bound_function` packable
  template<typename Fn, typename ...B>
  struct packing<bound_function<Fn,B...>> {
    static void size_ubound(parcel_layout &ub, const bound_function<Fn,B...> &fn) {
      packing<Fn>::size_ubound(ub, fn.fn_);
      packing<std::tuple<B...>>::size_ubound(ub, fn.b_);
    }
    
    static void pack(parcel_writer &w, const bound_function<Fn,B...> &fn) {
      packing<Fn>::pack(w, fn.fn_);
      packing<std::tuple<B...>>::pack(w, fn.b_);
    }
    
    static bound_function<Fn,B...> unpack(parcel_reader &r) {
      auto fn = packing<Fn>::unpack(r);
      auto b = packing<std::tuple<B...>>::unpack(r);
      return bound_function<Fn,B...>{std::move(fn), std::move(b)};
    }
  };
}


namespace upcxx {
  namespace detail {
    // general case
    template<typename Fn, int sizeof_B>
    struct bind {
      template<typename ...B>
      bound_function<Fn,B...> operator()(Fn fn, B ...b) const {
        return bound_function<Fn,B...>{std::move(fn), std::tuple<B...>{std::move(b)...}};
      }
    };

    // no bound args case
    template<typename Fn>
    struct bind<Fn,0> {
      template<typename ...B>
      bound_function<Fn> operator()(Fn fn) const {
        return bound_function<Fn>{std::move(fn), std::tuple<>{}};
      }
    };

    // nested bind(bind(...),...) case.
    template<typename Fn0, typename ...B0, int sizeof_B1>
    struct bind<bound_function<Fn0, B0...>, sizeof_B1> {
      template<typename ...B1>
      bound_function<Fn0, B0..., B1&&...> operator()(bound_function<Fn0, B0...> bf, B1 ...b1) const {
        return bound_function<Fn0, B0..., B1...>{
          std::move(bf.fn_),
          std::tuple_cat(
            std::move(bf.b_), 
            std::tuple<B1...>{std::forward<B1>(b1)...}
          )
        };
      }
    };
  }
  
  template<typename Fn, typename ...B>
  auto bind(Fn fn, B ...b)
    -> decltype(
      detail::bind<Fn, sizeof...(B)>()(std::move(fn), std::move(b)...)
    ) {
    return detail::bind<Fn, sizeof...(B)>()(std::move(fn), std::move(b)...);
  }
}
#endif
