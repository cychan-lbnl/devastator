#ifndef _4a97f527_2fd6_4ead_98fc_f6bdf6a7f4a4
#define _4a97f527_2fd6_4ead_98fc_f6bdf6a7f4a4

#include <cstdint>
#include <cstring>
#include <utility>

namespace upcxx {
  //////////////////////////////////////////////////////////////////////
  // parcel_size: Essentially just a pair of size_t's for the size and
  // alignment of a memory block, but we use templates to track whether
  // the size and/or alignment are known at compiler time.

  /*
  template<std::size_t static_size, std::size_t static_align>
  struct parcel_size {
    // The two fields.
    const size_t size, align;
    
    // The size padded upto the alignment.
    size_t size_aligned();
    
    // Return the new size with the flat representation of T appended after `this`
    parcel_size<?,?> trivial_added<T>();
    
    // Return the new size with the flat representation of T appended after `this`,
    // if `ok` is true, otherwise returns `this` unaltered.
    parcel_size<?,?> trivial_added_if<T>(std::integral_constant<bool,ok>);
    
    // Return the new size of `that` appended after `this`.
    parcel_size<?,?> added(parcel_size<?,?> that);
    
    // Repeat `this` `n` times contiguously, for compile time known `n`.
    parcel_size<?,?> arrayed<n>();
    
    // Repeat `this` `n` times contiguously, for runtime known `n`.
    parcel_size<?,?> arrayed(size_t n);
  };*/

  #if 1 // Enable static tracking by parcel_size of size and alignment.
  
    template<std::size_t static_size, std::size_t static_align>
    struct parcel_size;
    
    // parcel_size helpers...
    namespace detail {
      template<std::size_t static_size>
      struct parcel_size_size;
      template<std::size_t static_align>
      struct parcel_size_align;
      
      template<>
      struct parcel_size_size<std::size_t(-1)> {
        static constexpr bool is_size_static = false;
        static constexpr std::size_t static_size = std::size_t(-1);
        
        std::size_t size;
                
        constexpr parcel_size_size(std::size_t size = 0)
          : size{size} {}
        constexpr parcel_size_size(std::nullptr_t, std::size_t size)
          : size{size} {}
      };
      template<std::size_t static_size1>
      struct parcel_size_size {
        static constexpr bool is_size_static = true;
        static constexpr std::size_t static_size = static_size1;
        static constexpr std::size_t size = static_size1;
        
        constexpr parcel_size_size() = default;
        constexpr parcel_size_size(std::nullptr_t, std::size_t) {}
      };
      
      template<>
      struct parcel_size_align<0> {
        static constexpr bool is_align_static = false;
        static constexpr std::size_t static_align = 0;
        
        std::size_t align;
        
        constexpr parcel_size_align(std::size_t align = 1)
          : align{align} {}
        constexpr parcel_size_align(std::nullptr_t, std::size_t align)
          : align{align} {}
      };
      template<std::size_t static_align1>
      struct parcel_size_align {
        static constexpr bool is_align_static = true;
        static constexpr std::size_t static_align = static_align1;
        static constexpr std::size_t align = static_align1;

        constexpr parcel_size_align() = default;
        constexpr parcel_size_align(std::nullptr_t, std::size_t) {}
      };
    }

    // parcel_size implementation
    
