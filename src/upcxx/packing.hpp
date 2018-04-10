#ifndef _8cd0d114_b731_411b_bb0e_36e3c846aab0
#define _8cd0d114_b731_411b_bb0e_36e3c846aab0

#include <upcxx/diagnostic.hpp>
#include <upcxx/parcel.hpp>
#include <upcxx/reflection.hpp>
#include <upcxx/utility.hpp>

#include <cstddef>
#include <cstdint>
#include <cstring>

#include <tuple>
#include <type_traits>
#include <utility>

// Define this to disable packing support for the std:: containers since this
// bloats the translation unit and bogs down the tool `creduce`.
//#define CREDUCING

#ifndef CREDUCING
  #include <array>
  #include <deque>
  #include <forward_list>
  #include <list>
  #include <map>
  #include <set>
  #include <string>
  #include <unordered_map>
  #include <unordered_set>
  #include <vector>
#endif

/* "Packing" is what I'm calling serialization for now so as not to
 * collide with boost::serialization. Since we'll support both
 * (with boost being a shim mapping back here), and since boost uses
 * ADL which does not respect namespaces well, I consider the term
 * "serialize" owned by boost's semantics. EXPERIMENTS SHOULD BE
 * CONDUCTED TO SEE IF THIS NAME AVOIDANCE IS NECESSARY.
 */

/* TODO: This is more permissive then we want. We aren't demanding
 * things without explicit packing support be trivially_copyable and
 * we byte-copy regardless.
 * UPDATE: At least one very trivial [=] lambda is not reporting to be
 * trivially_copyable by my GCC 5.4 so... welcome to the wild west.
 * Every type unknown to this header file is assumed to be trivially
 * serializable.
 */

namespace upcxx {
  //////////////////////////////////////////////////////////////////////////////
  // packing<T>: Class of static hooks for packing/unpacking a value of
  // T into/outof a parcel. May be specialized by user. Default case
  // inspects type traits of T to determine best packing strategy.
  template<typename T>
  struct packing /*{
    // Is this type definitely packable? Absent value interpreted as false.
    static constexpr bool is_definitely_supported;

    // A "tight" ubound is completely static and leaves no wasted space.
    // Formally, this means packing a value onto an empty parcel grows parcel
    // to size and alignment exactly matching `ubound` regardless of `skippable`.
    // Absent value interpreted as false.
    static constexpr bool is_ubound_tight;

    // Is this equivalent to an aligned memcpy of the values bytes? Absence
    // interpreted as false. Note, this *is not* how a packing class should
    // request that it be implemented with memcpy. This indicates whether the
    // implementation which is already present is equivalent to memcpy.
    static constexpr bool is_trivial;

    // In the following calls, `skippable` is a compile-time boolean indicating
    // if this value needs to be packed with an encoding yield efficient
    // skipping. Mismatched `skippable` for pack/unpack is UB.
    
    // Compute an upper-bound on the size of the packed message, no
    // side-effects. `ub` is initial size, return must be equal or larger.
    template<bool skippable>
    static parcel_size<?,?> ubound(parcel_size<?,?> ub, T const &x, std::integral_constant<bool,skippable>);
    
    // Pack the value into the parcel.
    template<bool skippable>
    static void pack(parcel_writer &w, T const &x, std::integral_constant<bool,skippable>);

    // Unpacking may return a type U not equal to T. To indicate this,
    // define the type unpacker_t = U. The unpacking<T> class will then
    // defer to packing<U>.
    using unpacked_t = U; // if absent defaults to T

    // Assuming the next value on the reader was packed by `pack<skippable=true>`,
    // skip over it. This assumption changes when this packing class is used as
    // the `Dumb` argument to `detail::packing_skippable_smart`, then the
    // assumption is that it was packed with `skippable=false`.
    static void skip(parcel_reader &r);
    
    // Read value out of parcel and construct into `into`. Only called when T=U
    // since `unpacking<T>::unpack` actually calls `packing<U>::unpack`.
    template<bool skippable>
    static void unpack(parcel_reader &r, void *into, std::integral_constant<bool,skippable>);
  }*/;

  //////////////////////////////////////////////////////////////////////////////
  // unpacked_of_t<T>: Type alias for the `unpacked_t` typedef from `packing<T>`
  // if it exists, otherwise `T`.

  template<typename T,
           typename PackingOfT = packing<T>,
           typename = void>
  struct unpacked_of {
    using type = T;
  };
  
  template<typename T, typename PackingOfT>
  struct unpacked_of<
      T, PackingOfT,
      typename std::conditional<true, void, typename PackingOfT::unpacked_t>::type
    > {
    using type = typename PackingOfT::unpacked_t;
  };

  template<typename T, typename PackingOfT = packing<T>>
  using unpacked_of_t = typename unpacked_of<T,PackingOfT>::type;

  //////////////////////////////////////////////////////////////////////////////
  // unpacking<T>: Use this to to unpack a T (via unpacking<T>::unpack())
  // since packing<T>::unpack isn't the right unpacker if unpacked_t != T.

  template<typename T>
  struct unpacking: packing<unpacked_of_t<T>> {};

  //////////////////////////////////////////////////////////////////////////////
  // forward declarations
  
  template<typename T>
  struct is_definitely_trivially_serializable;
  template<typename T>
  struct is_definitely_serializable;
  
  // Packing implementations for T specialize this to `std::true_type` if they
  // want `packing_trivial` to be used instead of their specialization of
  // `packing_screened<T>`.
  template<typename T>
  struct packing_screen_trivial: is_definitely_trivially_serializable<T> {};

  // The packing implementation used if screening failed.
  template<typename T>
  struct packing_screened;
  
  //////////////////////////////////////////////////////////////////////////////
  // packing_is_trivial: Whether packing is just a shallow byte copy.
  
  template<typename T, typename false_ = std::false_type>
  struct packing_is_trivial {
    static constexpr bool value = false;
  };
  template<typename T>
  struct packing_is_trivial<T, std::integral_constant<bool, false & packing<T>::is_trivial>> {
    static constexpr bool value = packing<T>::is_trivial;
  };

  //////////////////////////////////////////////////////////////////////////////
  // packing_is_ubound_tight: Whether packing for T computes a ubound that is
  // both a fully static parcel_size and wastes no space (pack will use all the
  // memory claimed by ubound).
  
  template<typename T,
           typename PackingOfT = packing<T>,
           typename false_ = std::false_type>
  struct packing_is_ubound_tight: std::false_type {};
  
  template<typename T, typename PackingOfT>
  struct packing_is_ubound_tight<
      T, PackingOfT,
      std::integral_constant<bool, false & PackingOfT::is_ubound_tight>
    > {
    static constexpr bool value = PackingOfT::is_ubound_tight;
  };
  
