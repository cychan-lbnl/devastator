#ifndef _661bba4d_9f90_4fbe_b617_4474e1ed8cab
#define _661bba4d_9f90_4fbe_b617_4474e1ed8cab

#include <upcxx/diagnostic.hpp>
#include <upcxx/upcxx_config.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <functional>
#include <iostream>
#include <sstream>
#include <tuple>
#include <utility>
#include <new> // launder

// RTTI support
#ifndef UPCXX_HAVE_RTTI
#define UPCXX_HAVE_RTTI (__GXX_RTTI || __cpp_rtti)
#endif
#if UPCXX_HAVE_RTTI
#include <typeinfo> // typeid
#endif

#include <cstdlib> // posix_memalign

// UPCXX_RETURN_DECLTYPE(type): use this inplace of "-> decltype(type)" so that
// for compilers which choke on such return types (icc) it can be elided in
// the presence of C++14.
#if !defined(__INTEL_COMPILER) || __cplusplus <= 201199L
  #define UPCXX_RETURN_DECLTYPE(...) -> decltype(__VA_ARGS__)
#else
  #define UPCXX_RETURN_DECLTYPE(...)
#endif

namespace upcxx {
namespace detail {
  //////////////////////////////////////////////////////////////////////
  // detail::nop_function

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
  // detail::constant_function

  template<typename T>
  struct constant_function {
    T value_;
    constant_function(T value): value_(std::move(value)) {}
    
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
    void *fn_;
    union {
      Ret(*indirect)(void *fn, Arg...);
      Ret(*direct)(Arg...);
    } invoker_;
    
  private:
    template<typename Fn>
    static Ret the_functor_invoker(void *fn, Arg ...arg) {
      return (*reinterpret_cast<Fn*>(fn))(static_cast<Arg>(arg)...);
    }
    
    static Ret the_nop_invoker(Arg ...arg) {
      return nop_function<Ret(Arg...)>{}();
    }
    
  public:
    function_ref() {
      fn_ = nullptr;
      invoker_.direct = the_nop_invoker;
    }
    function_ref(Ret(*fn)(Arg...)) {
      fn_ = nullptr;
      invoker_.direct = fn;
    }
    template<typename Fn>
    function_ref(Fn &&fn) {
      fn_ = reinterpret_cast<void*>(
          const_cast<typename std::decay<Fn>::type*>(&fn)
        );
      invoker_.indirect = the_functor_invoker<
          typename std::decay<Fn>::type
        >;
    }
    function_ref(function_ref const&) = default;
    function_ref(function_ref &that): function_ref(const_cast<function_ref const&>(that)) {}
    function_ref(function_ref&&) = default;
    
    function_ref& operator=(function_ref const &) = default;
    function_ref& operator=(function_ref&&) = default;
    
    Ret operator()(Arg ...arg) const {
      if(fn_ == nullptr)
        return invoker_.direct(static_cast<Arg>(arg)...);
      else
        return invoker_.indirect(fn_, static_cast<Arg>(arg)...);
    }
  };

  //////////////////////////////////////////////////////////////////////////////
  // detail::memcpy_aligned

  template<std::size_t align>
  inline void memcpy_aligned(void *dst, void const *src, std::size_t sz) noexcept {
  #if UPCXX_HAVE___BUILTIN_ASSUME_ALIGNED
    std::memcpy(__builtin_assume_aligned(dst, align),
                __builtin_assume_aligned(src, align), sz);
  #else
    std::memcpy(dst, src, sz);
  #endif
  }

  //////////////////////////////////////////////////////////////////////////////
  // detail::launder & detail::launder_unconstructed

  // *Hopefully*, make it impossible for the compiler to prove to itself that
  // that `p` doesn't point to an object of type T. This can be used when you
  // know the underlying bytes encode a valid T, but it hasn't been actually
  // constructed as such.
  template<typename T>
  T* launder_unconstructed(T *p) noexcept {
    asm("" : "+rm"(p) : "rm"(p) :);
    return p;
  }

