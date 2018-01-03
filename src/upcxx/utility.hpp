#ifndef _661bba4d_9f90_4fbe_b617_4474e1ed8cab
#define _661bba4d_9f90_4fbe_b617_4474e1ed8cab

#include "diagnostic.hpp"

#include <array>
#include <cstdint>
#include <functional>
#include <iostream>
#include <sstream>
#include <tuple>
#include <utility>

namespace upcxx {
  //////////////////////////////////////////////////////////////////////
  // nop_function
  
  template<typename Sig>
  struct nop_function;
  
  template<typename Ret, typename ...Arg>
  struct nop_function<Ret(Arg...)> {
    template<typename ...Arg1>
    Ret operator()(Arg1 &&...a) const {
      UPCXX_ASSERT(false);
      throw std::bad_function_call();
    }
  };
  template<typename ...Arg>
  struct nop_function<void(Arg...)> {
    template<typename ...Arg1>
    void operator()(Arg1 &&...a) const {}
  };
  
  template<typename Sig>
  nop_function<Sig> nop() {
    return nop_function<Sig>{};
  }
  
  //////////////////////////////////////////////////////////////////////
  // constant_function
  
  template<typename T>
  struct constant_function {
    T value_;
    constant_function(T value): value_{std::move(value)} {}
    
    template<typename ...Arg>
    T operator()(Arg &&...args) const {
      return value_;
    }
  };
  
  template<typename T>
  inline constant_function<T> constant(T value) {
    return constant_function<T>{std::move(value)};
  }
  
  //////////////////////////////////////////////////////////////////////
  // function_ref: reference to a function. Useful when you want to pass
  // a lambda into a subroutine knowing the lambda won't be used after
  // the subroutine exits. A regular std::function could be used in this
  // case but this has the advantage of doing no heap allocations and
  // no virtual call for the destructor.
  //
  // void my_foreach(int n, function_ref<void(int)> fn) {
  //   for(int i=0; i < n; i++)
  //     fn(i);
  // }
  //
  // my_foreach(10, [=](int i) { cout << "i="<<i<<'\n'; });
  
  template<typename Sig>
  class function_ref;
  
  template<typename Ret, typename ...Arg>
  class function_ref<Ret(Arg...)> {
    Ret(*invoker_)(Arg...);
    void *fn_;
    
  private:
    template<typename Fn>
    static Ret the_invoker(void *fn, Arg ...arg) {
      return reinterpret_cast<Fn*>(fn)->operator()(static_cast<Arg>(arg)...);
    }
    
    static Ret the_nop_invoker(void *fn, Arg ...arg) {
      return nop_function<Ret(Arg...)>{}();
    }
    
  public:
    function_ref():
      invoker_{the_nop_invoker},
      fn_{nullptr} {
    }
    template<typename Fn>
    function_ref(Fn &&fn):
      invoker_{the_invoker<typename std::remove_reference<Fn>::type>},
      fn_{reinterpret_cast<void*>(const_cast<Fn*>(&fn))} {
    }
    function_ref(const function_ref&) = default;
    function_ref& operator=(const function_ref&) = default;
    function_ref(function_ref&&) = default;
    function_ref& operator=(function_ref&&) = default;
    
    Ret operator()(Arg ...arg) const {
      return invoker_(fn_, static_cast<Arg>(arg)...);
    }
  };
  
  //////////////////////////////////////////////////////////////////////
  // trait_forall
  
  template<template<typename...> class Test, typename ...T>
  struct trait_forall;
  template<template<typename...> class Test>
  struct trait_forall<Test> {
    static constexpr bool value = true;
  };
  template<template<typename...> class Test, typename T, typename ...Ts>
  struct trait_forall<Test,T,Ts...> {
    static constexpr bool value = Test<T>::value && trait_forall<Test,Ts...>::value;
  };
  
  template<template<typename...> class Test, typename Tuple>
  struct trait_forall_tupled;
  template<template<typename...> class Test, typename ...T>
  struct trait_forall_tupled<Test, std::tuple<T...>> {
    static constexpr bool value = trait_forall<Test, T...>::value;
  };
  
  //////////////////////////////////////////////////////////////////////
  // trait_any
  
  template<template<typename...> class ...Tr>
  struct trait_any;
  
  template<>
  struct trait_any<> {
    template<typename T>
    using type = std::false_type;
  };
  
  template<template<typename...> class Tr0,
           template<typename...> class ...Trs>
  struct trait_any<Tr0,Trs...> {
    template<typename T>
    struct type {
      static constexpr bool value = Tr0<T>::value || trait_any<Trs...>::template type<T>::value;
    };
  };
  
  //////////////////////////////////////////////////////////////////////
  // trait_all
  
  template<template<typename...> class ...Tr>
  struct trait_all;
  
  template<>
  struct trait_all<> {
    template<typename T>
    using type = std::true_type;
  };
  