  ////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
  // packing_is_opaque

  template<typename T,
           typename PackingOfT = packing<T>,
           typename false_ = std::false_type>
  struct packing_is_opaque: std::false_type {};

  template<typename T, typename PackingOfT>
  struct packing_is_opaque<
      T, PackingOfT,
      std::integral_constant<bool, false & PackingOfT::is_opaque>
    > {
    static constexpr bool value = PackingOfT::is_opaque;
  };
  
  ////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
  // packing_is_definitely_supported

  namespace detail {
    template<typename PackingOfT, typename false_ = std::false_type>
    struct packing_is_definitely_supported__from_member:
      // We assume support is true if `is_definitely_supported` isn't defined
      // because that means we're in a user-speicialization
      std::true_type {
    };
    template<typename PackingOfT>
    struct packing_is_definitely_supported__from_member<
        PackingOfT,
        std::integral_constant<bool, false & PackingOfT::is_definitely_supported>
      >:
      std::integral_constant<bool, PackingOfT::is_definitely_supported> {
    };
  }
  
  template<typename T, typename PackingOfT = packing<T>>
  struct packing_is_definitely_supported:
    detail::packing_is_definitely_supported__from_member<PackingOfT> {
  };
  
  //////////////////////////////////////////////////////////////////////////////
  // packing_is_owning: Whether the value type does not reference the
  // underlying parcel buffer.
  
  template<typename T,
           typename PackingOfT = packing<T>,
           typename false_ = std::false_type>
  struct packing_is_owning {
    static constexpr bool value = true;
  };
  template<typename T, typename PackingOfT>
  struct packing_is_owning<
      T, PackingOfT,
      std::integral_constant<bool, false & PackingOfT::is_owning>
    > {
    static constexpr bool value = PackingOfT::is_owning;
  };

  //////////////////////////////////////////////////////////////////////////////
  // Serialization facade to underlying packing infrastructure.
  
  template<typename T>
  struct is_definitely_trivially_serializable {
    static constexpr bool value =
      packing_is_opaque<T, packing_screened<T>>::value &&
      std::is_trivially_copyable<T>::value;
  };

  template<typename T>
  struct is_definitely_serializable {
    static constexpr bool value =
      is_definitely_trivially_serializable<T>::value ||
      packing_is_definitely_supported<T>::value;
  };

  ////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
  // packing_not_supported
  
  template<typename T>
  struct packing_not_supported {
    static constexpr bool is_definitely_supported = false;
    
    template<typename ParcelSize, bool skippable>
    static ParcelSize ubound(ParcelSize ub, T const &x, std::integral_constant<bool, skippable>) {
      return ub;
    }

    // other members purposely not not defined...
  };
  
  //////////////////////////////////////////////////////////////////////////////
  // packing_empty: Packing implementation for empty types.
  
  template<typename T,
           bool is_default_constructible = std::is_default_constructible<T>::value>
  struct packing_empty;
  
  template<typename T>
  struct packing_empty<T, /*is_default_constructible=*/true> {
    static constexpr bool is_definitely_supported = true;
    static constexpr bool is_owning = true;
    static constexpr bool is_trivial = false;
    static constexpr bool is_ubound_tight = true;
    
    template<typename Ub, bool skippable>
    static Ub ubound(Ub ub, T const&, std::integral_constant<bool,skippable>) {
      return ub;
    }

    template<bool skippable>
    static void pack(parcel_writer &w, const T &x, std::integral_constant<bool,skippable>) {}

    static void skip(parcel_reader &r) {}

    template<bool skippable>
    static void unpack(parcel_reader &r, void *into, std::integral_constant<bool,skippable>) {
      ::new(into) T;
    }
  };
  
  template<typename T>
  struct packing_empty<T, /*is_default_constructible=*/false> {
    static constexpr bool is_definitely_supported = true;
    static constexpr bool is_owning = true;
    static constexpr bool is_trivial = true;
    static constexpr bool is_ubound_tight = true;
    
    template<typename Ub, bool skippable>
    static Ub ubound(Ub ub, const T&, std::integral_constant<bool,skippable>) {
      return ub;
    }

    template<bool skippable>
    static void pack(parcel_writer &w, const T &x, std::integral_constant<bool,skippable>) {}

    static void skip(parcel_reader &r) {}

    template<bool skippable>
    static void unpack(parcel_reader &r, void *into, std::integral_constant<bool,skippable>) {}
  };
  
  //////////////////////////////////////////////////////////////////////////////
  // packing_trivial: Packs byte-wise satisfying alignment.
  
  template<typename T>
  struct packing_trivial {
    static constexpr bool is_definitely_supported = true;
    static constexpr bool is_owning = true;
    static constexpr bool is_trivial = true;
    static constexpr bool is_ubound_tight = true;
    
    template<typename Ub, bool skippable>
    static auto ubound(Ub ub, const T&, std::integral_constant<bool,skippable>) ->
      decltype(ub.template trivial_added<T>()) {
      return ub.template trivial_added<T>();
    }

    template<bool skippable>
    static void pack(parcel_writer &w, const T &x, std::integral_constant<bool,skippable>) {
      w.put_trivial_aligned(x);
    }
    
    static void skip(parcel_reader &r) {
      r.pop_trivial_aligned<T>();
    }

    template<bool skippable>
    static void unpack(parcel_reader &r, void *into, std::integral_constant<bool,skippable>) {
      ::new(into) T{r.pop_trivial_aligned<T>()};
    }
  };
  
  //////////////////////////////////////////////////////////////////////////////
  // packing_opaque: How to pack a type we know nothing about.
  
  template<typename T,
           bool is_empty = std::is_empty<T>::value,
           bool is_trivially_copyable = std::is_trivially_copyable<T>::value>
  struct packing_opaque;
  
  template<typename T, bool is_trivially_copyable>
  struct packing_opaque<T, /*is_empty=*/true, is_trivially_copyable>:
    packing_empty<T> {

    static constexpr bool is_opaque = true;
    // override base class
    static constexpr bool is_definitely_supported = false;
  };
  
  template<typename T>
  struct packing_opaque<T, /*is_empty=*/false, /*is_trivially_copyable=*/true>:
    packing_trivial<T> {

    static constexpr bool is_opaque = true;
    // override base class
    static constexpr bool is_definitely_supported = false;
  };
  