  #if __cpp_lib_launder >= 201606 /* std::launder is not reliably available in C++17 (eg clang 8) */ \
      && !__PGI /* issue #289/#297: std::launder broken for all known versions of PGI */
    template<typename T>
    constexpr T* launder(T *p) {
      return std::launder(p);
    }
  #else
    template<typename T>
    T* launder(T *p) noexcept {
      return launder_unconstructed(p);
    }
  #endif
  
  //////////////////////////////////////////////////////////////////////////////
  // detail::construct_default: Default constructs a T object if possible,
  // otherwise cheat (UB!) to convince compiler such an object already existed.

  namespace help {
    template<typename T>
    T* construct_default(void *spot, std::true_type deft_ctor) {
      return reinterpret_cast<T*>(::new(spot) T); // extra reinterpret_cast needed for T = U[n] (compiler bug?!)
    }
    template<typename T>
    T* construct_default(void *spot, std::false_type deft_ctor) {
      return detail::template launder_unconstructed<T>(reinterpret_cast<T*>(spot));
    }
  }
  
  template<typename T>
  T* construct_default(void *spot) {
    return help::template construct_default<T>(
      spot,
      std::integral_constant<bool, std::is_default_constructible<T>::value>()
    );
  }
  
  //////////////////////////////////////////////////////////////////////////////
  // detail::construct_trivial: Constructs T objects from raw bytes. If
  // T doesn't have the appropriate constructors, this will cheat (UB!) by
  // copying the bytes and then *blessing* the memory as holding a valid T.
  
  namespace help {
    template<typename T>
    T* construct_trivial(void *dest, const void *src, std::true_type deft_ctor, std::true_type triv_copy) {
      using T1 = typename std::remove_const<T>::type;
      T1 *ans = reinterpret_cast<T1*>(::new(dest) T1);
      detail::template memcpy_aligned<alignof(T1)>(ans, src, sizeof(T1));
      return ans;
    }
    template<typename T>
    T* construct_trivial(void *dest, const void *src, std::true_type deft_ctor, std::false_type triv_copy) {
      using T1 = typename std::remove_const<T>::type;
      ::new(dest) T1;
      detail::template memcpy_aligned<alignof(T1)>(dest, src, sizeof(T1));
      return detail::template launder<T1>(reinterpret_cast<T1*>(dest));
    }
    template<typename T, bool any>
    T* construct_trivial(void *dest, const void *src, std::false_type deft_ctor, std::integral_constant<bool,any> triv_copy) {
      detail::template memcpy_aligned<alignof(T)>(dest, src, sizeof(T));
      return detail::launder_unconstructed(reinterpret_cast<T*>(dest));
    }
    
    template<typename T>
    T* construct_trivial(void *dest, const void *src, std::size_t n, std::true_type deft_ctor, std::true_type triv_copy) {
      using T1 = typename std::remove_const<T>::type;
      T1 *ans = nullptr;
      for(std::size_t i=n; i != 0;)
        ans = ::new((T1*)dest + --i) T1;
      detail::template memcpy_aligned<alignof(T1)>(ans, src, n*sizeof(T1));
      return ans;
    }
    template<typename T>
    T* construct_trivial(void *dest, const void *src, std::size_t n, std::true_type deft_ctor, std::false_type triv_copy) {
      using T1 = typename std::remove_const<T>::type;
      for(std::size_t i=n; i != 0;)
        ::new((T1*)dest + --i) T1;
      detail::template memcpy_aligned<alignof(T1)>(dest, src, n*sizeof(T1));
      return detail::template launder<T1>(reinterpret_cast<T1*>(dest));
    }
    template<typename T, bool any>
    T* construct_trivial(void *dest, const void *src, std::size_t n, std::false_type deft_ctor, std::integral_constant<bool,any> triv_copy) {
      detail::template memcpy_aligned<alignof(T)>(dest, src, n*sizeof(T));
      return detail::launder_unconstructed(reinterpret_cast<T*>(dest));
    }
  }
  