  template<template<typename...> class Tr0,
           template<typename...> class ...Trs>
  struct trait_all<Tr0,Trs...> {
    template<typename T>
    struct type {
      static constexpr bool value = Tr0<T>::value && trait_all<Trs...>::template type<T>::value;
    };
  };
  
  //////////////////////////////////////////////////////////////////////

  template<typename Tuple, template<typename...> class Into>
  struct tuple_types_into;
  template<typename ...T, template<typename...> class Into>
  struct tuple_types_into<std::tuple<T...>, Into> {
    typedef Into<T...> type;
  };
  template<typename Tuple, template<typename...> class Into>
  using tuple_types_into_t = typename tuple_types_into<Tuple,Into>::type;
  
  //////////////////////////////////////////////////////////////////////

  template<int...>
  struct index_sequence {};

  //////////////////////////////////////////////////////////////////////

  namespace detail {
    template<int n, int ...s>
    struct make_index_sequence: make_index_sequence<n-1, n-1, s...> {};

    template<int ...s>
    struct make_index_sequence<0, s...> {
      typedef index_sequence<s...> type;
    };
  }
  
  template<int n>
  using make_index_sequence = typename detail::make_index_sequence<n>::type;

  //////////////////////////////////////////////////////////////////////
  // add_lref_if_nonref: Add a lvalue-reference (&) to type T if T isn't
  // already a reference (& or &&) type.
  
  template<typename T>
  struct add_lref_if_nonref { using type = T&; };
  
  template<typename T>
  struct add_lref_if_nonref<T&> { using type = T&; };
  
  template<typename T>
  struct add_lref_if_nonref<T&&> { using type = T&&; };
  
  //////////////////////////////////////////////////////////////////////
  
  template<typename Tup>
  struct decay_tupled;
  template<typename ...T>
  struct decay_tupled<std::tuple<T...>> {
    typedef std::tuple<typename std::decay<T>::type...> type;
  };
  
  //////////////////////////////////////////////////////////////////////
  // get_or_void & tuple_element_or_void: analogs of std::get &
  // std::tuple_elemenet which return void for out-of-range indices
  
  namespace detail {
    template<int i, typename TupRef,
             bool in_range = (
               0 <= i &&
               i < std::tuple_size<typename std::decay<TupRef>::type>::value
             )>
    struct tuple_get_or_void {
      auto operator()(TupRef t)
        -> decltype(std::get<i>(t)) {
        return std::get<i>(t);
      }
    };
    
    template<int i, typename TupRef>
    struct tuple_get_or_void<i, TupRef, /*in_range=*/false>{
      void operator()(TupRef t) {}
    };
  }
  
  template<int i, typename Tup>
  auto get_or_void(Tup &&tup)
    -> decltype(
      detail::tuple_get_or_void<i,Tup>()(std::forward<Tup>(tup))
    ) {
    return detail::tuple_get_or_void<i,Tup>()(std::forward<Tup>(tup));
  }
  
  template<int i, typename Tup,
           bool in_range = 0 <= i && i < std::tuple_size<Tup>::value>
  struct tuple_element_or_void: std::tuple_element<i,Tup> {};
  
  template<int i, typename Tup>
  struct tuple_element_or_void<i,Tup,/*in_range=*/false> {
    using type = void;
  };
  
  //////////////////////////////////////////////////////////////////////
  // tuple_lrefs: Get individual lvalue-references to tuple componenets.
  // For components which are already `&` or `&&` types you'll get those
  // back unmodified. If the tuple itself isn't passed in with `&`
  // then this will only work if all components are `&` or `&&`.
  
  namespace detail {
    template<typename Tup, int i,
             typename Ti = typename std::tuple_element<i, typename std::decay<Tup>::type>::type>
    struct tuple_lrefs_get;
    
    template<typename Tup, int i, typename Ti>
    struct tuple_lrefs_get<Tup&, i, Ti> {
      Ti& operator()(Tup &tup) const {
        return std::get<i>(tup);
      }
    };
    template<typename Tup, int i, typename Ti>
    struct tuple_lrefs_get<Tup&, i, Ti&&> {
      Ti&& operator()(Tup &tup) const {
        return static_cast<Ti&&>(std::get<i>(tup));
      }
    };
    template<typename Tup, int i, typename Ti>
    struct tuple_lrefs_get<Tup&&, i, Ti&> {
      Ti& operator()(Tup &&tup) const {
        return std::get<i>(tup);
      }
    };
    template<typename Tup, int i, typename Ti>
    struct tuple_lrefs_get<Tup&&, i, Ti&&> {
      Ti&& operator()(Tup &&tup) const {
        return std::get<i>(tup);
      }
    };
    
    template<typename Tup, int ...i>
    inline auto tuple_lrefs(Tup &&tup, index_sequence<i...>)
      -> std::tuple<decltype(tuple_lrefs_get<Tup&&, i>()(tup))...> {
      return std::tuple<decltype(tuple_lrefs_get<Tup&&, i>()(tup))...>{
        tuple_lrefs_get<Tup&&, i>()(tup)...
      };
    }
  }
  
