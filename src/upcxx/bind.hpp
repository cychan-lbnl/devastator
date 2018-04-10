#ifndef _ddfd9ec8_a1f2_48b9_8797_fc579cedfb18
#define _ddfd9ec8_a1f2_48b9_8797_fc579cedfb18

#include <upcxx/packing.hpp>
#include <upcxx/global_fnptr.hpp>
#include <upcxx/utility.hpp>

#include <tuple>
#include <type_traits>
#include <utility>

namespace upcxx {
  //////////////////////////////////////////////////////////////////////////////
  // bound_function: Packable callable wrapping an internal callable
  // and bound leading arguments. The "on-wire" type of each thing is
  // stored in this object. Calling the object invokes "off-wire"
  // translation to produce futures, and when those are all ready, then
  // the callable is applied to the bound arguments followed by the
  // immediate arguments. A future of the internal callable's return
  // value is returned. If all of the callable and the bound arguments
  // are trivially-binding, then the futures are all elided and invoking
  // this object with immediate arguments is just like invoking its
  // internal callable against leading bound arguments + immediate
  // arguments, no future returned by this level.
  template<typename Fn, typename ...B>
  struct bound_function {
    Fn fn_;
    std::tuple<B...> b_;
    
    bound_function(Fn fn, std::tuple<B...> b):
      fn_(static_cast<Fn&&>(fn)),
      b_(static_cast<std::tuple<B...>&&>(b)) {
    }
    
    template<typename Me, int ...bi, typename ...Arg>
    static auto apply_(Me &&me, upcxx::index_sequence<bi...> bis, Arg &&...a)
      -> decltype(
        me.fn_(
          std::declval<B&>()...,
          std::forward<Arg>(a)...
        )
      ) {
      return me.fn_(
        const_cast<B&>(std::get<bi>(me.b_))...,
        std::forward<Arg>(a)...
      );
    }
  
    using the_b_ixs = upcxx::make_index_sequence<sizeof...(B)>;
    
    template<typename ...Arg>
    auto operator()(Arg &&...a) const ->
      decltype(apply_(*this, the_b_ixs(), std::forward<Arg>(a)...)) {
      return apply_(*this, the_b_ixs(), std::forward<Arg>(a)...);
    }
    
    template<typename ...Arg>
    auto operator()(Arg &&...a) ->
      decltype(apply_(*this, the_b_ixs(), std::forward<Arg>(a)...)) {
      return apply_(*this, the_b_ixs(), std::forward<Arg>(a)... );
    }
  };

  // make `bound_function` packable
  namespace detail {
    template<typename Fn, typename ...B>
    struct packing_skippable_dumb<bound_function<Fn,B...>> {
      static constexpr bool is_definitely_supported =
        packing_is_definitely_supported<Fn>::value &&
        packing_is_definitely_supported<std::tuple<B...>>::value;

      static constexpr bool is_owning = 
        packing_is_owning<Fn>::value &&
        packing_is_owning<std::tuple<B...>>::value;

      static constexpr bool is_ubound_tight = 
        packing_is_ubound_tight<Fn>::value &&
        packing_is_ubound_tight<std::tuple<B...>>::value;
      
      template<typename Ub, bool skippable>
      static auto ubound(Ub ub, const bound_function<Fn,B...> &fn, std::integral_constant<bool,skippable>)
        -> decltype(
          packing<std::tuple<B...>>::ubound(
            packing<Fn>::ubound(
              ub, fn.fn_, std::false_type()
            ),
            fn.b_, std::false_type()
          )
        ) {
        return packing<std::tuple<B...>>::ubound(
          packing<Fn>::ubound(
            ub, fn.fn_, /*skippable=*/std::false_type()
          ),
          fn.b_, /*skippable=*/std::false_type()
        );
      }
      
      template<bool skippable>
      static void pack(parcel_writer &w, const bound_function<Fn,B...> &fn, std::integral_constant<bool,skippable>) {
        packing<Fn>::pack(w, fn.fn_, std::false_type());
        packing<std::tuple<B...>>::pack(w, fn.b_, std::false_type());
      }

      static void skip(parcel_reader &r) {
        packing<Fn>::skip(r);
        packing<std::tuple<B...>>::skip(r);
      }

      using unpacked_t = bound_function<Fn, unpacked_of_t<B>...>;
      
      template<bool skippable>
      static void unpack(parcel_reader &r, void *into, std::integral_constant<bool,skippable>) {
        raw_storage<Fn> fn;
        packing<Fn>::unpack(r, &fn, /*skippable=*/std::false_type());

        raw_storage<std::tuple<B...>> b;
        packing<std::tuple<B...>>::unpack(r, &b, /*skippable=*/std::false_type());
        
        ::new(into) bound_function<Fn,B...>(
          fn.value_and_destruct(),
          b.value_and_destruct()
        );
      }
    };
  }

  template<typename Fn, typename ...B>
  struct packing_screen_trivial<bound_function<Fn,B...>>:
    upcxx::trait_forall<packing_is_trivial, Fn, B...> {
  };
  
  template<typename Fn, typename ...B>
  struct is_definitely_trivially_serializable<bound_function<Fn,B...>>:
    upcxx::trait_forall<is_definitely_trivially_serializable, Fn, B...> {
  };

  template<typename Fn, typename ...B>
  struct packing_is_definitely_supported<bound_function<Fn,B...>>:
    upcxx::trait_forall<packing_is_definitely_supported, Fn, B...> {
  };
  
  template<typename Fn, typename ...B>
  struct packing_screened<bound_function<Fn,B...>>:
    detail::packing_skippable_smart<bound_function<Fn,B...>> {
  };
}


////////////////////////////////////////////////////////////////////////////////
// upcxx::bind: Similar to std::bind but doesn't support placeholders. Most
// importantly, these can be packed. The `binding` typeclass is used for
// producing the on-wire and off-wire representations. If the wrapped callable
// and all bound arguments have trivial binding traits, then the returned
// callable has a return type equal to that of the wrapped callable. Otherwise,
// the returned callable will have a future return type.

namespace upcxx {
  template<typename Fn, typename ...B>
  using bound_function_of = bound_function<
      typename std::decay<
          decltype(detail::globalize_fnptr(std::declval<Fn>()))
        >::type,
      typename std::decay<B>::type...
    >;
  
  template<typename Fn, typename ...B>
  auto bind(Fn &&fn, B &&...b)
    -> bound_function_of<Fn,B...> {
    return bound_function_of<Fn,B...>(
      detail::globalize_fnptr(std::forward<Fn>(fn)),
      std::tuple<typename std::decay<B>::type...>(
        std::forward<B>(b)...
      )
    );
  }
}

#endif
