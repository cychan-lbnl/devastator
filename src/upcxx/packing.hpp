#ifndef _8cd0d114_b731_411b_bb0e_36e3c846aab0
#define _8cd0d114_b731_411b_bb0e_36e3c846aab0

#include "diagnostic.hpp"
#include "reflection.hpp"
#include "utility.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <tuple>
#include <type_traits>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

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
  template<typename T>
  union raw_storage {
    typename std::aligned_storage<sizeof(T), alignof(T)>::type raw;
    T value;
    
    raw_storage() {}
    raw_storage(raw_storage const &that): raw{that.raw} {}
    raw_storage& operator=(raw_storage const &that) {
      this->raw = that.raw;
      return *this;
    }
    ~raw_storage() {}
  };
  
  //////////////////////////////////////////////////////////////////////
  // parcel_layout: Used for accumulating the size and alignment of
  // packed data as well as determining the position of packed items.
  
  class parcel_layout {
    std::size_t size_;
    std::size_t align_;
    
  public:
    constexpr parcel_layout(std::size_t size=0, std::size_t align=1):
      size_{size},
      align_{align} {
    }
    
    std::size_t size() const { return size_; }
    std::size_t alignment() const { return align_; }
    
    std::size_t size_aligned() const { return (size_ + align_-1) & -align_; }
    
    template<std::size_t align=1>
    std::size_t add_bytes(std::size_t size) {
      std::size_t off = (size_ + align-1) & -align;
      size_ = off + size;
      if(align > align_) align_ = align;
      return off;
    }
    
    std::size_t add_bytes(std::size_t size, std::size_t align) {
      std::size_t off = (size_ + align-1) & -align;
      size_ = off + size;
      if(align > align_) align_ = align;
      return off;
    }
    
    template<typename T>
    std::size_t add_trivial_aligned() {
      return this->template add_bytes<alignof(T)>(sizeof(T));
    }
    template<typename T>
    std::size_t add_trivial_unaligned() {
      return this->template add_bytes<1>(sizeof(T));
    }
    
    std::size_t embed(parcel_layout that) {
      std::size_t off = (this->size_ + that.align_-1) & -that.align_;
      this->size_ = off + that.size_;
      if(that.align_ > this->align_)
        this->align_ = that.align_;
      return off;
    }
    
    friend std::ostream& operator<<(std::ostream &o, const parcel_layout &x) {
      return o << "{bytes="<<x.size()<<",align="<<x.alignment()<<"}";
    }
  };
  
  
  //////////////////////////////////////////////////////////////////////
  // parcel_writer: Writes trivial data into a contiguous buffer. Each 
  // "put" advances state of writer to next position.
  
  struct parcel_writer {
    char *__restrict buf_;
    parcel_layout lay_;
    
  public:
    parcel_writer(void *buf, std::size_t offset=0):
      buf_{(char*)buf},
      lay_{offset} {
    }
    
    char* buffer() const { return buf_; }
    parcel_layout layout() const { return lay_; }
    
    std::size_t size() const { return lay_.size(); }
    std::size_t alignment() const { return lay_.alignment(); }
    
    char* put(std::size_t size, std::size_t align) {
      std::size_t p = lay_.add_bytes(size, align);
      return buf_ + p;
    }
    
    char* put_char(char x) {
      std::size_t p = lay_.add_bytes(1);
      buf_[p] = x;
      return &buf_[p];
    }
    std::uint8_t* put_uint8(std::uint8_t x) {
      std::size_t p = lay_.add_bytes(1);
      ((std::uint8_t*)buf_)[p] = x;
      return (std::uint8_t*)buf_ + p;
    }
    std::int8_t* put_int8(std::int8_t x) {
      std::size_t p = lay_.add_bytes(1);
      ((std::int8_t*)buf_)[p] = x;
      return (std::int8_t*)buf_ + p;
    }
    
    template<typename T>
    T* put_trivial_aligned(const T &x) {
      std::size_t p = lay_.add_bytes(sizeof(T), alignof(T));
      std::memcpy((T*)(buf_ + p), &x, sizeof(T));
      return (T*)(buf_ + p);
    }
    template<typename T>
    T* put_trivial_aligned(const T *x, std::size_t n) {
      std::size_t p = lay_.add_bytes(n*sizeof(T), alignof(T));
      std::memcpy((T*)(buf_ + p), x, n*sizeof(T));
      return (T*)(buf_ + p);
    }
    
    template<typename T>
    void* put_trivial_unaligned(const T &x) {
      std::size_t p = lay_.add_bytes(sizeof(T));
      std::memcpy(buf_ + p, &x, sizeof(T));
      return buf_ + p;
    }
    template<typename T>
    void* put_trivial_unaligned(const T *x, std::size_t n) {
      std::size_t p = lay_.add_bytes(n*sizeof(T));
      std::memcpy((T*)(buf_ + p), x, n*sizeof(T));
      return buf_ + p;
    }
  };
  
  
  //////////////////////////////////////////////////////////////////////
  // parcel_reader: Reads trivial data out of buffer. Each "pop" 
  // advances state of reader to next position.
  
  class parcel_reader {
    const char *__restrict buf_;
    parcel_layout lay_;
    
  public:
    parcel_reader(void const *buf, std::size_t offset=0):
      buf_{(char const*)buf},
      lay_{offset} {
    }
    ~parcel_reader() {
      UPCXX_ASSERT(0 == (reinterpret_cast<std::uintptr_t>(buf_) & (lay_.alignment()-1)));
    }
    
    char const* buffer() const { return buf_; }
    parcel_layout layout() const { return lay_; }
    
    char const* pop(std::size_t size, std::size_t align) {
      std::size_t p = lay_.add_bytes(size, align);
      return buf_ + p;
    }
    
    char pop_char() {
      std::size_t p = lay_.add_bytes(1);
      char ans = buf_[p];
      return ans;
    }
    std::uint8_t pop_uint8() {
      std::size_t p = lay_.add_bytes(1);
      std::uint8_t ans = ((std::uint8_t const*)buf_)[p];
      return ans;
    }
    std::int8_t pop_int8() {
      std::size_t p = lay_.add_bytes(1);
      std::int8_t ans = ((std::int8_t const*)buf_)[p];
      return ans;
    }
    
    template<typename T>
    T const& pop_trivial_aligned() {
      std::size_t p = lay_.add_bytes(sizeof(T), alignof(T));
      return *(const T*)(buf_ + p);
    }
    template<typename T>
    T const* pop_trivial_aligned(std::size_t n) {
      std::size_t p = lay_.add_bytes(n*sizeof(T), alignof(T));
      return (const T*)(buf_ + p);
    }
    
    template<typename T>
    T pop_trivial_unaligned() {
      std::size_t p = lay_.add_bytes(sizeof(T));
      raw_storage<T> tmp;
      std::memcpy(&tmp.value, buf_ + p, sizeof(T));
      return tmp.value;
    }
    template<typename T>
    char const* pop_trivial_unaligned(std::size_t n) {
      std::size_t p = lay_.add_bytes(n*sizeof(T));
      return buf_ + p;
    }
  };
  
  
  //////////////////////////////////////////////////////////////////////
  /* packing<T>: Class of static hooks for packing/unpacking a value of
   * T into/outof a parcel. May be specialized by user. Default case
   * inspects type traits of T to determine best packing strategy.
   */
  template<typename T>
  struct packing /*{
    // Compute an upper-bound on the size of the packed message.
    static void size_ubound(parcel_layout &ub, T const &x);
    
    // Pack the value into the parcel.
    static void pack(parcel_writer &w, T const &x);
    
    // Read value out of parcel.
    static T unpack(parcel_reader &r);
  }*/;
  
  //////////////////////////////////////////////////////////////////////
  // packing_not_supported
  
  template<typename T>
  struct packing_not_supported {
    static void size_ubound(parcel_layout &ub, const T &x) {}
    
    #if 0
    static void pack(parcel_writer &w, const T &x) {
      UPCXX_INVOKE_UB();
    }
    static T unpack(parcel_reader &r) {
      UPCXX_INVOKE_UB();
    }
    #endif
  };
  
  //////////////////////////////////////////////////////////////////////
  // packing_is_trivial
  
  template<typename T, typename false_ = std::false_type>
  struct packing_is_trivial {
    static constexpr bool value = false;
  };
  template<typename T>
  struct packing_is_trivial<T, std::integral_constant<bool, false & packing<T>::is_trivial>> {
    static constexpr bool value = packing<T>::is_trivial;
  };
  
  //////////////////////////////////////////////////////////////////////
  // packing_empty
  
  template<typename T,
           bool is_default_constructible = std::is_default_constructible<T>::value>
  struct packing_empty;
  
  template<typename T>
  struct packing_empty<T, /*is_default_constructible=*/true> {
    static constexpr bool is_trivial = true;
    
    static void size_ubound(parcel_layout &ub, const T &x) {}
    static void pack(parcel_writer &w, const T &x) {}
    static T unpack(parcel_reader &r) { return T{}; }
  };
  
  template<typename T>
  struct packing_empty<T, /*is_default_constructible=*/false> {
    static constexpr bool is_trivial = true;
    
    static void size_ubound(parcel_layout &ub, const T &x) {}
    static void pack(parcel_writer &w, const T &x) {}
    static T unpack(parcel_reader &r) {
      raw_storage<T> ooze;
      return ooze.value;
      //typename std::aligned_storage<sizeof(T),alignof(T)>::type ooze = {};
      //return *reinterpret_cast<T*>(&ooze);
    }
  };
  
  //////////////////////////////////////////////////////////////////////
  // packing_trivial: Packs byte-wise satisfying alignment.
  
  template<typename T>
  struct packing_trivial {
    static constexpr bool is_trivial = true;
    
    static void size_ubound(parcel_layout &ub, const T &x) {
      ub.add_bytes(sizeof(T), alignof(T));
    }
    static void pack(parcel_writer &w, const T &x) {
      w.put_trivial_aligned(x);
    }
    static T unpack(parcel_reader &r) {
      return r.pop_trivial_aligned<T>();
    }
  };
  
  //////////////////////////////////////////////////////////////////////
  // packing_opaque: Packs byte-wise satisfying alignment.
  
  template<typename T,
           bool is_empty = std::is_empty<T>::value,
           bool is_trivially_copyable = std::is_trivially_copyable<T>::value>
  struct packing_opaque;
  
  template<typename T, bool is_trivially_copyable>
  struct packing_opaque<T, /*is_empty=*/true, is_trivially_copyable>:
    packing_empty<T> {
  };
  template<typename T>
  struct packing_opaque<T, /*is_empty=*/false, /*is_trivially_copyable=*/true>:
    packing_trivial<T> {
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
  };
  
  // Reflection visitor for calling packing::size_ubound.
  struct packing_ubound_reflector {
    parcel_layout &ub;
    
    template<typename T>
    void operator()(const T &mbr) {
      packing<T>::size_ubound(ub, mbr);
    }
    template<typename T>
    void opaque(const T &mbr) {
      packing_opaque<T>::size_ubound(ub, mbr);
    }
  };
  
  // Reflection visitor for calling packing::pack.
  struct packing_pack_reflector {
    parcel_writer &w;
    
    template<typename T>
    void operator()(const T &mbr) {
      packing<T>::pack(w, mbr);
    }
    template<typename T>
    void opaque(const T &mbr) {
      packing_opaque<T>::pack(w, mbr);
    }
  };
  
  // Reflection visitor for calling packing::unpack.
  template<bool member_assignment_not_construction>
  struct packing_unpack_reflector;
  
  template<>
  struct packing_unpack_reflector</*member_assignment_not_construction=*/true> {
    parcel_reader &r;
    
    template<typename T>
    void operator()(T &mbr) {
      mbr = packing<T>::unpack(r);
    }
    template<typename T>
    void opaque(T &mbr) {
      mbr = packing_opaque<T>::unpack(r);
    }
  };
  template<>
  struct packing_unpack_reflector</*member_assignment_not_construction=*/false> {
    parcel_reader &r;
    
    template<typename T>
    void operator()(T &mbr) {
      ::new(&mbr) T(packing<T>::unpack(r));
    }
    template<typename T>
    void opaque(T &mbr) {
      ::new(&mbr) T(packing_opaque<T>::unpack(r));
    }
  };
  
  template<typename T,
           bool is_default_constructible = std::is_default_constructible<T>::value>
  struct packing_reflected;
  
  template<typename T>
  struct packing_reflected<T, /*is_default_constructible=*/true> {
    static void size_ubound(parcel_layout &ub, const T &x) {
      packing_ubound_reflector re{ub};
      reflect_upon(re, x);
    }
    
    static void pack(parcel_writer &w, const T &x) {
      packing_pack_reflector re{w};
      reflect_upon(re, x);
    }
    
    static T unpack(parcel_reader &r) {
      T ans;
      packing_unpack_reflector</*member_assignment_not_construction=*/true> re{r};
      reflect_upon(re, ans);
      return ans;
    }
  };
  
  template<typename T>
  struct packing_reflected<T, /*is_default_constructible=*/false> {
    static void size_ubound(parcel_layout &ub, const T &x) {
      packing_ubound_reflector re{ub};
      reflect_upon(re, x);
    }
    
    static void pack(parcel_writer &w, const T &x) {
      packing_pack_reflector re{w};
      reflect_upon(re, x);
    }
    
    static T unpack(parcel_reader &r) {
      /* This is so scary. We never actually call T's constructor, just
       * its reflected members' constructors. Therefor, only use
       * reflection-based unpacking on non-default-constructible types
       * if the uncalled constructors (those for T and the non-reflected
       * members, if any) are trivial.
       */
      
      // Uninitialized memory to hold unpacked value.
      raw_storage<T> ooze;
      //typename std::aligned_storage<sizeof(T),alignof(T)>::type ooze = {};
      
      // The unpack reflecter will placement-construct each member.
      packing_unpack_reflector</*member_assignment_not_construction=*/false> re{r};
      //reflect_upon(re, *reinterpret_cast<T*>(&ooze));
      reflect_upon(re, ooze.value);
      
      // Return to user, make sure to destruct moved-out object.
      T ans = reinterpret_cast<T&&>(ooze.value);
      ooze.value.~T();
      return ans;
    }
  };
  
  
  //////////////////////////////////////////////////////////////////////
  // packing_function_pointer
  
  namespace detail {
    void packing_funptr_basis();

    template<typename Fp>
    static std::uintptr_t funptr_to_uintptr(Fp fp) {
      std::uintptr_t ans;
      std::memcpy(&ans, &fp, sizeof(Fp));
      return ans;
    }
    
    template<typename Fp>
    static Fp funptr_from_uintptr(std::uintptr_t u) {
      Fp ans;
      std::memcpy(&ans, &u, sizeof(Fp));
      return ans;
    }
  }
  
  template<typename T>
  struct packing_function_pointer {
    static_assert(
      sizeof(T) == sizeof(std::uintptr_t),
      "Function pointers must be the same size as regular pointers."
    );
    
    static constexpr bool is_trivial = false;
    
    static void size_ubound(parcel_layout &ub, T fp) {
      ub.add_bytes(sizeof(T), alignof(T));
    }
    
    static void pack(parcel_writer &w, T fp) {
      std::uintptr_t basis = detail::funptr_to_uintptr(detail::packing_funptr_basis);
      std::uintptr_t u = detail::funptr_to_uintptr(fp);
      u -= basis;
      // Use uint32_t? Should work unless final exe is larger than 2GB.
      w.put_trivial_aligned(u);
    }
    
    static T unpack(parcel_reader &r) {
      std::uintptr_t basis = detail::funptr_to_uintptr(detail::packing_funptr_basis);
      std::uintptr_t u = r.pop_trivial_aligned<std::uintptr_t>();
      u += basis;
      return detail::template funptr_from_uintptr<T>(u);
    }
  };
  
  //////////////////////////////////////////////////////////////////////
  // The default strategy for packing<T>
  
  template<typename T,
    bool is_empty = std::is_empty<T>::value,
    bool is_funptr = std::is_pointer<T>::value && std::is_function<typename std::remove_pointer<T>::type>::value,
    bool is_scalar = std::is_scalar<T>::value>
  struct packing_specializer;
  
  template<typename T, bool is_funptr, bool is_scalar>
  struct packing_specializer<T, /*is_empty=*/true, is_funptr, is_scalar>:
    packing_empty<T> {
  };
  template<typename T, bool is_scalar>
  struct packing_specializer<T, /*is_empty=*/false, /*is_funptr=*/true, is_scalar>:
    packing_function_pointer<T> {
  };
  template<typename T>
  struct packing_specializer<T, /*is_empty=*/false, /*is_funptr=*/false, /*is_scalar=*/true>:
    packing_trivial<T> {
  };
  template<typename T>
  struct packing_specializer<T, /*is_empty=*/false, /*is_funptr=*/false, /*is_scalar=*/false>:
    packing_reflected<T> {
  };
  
  // here it is
  template<typename T>
  struct packing: packing_specializer<T> {};
  
  template<typename T>
  struct packing<T const>: packing<T> {};
  template<typename T>
  struct packing<T volatile>: packing<T> {};
  
  template<typename T>
  struct packing<T&>: packing_not_supported<T&> {};
  template<typename T>
  struct packing<T&&>: packing_not_supported<T&&> {};
  
  //////////////////////////////////////////////////////////////////////
  // packing<std::tuple>
  
  namespace detail {
    template<int n, int i, typename ...T>
    struct packing_tuple_each {
      typedef typename std::tuple_element<i,std::tuple<T...>>::type Ti;
      
      static void size_ubound(parcel_layout &ub, const std::tuple<T...> &x) {
        packing<Ti>::size_ubound(ub, std::get<i>(x));
        packing_tuple_each<n, i+1, T...>::size_ubound(ub, x);
      }
      
      static void pack(parcel_writer &w, const std::tuple<T...> &x) {
        packing<Ti>::pack(w, std::get<i>(x));
        packing_tuple_each<n, i+1, T...>::pack(w, x);
      }
      
      template<typename Storage>
      static void unpack_into(parcel_reader &r, Storage &storage) {
        new(&std::get<i>(storage)) Ti{packing<Ti>::unpack(r)};
        packing_tuple_each<n, i+1, T...>::unpack_into(r, storage);
      }
      
      template<typename Storage>
      static void destruct(Storage &storage) {
        reinterpret_cast<Ti&>(std::get<i>(storage)).~Ti();
        packing_tuple_each<n, i+1, T...>::destruct(storage);
      }
    };
    
    template<int n, typename ...T>
    struct packing_tuple_each<n, n, T...> {
      static void size_ubound(parcel_layout &ub, const std::tuple<T...> &x) {}
      static void pack(parcel_writer &w, const std::tuple<T...> &x) {}
      template<typename Storage>
      static void unpack_into(parcel_reader &r, Storage&) {}
      template<typename Storage>
      static void destruct(Storage&) {}
    };
  }
  
  template<typename ...T>
  struct packing<std::tuple<T...>> {
    static void size_ubound(parcel_layout &ub, const std::tuple<T...> &x) {
      detail::packing_tuple_each<sizeof...(T), 0, T...>::size_ubound(ub, x);
    }
    
    static void pack(parcel_writer &w, const std::tuple<T...> &x) {
      detail::packing_tuple_each<sizeof...(T), 0, T...>::pack(w, x);
    }
    
    template<typename Storage, int ...i>
    static std::tuple<T...> move_from_storage(
        Storage &storage,
        upcxx::index_sequence<i...>
      ) {
      return std::tuple<T...>{
        reinterpret_cast<T&&>(std::get<i>(storage))...
      };
    }
    
    static std::tuple<T...> unpack(parcel_reader &r) {
      std::tuple<
          typename std::aligned_storage<sizeof(T),alignof(T)>::type...
        > storage;
      
      detail::packing_tuple_each<sizeof...(T), 0, T...>::unpack_into(r, storage);
      
      std::tuple<T...> ans{
        move_from_storage(storage, upcxx::make_index_sequence<sizeof...(T)>())
      };
      
      detail::packing_tuple_each<sizeof...(T), 0, T...>::destruct(storage);
      
      return ans;
    }
  };
  
  //////////////////////////////////////////////////////////////////////
  // packing<std::pair>
  
  template<typename A, typename B>
  struct packing<std::pair<A,B>> {
    static void size_ubound(parcel_layout &ub, const std::pair<A,B> &x) {
      packing<A>::size_ubound(ub, x.first);
      packing<B>::size_ubound(ub, x.second);
    }
    
    static void pack(parcel_writer &w, const std::pair<A,B> &x) {
      packing<A>::pack(w, x.first);
      packing<B>::pack(w, x.second);
    }
    
    static std::pair<A,B> unpack(parcel_reader &r) {
      auto a = packing<A>::unpack(r);
      auto b = packing<B>::unpack(r);
      return std::pair<A,B>{std::move(a), std::move(b)};
    }
  };
  
  
  //////////////////////////////////////////////////////////////////////
  // packing<std::array>
  
  template<typename T, std::size_t n>
  struct packing<std::array<T,n>> {
    static void size_ubound(parcel_layout &ub, const std::array<T,n> &x) {
      for(std::size_t i=0; i != n; i++)
        packing<T>::size_ubound(ub, x[i]);
    }
    
    static void pack(parcel_writer &w, const std::array<T,n> &x) {
      for(std::size_t i=0; i != n; i++)
        packing<T>::pack(w, x[i]);
    }
    
    static std::array<T,n> unpack(parcel_reader &r) {
      typedef std::array<T,n> A;
      typename std::aligned_storage<sizeof(A), alignof(A)>::type tmp;
      
      for(std::size_t i=0; i != n; i++)
        new(&reinterpret_cast<A&>(tmp)[i]) T{packing<T>::unpack(r)};
      
      A ans{reinterpret_cast<A&&>(tmp)};
      reinterpret_cast<A&>(tmp).~A();
      return ans;
    }
  };
  
  
  //////////////////////////////////////////////////////////////////////
  // packing<std::string>
  
  template<>
  struct packing<std::string> {
    static void size_ubound(parcel_layout &ub, const std::string &x) {
      std::size_t n = x.size();
      packing<std::size_t>::size_ubound(ub, n);
      ub.add_bytes(n);
    }
    
    static void pack(parcel_writer &w, const std::string &x) {
      std::size_t n = x.size();
      packing<std::size_t>::pack(w, n);
      w.put_trivial_aligned(x.data(), n);
    }
    
    static std::string unpack(parcel_reader &r) {
      std::size_t n = packing<std::size_t>::unpack(r);
      std::string s;
      s.resize(n+1);
      s.assign(r.pop_trivial_aligned<char>(n), n);
      return s;
    }
  };
  
  
  //////////////////////////////////////////////////////////////////////
  // packing<std::vector>
  
  template<typename T>
  struct packing<std::vector<T>> {
    static void size_ubound(parcel_layout &ub, const std::vector<T> &x) {
      std::size_t n = x.size();
      packing<std::size_t>::size_ubound(ub, n);
      for(std::size_t i=0; i != n; i++)
        packing<T>::size_ubound(ub, x[i]);
    }
    
    static void pack(parcel_writer &w, const std::vector<T> &x) {
      std::size_t n = x.size();
      packing<std::size_t>::pack(w, n);
      for(std::size_t i=0; i != n; i++)
        packing<T>::pack(w, x[i]);
    }
    
    static std::vector<T> unpack(parcel_reader &r) {
      std::size_t n = packing<std::size_t>::unpack(r);
      std::vector<T> v;
      v.reserve(n);
      for(std::size_t i=0; i != n; i++)
        v.push_back(packing<T>::unpack(r));
      return v;
    }
  };
  
  //////////////////////////////////////////////////////////////////////
  // packing<std::unordered_set>
  
  template<typename T>
  struct packing<std::unordered_set<T>> {
    static void size_ubound(parcel_layout &ub, const std::unordered_set<T> &xs) {
      std::size_t n = xs.size();
      packing<std::size_t>::size_ubound(ub, n);
      for(const T &x: xs)
        packing<T>::size_ubound(ub, x);
    }
    
    static void pack(parcel_writer &w, const std::unordered_set<T> &xs) {
      std::size_t n = xs.size();
      packing<std::size_t>::pack(w, n);
      for(const T &x: xs)
        packing<T>::pack(w, x);
    }
    
    static std::unordered_set<T> unpack(parcel_reader &r) {
      std::size_t n = packing<std::size_t>::unpack(r);
      std::unordered_set<T> xs(n/4);
      for(std::size_t i=0; i != n; i++)
        xs.insert(packing<T>::unpack(r));
      return xs;
    }
  };
  
  
  //////////////////////////////////////////////////////////////////////
  // packing<std::unordered_map>
  
  template<typename K, typename V>
  struct packing<std::unordered_map<K,V>> {
    static void size_ubound(parcel_layout &ub, const std::unordered_map<K,V> &xs) {
      std::size_t n = xs.size();
      packing<std::size_t>::size_ubound(ub, n);
      for(const std::pair<const K,V> &x: xs)
        packing<std::pair<const K,V>>::size_ubound(ub, x);
    }
    
    static void pack(parcel_writer &w, const std::unordered_map<K,V> &xs) {
      std::size_t n = xs.size();
      packing<std::size_t>::pack(w, n);
      for(const std::pair<const K,V> &x: xs)
        packing<std::pair<const K,V>>::pack(w, x);
    }
    
    static std::unordered_map<K,V> unpack(parcel_reader &r) {
      std::size_t n = packing<std::size_t>::unpack(r);
      std::unordered_map<K,V> xs(n/4);
      for(std::size_t i=0; i != n; i++)
        xs.insert(packing<std::pair<const K,V>>::unpack(r));
      return xs;
    }
  };
}

#endif