  template<typename T>
  T* construct_trivial(void *dest, const void *src) {
    return help::template construct_trivial<T>(
        dest, src, 
        std::integral_constant<bool, std::is_default_constructible<T>::value>(),
        std::integral_constant<bool, std::is_trivially_copyable<T>::value>()
      );
  }
  template<typename T>
  T* construct_trivial(void *dest, const void *src, std::size_t n) {
    return help::template construct_trivial<T>(
        dest, src, n,
        std::integral_constant<bool, std::is_default_constructible<T>::value>(),
        std::integral_constant<bool, std::is_trivially_copyable<T>::value>()
      );
  }
  
  //////////////////////////////////////////////////////////////////////////////
  // detail::destruct

  template<typename T>
  struct destruct_dispatch {
    void operator()(T &x) const noexcept { x.~T(); }
  };
  template<typename T, std::size_t n>
  struct destruct_dispatch<T[n]> {
    void operator()(T (&x)[n]) const noexcept {
      for(std::size_t i=0; i != n; i++)
        destruct_dispatch<T>()(x[i]);
    }
  };
  
  template<typename T>
  void destruct(T &x) noexcept { destruct_dispatch<T>()(x); }
  
  //////////////////////////////////////////////////////////////////////////////
  // detail::alloc_aligned

  inline void* alloc_aligned(std::size_t size, std::size_t align) noexcept {
    UPCXX_ASSERT(
      align != 0 && (align & (align-1)) == 0,
      "upcxx::detail::alloc_aligned: Invalid align="<<align
    );

    // Note: do not use std::aligned_alloc (C++17) since it does not *portably*
    // support extended alignments (those greater than std::max_align_t), and
    // thus is no better than ::operator new().

    // Make align = max(align, sizeof(void*))
    align -= 1;
    align |= sizeof(void*)-1;
    align += 1;
    // Round size up to a multiple of alignment
    size = (size + align-1) & -align;
    
    void *p;
    int err = posix_memalign(&p, align, size);
    UPCXX_ASSERT_ALWAYS(err == 0,
      "upcxx::detail::alloc_aligned: posix_memalign(align="<<align<<", size="<<size<<"): failed with return="<<err
    );
    return p;
  }

  //////////////////////////////////////////////////////////////////////////////
  // detail::is_aligned

  inline bool is_aligned(void const *x, std::size_t align) {
    UPCXX_ASSERT((align & (align-1)) == 0, "align must be a power of 2");
    return 0 == (reinterpret_cast<std::uintptr_t>(x) & (align-1));
  }
  
  //////////////////////////////////////////////////////////////////////////////
  // xaligned_storage: like std::aligned_storage::type except:
  //   1. Supports extended alignemnts greater than alignof(std::max_align_t)
  //   2. Does not guarantee that `sizeof(xaligned_storage<S,A>::type) == S` since it might
  //      include padding space to achieve the alignemnt dynamically.
  //   3. Does not guarantee to be a trivial type.
  //   4. Access to the aligned memory is provided by the `storage()` member,
  //      alignment of the xaligned_storage object itself is unspecified.
  //
  // The intenteded use case is for small stack-allocated serialization buffers.
  // The other potential use case is to hold overly-aligned types, but currently
  // we have none so for that usage we stick to `std::aligned_storage<>::type`.
  
  template<std::size_t size, std::size_t align,
           bool valid = 0 == (align & (align-1)),
           bool extended = (align > alignof(std::max_align_t))>
  struct xaligned_storage;

  template<std::size_t size, std::size_t align>
  struct xaligned_storage<size, align, /*valid=*/true, /*extended=*/false> {
    typename std::aligned_storage<(size + align-1) & -align, align>::type storage_;

    void const* storage() const noexcept { return &storage_; }
    void*       storage()       noexcept { return &storage_; }
  };
  
  template<std::size_t size, std::size_t align>
  struct xaligned_storage<size, align, /*valid=*/true, /*extended=*/true> {
    char xbuf_[size + align-1];
    