    template<// -1 = dynamic value
             std::size_t static_size = std::size_t(-1),
             // 0 = dynamic value
             std::size_t static_align = 0>
    struct parcel_size:
      detail::parcel_size_size<static_size>,
      detail::parcel_size_align<static_align> {

      static constexpr bool all_static =
        detail::parcel_size_size<static_size>::is_size_static &&
        detail::parcel_size_align<static_align>::is_align_static;
      
      constexpr parcel_size() = default;
      constexpr parcel_size(std::size_t size):
        detail::parcel_size_size<static_size>{size} {
      }
      constexpr parcel_size(std::size_t size, std::size_t align):
        detail::parcel_size_size<static_size>{size},
        detail::parcel_size_align<static_align>{align} {
      }

      // "Internal" constructor which accepts runtime values but ignores them
      // if we track that value statically.
      constexpr parcel_size(std::nullptr_t, std::size_t size, std::size_t align):
        detail::parcel_size_size<static_size>{nullptr, size},
        detail::parcel_size_align<static_align>{nullptr, align} {
      }

      // Constructor where static information may be weakened but not strengthened.
      template<std::size_t s, std::size_t a,
               typename = typename std::enable_if<
                   (static_size == std::size_t(-1) || static_size >= s) &&
                   (static_align == 0 || static_align >= a)
                 >::type>
      constexpr parcel_size(parcel_size<s,a> that):
        detail::parcel_size_size<static_size>{nullptr, that.size},
        detail::parcel_size_align<static_align>{nullptr, that.align} {
      }

      // Weaken static size to dynamic if not already.
      constexpr auto type_size_weakened() const
        -> parcel_size<std::size_t(-1), static_align> {
        return {nullptr, this->size, this->align};
      }
      
      // Weaken static alignment to `align` if necessary.
      template<std::size_t align>
      constexpr auto type_align_weakened() const ->
        parcel_size<
          static_size,
          static_align == 0 || align == 0
            ? 0
            : (static_align > align) ? static_align : align
        > {
        return {nullptr, this->size, this->align > align ? this->align : align};
      }
      
      constexpr std::size_t size_aligned() const {
        return (this->size + this->align-1) & -this->align;
      }

      // a.added(b): Returns the parcel_size of `a` followed by `b`.
      template<std::size_t that_ss, std::size_t that_sa>
      constexpr auto added(parcel_size<that_ss, that_sa> that) const
        -> parcel_size<
          parcel_size::all_static && decltype(that)::all_static
            ? ((static_size + that_sa-1) & -that_sa) + that_ss
            : std::size_t(-1),
          parcel_size::is_align_static && decltype(that)::is_align_static
            ? (static_align > that_sa) ? static_align : that_sa
            : 0
        > {
        return {nullptr,
          ((this->size + that.align-1) & -that.align) + that.size,
          this->align > that.align ? this->align : that.align
        };
      }

      // a.arrayed<n>(): Returns `this` repearted `n` times contiguously, for static `n`.
      template<std::size_t n>
      constexpr auto arrayed() const
        -> parcel_size<
          parcel_size::is_size_static ? n*static_size : std::size_t(-1),
          static_align
        > {
        return {nullptr, n*this->size, this->align};
      }

      // a.arrayed(n): Returns `this` repearted `n` times contiguously, for dynamic `n`.
      constexpr auto arrayed(std::size_t n) const
        -> parcel_size<std::size_t(-1), static_align> {
        return {nullptr, n*this->size, this->align};
      }
      
      template<typename T>
      constexpr auto trivial_added() const ->
        decltype(this->added(parcel_size<sizeof(T),alignof(T)>{})) {
        return this->added(parcel_size<sizeof(T),alignof(T)>{});
      }

      template<typename T>
      constexpr auto trivial_added_if(std::true_type) const ->
        decltype(this->added(parcel_size<sizeof(T),alignof(T)>{})) {
        return this->added(parcel_size<sizeof(T),alignof(T)>{});
      }
      template<typename T>
      constexpr auto trivial_added_if(std::false_type) const ->
        decltype(*this) {
        return *this;
      }
      
      template<typename T, std::size_t n>
      constexpr auto trivial_array_added() const ->
        decltype(this->added(parcel_size<n*sizeof(T), alignof(T)>{})) {
        return this->added(parcel_size<n*sizeof(T), alignof(T)>{});
      }
      template<typename T>
      constexpr auto trivial_array_added(std::size_t n) const ->
        decltype(this->added(parcel_size<std::size_t(-1), alignof(T)>{})) {
        return this->added(parcel_size<std::size_t(-1), alignof(T)>{nullptr, n*sizeof(T), alignof(T)});
      }
    };

    template<std::size_t sz, std::size_t al>
    using parcel_size_t = parcel_size<sz,al>;

    // Construct a parcel_size give size and alignment as compile-time values.
    template<std::size_t sz, std::size_t al>
    constexpr parcel_size<sz,al> parcel_size_of() {
      return {};
    }

    // Construct the empty parcel_size (0 bytes)
    constexpr parcel_size<0,1> parcel_size_empty() {
      return {};
    }
    
  #else // This version does no static tracking
    
    struct parcel_size {
      static constexpr bool all_static = false;
      static constexpr bool is_align_static = false;
      static constexpr std::size_t static_align = 0;
      static constexpr bool is_size_static = false;
      static constexpr std::size_t static_size = -1;
      
      // The two fields.
      size_t size, align;

      // The size padded upto the alignment.
      constexpr size_t size_aligned() const {
        return (size + align-1) & -align;
      }

      parcel_size type_size_weakened() const { return *this; }
      template<std::size_t al>
      parcel_size type_align_weakened() const { return *this; }
      
      // Return the new size with the flat representation of T appended after `this`
      template<typename T>
      parcel_size trivial_added() const {
        return {
          ((size + alignof(T)-1) & -alignof(T)) + sizeof(T),
          align > alignof(T) ? align : alignof(T)
        };
      }
      
      // Return the new size with the flat representation of T appended after `this`,
      // if `ok` is true, otherwise returns `this` unaltered.
      template<typename T>
      parcel_size trivial_added_if(std::true_type ok) const {
        return this->template trivial_added<T>();
      }
      template<typename T>
      parcel_size trivial_added_if(std::false_type ok) const {
        return *this;
      }
      
      // Return the new size of `that` appended after `this`.
      parcel_size added(parcel_size that) const {
        return {
          ((size + that.align-1) & -that.align) + that.size,
          align > that.align ? align : that.align
        };
      }
      
      // Repeat `this` `n` times contiguously, for compile time known `n`.
      template<std::size_t n>
      parcel_size arrayed() const {
        return {n*size, align};
      }
      
      // Repeat `this` `n` times contiguously, for runtime known `n`.
      parcel_size arrayed(size_t n) const {
        return {n*size, align};
      }