  template<typename Tup>
  inline auto tuple_lrefs(Tup &&tup)
    -> decltype(
      detail::tuple_lrefs(
        std::forward<Tup>(tup),
        make_index_sequence<std::tuple_size<typename std::decay<Tup>::type>::value>()
      )
    ) {
    return detail::tuple_lrefs(
      std::forward<Tup>(tup),
      make_index_sequence<std::tuple_size<typename std::decay<Tup>::type>::value>()
    );
  }
  
  //////////////////////////////////////////////////////////////////////
  // tuple_rvals: Get a tuple of rvalue-references to tuple componenets.
  // Components which are already `&` or `&&` are returned unmodified.
  // Non-reference componenets are returned as `&&` only if the tuple is
  // passed by non-const `&', otherwise the non-reference type is used
  // and the value is moved or copied from the input to output tuple.
  
  namespace detail {
    template<typename Tup, int i,
             typename Ti = typename std::tuple_element<i, typename std::decay<Tup>::type>::type>
    struct tuple_rvals_get;
    
    // tuple passed by &
    template<typename Tup, int i, typename Ti>
    struct tuple_rvals_get<Tup&, i, Ti&> {
      Ti& operator()(Tup &tup) const {
        return std::get<i>(tup);
      }
    };
    template<typename Tup, int i, typename Ti>
    struct tuple_rvals_get<Tup&, i, Ti&&> {
      Ti&& operator()(Tup &tup) const {
        return std::get<i>(tup);
      }
    };
    template<typename Tup, int i, typename Ti>
    struct tuple_rvals_get<Tup&, i, Ti> {
      Ti&& operator()(Tup &tup) const {
        return static_cast<Ti&&>(std::get<i>(tup));
      }
    };
    
    // tuple passed by const&
    template<typename Tup, int i, typename Ti>
    struct tuple_rvals_get<Tup const&, i, Ti&> {
      Ti& operator()(Tup const &tup) const {
        return std::get<i>(tup);
      }
    };
    template<typename Tup, int i, typename Ti>
    struct tuple_rvals_get<Tup const&, i, Ti&&> {
      Ti&& operator()(Tup const &tup) const {
        return std::get<i>(tup);
      }
    };
    template<typename Tup, int i, typename Ti>
    struct tuple_rvals_get<Tup const&, i, Ti> {
      Ti const& operator()(Tup const &tup) const {
        return std::get<i>(tup);
      }
    };
    
    // tuple passed by &&
    template<typename Tup, int i, typename Ti>
    struct tuple_rvals_get<Tup&&, i, Ti&> {
      Ti& operator()(Tup &&tup) const {
        return std::get<i>(tup);
      }
    };
    template<typename Tup, int i, typename Ti>
    struct tuple_rvals_get<Tup&&, i, Ti&&> {
      Ti&& operator()(Tup &&tup) const {
        return std::get<i>(tup);
      }
    };
    template<typename Tup, int i, typename Ti>
    struct tuple_rvals_get<Tup&&, i, Ti> {
      Ti operator()(Tup &&tup) const {
        return Ti{static_cast<Ti&&>(std::get<i>(tup))};
      }
    };
    
    template<typename Tup, int ...i>
    inline auto tuple_rvals(Tup &&tup, index_sequence<i...>)
      -> std::tuple<decltype(tuple_rvals_get<Tup&&, i>()(tup))...> {
      return std::tuple<decltype(tuple_rvals_get<Tup&&, i>()(tup))...>{
        tuple_rvals_get<Tup&&, i>()(tup)...
      };
    }
  }
  
  template<typename Tup>
  inline auto tuple_rvals(Tup &&tup)
    -> decltype(
      detail::tuple_rvals(
        std::forward<Tup>(tup),
        make_index_sequence<std::tuple_size<typename std::decay<Tup>::type>::value>()
      )
    ) {
    return detail::tuple_rvals(
      std::forward<Tup>(tup),
      make_index_sequence<std::tuple_size<typename std::decay<Tup>::type>::value>()
    );
  }
  
  //////////////////////////////////////////////////////////////////////
  // apply_tupled: Apply a callable against an argument list wrapped
  // in a tuple.
  
  namespace detail {
    template<typename Fn, typename Tup, int ...i>
    inline auto apply_tupled(
        Fn &&fn, Tup &&args, index_sequence<i...>
      )
      -> decltype(fn(std::get<i>(args)...)) {
      return fn(std::get<i>(args)...);
    }
  }
  
  template<typename Fn, typename Tup>
  inline auto apply_tupled(Fn &&fn, Tup &&args)
    -> decltype(
      detail::apply_tupled(
        std::forward<Fn>(fn), std::forward<Tup>(args),
        make_index_sequence<std::tuple_size<Tup>::value>()
      )
    ) {
    return detail::apply_tupled(
      std::forward<Fn>(fn), std::forward<Tup>(args),
      make_index_sequence<std::tuple_size<Tup>::value>()
    );
  }
}

#endif