  template<typename T>
  struct packing_opaque<T, /*is_empty=*/false, /*is_trivially_copyable=*/false>:
    // This is where we would refuse to serialize a non trivially-
    // copyable type, except that it is implementation defined whether
    // lambdas get this property. And in fact my GCC 5.4 does not make
    // some pretty simple [=] capturing lambdas trivial.
    // https://stackoverflow.com/questions/32986193/when-is-a-lambda-trivial
    #if 1
      packing_trivial<T>
    #else
      packing_not_supported<T> {
    #endif
    {
    static constexpr bool is_opaque = true;
    // override base class
    static constexpr bool is_definitely_supported = false;
  };
  
  //////////////////////////////////////////////////////////////////////////////
  // detail::packing_skippable_smart<T,Dumb>: wraps Dumb (defaults to
  // detail::packing_skippable_dumb<T>) with extra metadata (one word) for fast
  // skips. If the underlying dumb packing has tight ubounds, then the extra word
  // is elided since the bound can be used as the skip distance.

  namespace detail {
    template<typename T>
    struct packing_skippable_dumb;
  
    template<typename T, typename Dumb = packing_skippable_dumb<T>,
             bool is_ubound_tight = packing_is_ubound_tight<T,Dumb>::value>
    struct packing_skippable_smart;

    template<typename T, typename Dumb>
    struct packing_skippable_smart<T, Dumb, /*is_ubound_tight=*/true>:
      Dumb {
    };

    template<typename T, typename Dumb>
    struct packing_skippable_smart<T, Dumb, /*is_ubound_tight=*/false> {
      static constexpr bool is_definitely_supported = packing_is_definitely_supported<T,Dumb>::value;
      static constexpr bool is_owning = packing_is_owning<T,Dumb>::value;
      static constexpr bool is_ubound_tight = false;

      template<typename Ub>
      static constexpr auto ubound(Ub ub, T const &x, std::false_type skippable) ->
        decltype(Dumb::ubound(ub, x, std::false_type())) {
        return Dumb::ubound(ub, x, std::false_type());
      }
      
      template<typename Ub>
      static constexpr auto ubound(Ub ub, T const &x, std::true_type skippable) ->
        decltype(Dumb::ubound(ub.template trivial_added<std::size_t>(), x, std::false_type())) {
        return Dumb::ubound(ub.template trivial_added<std::size_t>(), x, std::false_type());
      }

      static void pack(parcel_writer &w, T const &x, std::false_type skippable) {
        Dumb::pack(w, x, std::false_type());
      }

      static void pack(parcel_writer &w, T const &x, std::true_type skippable) {
        std::size_t *delta = w.place_trivial_aligned<std::size_t>();
        std::size_t size0 = w.size();
        
        Dumb::pack(w, x, std::false_type());

        *delta = w.size() - size0;
      }

      using unpacked_t = unpacked_of_t<T,Dumb>;

      static void skip(parcel_reader &r) {
        std::size_t delta = r.pop_trivial_aligned<std::size_t>();
        r.jump(delta);
      }

      template<bool skippable>
      static void unpack(parcel_reader &r, void *into, std::integral_constant<bool,skippable>) {
        if(skippable)
          r.pop_trivial_aligned<std::size_t>();

        Dumb::unpack(r, into, std::false_type());
      }
    };
  }
  
  //////////////////////////////////////////////////////////////////////////////
  // packing_reflected: Packing a type by inductively packing each of its members
  // as reported through upcxx::reflection.

  namespace detail {
    // Reflection visitor for accumulating is_definitely_supported and is_ubound_tight.
    template<bool is_definitely_supported1=true,
             bool is_owning1=true,
             bool is_ubound_tight1=true>
    struct packing_meta_reflector {
      static constexpr bool is_definitely_supported = is_definitely_supported1;
      static constexpr bool is_owning = is_owning1;
      static constexpr bool is_ubound_tight = is_ubound_tight1;
      
      template<typename T>
      auto operator&(const T &mbr) const ->
        packing_meta_reflector<
          is_definitely_supported1 && packing_is_definitely_supported<T>::value,
          is_owning1 && packing_is_owning<T>::value,
          is_ubound_tight1 && packing_is_ubound_tight<T>::value
        > {
        return {};
      }

      template<typename T>
      auto opaque(const T &mbr) const ->
        packing_meta_reflector<
          is_definitely_supported1 && /*false=*/packing_opaque<T>::is_definitely_supported,
          is_owning1 && packing_is_owning<T>::value,
          is_ubound_tight1 && packing_opaque<T>::is_ubound_tight
        > {
        return {};
      }
    };
    
    // Reflection visitor for calling packing::ubound.
    template<typename ParcelSize>
    struct packing_ubound_reflector {
      ParcelSize ub;
      
      template<typename T>
      auto operator&(const T &mbr) const ->
        packing_ubound_reflector<
          decltype(packing<T>::ubound(ub, mbr, /*skippable=*/std::false_type()))
        > {
        return {packing<T>::ubound(ub, mbr, /*skippable=*/std::false_type())};
      }
      
      template<typename T>
      auto opaque(const T &mbr) const ->
        packing_ubound_reflector<
          decltype(packing_opaque<T>::ubound(ub, mbr, /*skippable=*/std::false_type()))
        > {
        return {packing_opaque<T>::ubound(ub, mbr, /*skippable=*/std::false_type())};
      }
    };

    // Reflection visitor for calling packing::pack.
    struct packing_pack_reflector {
      parcel_writer &w;
      
      template<typename T>
      packing_pack_reflector operator&(const T &mbr) const {
        packing<T>::pack(w, mbr, /*skippable=*/std::false_type());
        return *this;
      }
      
      template<typename T>
      packing_pack_reflector opaque(const T &mbr) const {
        packing_opaque<T>::pack(w, mbr, /*skippable=*/std::false_type());
        return *this;
      }
    };

    // Reflection visitor for calling packing::skip.
    struct packing_skip_reflector {
      parcel_reader &r;
      
      template<typename T>
      packing_skip_reflector operator&(T &mbr) {
        packing<T>::skip(r);
        return *this;
      }
      template<typename T>
      packing_skip_reflector opaque(T &mbr) {
        packing_opaque<T>::skip(r);
        return *this;
      }
    };

    // Reflection visitor for calling packing::unpack.
    template<bool constructed>
    struct packing_unpack_reflector;
    
    template<>
    struct packing_unpack_reflector</*constructed=*/true> {
      parcel_reader &r;
      
      template<typename T>
      packing_unpack_reflector operator&(T &mbr) {
        mbr.~T();
        packing<T>::unpack(r, &mbr, /*skippable=*/std::false_type());
        return *this;
      }
      template<typename T>
      packing_unpack_reflector opaque(T &mbr) {
        mbr.~T();
        packing_opaque<T>::unpack(r, &mbr, /*skippable=*/std::false_type());
        return *this;
      }
    };
    template<>
    struct packing_unpack_reflector</*constructed=*/false> {
      parcel_reader &r;
      
      template<typename T>
      packing_unpack_reflector operator&(T &mbr) {
        packing<T>::unpack(r, &mbr, /*skippable=*/std::false_type());
        return *this;
      }
      template<typename T>
      packing_unpack_reflector opaque(T &mbr) {
        packing_opaque<T>::unpack(r, &mbr, /*skippable=*/std::false_type());
        return *this;
      }
    };
    
    template<typename T>
    struct packing_reflected_base:
      decltype(reflection<T>()(packing_meta_reflector<>(), std::declval<T&>())) {

      // inherits is_definitely_supported
      // inherits is_owning
      // inherits is_ubound_tight

      template<typename Ub, bool skippable>
      static auto ubound(Ub ub, const T &x, std::integral_constant<bool,skippable>) ->
        decltype(reflect_upon(packing_ubound_reflector<Ub>{ub}, x).ub) {
        return reflect_upon(packing_ubound_reflector<Ub>{ub}, x).ub;
      }

      template<bool skippable>
      static void pack(parcel_writer &w, const T &x, std::integral_constant<bool,skippable>) {
        reflect_upon(packing_pack_reflector{w}, x);
      }

      static void skip(parcel_reader &r) {
        raw_storage<T> dummy;
        reflect_upon(packing_skip_reflector{r}, dummy.value);
      }
    };
    
    template<typename T,
             bool is_default_constructible = std::is_default_constructible<T>::value>
    struct packing_reflected;

    template<typename T>
    struct packing_reflected<T, /*is_default_constructible=*/true>:
      packing_reflected_base<T> {

      template<bool skippable>
      static void unpack(parcel_reader &r, void *into, std::integral_constant<bool,skippable>) {
        reflect_upon(packing_unpack_reflector</*constructed=*/true>{r}, *(T*)into);
      }
    };
    
    template<typename T>
    struct packing_reflected<T, /*is_default_constructible=*/false>:
      packing_reflected_base<T> {
      
      template<bool skippable>
      static void unpack(parcel_reader &r, void *into, std::integral_constant<bool,skippable>) {
        /* This is so scary. We never actually call T's constructor, just
         * its reflected members' constructors. Therefor, only use
         * reflection-based unpacking on non-default-constructible types
         * if the uncalled constructors (those for T and the non-reflected
         * members, if any) are trivial.
         */
        reflect_upon(packing_unpack_reflector</*constructed=*/false>{r}, *(T*)into);
      }
    };
  }
  
  //////////////////////////////////////////////////////////////////////////////
  // The default strategy for packing<T>

  namespace detail {
    template<typename T,
      bool is_empty = std::is_empty<T>::value,
      bool is_trivial = std::is_scalar<T>::value || packing_screen_trivial<T>::value>
    struct packing_specialize;
    
    template<typename T, bool is_trivial>
    struct packing_specialize<T, /*is_empty=*/true, is_trivial>:
      packing_empty<T> {
    };
    template<typename T>
    struct packing_specialize<T, /*is_empty=*/false, /*is_trivial=*/true>:
      packing_trivial<T> {
    };
    template<typename T>
    struct packing_specialize<T, /*is_empty=*/false, /*is_trivial=*/false>:
      packing_screened<T> {
    };

    template<typename T,
      bool is_opaque = upcxx::reflection<T>::is_opaque>
    struct packing_screened_specialize;

    template<typename T>
    struct packing_screened_specialize<T, /*is_opaque=*/true>:
      packing_opaque<T> {
    };
    template<typename T>
    struct packing_screened_specialize<T, /*is_opaque=*/false>:
      detail::packing_skippable_smart<T, detail::packing_reflected<T>> {
    };
  }

  template<typename T>
  struct packing: detail::packing_specialize<T> {};
  
  template<typename T>
  struct packing_screened: detail::packing_screened_specialize<T> {};
  
  template<typename T>
  struct packing<T const>: packing<T> {};
  template<typename T>
  struct packing<T volatile>: packing<T> {};
  
  template<typename T>
  struct packing<T&>: packing_not_supported<T&> {};
  template<typename T>
  struct packing<T&&>: packing_not_supported<T&&> {};
  
  //////////////////////////////////////////////////////////////////////////////
  // packing<R(&)(A...)>: reference to function

  template<typename R, typename ...A>
  struct packing<R(&)(A...)> {
    static constexpr bool is_definitely_supported = true;
    static constexpr bool is_trivial = false;
    static constexpr bool is_ubound_tight = true;
    
    template<typename Ub, bool skippable>
    static auto ubound(Ub ub, R(&x)(A...), std::integral_constant<bool,skippable>) ->
      decltype(ub.template trivial_added<R(*)(A...)>()) {
      return ub.template trivial_added<R(*)(A...)>();
    }

    template<bool skippable>
    static void pack(parcel_writer &w, R(&x)(A...), std::integral_constant<bool,skippable>) {
      w.put_trivial_aligned<R(*)(A...)>(&x);
    }

    static void skip(parcel_reader &r) {
      r.pop_trivial_aligned<R(*)(A...)>();
    }

    template<bool skippable>
    static void unpack(parcel_reader &r, void *into, std::integral_constant<bool,skippable>) {
      using fn_t = R(A...);
      struct box_t { fn_t &r; };
      ::new(into) box_t{*r.pop_trivial_aligned<fn_t*>()};
    }
  };

  //////////////////////////////////////////////////////////////////////////////
  // packing<std::tuple>
  
  namespace detail {
    template<int n, int i, typename ...T>
    struct packing_tuple_each {
      typedef typename std::tuple_element<i,std::tuple<T...>>::type Ti;

      static constexpr bool is_owning =
        packing_is_owning<Ti>::value && packing_tuple_each<n, i+1, T...>::is_owning;

      static constexpr bool is_ubound_tight =
        packing_is_ubound_tight<Ti>::value && packing_tuple_each<n, i+1, T...>::is_ubound_tight;

      template<typename Ub, bool skippable>
      static auto ubound(Ub ub, const std::tuple<T...> &x, std::integral_constant<bool,skippable>)
        -> decltype(
          packing_tuple_each<n, i+1, T...>::ubound(
            packing<Ti>::ubound(ub, std::get<i>(x), std::false_type()),
            x,
            std::false_type()
          )
        ) {
        return packing_tuple_each<n, i+1, T...>::ubound(
          packing<Ti>::ubound(ub, std::get<i>(x), std::false_type()),
          x,
          std::false_type()
        );
      }
      
      template<bool skippable>
      static void pack(parcel_writer &w, const std::tuple<T...> &x, std::integral_constant<bool,skippable>) {
        packing<Ti>::pack(w, std::get<i>(x), std::false_type());
        packing_tuple_each<n, i+1, T...>::pack(w, x, std::false_type());
      }
      
      static void skip(parcel_reader &r) {
        packing<Ti>::skip(r);
        packing_tuple_each<n, i+1, T...>::skip(r);
      }
      
      template<typename Storage>
      static void unpack_into(parcel_reader &r, Storage &storage) {
        packing<Ti>::unpack(r, &std::get<i>(storage), std::false_type());
        packing_tuple_each<n, i+1, T...>::unpack_into(r, storage);
      }
    };
    
    template<int n, typename ...T>
    struct packing_tuple_each<n, n, T...> {
      static constexpr bool is_owning = true;
      static constexpr bool is_ubound_tight = true;
      
      template<typename Ub, bool skippable>
      static Ub ubound(Ub ub, const std::tuple<T...> &x, std::integral_constant<bool,skippable>) {
        return ub;
      }
      
      template<bool skippable>
      static void pack(parcel_writer &w, const std::tuple<T...> &x, std::integral_constant<bool,skippable>) {}

      static void skip(parcel_reader &r) {}
      
      template<typename Storage>
      static void unpack_into(parcel_reader &r, Storage&) {}
    };

    template<typename ...T>
    struct packing_skippable_dumb<std::tuple<T...>>:
      detail::packing_tuple_each<sizeof...(T), 0, T...> {

      // inherits is_owning, is_ubound_tight
      // inherits ubound()
      // inherits pack()

      using unpacked_t = std::tuple<unpacked_of_t<T>...>;

      // inherits skip()
      
      template<int ...i>
      static void take_from_storage(
          std::tuple<raw_storage<T>...> &storage,
          void *into,
          upcxx::index_sequence<i...>
        ) {
        ::new(into) std::tuple<T...>{std::get<i>(storage).value_and_destruct()...};
      }
      
      template<bool skippable>
      static void unpack(parcel_reader &r, void *into, std::integral_constant<bool,skippable>) {
        std::tuple<raw_storage<T>...> storage;
        
        detail::packing_tuple_each<sizeof...(T), 0, T...>::unpack_into(r, storage);
        
        take_from_storage(storage, into, upcxx::make_index_sequence<sizeof...(T)>());
      }
    };
  }

  template<typename ...T>
  struct packing_screen_trivial<std::tuple<T...>>:
    upcxx::trait_forall<packing_is_trivial, T...> {};
  
  template<typename ...T>
  struct is_definitely_trivially_serializable<std::tuple<T...>>:
    upcxx::trait_forall<is_definitely_trivially_serializable, T...> {};

  template<typename ...T>
  struct packing_is_definitely_supported<
      std::tuple<T...>, packing<std::tuple<T...>>
    >:
    upcxx::trait_forall<packing_is_definitely_supported, T...> {};

  template<typename ...T>
  struct packing_screened<std::tuple<T...>>:
    detail::packing_skippable_smart<std::tuple<T...>> {};

  #ifndef CREDUCING
  
  //////////////////////////////////////////////////////////////////////////////
  // packing<std::pair>

  template<typename A, typename B>
  struct packing_screen_trivial<std::pair<A,B>> {
    static constexpr bool value = packing_is_trivial<A>::value && packing_is_trivial<B>::value;
  };
  
  template<typename A, typename B>
  struct is_definitely_trivially_serializable<std::pair<A,B>> {
    static constexpr bool value =
      is_definitely_trivially_serializable<A>::value &&
      is_definitely_trivially_serializable<B>::value;
  };

  template<typename A, typename B, typename PackingOfT>
  struct packing_is_definitely_supported<std::pair<A,B>, PackingOfT> {
    static constexpr bool value =
      packing_is_definitely_supported<A>::value &&
      packing_is_definitely_supported<B>::value;
  };
  
  namespace detail {
    template<typename A, typename B>
    struct packing_skippable_dumb<std::pair<A,B>> {
      static constexpr bool is_owning = packing_is_owning<A>::value
                                     && packing_is_owning<B>::value;
      
      static constexpr bool is_ubound_tight = packing_is_ubound_tight<A>::value
                                           && packing_is_ubound_tight<B>::value;
      
      template<typename Ub, bool skippable>
      static auto ubound(Ub ub, const std::pair<A,B> &x, std::integral_constant<bool,skippable>)
        -> decltype(packing<B>::ubound(
          packing<A>::ubound(ub, x.first, std::false_type()),
          x.second, std::false_type()
        )) {
        return packing<B>::ubound(
          packing<A>::ubound(ub, x.first, std::false_type()),
          x.second, std::false_type()
        );
      }

      template<bool skippable>
      static void pack(parcel_writer &w, const std::pair<A,B> &x, std::integral_constant<bool,skippable>) {
        packing<A>::pack(w, x.first, std::false_type());
        packing<B>::pack(w, x.second, std::false_type());
      }

      using unpacked_t = std::pair<unpacked_of_t<A>, unpacked_of_t<B>>;
      
      static void skip(parcel_reader &r) {
        packing<A>::skip(r);
        packing<B>::skip(r);
      }
      
      template<bool skippable>
      static void unpack(parcel_reader &r, void *into, std::integral_constant<bool,skippable>) {
        raw_storage<A> a;
        packing<A>::unpack(r, &a, std::false_type());
        
        raw_storage<B> b;
        packing<B>::unpack(r, &b, std::false_type());
        
        ::new(into) std::pair<A,B>{a.value_and_destruct(), b.value_and_destruct()};
      }
    };
  }
  
  template<typename A, typename B>
  struct packing_screened<std::pair<A,B>>:
    detail::packing_skippable_smart<std::pair<A,B>> {
  };
  
  //////////////////////////////////////////////////////////////////////////////
  // packing<std::array> & packing<T[]>

  namespace detail {
    template<typename T,
             typename T_Ub = decltype(packing<T>::ubound(parcel_size_empty(), std::declval<T&>(), std::false_type())),
             bool T_Ub_static = T_Ub::all_static>
    struct packing_sequence_base;

    template<typename T, typename T_Ub>
    struct packing_sequence_base<T, T_Ub, /*T_Ub_static=*/true> {
      //static constexpr std::size_t t_size = T_Ub::static_size;
      //static constexpr std::size_t t_align = T_Ub::static_align;

      template<typename Ub, typename Iter, bool elt_skippable>
      static auto ubound_elts(Ub ub, Iter begin, std::size_t n, std::integral_constant<bool,elt_skippable>) ->
        decltype(ub.added(T_Ub{}.arrayed(n))) {
        return ub.added(T_Ub{}.arrayed(n));
      }

      static void skip_elts(parcel_reader &r, std::size_t n) {
        if(n > 0) {
          packing<T>::skip(r);
          if(n > 1) {
            std::size_t head0 = r.head();
            packing<T>::skip(r);
            std::size_t delta = r.head() - head0;
            
            r.pop((n-2)*delta, 1);
          }
        }
      }
    };

    template<typename T, typename T_Ub>
    struct packing_sequence_base<T, T_Ub, /*T_Ub_static=*/false> {
      static constexpr std::size_t t_size = T_Ub::static_size;
      static constexpr std::size_t t_align = T_Ub::static_align;

      template<typename Ub, typename Iter, bool elt_skippable>
      static auto ubound_elts(Ub ub, Iter begin, std::size_t n, std::integral_constant<bool,elt_skippable> elt_skippable1) ->
        decltype(ub.type_size_weakened().template type_align_weakened<t_align>()) {
        
        auto ub1 = ub.type_size_weakened().template type_align_weakened<t_align>();
        while(n--) {
          ub1 = packing<T>::ubound(ub1, *begin, elt_skippable1);
          ++begin;
        }
        return ub1;
      }

      static void skip_elts(parcel_reader &r, std::size_t n) {
        while(n--)
          packing<T>::skip(r);
      }
    };

    template<typename T, bool trivial = packing_is_trivial<T>::value>
    struct packing_sequence;

    template<typename T>
    struct packing_sequence<T, /*trivial=*/true>:
      packing_sequence_base<T> {

      template<typename Iter, bool elt_skippable>
      static void pack_elts(parcel_writer &w, Iter begin, std::size_t n, std::integral_constant<bool,elt_skippable>) {
        T *y = w.put_trivial_aligned<T>(n);
        while(n--) {
          std::memcpy(y++, &*begin, sizeof(T));
          ++begin;
        }
      }

      static void skip_elts(parcel_reader &r, std::size_t n) {
        r.pop_trivial_aligned<T>(n);
      }

      template<typename Iter, bool elt_skippable>
      static void unpack_elts(parcel_reader &r, Iter into, std::size_t n, std::integral_constant<bool,elt_skippable>) {
        T const *x = r.pop_trivial_aligned<T>(n);
        while(n--) {
          *into = *x++;
          ++into;
        }
      }
    };

    template<typename T>
    struct packing_sequence<T, /*trivial=*/false>:
      packing_sequence_base<T> {

      template<typename Iter, bool elt_skippable>
      static void pack_elts(parcel_writer &w, Iter begin, std::size_t n, std::integral_constant<bool,elt_skippable> elt_skippable1) {
        while(n--) {
          packing<T>::pack(w, *begin, elt_skippable1);
          ++begin;
        }
      }

      // inherits skip_elts()

      template<typename Iter, bool elt_skippable>
      static void unpack_elts(parcel_reader &r, Iter into, std::size_t n, std::integral_constant<bool,elt_skippable> elt_skippable1) {
        while(n--) {
          raw_storage<T> raw;
          packing<T>::unpack(r, &raw, elt_skippable1);
          *into = raw.value_and_destruct();
          ++into;
        }
      }
    };

    template<typename T, typename Arr,
             typename T_Ub = decltype(packing<T>::ubound(parcel_size_empty(), std::declval<T>(), std::false_type())),
             bool T_Ub_static = T_Ub::all_static>
    struct packing_array_base;
      
    template<typename T, typename Arr, typename T_Ub>
    struct packing_array_base<T, Arr, T_Ub, /*T_Ub_static=*/true> {
      template<typename Ub, bool skippable>
      static auto ubound(Ub ub, Arr const &x, std::integral_constant<bool,skippable>) ->
        decltype(ub.added(T_Ub{}.template arrayed<sizeof(Arr)/sizeof(T)>())) {
        return ub.added(T_Ub{}.template arrayed<sizeof(Arr)/sizeof(T)>());
      }
    };
    
    template<typename T, typename Arr, typename T_Ub>
    struct packing_array_base<T, Arr, T_Ub, /*T_Ub_static=*/false> {
      static constexpr std::size_t t_size = T_Ub::static_size;
      static constexpr std::size_t t_align = T_Ub::static_align;
      
      template<typename Ub, bool skippable>
      static auto ubound(Ub ub, Arr const &x, std::integral_constant<bool,skippable>)
        -> decltype(ub.type_size_weakened().template type_align_weakened<t_align>()) {
        
        auto ub1 = ub.type_size_weakened().template type_align_weakened<t_align>();
        for(std::size_t i=0; i != sizeof(Arr)/sizeof(T); i++)
          ub1 = packing<T>::ubound(ub1, x[i], std::false_type());
        return ub1;
      }

      static void skip(parcel_reader &r) {
        for(std::size_t i=0; i != sizeof(Arr)/sizeof(T); i++)
          packing<T>::skip(r);
      }
    };

    template<typename T, typename Arr>
    struct packing_array: packing_array_base<T, Arr>{
      static_assert(!packing_is_trivial<T>::value, "Arrays of trivial T should have been picked up by packing_screen_trivial<T>.");

      static constexpr bool is_owning = packing_is_owning<T>::value;
      static constexpr bool is_ubound_tight = packing_is_ubound_tight<T>::value;
      
      // inherits ubound()
      
      template<bool skippable>
      static void pack(parcel_writer &w, Arr const &x, std::integral_constant<bool,skippable>) {
        packing_sequence<T>::pack_elts(w, &x[0], sizeof(Arr)/sizeof(T), std::false_type());
      }

      static void skip(parcel_reader &r) {
        packing_sequence<T>::skip_elts(r, sizeof(Arr)/sizeof(T));
      }
      
      template<bool skippable>
      static void unpack(parcel_reader &r, void *into, std::integral_constant<bool,skippable>) {
        packing_sequence<T>::unpack_elts(r, (T*)into, sizeof(Arr)/sizeof(T), std::false_type());
      }
    };

    template<typename T, std::size_t n>
    struct packing_skippable_dumb<T[n]>:
      packing_array<T, T[n]> {
    };
    template<typename T, std::size_t n>
    struct packing_skippable_dumb<std::array<T,n>>:
      packing_array<T, std::array<T,n>> {
    };
  }

  template<typename T, std::size_t n>
  struct packing_screen_trivial<std::array<T,n>>: packing_is_trivial<T> {};

  template<typename T, std::size_t n>
  struct is_definitely_trivially_serializable<std::array<T,n>>:
    is_definitely_trivially_serializable<T> {};
  
  template<typename T, std::size_t n, typename PackingOfT>
  struct packing_is_definitely_supported<std::array<T,n>, PackingOfT>:
    packing_is_definitely_supported<T> {};
  
  template<typename T, std::size_t n>
  struct packing_screened<std::array<T,n>>:
    detail::packing_skippable_smart<std::array<T,n>> {

    using unpacked_t = std::array<unpacked_of_t<T>, n>;
  };
  
  template<typename T, std::size_t n>
  struct packing_screen_trivial<T[n]>: packing_is_trivial<T> {};
  
  template<typename T, std::size_t n>
  struct is_definitely_trivially_serializable<T[n]>: is_definitely_trivially_serializable<T> {};

  template<typename T, std::size_t n, typename PackingOfT>
  struct packing_is_definitely_supported<T[n], PackingOfT>:
    packing_is_definitely_supported<T> {};
  
  template<typename T, std::size_t n>
  struct packing_screened<T[n]>:
    detail::packing_skippable_smart<T[n]> {

    using unpacked_t = unpacked_of_t<T>[n];
  };
  
  //////////////////////////////////////////////////////////////////////////////
  // packing<std::string>
  
  template<typename CharT, typename Traits, typename Alloc>
  struct packing<std::basic_string<CharT, Traits, Alloc>> {
    using string = std::basic_string<CharT, Traits, Alloc>;

    static constexpr bool is_definitely_supported = true;
    static constexpr bool is_owning = true;
    
    template<typename Ub0, bool skippable>
    static auto ubound(Ub0 ub0, const string &x, std::integral_constant<bool,skippable>) ->
      decltype(
        ub0.template trivial_added<std::size_t>()
           .template trivial_array_added<CharT>(x.size())
      ) {
      return ub0.template trivial_added<std::size_t>()
                .template trivial_array_added<CharT>(x.size());
    }
    
    template<bool skippable>
    static void pack(parcel_writer &w, const string &x, std::integral_constant<bool,skippable>) {
      std::size_t n = x.size();
      w.put_trivial_aligned<std::size_t>(n);
      w.put_trivial_aligned<CharT>(x.data(), n);
    }

    static void skip(parcel_reader &r) {
      std::size_t n = r.pop_trivial_aligned<std::size_t>();
      r.pop_trivial_aligned<CharT>(n);
    }
    
    template<bool skippable>
    static void unpack(parcel_reader &r, void *into, std::integral_constant<bool,skippable>) {
      std::size_t n = r.pop_trivial_aligned<std::size_t>();
      ::new(into) string{r.pop_trivial_aligned<CharT>(n), n};
    }
  };
  
  //////////////////////////////////////////////////////////////////////////////
  // packing<std::*container*>

  namespace detail {
    template<typename Bag,
             bool skippable_adds_size_t,
             typename T = typename Bag::value_type>
    struct packing_container_base {
      static constexpr bool is_definitely_supported = packing_is_definitely_supported<T>::value;
      static constexpr bool is_owning = packing_is_owning<T>::value;
      
      template<typename Ub0, bool skippable>
      static auto ubound(Ub0 ub0, const Bag &xs, std::integral_constant<bool,skippable>)
        -> decltype(
          packing_sequence<T>::ubound_elts(
            ub0.template trivial_added_if<std::size_t>(std::integral_constant<bool, skippable_adds_size_t && skippable>())
               .template trivial_added<std::size_t>(),
            xs.begin(), xs.size(), std::false_type()
          )
        ) {
        return packing_sequence<T>::ubound_elts(
          ub0.template trivial_added_if<std::size_t>(std::integral_constant<bool, skippable_adds_size_t && skippable>())
             .template trivial_added<std::size_t>(),
          xs.begin(), xs.size(), std::false_type()
        );
      }
    };
    
    template<typename Bag, typename = void>
    struct packing_container_reserve {
      static constexpr bool is_nop = true;
      
      void operator()(Bag&, std::size_t n) {}
    };
    template<typename Bag>
    struct packing_container_reserve<
        Bag,
        typename std::conditional<true,
            void,
            decltype(std::declval<Bag&>().reserve(0))
          >::type
      > {
      static constexpr bool is_nop = false;

      void operator()(Bag &bag, std::size_t n) {
        bag.reserve(n);
      }
    };

    template<typename Bag, typename = void>
    struct packing_container_insert {
      void operator()(Bag &bag, typename Bag::value_type &&x) {
        bag.insert(bag.end(), static_cast<typename Bag::value_type&&>(x));
      }
    };
    template<typename Bag>
    struct packing_container_insert<
        Bag,
        typename std::conditional<true,
            void,
            decltype(std::declval<Bag&>().push_back(std::declval<typename Bag::value_type>()))
          >::type
      > {
      void operator()(Bag &bag, typename Bag::value_type &&x) {
        bag.push_back(static_cast<typename Bag::value_type&&>(x));
      }
    };

    template<typename Bag,
             typename T = typename Bag::value_type,
             bool reserve_is_nop = packing_container_reserve<Bag>::is_nop,
             bool ub_tight = packing_is_ubound_tight<T>::value>
    struct packing_container;

    template<typename Bag, typename T>
    struct packing_container<Bag, T, /*reserve_is_nop=*/false, /*ub_tight=*/true>:
      packing_container_base<Bag, /*skippable_adds_size_t=*/false> {

      // inherits is_definitely_supported, is_owning
      // inherits ubound()
      
      template<bool skippable>
      static void pack(parcel_writer &w, const Bag &xs, std::integral_constant<bool,skippable>) {
        std::size_t n = xs.size();
        w.put_trivial_aligned<std::size_t>(n);
        packing_sequence<T>::pack_elts(w, xs.begin(), n, std::false_type());
      }

      static void skip(parcel_reader &r) {
        std::size_t n = r.pop_trivial_aligned<std::size_t>();
        packing_sequence<T>::skip_elts(r, n);
      }

      template<bool skippable>
      static void unpack(parcel_reader &r, void *into, std::integral_constant<bool,skippable>) {
        std::size_t n = r.pop_trivial_aligned<std::size_t>();
        Bag *bag = ::new(into) Bag;
        
        while(n--) {
          raw_storage<T> raw;
          packing<T>::unpack(r, &raw, std::false_type());
          packing_container_insert<Bag>()(*bag, raw.value_and_destruct());
        }
      }
    };

    template<typename Bag, typename T>
    struct packing_container<Bag, T, /*reserve_is_nop=*/false, /*ub_tight=*/false>:
      packing_container_base<Bag, /*skippable_adds_size_t=*/true> {

      // inherits is_definitely_supported, is_owning
      // inherits ubound()

      static void pack(parcel_writer &w, const Bag &xs, std::false_type skippable) {
        std::size_t n = xs.size();
        w.put_trivial_aligned<std::size_t>(n);
        
        for(auto x = xs.begin(); n--; ++x)
          packing<T>::pack(w, *x, std::false_type());
      }
      
      static void pack(parcel_writer &w, const Bag &xs, std::true_type skippable) {
        std::size_t n = xs.size();
        std::size_t *delta = w.place_trivial_aligned<std::size_t>();
        std::size_t head0 = w.size();
        
        w.put_trivial_aligned<std::size_t>(n);
        for(auto x = xs.begin(); n--; ++x)
          packing<T>::pack(w, *x, std::false_type());
        
        *delta = w.size() - head0;
      }

      static void skip(parcel_reader &r) {
        std::size_t delta = r.pop_trivial_aligned<std::size_t>();
        r.jump(delta);
      }

      template<bool skippable>
      static void unpack(parcel_reader &r, void *into, std::integral_constant<bool,skippable>) {
        if(skippable)
          r.pop_trivial_aligned<std::size_t>();
        
        std::size_t n = r.pop_trivial_aligned<std::size_t>();

        Bag *bag = ::new(into) Bag;
        packing_container_reserve<Bag>()(*bag, n);

        while(n--) {
          raw_storage<T> raw;
          packing<T>::unpack(r, &raw, std::false_type());
          packing_container_insert<Bag>()(*bag, raw.value_and_destruct());
        }
      }
    };

    template<typename Bag, typename T, bool ub_tight>
    struct packing_container<Bag, T, /*reserve_is_nop=*/true, ub_tight>:
      packing_container_base<Bag, /*skippable_adds_size_t=*/false> {

      // inherits is_definitely_supported
      // inherits is_owning
      // inherits ubound()

      template<bool skippable>
      static void pack(parcel_writer &w, const Bag &xs, std::integral_constant<bool,skippable>) {
        std::size_t *delta = w.place_trivial_aligned<std::size_t>();
        std::size_t size0 = w.size();
        std::size_t n = xs.size();
        
        for(auto x = xs.begin(); n--; ++x)
          packing<T>::pack(w, *x, std::false_type());

        *delta = w.size() - size0;
      }
      
      static void skip(parcel_reader &r) {
        std::size_t delta = r.pop_trivial_aligned<std::size_t>();
        r.jump(delta);
      }

      template<bool skippable>
      static void unpack(parcel_reader &r, void *into, std::integral_constant<bool,skippable>) {
        std::size_t delta = r.pop_trivial_aligned<std::size_t>();
        std::size_t end = r.head() + delta;

        Bag *bag = ::new(into) Bag;
        
        while(r.head() != end) {
          raw_storage<T> raw;
          packing<T>::unpack(r, &raw, std::false_type());
          packing_container_insert<Bag>()(*bag, raw.value_and_destruct());
        }
      }
    };
  }
  
  template<typename T, typename Alloc>
  struct packing<std::vector<T,Alloc>>:
    detail::packing_container<std::vector<T,Alloc>> {
    using unpacked_t = std::vector<unpacked_of_t<T>, Alloc>;
  };
  
  template<typename T, typename Alloc>
  struct packing<std::deque<T,Alloc>>:
    detail::packing_container<std::deque<T,Alloc>> {
    using unpacked_t = std::deque<unpacked_of_t<T>, Alloc>;
  };

  template<typename T, typename Alloc>
  struct packing<std::list<T,Alloc>>:
    detail::packing_container<std::list<T,Alloc>> {
    using unpacked_t = std::list<unpacked_of_t<T>,Alloc>;
  };

  template<typename T, typename Alloc>
  struct packing<std::forward_list<T,Alloc>>:
    detail::packing_container<std::forward_list<T,Alloc>> {
    using unpacked_t = std::forward_list<unpacked_of_t<T>,Alloc>;
  };


  template<typename T, typename Cmp, typename Alloc>
  struct packing<std::set<T,Cmp,Alloc>>:
    detail::packing_container<std::set<T,Cmp,Alloc>> {
    using unpacked_t = std::set<unpacked_of_t<T>,Cmp,Alloc>;
  };

  template<typename T, typename Cmp, typename Alloc>
  struct packing<std::multiset<T,Cmp,Alloc>>:
    detail::packing_container<std::multiset<T,Cmp,Alloc>> {
    using unpacked_t = std::multiset<unpacked_of_t<T>,Cmp,Alloc>;
  };

  template<typename T, typename Hash, typename Eq, typename Alloc>
  struct packing<std::unordered_set<T,Hash,Eq,Alloc>>:
    detail::packing_container<std::unordered_set<T,Hash,Eq,Alloc>> {
    using unpacked_t = std::unordered_set<unpacked_of_t<T>,Hash,Eq,Alloc>;
  };

  template<typename T, typename Hash, typename Eq, typename Alloc>
  struct packing<std::unordered_multiset<T,Hash,Eq,Alloc>>:
    detail::packing_container<std::unordered_multiset<T,Hash,Eq,Alloc>> {
    using unpacked_t = std::unordered_multiset<unpacked_of_t<T>,Hash,Eq,Alloc>;
  };


  template<typename K, typename V, typename Cmp, typename Alloc>
  struct packing<std::map<K,V,Cmp,Alloc>>:
    detail::packing_container<std::map<K,V,Cmp,Alloc>> {
    using unpacked_t = std::map<unpacked_of_t<K>, unpacked_of_t<V>, Cmp, Alloc>;
  };
  
  template<typename K, typename V, typename Cmp, typename Alloc>
  struct packing<std::multimap<K,V,Cmp,Alloc>>:
    detail::packing_container<std::multimap<K,V,Cmp,Alloc>> {
    using unpacked_t = std::multimap<unpacked_of_t<K>, unpacked_of_t<V>, Cmp, Alloc>;
  };
  
  template<typename K, typename V, typename Hash, typename Eq, typename Alloc>
  struct packing<std::unordered_map<K,V,Hash,Eq,Alloc>>:
    detail::packing_container<std::unordered_map<K,V,Hash,Eq,Alloc>> {
    using unpacked_t = std::unordered_map<unpacked_of_t<K>, unpacked_of_t<V>, Hash, Eq, Alloc>;
  };
  
  template<typename K, typename V, typename Hash, typename Eq, typename Alloc>
  struct packing<std::unordered_multimap<K,V,Hash,Eq,Alloc>>:
    detail::packing_container<std::unordered_multimap<K,V,Hash,Eq,Alloc>> {
    using unpacked_t = std::unordered_multimap<unpacked_of_t<K>, unpacked_of_t<V>, Hash, Eq, Alloc>;
  };

  #endif // #ifndef CREDUCING
}
#endif