    void const* storage() const noexcept {
      std::uintptr_t u = reinterpret_cast<std::uintptr_t>(&xbuf_);
      return &xbuf_[-u & (align-1)];
    }
    void*       storage()       noexcept {
      std::uintptr_t u = reinterpret_cast<std::uintptr_t>(&xbuf_);
      return &xbuf_[-u & (align-1)];
    }

    xaligned_storage() noexcept = default;
    xaligned_storage(xaligned_storage const &that) noexcept {
      detail::memcpy_aligned<align>(this->storage(), that.storage(), size);
    }
    xaligned_storage& operator=(xaligned_storage const &that) noexcept {
      if(this != &that)
        detail::memcpy_aligned<align>(this->storage(), that.storage(), size);
      return *this;
    }
  };

  //////////////////////////////////////////////////////////////////////////////
  // detail::raw_storage<T>: Like std::aligned_storage<>::type, except more convenient.
  // The typed value exists in the `value()` member, but isnt implicitly
  // constructed. Construction should be done by user with placement new like:
  //   `::new(&my_storage) T(...)`.
  // Also, the value won't be implicilty destructed either. That too is the user's
  // responsibility.

  template<typename T>
  class raw_storage {
    typename std::aligned_storage<sizeof(T), alignof(T)>::type raw_;

  public:
    void* raw() noexcept {
      return &raw_;
    }
    
    T& value() noexcept {
      return *detail::launder(reinterpret_cast<T*>(&raw_));
    }
    
    // Invoke value's destructor.
    void destruct() noexcept {
      detail::destruct(value());
    }

    // Move value out into return value and destruct a temporary and destruct it.
    T value_and_destruct() noexcept {
      T &val = this->value();
      T ans(std::move(val));
      detail::destruct(val);
      return ans;
    }
  };
  
  //////////////////////////////////////////////////////////////////////
  // trait_forall: logical conjunction of one trait applied to
  // variadically-many argument types.

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
  // trait_any: disjunction, combines multiple traits into a new trait.
  
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
  // trait_all: conjunction, combines multiple traits into a new trait
  
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

  namespace help {
    template<int n, int ...s>
    struct make_index_sequence: make_index_sequence<n-1, n-1, s...> {};

    template<int ...s>
    struct make_index_sequence<0, s...> {
      typedef index_sequence<s...> type;
    };
  }
  
  template<int n>
  using make_index_sequence = typename help::make_index_sequence<n>::type;

  //////////////////////////////////////////////////////////////////////////////
  // add_lref_if_nonref: Add a lvalue-reference (&) to type T if T isn't already
  // a reference (& or &&) type.
  
  template<typename T>
  struct add_lref_if_nonref { using type = T&; };
  
  template<typename T>
  struct add_lref_if_nonref<T&> { using type = T&; };
  
  template<typename T>
  struct add_lref_if_nonref<T&&> { using type = T&&; };

  //////////////////////////////////////////////////////////////////////////////
  // add_clref_if_nonref: Add a const-lvalue-reference (const &) to type T if T
  // isn't already a reference (& or &&) type.
  
  template<typename T>
  struct add_clref_if_nonref { using type = T const&; };
  
  template<typename T>
  struct add_clref_if_nonref<T&> { using type = T&; };
  
  template<typename T>
  struct add_clref_if_nonref<T&&> { using type = T&&; };

  //////////////////////////////////////////////////////////////////////////////
  // add_rref_if_nonref: Add a rvalue-reference (&&) to type T if T isn't
  // already a reference (& or &&) type.
  
  template<typename T>
  struct add_rref_if_nonref { using type = T&&; };
  
  template<typename T>
  struct add_rref_if_nonref<T&> { using type = T&; };
  
  template<typename T>
  struct add_rref_if_nonref<T&&> { using type = T&&; };
  
  //////////////////////////////////////////////////////////////////////

  #if 0
  template<typename Tup>
  struct decay_tupled;
  template<typename ...T>
  struct decay_tupled<std::tuple<T...>> {
    typedef std::tuple<typename std::decay<T>::type...> type;
  };
  #endif
  
