#ifndef _ddfd9ec8_a1f2_48b9_8797_fc579cedfb18
#define _ddfd9ec8_a1f2_48b9_8797_fc579cedfb18

#include <upcxx/global_fnptr.hpp>
#include <upcxx/serialization.hpp>
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
    
    template<typename Fn1, typename TupBs, int ...bi, typename ...Arg>
    static auto apply_(Fn1 &&fn, TupBs &&bs, upcxx::detail::index_sequence<bi...> bis, Arg &&...a)
      -> decltype(
        static_cast<Fn1&&>(fn)(
          std::get<bi>(static_cast<TupBs&&>(bs))...,
          static_cast<Arg&&>(a)...
        )
      ) {
      return static_cast<Fn1&&>(fn)(
        std::get<bi>(static_cast<TupBs&&>(bs))...,
        static_cast<Arg&&>(a)...
      );
    }
  
    using the_b_ixs = upcxx::detail::make_index_sequence<sizeof...(B)>;
    
    template<typename ...Arg>
    auto operator()(Arg &&...a) const& ->
      decltype(apply_(this->fn_, this->b_, the_b_ixs(), static_cast<Arg&&>(a)...)) {
      return apply_(this->fn_, this->b_, the_b_ixs(), static_cast<Arg&&>(a)...);
    }
    
    template<typename ...Arg>
    auto operator()(Arg &&...a) & ->
      decltype(apply_(this->fn_, this->b_, the_b_ixs(), static_cast<Arg&&>(a)...)) {
      return apply_(this->fn_, this->b_, the_b_ixs(), static_cast<Arg&&>(a)...);
    }
    
    template<typename ...Arg>
    auto operator()(Arg &&...a) && ->
      decltype(
        apply_(static_cast<Fn&&>(this->fn_),
               static_cast<std::tuple<B...>&&>(this->b_),
               the_b_ixs(),
               static_cast<Arg&&>(a)...)
      ) {
      return apply_(static_cast<Fn&&>(this->fn_),
                    static_cast<std::tuple<B...>&&>(this->b_),
                    the_b_ixs(),
                    static_cast<Arg&&>(a)...);
    }
  };

  // make `bound_function` packable
  template<typename Fn, typename ...B>
  struct serialization<bound_function<Fn,B...>> {
    static constexpr bool is_definitely_serializable =
      serialization_traits<Fn>::is_definitely_serializable &&
      serialization_traits<std::tuple<B...>>::is_definitely_serializable;

    static constexpr bool references_buffer = 
      serialization_traits<Fn>::references_buffer ||
      serialization_traits<std::tuple<B...>>::references_buffer;

    template<typename Ub>
    static auto ubound(Ub ub, const bound_function<Fn,B...> &fn)
      -> decltype(
        ub.cat_ubound_of(fn.fn_)
          .cat_ubound_of(fn.b_)
      ) {
      return ub.cat_ubound_of(fn.fn_)
               .cat_ubound_of(fn.b_);
    }
    
    template<typename W>
    static void serialize(W &w, const bound_function<Fn,B...> &fn) {
      w.write(fn.fn_);
      w.write(fn.b_);
    }

    template<typename R>
    static void skip(R &r) {
      r.template skip<Fn>();
      r.template skip<std::tuple<B...>>();
    }

    using deserialized_type = bound_function<deserialized_type_t<Fn>, deserialized_type_t<B>...>;
    
    template<typename R>
    static deserialized_type* deserialize(R &r, void *spot) {
      detail::raw_storage<deserialized_type_t<Fn>> fn;
      r.template read_into<Fn>(&fn);

      detail::raw_storage<deserialized_type_t<std::tuple<B...>>> b;
      r.template read_into<std::tuple<B...>>(&b);

      return ::new(spot) deserialized_type(
        fn.value_and_destruct(),
        b.value_and_destruct()
      );
    }
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
  Fn&& bind(Fn &&fn) {
    return static_cast<Fn&&>(fn);
  }

  template<typename Fn, typename B0, typename ...Bs>
  auto bind(Fn &&fn, B0 &&b0, Bs &&...bs)
    -> bound_function_of<Fn&&,B0&&,Bs&&...> {
    return bound_function_of<Fn&&,B0&&,Bs&&...>(
      detail::globalize_fnptr(static_cast<Fn&&>(fn)),
      std::tuple<
          typename std::decay<B0>::type,
          typename std::decay<Bs>::type...
        >(
        static_cast<B0&&>(b0), static_cast<Bs&&>(bs)...
      )
    );
  }
}

#endif