      template<typename T, std::size_t n>
      parcel_size trivial_array_added() const {
        return this->added({n*sizeof(T), alignof(T)});
      }
      template<typename T>
      parcel_size trivial_array_added(std::size_t n) const {
        return this->added({n*sizeof(T), alignof(T)});
      }
    };

    template<std::size_t sz, std::size_t al>
    using parcel_size_t = parcel_size;
    
    // Construct a parcel_size give size and alignment as compile-time values.
    template<std::size_t sz, std::size_t al>
    constexpr parcel_size parcel_size_of() {
      return {sz, al};
    }

    // Construct the empty parcel_size (0 bytes)
    constexpr parcel_size parcel_size_empty() {
      return {0, 1};
    }
  #endif
  
  //////////////////////////////////////////////////////////////////////
  // parcel_writer: Writes trivial data into a contiguous buffer. Each 
  // "put" advances state of writer to next position.
  
  struct parcel_writer {
    char *__restrict buf_;
    std::size_t size_, align_;
    
  public:
    // Start a writer pointing at buf assuming that buf is allocated big enough
    // to handle all the puts following.
    parcel_writer(void *buf):
      buf_{(char*)buf},
      size_{0}, align_{1} {
    }

    // Base pointer we were constructed with.
    char* buffer() const { return buf_; }
    
    // Total size of bytes put on writer.
    std::size_t size() const { return size_; }
    
    // Maximum alignment needed by anything put on writer.
    std::size_t align() const { return align_; }

    // Push an uninitialized chunk of memory on the end, returns chunk's pointer.
    char* place(std::size_t size, std::size_t align) {
      size_ = (size_ + align-1) & -align;
      std::size_t p = size_;
      size_ += size;
      align_ = align_ > align ? align_ : align;
      return buf_ + p;
    }
    
    // Push an uninitialized chunk of memory for T, returns chunk's pointer.
    template<typename T>
    T* place_trivial_aligned(std::size_t n=1) {
      return (T*)place(n*sizeof(T), alignof(T));
    }

    // Push and initialize a T given its value.
    template<typename T>
    T* put_trivial_aligned(const T &x) {
      T *y = place_trivial_aligned<T>();
      std::memcpy(y, &x, sizeof(T));
      return y;
    }
    // Push and initialize an array of T given its values.
    template<typename T>
    T* put_trivial_aligned(const T *x, std::size_t n) {
      T *y = place_trivial_aligned<T>(n);
      std::memcpy(y, x, n*sizeof(T));
      return y;
    }

    // Push and initialize a T without respecting alignment (skips padding).
    template<typename T>
    void* put_trivial_unaligned(const T &x) {
      char *y = place(sizeof(T), 1);
      std::memcpy(y, &x, sizeof(T));
      return (void*)y;
    }
    // Push and initialize a T array without respecting alignment (skips padding).
    template<typename T>
    void* put_trivial_unaligned(const T *x, std::size_t n) {
      char *y = place(n*sizeof(T), 1);
      std::memcpy(y, x, n*sizeof(T));
      return (void*)y;
    }
  };
  
  
  //////////////////////////////////////////////////////////////////////
  // parcel_reader: Reads trivial data out of buffer. Each "pop" 
  // advances state of reader to next position.
  
  class parcel_reader {
    std::uintptr_t head_;

  public:
    // Construct a reader starting at `buf`, pops increase the pointer.
    parcel_reader(void const *buf):
      head_{reinterpret_cast<std::uintptr_t>(buf)} {
    }

    // Returns pointer to a popped chunk of memory of given size and alignment.
    char const* pop(std::uintptr_t size, std::uintptr_t align) {
      head_ = (head_ + align-1) & -align;
      char const *p = reinterpret_cast<char const*>(head_);
      head_ += size;
      return p;
    }

    // Pops a T that had its bytes put trivially and respecting T's alignment.
    template<typename T>
    T const& pop_trivial_aligned() {
      return *reinterpret_cast<T const*>(pop(sizeof(T), alignof(T)));
    }
    // Pops a T array that was put there trivially and aligned.
    template<typename T>
    T const* pop_trivial_aligned(std::size_t n) {
      return reinterpret_cast<T const*>(pop(n*sizeof(T), alignof(T)));
    }

    // Pops a T that was pushed wihtout alignment.
    template<typename T>
    T pop_trivial_unaligned() {
      raw_storage<T> tmp;
      std::memcpy(&tmp.value, pop(sizeof(T), 1), sizeof(T));
      return tmp.value;
    }
    // Pops a T array (as char*) that was pushed without alignemnt. The returned
    // pointer should not be cast to T* since it isnt aligned.
    template<typename T>
    char const* pop_trivial_unaligned(std::size_t n) {
      return pop(n*sizeof(T), 1);
    }

    // Return an abstract "position" integer into the buffer. The absolute values
    // of heads are not defined to mean anything, but differences between head
    // values can be used as inputs to jump().
    std::uintptr_t head() const {
      return head_;
    }

    // Returns the head of the writer as a pointer.
    char const* head_pointer() const {
      return reinterpret_cast<char const*>(head_);
    }

    // Given a positive difference between two head positions, advances the head
    // accordingly.
    void jump(std::size_t delta) {
      head_ += delta;
    }
  };
}
#endif