  //////////////////////////////////////////////////////////////////////
  // get_or_void & tuple_element_or_void: analogs of std::get &
  // std::tuple_elemenet which return void for out-of-range indices

  #if 0
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
  
  template<int i, typename Tup>
  auto get_or_void(Tup &&tup)
    -> decltype(
      tuple_get_or_void<i,Tup>()(std::forward<Tup>(tup))
    ) {
    return tuple_get_or_void<i,Tup>()(std::forward<Tup>(tup));
  }
  #endif
  
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
  
  namespace help {
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
      help::tuple_lrefs(
        std::forward<Tup>(tup),
        make_index_sequence<std::tuple_size<typename std::decay<Tup>::type>::value>()
      )
    ) {
    return help::tuple_lrefs(
      std::forward<Tup>(tup),
      make_index_sequence<std::tuple_size<typename std::decay<Tup>::type>::value>()
    );
  }
  
  //////////////////////////////////////////////////////////////////////
  // tuple_rvals: Get a tuple of rvalue-references to tuple componenets.
  // Components which are already `&` or `&&` are returned unmodified.
  // Non-reference componenets are returned as `&&` only if the tuple is
  // passed by non-const `&`, otherwise the non-reference type is used
  // and the value is moved or copied from the input to output tuple.
  
  namespace help {
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
      help::tuple_rvals(
        std::forward<Tup>(tup),
        make_index_sequence<std::tuple_size<typename std::decay<Tup>::type>::value>()
      )
    ) {
    return help::tuple_rvals(
      std::forward<Tup>(tup),
      make_index_sequence<std::tuple_size<typename std::decay<Tup>::type>::value>()
    );
  }
  
  //////////////////////////////////////////////////////////////////////
  // forward_as_tuple_decay_rrefs: like std::forward_as_tuple, but drops
  // rvalue-refs (&&) from components, lvalues (&, const&) are preserved.
  
  template<typename ...T>
  std::tuple<T...> forward_as_tuple_decay_rrefs(T &&...refs) {
    return std::tuple<T...>(static_cast<T&&>(refs)...);
  }
    
  //////////////////////////////////////////////////////////////////////
  // apply_tupled: Apply a callable against an argument list wrapped
  // in a tuple.
  
  namespace help {
    template<typename Fn, typename Tup, int ...i>
    inline auto apply_tupled(
        Fn &&fn, Tup &&args, index_sequence<i...>
      )
      -> decltype(static_cast<Fn&&>(fn)(std::get<i>(static_cast<Tup&&>(args))...)) {
      return static_cast<Fn&&>(fn)(std::get<i>(static_cast<Tup&&>(args))...);
    }
  }
  
  template<typename Fn, typename Tup>
  inline auto apply_tupled(Fn &&fn, Tup &&args)
    -> decltype(
      help::apply_tupled(
        std::forward<Fn>(fn), std::forward<Tup>(args),
        make_index_sequence<std::tuple_size<Tup>::value>()
      )
    ) {
    return help::apply_tupled(
      std::forward<Fn>(fn), std::forward<Tup>(args),
      make_index_sequence<std::tuple_size<Tup>::value>()
    );
  }

  //////////////////////////////////////////////////////////////////////
  // align_of<T>(): returns alignof(T) or 0 for incomplete types

  template <typename T, std::size_t align = alignof(T)>
  constexpr size_t align_of_(T *_) { return align; }
  constexpr size_t align_of_(...) { return 0; }

  template<typename T>
  constexpr size_t align_of() { return align_of_((T*)nullptr); }

  //////////////////////////////////////////////////////////////////////
  // typename_of<T>(): returns typeid(T).name() or null for incomplete types

  template <typename T, std::size_t x = sizeof(T)>
  inline const char *typename_of_(T *_) { 
    #if UPCXX_HAVE_RTTI
      return typeid(T).name(); 
    #else
      return "";
    #endif
  }
  inline const char *typename_of_(...) { return nullptr; }

  template<typename T>
  inline const char *typename_of() { return typename_of_((T*)nullptr); }

} // namespace detail
} // namespace upcxx
#endif
