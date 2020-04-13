#ifndef _af3a33ad_9fc7_4d84_990b_b9caa94daf02
#define _af3a33ad_9fc7_4d84_990b_b9caa94daf02

#include <upcxx/utility.hpp>

#include <algorithm>
#include <cstdint>

#include <tuple>
#include <type_traits>
#include <utility>

#ifndef UPCXX_CREDUCE_SLIM
  /* UPCXX_CREDUCE_SLIM should be defined when we're hunting down compiler ICE's
   * using the cReduce tool. Including all these std containers greatly
   * increases the size of the translation unit which slows down cReduce
   * considerably. The end effect is that serialization logic is not registered
   * for most std types.
   */
  #include <array>
  #include <deque>
  #include <forward_list>
  #include <list>
  #include <map>
  #include <memory>
  #include <set>
  #include <unordered_map>
  #include <unordered_set>
  #include <vector>
#endif

namespace upcxx {
  constexpr std::uintptr_t serialization_align_max = 64;

  template<typename T>
  struct serialization;

  template<typename T>
  struct serialization_traits;

  namespace detail {
    enum class serialization_existence {
      user, // the user has provided serialization via specialization, nested subclass, or macros
      trivial_unsafe, // best effort trivial serialization will be performed, is_serializable will report false
      trivial_blessed, // trivial serialization that reports as is_serializable and is_trivially_serializable
      trivial_asserted, // currently unused
      invalid // definitely never used
    };

    // determines what kind of serialization has been registered excepting specialization of is_trivially_serializable
    template<typename T, typename=std::false_type>
    struct serialization_get_existence;
  }

  template<typename T>
  struct is_trivially_serializable {
    static constexpr bool value =
      detail::serialization_get_existence<T>::value == detail::serialization_existence::user ? false :
      detail::serialization_get_existence<T>::value == detail::serialization_existence::trivial_unsafe ? std::is_trivially_copyable<T>::value :
      detail::serialization_get_existence<T>::value == detail::serialization_existence::trivial_blessed ? true :
      detail::serialization_get_existence<T>::value == detail::serialization_existence::trivial_asserted ? true :
      false; // impossible
  };
  
  template<typename T>
  struct is_serializable {
    static constexpr bool value = serialization_traits<T>::is_serializable;
  };
  
  template<typename T>
  struct deserialized_type {
    using type = typename serialization_traits<T>::deserialized_type;
  };
  template<typename T>
  using deserialized_type_t = typename serialization_traits<T>::deserialized_type;

  namespace detail {
    template<typename T>
    struct serialization_references_buffer_not {
      static constexpr bool value = !serialization_traits<T>::references_buffer;
    };
  }
  
  template<std::size_t s_size=std::size_t(-2),
           std::size_t s_align=std::size_t(-2)>
  struct storage_size;

  using invalid_storage_size_t = storage_size<std::size_t(-1), std::size_t(-1)>;
  
  namespace detail {
    template<std::size_t s_size, std::size_t s_align>
    struct storage_size_base;

    template<>
    struct storage_size_base<std::size_t(-1), std::size_t(-1)> {
      static constexpr bool is_valid = false, is_static = false;
      static constexpr std::size_t static_size = std::size_t(-1);
      static constexpr std::size_t static_align = std::size_t(-1);
      static constexpr std::size_t static_align_ub = std::size_t(-1);
      static constexpr std::size_t size = std::size_t(-1), align = std::size_t(-1);

      using static_otherwise_invalid_t = invalid_storage_size_t;
      
      constexpr storage_size_base(std::size_t dyn_size, std::size_t dyn_align) {}

      template<std::size_t s_size1, std::size_t s_align1>
      constexpr storage_size_base(storage_size_base<s_size1,s_align1> const &that) {}
    };
    
    template<std::size_t s_align_ub>
    struct storage_size_base<std::size_t(-2), s_align_ub> {
      static constexpr bool is_valid = true, is_static = false;
      static constexpr std::size_t static_size = std::size_t(-2);
      static constexpr std::size_t static_align = std::size_t(-2);
      static constexpr std::size_t static_align_ub = s_align_ub;
      
      using static_otherwise_invalid_t = invalid_storage_size_t;

      std::size_t size, align;

      constexpr storage_size_base(std::size_t dyn_size, std::size_t dyn_align):
        size(dyn_size),
        align(dyn_align) {
      }

      template<std::size_t s_size1, std::size_t s_align1>
      constexpr storage_size_base(storage_size_base<s_size1,s_align1> const &that):
        size(that.size),
        align(that.align) {
      }
    };
    
    template<std::size_t s_size, std::size_t s_align>
    struct storage_size_base {
      static_assert(s_align < std::size_t(-2), "Internal error: storage_size<size,align>: size is static but align is not.");
      
      static constexpr bool is_valid = true, is_static = true;
      static constexpr std::size_t static_size = s_size;
      static constexpr std::size_t static_align = s_align;
      static constexpr std::size_t static_align_ub = s_align;
      static constexpr std::size_t size = s_size, align = s_align;
      
      using static_otherwise_invalid_t = storage_size<s_size,s_align>;

      constexpr storage_size_base(std::size_t dyn_size, std::size_t dyn_align) {}
    };
  }

  template<typename T>
  constexpr storage_size<sizeof(T), alignof(T)> storage_size_of();
  
  template<std::size_t s_size, std::size_t s_align>
  struct storage_size: detail::storage_size_base<s_size, s_align> {
    constexpr storage_size(std::size_t dyn_size, std::size_t dyn_align):
      detail::storage_size_base<s_size, s_align>(dyn_size, dyn_align) {
    }
    
    template<std::size_t s_size1, std::size_t s_align1>
    constexpr storage_size(storage_size<s_size1, s_align1> const &that):
      detail::storage_size_base<s_size, s_align>(
        static_cast<detail::storage_size_base<s_size1, s_align1> const&>(that)
      ) {
    }

    constexpr std::size_t size_aligned(std::size_t min_align=1) const {
      #define UPCXX_a (min_align > this->align ? min_align : this->align)
      return (this->size + UPCXX_a-1) & -UPCXX_a;
      #undef UPCXX_a
    }

    constexpr typename detail::storage_size_base<s_size, s_align>::static_otherwise_invalid_t
    static_otherwise_invalid() const {
      return {s_size, s_align};
    }

  public:
    template<std::size_t s_size1, std::size_t s_align1>
    using cat_return_t = storage_size<
      /*s_size = */(
        s_size >= std::size_t(-2) || s_size1 >= std::size_t(-2)
          ? /*max*/(s_size > s_size1 ? s_size : s_size1)
          : ((s_size + s_align1-1) & -s_align1) + s_size1
      ),
      /*s_align = max*/(
        s_align > s_align1 ? s_align : s_align1
      )
    >;

    template<std::size_t s_size1, std::size_t s_align1>
    constexpr cat_return_t<s_size1, s_align1> cat_help(std::size_t size1, std::size_t align1) const {
      return {
        ((this->size + align1-1) & -align1) + size1,
        this->align > align1 ? this->align : align1
      };
    }

    template<std::size_t s_size1, std::size_t s_align1>
    constexpr cat_return_t<s_size1, s_align1> cat(storage_size<s_size1, s_align1> that) const {
      return this->template cat_help<s_size1, s_align1>(that.size, that.align);
    }

    template<std::size_t s_size1, std::size_t s_align1>
    constexpr cat_return_t<s_size1, s_align1> cat() const {
      return this->template cat_help<s_size1, s_align1>(s_size1, s_align1);
    }
    
    constexpr cat_return_t<std::size_t(-2), std::size_t(-2)> cat(std::size_t size1, std::size_t align1) const {
      return this->template cat_help<std::size_t(-2), std::size_t(-2)>(size1, align1);
    }
    
    template<typename T>
    constexpr cat_return_t<sizeof(T), alignof(T)> cat_size_of() const {
      return this->cat(storage_size_of<T>());
    }

    template<typename T>
    constexpr auto cat_ubound_of(T const &x) const
      UPCXX_RETURN_DECLTYPE(
        serialization_traits<T>::ubound(*this, x)
      ) {
      return serialization_traits<T>::ubound(*this, x);
    }

    constexpr auto arrayed(std::size_t n) const
      -> storage_size<
        -s_size == 1 ? std::size_t(-1) : std::size_t(-2),
        -s_size == 1 ? std::size_t(-1) : s_align
      > {
      return {
        n==1 ? this->size : n*this->size_aligned(),
        n==0 ? 1 : this->align
      };
    }

    template<std::size_t n>
    constexpr auto arrayed() const
      -> storage_size<
        // size_ub
        n==0 ? 0 :
        -s_size==1 ? std::size_t(-1) :
        -s_size==2 ? std::size_t(-2) :
        (n==1 ? s_size : n*((s_size + s_align-1) & -s_align)),

        // align_ub
        n==0 ? 1 :
        -s_align==1 ? std::size_t(-1) :
        s_align
      > {
      return {
        n==1 ? this->size : n*this->size_aligned(),
        n==0 ? 1 : this->align
      };
    }
  };
  
  template<typename T>
  constexpr storage_size<sizeof(T), alignof(T)> storage_size_of() {
    return {sizeof(T), alignof(T)};
  }

  constexpr storage_size<0,1> empty_storage_size(0,1);
  constexpr invalid_storage_size_t invalid_storage_size(std::size_t(-1), std::size_t(-1));

  namespace detail {
    template<typename Iter,
           typename T = typename std::remove_cv<typename std::iterator_traits<Iter>::value_type>::type>
    struct is_iterator_contiguous:
      std::integral_constant<bool,
        std::is_same<Iter, T*>::value ||
        std::is_same<Iter, T const*>::value ||

        #ifndef UPCXX_CREDUCE_SLIM
        std::is_same<Iter, typename std::array<T,1>::iterator>::value ||
        std::is_same<Iter, typename std::array<T,1>::const_iterator>::value ||
        (!std::is_same<T,bool>::value && (
          std::is_same<Iter, typename std::vector<T>::iterator>::value ||
          std::is_same<Iter, typename std::vector<T>::const_iterator>::value
        ))
        #else
        false
        #endif
      > {
    };

    template<typename Writer>
    struct serialization_writer_base {
      void* place(storage_size<> obj) {
        return static_cast<Writer*>(this)->place(obj.size, obj.align);
      }

      template<typename T>
      struct reserve_handle { void *ptr; };

      template<typename T>
      reserve_handle<T> reserve() {
        return reserve_handle<T>{this->place(storage_size_of<T>())};
      }

      template<typename T>
      void commit(reserve_handle<T> handle, T const &val) {
        ::new(handle.ptr) T(val);
      }
      
      template<typename T>
      void write(T const &x) {
        upcxx::template serialization_traits<T>::serialize(*static_cast<Writer*>(this), x);
      }

      template<typename T>
      void write_trivial(T const &x) {
        void *spot = this->place(storage_size_of<T>());
        detail::template memcpy_aligned<alignof(T)>(spot, &x, sizeof(T));
      }
    };
    
    template<bool bounded>
    class serialization_writer;

    template<>
    class serialization_writer</*bounded=*/true>:
      public serialization_writer_base<serialization_writer</*bounded=*/true>> {
      
      friend class serialization_writer<false>;
      
      char *buf_;
      std::size_t size_, align_;

    public:
      // common constructor
      serialization_writer(void *buf, std::size_t buf_capacity=0):
        buf_((char*)buf),
        size_(0), align_(1) {
      }
      
      std::size_t size() const { return size_; }
      std::size_t align() const { return align_; }

      bool contained_in_initial() const {
        return true;
      }

      void compact_and_invalidate(void *buf) {
        std::memcpy(buf, buf_, size_);
      }
      
      void* place(std::size_t obj_size, std::size_t obj_align) {
        UPCXX_ASSERT(detail::is_aligned(buf_, obj_align));
        
        size_ = (size_ + obj_align-1) & -obj_align;
        void *spot = reinterpret_cast<void*>(buf_ + size_);
        size_ += obj_size;
        align_ = obj_align > align_ ? obj_align : align_;
        return spot;
      }

      using serialization_writer_base<serialization_writer</*bounded=*/true>>::place;
      
    private:
      template<typename T, typename Iter>
      std::size_t write_sequence_(Iter beg, Iter end, std::true_type trivial_and_contiguous) {
        std::size_t n = std::distance(beg, end);
        void *spot = this->place(storage_size_of<T>().arrayed(n));
        detail::template memcpy_aligned<alignof(T)>(spot, &*beg, n*sizeof(T));
        return n;
      }
      
      template<typename T, typename Iter>
      std::size_t write_sequence_(Iter beg, Iter end, std::false_type trivial_and_contiguous) {
        std::size_t n = 0;
        for(Iter x=beg; x != end; ++x, ++n)
          upcxx::template serialization_traits<T>::serialize(*this, *x);
        return n;
      }

    public:
      template<typename Iter>
      std::size_t write_sequence(Iter beg, Iter end, std::size_t n=-1) {
        using T = typename std::remove_cv<
            typename std::iterator_traits<Iter>::value_type
          >::type;
        
        return this->template write_sequence_<T,Iter>(beg, end,
          /*trivial_and_contiguous=*/std::integral_constant<bool,
              serialization_traits<T>::is_actually_trivially_serializable &&
              is_iterator_contiguous<Iter>::value
            >()
        );
      }
    };

    template<>
    class serialization_writer</*bounded=*/false>:
      public serialization_writer_base<serialization_writer</*bounded=*/false>> {
      
      std::uintptr_t base_;
      std::size_t edge_;
      std::size_t size_, align_;

      struct hunk_footer {
        hunk_footer *next;
        void *front;
        std::size_t size0;
      };
      hunk_footer *head_, *tail_;
      
      void grow(std::size_t size0, std::size_t size1);
      void compact_and_invalidate_(void *buf);
      
    public:
      serialization_writer(void *initial_buf, std::size_t initial_capacity):
        base_(reinterpret_cast<std::uintptr_t>(initial_buf)),
        edge_((initial_capacity & -alignof(hunk_footer)) - sizeof(hunk_footer)),
        size_(0), align_(1),
        head_(::new((char*)initial_buf + edge_) hunk_footer),
        tail_(head_) {

        UPCXX_ASSERT(sizeof(hunk_footer) <= initial_capacity);
        UPCXX_ASSERT(detail::is_aligned(initial_buf, serialization_align_max));
        
        head_->next = nullptr;
        head_->front = initial_buf;
        head_->size0 = 0;
      }

      ~serialization_writer() {
        hunk_footer *h = head_ ? head_->next : nullptr;
        while(h != nullptr) {
          hunk_footer *h1 = h->next;
          std::free(h->front);
          h = h1;
        }
      }

      serialization_writer(serialization_writer<false> const&) = delete;

      serialization_writer(serialization_writer<false> &&that) {
        this->base_ = that.base_;
        this->edge_ = that.edge_;
        this->size_ = that.size_;
        this->align_ = that.align_;
        this->head_ = that.head_;
        this->tail_ = that.tail_;
        
        that.base_ = 0; that.edge_ = 0;
        that.size_ = 0; that.align_ = 1;
        that.head_ = that.tail_ = nullptr;
      }
      
      std::size_t size() const { return size_; }
      std::size_t align() const { return align_; }

      bool contained_in_initial() const {
        return head_ == tail_;
      }
      
      void compact_and_invalidate(void *buf) {
        this->compact_and_invalidate_(buf);
        head_ = nullptr; // do this in inlineable code so the compiler can elide the destructor body
      }
      
      void* place(std::size_t obj_size, std::size_t obj_align) {
        std::size_t size0 = size_;
        size0 = (size0 + obj_align-1) & -obj_align;

        std::size_t size1 = size0 + obj_size;
        std::size_t align1 = obj_align > align_ ? obj_align : align_;
        
        if(size1 > edge_)
          this->grow(size0, size1);

        size_ = size1;
        align_ = align1;
        return reinterpret_cast<void*>(base_ + size0);
      }

      using serialization_writer_base<serialization_writer</*bounded=*/false>>::place;
      
    private:
      template<typename T, typename Iter>
      Iter write_elts_bounded_(Iter xs, std::size_t n, std::true_type trivial_and_contiguous) {
        const std::size_t alignof_T = n == 0 ? 1 : alignof(T);

        std::size_t size0 = size_;
        size0 = (size0 + alignof_T-1) & -alignof_T;
        
        detail::template memcpy_aligned<alignof(T)>(
            reinterpret_cast<void*>(base_ + size0), &*xs, n*sizeof(T)
          );
        
        size_ = size0 + n*sizeof(T);
        align_ = alignof_T > align_ ? alignof_T : align_;
        return xs + n;
      }
      
      template<typename T, typename Iter>
      Iter write_elts_bounded_(Iter xs, std::size_t n, std::false_type trivial_and_contiguous) {
        detail::template serialization_writer</*bounded=*/true> w1(reinterpret_cast<void*>(base_));
        w1.size_ = size_;
        w1.align_ = align_;
        while(n--) {
          upcxx::template serialization_traits<T>::serialize(w1, *xs);
          ++xs;
        }
        size_ = w1.size_;
        align_ = w1.align_;
        return xs;
      }
      
      template<typename T, typename Iter, bool n_is_valid, typename Size>
      std::size_t write_sequence_(Iter beg, Iter end, std::integral_constant<bool,n_is_valid>, std::size_t n, Size elt_ub) {
        constexpr auto trivial_and_contiguous = std::integral_constant<bool,
            serialization_traits<T>::is_actually_trivially_serializable &&
            is_iterator_contiguous<Iter>::value
          >();
        
        if(n_is_valid || std::is_same<std::random_access_iterator_tag, typename std::iterator_traits<Iter>::iterator_category>::value) {
          if(!n_is_valid)
            n = std::distance(beg, end);
          
          std::size_t size0 = size_;
          size0 = (size0 + elt_ub.align-1) & -elt_ub.align;
          
          std::size_t n0 = (edge_ - size0)/elt_ub.size;
          n0 = n < n0 ? n : n0;
          
          beg = this->template write_elts_bounded_<T,Iter>(beg, n0, trivial_and_contiguous);
          n -= n0;
          
          if(n != 0) {
            size0 = size_;
            std::size_t size1 = size0 + n*elt_ub.size;
            this->grow(size0, size1);
            
            this->template write_elts_bounded_<T,Iter>(beg, n, trivial_and_contiguous);
          }
        }
        else {
          n = 0;
          while(beg != end) {
            upcxx::template serialization_traits<T>::serialize(*this, *beg);
            ++beg;
            ++n;
          }
        }
        
        return n;
      }

      template<typename T, typename Iter, bool n_is_valid>
      std::size_t write_sequence_(Iter beg, Iter end, std::integral_constant<bool,n_is_valid>, std::size_t n, invalid_storage_size_t elt_ub) {
        if(n_is_valid) {
          std::size_t n1 = n;
          while(n1-- != 0) {
            upcxx::template serialization_traits<T>::serialize(*this, *beg);
            ++beg;
          }
        }
        else {
          n = 0;
          while(beg != end) {
            upcxx::template serialization_traits<T>::serialize(*this, *beg);
            ++beg;
            ++n;
          }
        }
        return n;
      }
      
    public:
      template<typename Iter>
      std::size_t write_sequence(Iter beg, Iter end) {
        using T = typename std::remove_cv<
            typename std::iterator_traits<Iter>::value_type
          >::type;
        
        return this->template write_sequence_<T,Iter>(beg, end, std::false_type(), 0, serialization_traits<T>::static_ubound);
      }
      
      template<typename Iter>
      std::size_t write_sequence(Iter beg, Iter end, std::size_t n) {
        using T = typename std::remove_cv<
            typename std::iterator_traits<Iter>::value_type
          >::type;
        
        return this->template write_sequence_<T,Iter>(beg, end, std::true_type(), n, serialization_traits<T>::static_ubound);
      }
    };

    class serialization_reader {
      std::uintptr_t head_;
      
    public:
      serialization_reader(void const *buf):
        head_(reinterpret_cast<std::uintptr_t>(buf)) {
      }

      char* head() const { return reinterpret_cast<char*>(head_); }
      
      void jump(std::uintptr_t delta) { head_ += delta; }
      
      template<typename T, typename T1 = typename serialization_traits<T>::deserialized_type>
      T1 read() {
        detail::raw_storage<T1> raw;
        upcxx::template serialization_traits<T>::deserialize(*this, &raw);
        return raw.value_and_destruct();
      }

      template<typename T, typename T1 = typename serialization_traits<T>::deserialized_type>
      T1* read_into(void *raw) {
        return upcxx::template serialization_traits<T>::deserialize(*this, raw);
      }

      void* unplace(std::size_t obj_size, std::size_t obj_align) {
        head_ = (head_ + obj_align-1) & -obj_align;
        void *ans = reinterpret_cast<void*>(head_);
        head_ += obj_size;
        return ans;
      }
      void* unplace(storage_size<> obj) {
        return this->unplace(obj.size, obj.align);
      }
      
      template<typename T>
      void skip() {
        serialization_traits<T>::skip(*this);
      }

      template<typename T>
      T* read_trivial_into(void *raw) {
        return detail::template construct_trivial<T>(raw, this->unplace(storage_size_of<T>()));
      }

      template<typename T>
      T read_trivial() {
        detail::raw_storage<T> raw;
        T ans = std::move(*this->template read_trivial_into<T>(&raw));
        raw.destruct();
        return ans;
      }

    private:
      template<typename T, typename T1>
      T1* read_sequence_into_(void *raw, std::size_t n, std::true_type trivial_serz) {
        auto ss = storage_size_of<T1>().arrayed(n);
        return detail::template construct_trivial<T1>(raw, this->unplace(ss), n);
      }

      template<typename T, typename T1>
      T1* read_sequence_into_(void *raw, std::size_t n, std::false_type trivial_serz) {
        T1 *ans = reinterpret_cast<T1*>(raw);
        for(std::size_t i=0; i != n; i++) {
          T1 *elt = this->template read_into<T>(reinterpret_cast<T1*>(raw) + i);
          if(i == 0) ans = elt;
        }
        return ans;
      }

    public:
      template<typename T,
               typename T1 = typename serialization_traits<T>::deserialized_type>
      T1* read_sequence_into(void *raw, std::size_t n) {
        return this->template read_sequence_into_<T,T1>(raw, n,
            std::integral_constant<bool, serialization_traits<T>::is_actually_trivially_serializable>()
          );
      }
      
      template<typename T, typename OutIter>
      void read_sequence_into_iterator(OutIter into, std::size_t n) {
        while(n--) {
          *into = this->template read<T>();
          ++into;
        }
      }

      template<typename T>
      static constexpr bool skip_sequence_is_fast() {
        return serialization_traits<T>::static_ubound_t::is_valid;
      }
      
      template<typename T>
      void skip_sequence(std::size_t n) {
        if(n != 0) {
          this->template skip<T>();
          n -= 1;
          if(serialization_traits<T>::static_ubound_t::is_valid)
            this->unplace(serialization_traits<T>::static_ubound.arrayed(n)); // skip rest
          else {
            while(n--)
              this->template skip<T>();
          }
        }
      }
    };
  }

  namespace detail {
    template<typename T, bool is_empty = std::is_empty<T>::value>
    struct serialization_trivial;

    template<typename T>
    struct serialization_trivial<T, /*is_empty=*/false> {
      static constexpr bool is_actually_trivially_serializable = true;
      
      template<typename Prefix>
      static constexpr auto ubound(Prefix pre, T const&)
        UPCXX_RETURN_DECLTYPE(
          pre.template cat_size_of<T>()
        ) {
        return pre.template cat_size_of<T>();
      }

      template<typename Writer>
      static void serialize(Writer &w, T const &x) {
        w.template write_trivial<T>(x);
      }

      static constexpr bool references_buffer = false;
      using deserialized_type = T;
      
      template<typename Reader>
      static T* deserialize(Reader &r, void *raw) {
        return r.template read_trivial_into<T>(raw);
      }

      static constexpr bool skip_is_fast = true;

      template<typename Reader>
      static void skip(Reader &r) {
        r.unplace(storage_size_of<T>());
      }
    };

    template<typename T>
    struct serialization_trivial<T, /*is_empty=*/true> {
      static constexpr bool is_actually_trivially_serializable = false;
      
      template<typename Prefix>
      static constexpr Prefix ubound(Prefix pre, T const&) {
        return pre;
      }
      
      template<typename Writer>
      static void serialize(Writer&, T const&) {}

      static constexpr bool references_buffer = false;
      using deserialized_type = T;
      
      template<typename Reader>
      static T* deserialize(Reader&, void *raw) {
        return detail::template construct_default<T>(raw);
      }

      static constexpr bool skip_is_fast = true;

      template<typename Reader>
      static void skip(Reader&) {}
    };

    #define UPCXX_SERIALIZED_DELETE() \
      struct upcxx_serialization { \
        template<typename T> \
        struct supply_type_please { \
          static constexpr bool is_serializable = false; \
          template<typename Writer> \
          static void serialize(Writer &w, T const&) { \
            static_assert(-sizeof(Writer)==1, "Type has serialization deleted via UPCXX_SERIALIZED_DELETE."); \
          } \
          template<typename Reader> \
          static T* deserialize(Reader &r, void *spot) { \
            static_assert(-sizeof(Reader)==1, "Type has serialization deleted via UPCXX_SERIALIZED_DELETE."); \
            return nullptr; \
          } \
        }; \
      };
    
    #define UPCXX_SERIALIZED_FIELDS(...) \
    private: /* this macro requires "public" protection so we know what to restore */ \
      template<typename> \
      friend struct ::upcxx::detail::serialization_fields; \
      template<typename upcxx_reserved_prefix_fields_not_values = ::std::true_type> \
      auto upcxx_reserved_prefix_serialized_fields() \
        UPCXX_RETURN_DECLTYPE(::std::forward_as_tuple(__VA_ARGS__)) { \
        return ::std::forward_as_tuple(__VA_ARGS__); \
      } \
    public: /* restore "public" protection */ \
      struct upcxx_serialization { \
      private: \
        template<typename> \
        friend struct ::upcxx::detail::serialization_fields; \
        template<typename upcxx_reserved_prefix_T> \
        static upcxx_reserved_prefix_T* default_construct(void *spot) { \
          return ::new(spot) upcxx_reserved_prefix_T; \
        } \
      public: \
        template<typename upcxx_reserved_prefix_T> \
        struct supply_type_please: ::upcxx::detail::serialization_fields<upcxx_reserved_prefix_T> {}; \
      };

    template<typename T, typename U, bool fields_not_values,
             typename T_maybe_const = typename std::conditional<fields_not_values, T, T const>::type>
    T_maybe_const* serialized_fields_base_cast(U *u, std::integral_constant<bool,fields_not_values>) {
      static_assert(fields_not_values ? !std::is_polymorphic<T>::value : true, "UPCXX_SERIALIZED_BASE(Type) within UPCXX_SERIALIZED_FIELDS(...) requires Type not be polymorphic.");
      static_assert(fields_not_values ? true : !std::is_abstract<T>::value, "UPCXX_SERIALIZED_BASE(Type) within UPCXX_SERIALIZED_VALUES(...) requires Type not be abstract.");
      return static_cast<T_maybe_const*>(u);
    }
    // Need to use "..." to accept a type since template instantiations can
    // contain commas not nested in parenthesis.
    #define UPCXX_SERIALIZED_BASE(...) *::upcxx::detail::template serialized_fields_base_cast<__VA_ARGS__>(this, upcxx_reserved_prefix_fields_not_values())

    template<typename TupRefs,
             int i = 0,
             int n = std::tuple_size<TupRefs>::value>
    struct serialization_fields_each;
    
    template<typename TupRefs, int i, int n>
    struct serialization_fields_each {
      using Ti = typename std::remove_reference<typename std::tuple_element<i, TupRefs>::type>::type;

      static_assert(
        std::is_same<Ti, typename serialization_traits<Ti>::deserialized_type>::value,
        "Serialization via UPCXX_SERIALIZED_FIELDS(...) requires that all "
        "fields serialize and deserialize as the same type."
      );

      template<typename Prefix>
      static auto ubound(Prefix pre, TupRefs const &refs)
        UPCXX_RETURN_DECLTYPE(
          serialization_fields_each<TupRefs, i+1, n>::ubound(
            pre.cat_ubound_of(std::template get<i>(refs)),
            refs
          )
        ) {
        return serialization_fields_each<TupRefs, i+1, n>::ubound(
          pre.cat_ubound_of(std::template get<i>(refs)),
          refs
        );
      }

      template<typename Writer>
      static void serialize(Writer &w, TupRefs refs) {
        w.write(std::template get<i>(refs));
        serialization_fields_each<TupRefs, i+1, n>::serialize(w, refs);
      }

      static constexpr bool references_buffer = serialization_traits<Ti>::references_buffer
                                             || serialization_fields_each<TupRefs, i+1, n>::references_buffer;

      static void deserialize_destruct(TupRefs refs) {
        Ti *spot = &std::template get<i>(refs);
        detail::template destruct<Ti>(*spot);
        
        serialization_fields_each<TupRefs, i+1, n>::deserialize_destruct(refs);
      }

      template<typename Reader>
      static void deserialize_read(Reader &r, TupRefs refs) {
        Ti *spot = &std::template get<i>(refs);
        r.template read_into<Ti>(spot);
        
        serialization_fields_each<TupRefs, i+1, n>::deserialize_read(r, refs);
      }

      static constexpr bool skip_is_fast = serialization_traits<Ti>::skip_is_fast
                                        && serialization_fields_each<TupRefs, i+1, n>::skip_is_fast;
      
      template<typename Reader>
      static void skip(Reader &r) {
        r.template skip<Ti>();
        serialization_fields_each<TupRefs, i+1, n>::skip(r);
      }
    };
    
    template<typename TupRefs, int n>
    struct serialization_fields_each<TupRefs, n, n> {
      template<typename Prefix>
      static Prefix ubound(Prefix pre, TupRefs const &refs) {
        return pre;
      }

      template<typename Writer>
      static void serialize(Writer &w, TupRefs refs) {}

      static constexpr bool references_buffer = false;
      
      static void deserialize_destruct(TupRefs refs) {}
      template<typename Reader>
      static void deserialize_read(Reader &r, TupRefs refs) {}

      static constexpr bool skip_is_fast = true;
      
      template<typename Reader>
      static void skip(Reader &r) {}
    };
    
    template<typename T>
    struct serialization_fields {
      using refs_tup_type = decltype(std::declval<T&>().upcxx_reserved_prefix_serialized_fields());
      
      static constexpr bool is_serializable = true;

      template<typename Prefix>
      static auto ubound(Prefix pre, T const &x)
        UPCXX_RETURN_DECLTYPE(
          serialization_fields_each<refs_tup_type>::ubound(pre, const_cast<T&>(x).upcxx_reserved_prefix_serialized_fields())
        ) {
        return serialization_fields_each<refs_tup_type>::ubound(pre, const_cast<T&>(x).upcxx_reserved_prefix_serialized_fields());
      }

      template<typename Writer>
      static void serialize(Writer &w, T const &x) {
        serialization_fields_each<refs_tup_type>::serialize(w, const_cast<T&>(x).upcxx_reserved_prefix_serialized_fields());
      }

      using deserialized_type = T;

      static constexpr bool references_buffer = serialization_fields_each<refs_tup_type>::references_buffer;
      
      template<typename Reader>
      static deserialized_type* deserialize(Reader &r, void *raw) {
        T *rec = T::upcxx_serialization::template default_construct<T>(raw);
        //T *rec = ::new(raw) T;
        refs_tup_type refs_tup(rec->upcxx_reserved_prefix_serialized_fields());
        
        // Deserialization happens in two phases: 1) destruct, 2) read.
        // This avoids a tiny corner case when empty base subobjects can alias
        // the storage of member fields (empty base optimization) and that base's
        // destructor likes to nuke the one byte it *thinks* it inhabits, e.g.:
        //
        // struct bad_empty_base { ~bad_empty_base() { std::memset(this, 0 , sizeof(bad_empty_base)); } };
        //
        serialization_fields_each<refs_tup_type>::deserialize_destruct(refs_tup);
        serialization_fields_each<refs_tup_type>::deserialize_read(r, refs_tup);
        
        // since we're destructing/placement-new'ing fields in-place we have to launder the instance pointer.
        return detail::launder(rec);
      }

      static constexpr bool skip_is_fast = serialization_fields_each<refs_tup_type>::skip_is_fast;
      
      template<typename Reader>
      static void skip(Reader &r) {
        serialization_fields_each<refs_tup_type>::skip(r);
      }
    };

    ////////////////////////////////////////////////////////////////////////////

    /* The tuple returned from upcxx_reserved_prefix_serialized_values() will have rvalue-refs
     * decayed to naked values but lvalue (const or not) preserved. Since the
     * expression generating each value has access to the object members but no
     * function locals or parameters, we know that any lvalues that come out
     * must be part of the object (or globals, weird). So we keep those as lvalues
     * to avoid copying them (which could be bad for perf). The rvalues must have been
     * computed by the expression so must be decayed and copied (via move) to
     * survive into the beyond.
     */
    #define UPCXX_SERIALIZED_VALUES(...) \
    private: /* macro requires "public" protection so we know what to restore */ \
      template<typename> \
      friend struct ::upcxx::detail::serialization_values; \
      template<typename upcxx_reserved_prefix_fields_not_values = ::std::false_type> \
      auto upcxx_reserved_prefix_serialized_values() const \
        UPCXX_RETURN_DECLTYPE(::upcxx::detail::forward_as_tuple_decay_rrefs(__VA_ARGS__)) { \
        return ::upcxx::detail::forward_as_tuple_decay_rrefs(__VA_ARGS__); \
      } \
    public: /* restore "public" protection */ \
      struct upcxx_serialization { \
      private: \
        template<typename, int, int> \
        friend struct ::upcxx::detail::serialization_values_each; \
        template<typename upcxx_reserved_prefix_T, typename ...upcxx_reserved_prefix_Arg> \
        static upcxx_reserved_prefix_T* construct(void *spot, upcxx_reserved_prefix_Arg &&...arg) { \
          return ::new(spot) upcxx_reserved_prefix_T(static_cast<upcxx_reserved_prefix_Arg&&>(arg)...); \
        } \
      public: \
        template<typename upcxx_reserved_prefix_T> \
        struct supply_type_please: ::upcxx::detail::serialization_values<upcxx_reserved_prefix_T> {}; \
      };
    
    template<typename TupRefs, int i=0, int n=std::tuple_size<TupRefs>::value>
    struct serialization_values_each {
      // Ti = decay(TupRefs[i]) but without decaying arrays, leave those be!
      using Ti = typename std::remove_cv<typename std::remove_reference<typename std::tuple_element<i, TupRefs>::type>::type>::type;
      using recurse_tail = serialization_values_each<TupRefs, i+1, n>;
      
      template<typename Prefix>
      static auto ubound(Prefix pre, TupRefs const &refs)
        UPCXX_RETURN_DECLTYPE(
          recurse_tail::ubound(
            pre.cat_ubound_of(std::template get<i>(refs)),
            refs
          )
        ) {
        return recurse_tail::ubound(
          pre.cat_ubound_of(std::template get<i>(refs)),
          refs
        );
      }

      template<typename Writer>
      static void serialize(Writer &w, TupRefs const &refs) {
        w.write(std::template get<i>(refs));
        recurse_tail::serialize(w, refs);
      }

      static constexpr bool references_buffer = serialization_traits<Ti>::references_buffer
                                             || recurse_tail::references_buffer;
      
      template<typename Obj, typename Reader, typename ...Ptrs>
      static Obj* deserialize(Reader &r, void *spot, Ptrs ...ptrs) {
        using Ti1 = typename serialization_traits<Ti>::deserialized_type;
        typename std::aligned_storage<sizeof(Ti1), alignof(Ti1)>::type storage;
        Ti1 *val = r.template read_into<Ti>(&storage);
        Obj *ans = recurse_tail::template deserialize<Obj>(r, spot, ptrs..., val);
        detail::template destruct<Ti1>(*val);
        return ans;
      }

      static constexpr bool skip_is_fast = serialization_traits<Ti>::skip_is_fast
                                        && recurse_tail::skip_is_fast;
      
      template<typename Reader>
      static void skip(Reader &r) {
        r.template skip<Ti>();
        recurse_tail::skip(r);
      }
    };

    template<typename TupRefs, int n>
    struct serialization_values_each<TupRefs, n, n> {
      template<typename Prefix>
      static Prefix ubound(Prefix pre, TupRefs const &refs) {
        return pre;
      }

      template<typename Writer>
      static void serialize(Writer &w, TupRefs refs) {}

      static constexpr bool references_buffer = false;

      template<typename Obj, typename Reader, typename ...Ptrs>
      static Obj* deserialize(Reader &r, void *spot, Ptrs ...ptrs) {
        //return ::new(spot) Obj(static_cast<typename std::remove_pointer<Ptrs>::type&&>(*ptrs)...);
        return Obj::upcxx_serialization::template construct<Obj>(spot, static_cast<typename std::remove_pointer<Ptrs>::type&&>(*ptrs)...);
      }
      
      static constexpr bool skip_is_fast = true;
      
      template<typename Reader>
      static void skip(Reader &r) {}
    };

    template<typename T>
    struct serialization_values {
      // a tuple possibly mixed of lvalue refs and naked values
      using refs_tup_type = decltype(std::declval<T&>().upcxx_reserved_prefix_serialized_values());

      static constexpr bool is_serializable = true;
    
      template<typename Prefix>
      static auto ubound(Prefix pre, T const &x)
        UPCXX_RETURN_DECLTYPE(
          serialization_values_each<refs_tup_type>::ubound(pre, x.upcxx_reserved_prefix_serialized_values())
        ) {
        return serialization_values_each<refs_tup_type>::ubound(pre, x.upcxx_reserved_prefix_serialized_values());
      }

      template<typename Writer>
      static void serialize(Writer &w, T const &x) {
        serialization_values_each<refs_tup_type>::serialize(w, x.upcxx_reserved_prefix_serialized_values());
      }

      using deserialized_type = T;

      static constexpr bool references_buffer = serialization_values_each<refs_tup_type>::references_buffer;
           
      template<typename Reader>
      static deserialized_type* deserialize(Reader &r, void *spot) {
        return serialization_values_each<refs_tup_type>::template deserialize<T>(r, spot);
      }

      static constexpr bool skip_is_fast = serialization_values_each<refs_tup_type>::skip_is_fast;
      
      template<typename Reader>
      static void skip(Reader &r) {
        serialization_values_each<refs_tup_type>::skip(r);
      }
    };
    
    ////////////////////////////////////////////////////////////////////////////

    // ...otherwise checks if has nested subclass T::upcxx_serialization::supply_type_please<typename>
    template<typename T, bool bless_trivial_default, typename=void>
    struct serialization_dispatch1;

    // ...otherwise checks if has T::upcxx_serialization
    // ...finally otherwise dispatch to trivial serialization
    template<typename T, bool bless_trivial_default, typename=void>
    struct serialization_dispatch2;

    ////////////////////////////////////////////////////////////////////////////

    template<typename T, bool bless_trivial_default>
    struct serialization_dispatch1<T, bless_trivial_default,
        typename std::conditional<true, void, typename T::upcxx_serialization::template supply_type_please<T>>::type
      >: T::upcxx_serialization::template supply_type_please<T> {
    };

    template<typename T, bool bless_trivial_default, typename>
    struct serialization_dispatch1: serialization_dispatch2<T, bless_trivial_default> {};

    template<typename T, bool bless_trivial_default>
    struct serialization_dispatch2<T, bless_trivial_default,
        typename std::conditional<true, void, typename T::upcxx_serialization>::type
      >: T::upcxx_serialization {
    };

    template<typename T, bool bless_trivial_default, typename>
    struct serialization_dispatch2:
      serialization_trivial<T> {

      static constexpr serialization_existence existence =
        bless_trivial_default ? serialization_existence::trivial_blessed
                              : serialization_existence::trivial_unsafe;

      static constexpr bool is_serializable = bless_trivial_default;
    };

    // query to determine if serialization has been specialized or provided
    // via nested class or macros.
    template<typename T, typename/*=std::false_type*/>
    struct serialization_get_existence {
      static constexpr serialization_existence value = serialization_existence::user;
    };

    template<typename T>
    struct serialization_get_existence<T,
        std::integral_constant<bool,
          // some expression involving serialization<T>::existence that always produces false
          serialization<T>::existence == serialization_existence::invalid
        >
      > {
      static constexpr serialization_existence value = serialization<T>::existence;
    };
  }

  template<typename T>
  struct serialization: detail::serialization_dispatch1<T, /*bless_trivial_default=*/false> {};

  namespace detail {
    template<typename T, typename=std::false_type>
    struct serialization_traits_serializable {
      static constexpr bool is_serializable = true; // true since its absence means the user has provided serialization
    };
    template<typename T>
    struct serialization_traits_serializable<T,
        std::integral_constant<bool, false & serialization<T>::is_serializable>
      > {
    };

    template<typename T, typename=std::false_type>
    struct serialization_traits_actually_trivially_serializable {
      static constexpr bool is_actually_trivially_serializable = false;
    };
    template<typename T>
    struct serialization_traits_actually_trivially_serializable<T,
        std::integral_constant<bool, false & serialization<T>::is_actually_trivially_serializable>
      > {
    };

    template<typename T, typename=std::false_type>
    struct serialization_traits_skip_is_fast {
      static constexpr bool skip_is_fast = false;
    };
    template<typename T>
    struct serialization_traits_skip_is_fast<T,
        std::integral_constant<bool, false & serialization<T>::skip_is_fast>
      > {
    };

    template<typename T, typename=void>
    struct serialization_traits_ubound {
      template<typename Prefix>
      static invalid_storage_size_t ubound(Prefix pre, T const&) {
        return invalid_storage_size;
      }
    };
    template<typename T>
    struct serialization_traits_ubound<T,
        decltype((
          serialization<T>().ubound(empty_storage_size, std::declval<T const&>()),
          void()
        ))
      > {
    };
    
    template<typename T, typename=std::false_type>
    struct serialization_traits_references_buffer {
      static constexpr bool references_buffer = false;
    };
    template<typename T>
    struct serialization_traits_references_buffer<T,
        std::integral_constant<bool, false & serialization<T>::references_buffer>
      > {
    };

    template<typename T, typename=void>
    struct serialization_traits_deserialized_type {
      using deserialized_type = typename std::remove_pointer<
          decltype(
            serialization<T>::deserialize(std::declval<detail::serialization_reader&>(), nullptr)
          )
        >::type;
    };
    template<typename T>
    struct serialization_traits_deserialized_type<T,
        typename std::conditional<true, void, typename serialization<T>::deserialized_type>::type
      > {
    };

    template<typename T, bool is_triv_serz = is_trivially_serializable<T>::value>
    struct serialization_traits2;

    template<typename T>
    struct serialization_traits2<T, /*is_triv_serz=*/true>:
      serialization_trivial<T> {
      static constexpr bool is_serializable = true;
    };
    
    template<typename T>
    struct serialization_traits2<T, /*is_triv_serz=*/false>:
      detail::serialization_traits_serializable<T>,
      detail::serialization_traits_actually_trivially_serializable<T>,
      detail::serialization_traits_skip_is_fast<T>,
      detail::serialization_traits_ubound<T>,
      detail::serialization_traits_references_buffer<T>,
      detail::serialization_traits_deserialized_type<T>,
      serialization<T> {
    };

    template<typename T, typename=void>
    struct serialization_traits_deserialized_value {
      // Can't implement deserialized_value() because it's return type would be a
      // not returnable type (like a native array).
    };
    template<typename T>
    struct serialization_traits_deserialized_value<T,
        // this specialization fails if deserilized_value has an invalid signature
        typename std::conditional<true, void, typename serialization_traits2<T>::deserialized_type(*)()>::type
      > {
      static typename serialization_traits2<T>::deserialized_type
      deserialized_value(T const &x) {
        using T1 = typename serialization_traits2<T>::deserialized_type;
        
        auto ub = serialization_traits2<T>::ubound(empty_storage_size, x);
        constexpr std::size_t static_storage_size = (decltype(ub)::static_size) < 512 ? decltype(ub)::static_size : 512;
        detail::xaligned_storage<static_storage_size, serialization_align_max> static_storage;
        
        void *storage;
        std::size_t storage_size;
        
        if(decltype(ub)::is_static || !decltype(ub)::is_valid) {
          storage = static_storage.storage();
          storage_size = static_storage_size;
        }
        else {
          storage = detail::alloc_aligned(ub.size, ub.align);
          storage_size = ub.size;
        }
        
        detail::serialization_writer</*bounded=*/decltype(ub)::is_valid> w(storage, storage_size);
        
        serialization_traits2<T>::serialize(w, x);
        
        if(!w.contained_in_initial()) {
          storage_size = w.size();
          storage = detail::alloc_aligned(storage_size, std::max(sizeof(void*), w.align()));
          w.compact_and_invalidate(storage);
        }
        
        detail::serialization_reader r(storage);
        detail::raw_storage<T1> x1_raw;
        (void)serialization_traits2<T>::deserialize(r, &x1_raw);
        
        if(storage != static_storage.storage())
          std::free(storage);
        
        return x1_raw.value_and_destruct();
      }
    };
    
    template<typename T>
    struct serialization_traits1:
      detail::serialization_traits_deserialized_value<T>,
      serialization_traits2<T> {
    };
  }
  
  template<typename T>
  struct serialization_traits: detail::serialization_traits1<T> {
    using static_ubound_t = typename decltype(
      detail::serialization_traits1<T>::ubound(
          empty_storage_size, std::declval<T const&>()
        )
      )::static_otherwise_invalid_t;
    
    static constexpr static_ubound_t static_ubound = static_ubound_t(static_ubound_t::static_size, static_ubound_t::static_align);
  };

  template<typename T>
  constexpr typename serialization_traits<T>::static_ubound_t serialization_traits<T>::static_ubound;
  
  //////////////////////////////////////////////////////////////////////////////

  namespace detail {
    struct serialization_not_supported {
      static constexpr bool is_serializable = false;
    };
  }
  
  template<typename T>
  struct serialization<T&>: detail::serialization_not_supported {};
  template<typename T>
  struct serialization<T&&>: detail::serialization_not_supported {};

  template<typename T>
  struct serialization<T const>: serialization_traits<T> {
    // inherit is_serializable
    // inherit references_buffer

    static constexpr detail::serialization_existence existence = detail::serialization_get_existence<T>::value;
    
    using deserialized_type = const typename serialization_traits<T>::deserialized_type;

    // inherit ubound
    // inherit serialize
    // inherit skip_is_fast

    template<typename Reader>
    static deserialized_type* deserialize(Reader &r, void *spot) {
      return serialization_traits<T>::deserialize(r, spot);
    }

    // inherit skip
  };
  
  //////////////////////////////////////////////////////////////////////////////

  template<typename R, typename ...A>
  struct serialization<R(&)(A...)> {
    static constexpr bool is_serializable = true;
    static constexpr bool references_buffer = false;

    using deserialized_type = std::reference_wrapper<R(A...)>;

    template<typename Prefix>
    static constexpr auto ubound(Prefix pre, R(&)(A...))
      UPCXX_RETURN_DECLTYPE(
        pre.template cat_size_of<deserialized_type>()
      ) {
      return pre.template cat_size_of<deserialized_type>();
    }

    template<typename Writer>
    static void serialize(Writer &w, R(&fn)(A...)) {
      w.write_trivial(deserialized_type(fn));
    }

    template<typename Reader>
    static deserialized_type* deserialize(Reader &r, void *raw) {
      return r.template read_trivial_into<deserialized_type>(raw);
    }

    static constexpr bool skip_is_fast = true;

    template<typename Reader>
    static void skip(Reader &r) {
      r.unplace(storage_size_of<deserialized_type>());
    }
  };

  //////////////////////////////////////////////////////////////////////////////

  template<>
  struct is_trivially_serializable<std::tuple<>>: std::true_type {};
  
  template<typename T0, typename ...Ts>
  struct is_trivially_serializable<std::tuple<T0,Ts...>> {
    static constexpr bool value =
      is_trivially_serializable<T0>::value &&
      is_trivially_serializable<std::tuple<Ts...>>::value;
  };
  
  namespace detail {
    template<typename Tup, int i=0, int n=std::tuple_size<Tup>::value>
    struct serialization_tuple;

    template<typename ...T, int i, int n>
    struct serialization_tuple<std::tuple<T...>, i, n> {
      using Ti = typename std::tuple_element<i, std::tuple<T...>>::type;
      using Ti1 = typename serialization_traits<Ti>::deserialized_type;
      using recurse_tail = serialization_tuple<std::tuple<T...>, i+1, n>;
      
      static constexpr bool is_serializable =
        serialization_traits<Ti>::is_serializable &&
        recurse_tail::is_serializable;

      template<typename Prefix>
      static auto ubound(Prefix pre, std::tuple<T...> const &x)
        UPCXX_RETURN_DECLTYPE(
          recurse_tail::ubound(
            pre.template cat_ubound_of<Ti>(std::template get<i>(x)),
            x
          )
        ) {
        return recurse_tail::ubound(
          pre.template cat_ubound_of<Ti>(std::template get<i>(x)),
          x
        );
      }
      
      static constexpr bool references_buffer =
        serialization_traits<Ti>::references_buffer ||
        recurse_tail::references_buffer;

      template<typename Writer>
      static void serialize(Writer &w, std::tuple<T...> const &x) {
        w.template write<Ti>(std::template get<i>(x));
        recurse_tail::serialize(w, x);
      }

      template<typename TupOut, typename Reader, typename ...Ptrs>
      static TupOut* deserialize_each(Reader &r, void *spot, Ptrs ...ptrs) {
        typename std::aligned_storage<sizeof(Ti1),alignof(Ti1)>::type storage;
        Ti1 *val = r.template read_into<Ti>(&storage);
        TupOut *ans = recurse_tail::template deserialize_each<TupOut>(r, spot, ptrs..., val);
        detail::template destruct<Ti1>(*val);
        return ans;
      }

      static constexpr bool skip_is_fast =
        serialization_traits<Ti>::skip_is_fast &&
        recurse_tail::skip_is_fast;
      
      template<typename Reader>
      static void skip(Reader &r) {
        r.template skip<Ti>();
        recurse_tail::skip(r);
      }
    };
    
    template<typename ...T, int n>
    struct serialization_tuple<std::tuple<T...>, n, n> {
      static constexpr bool is_serializable = true;

      template<typename Prefix>
      static Prefix ubound(Prefix pre, std::tuple<T...> const &x) {
        return pre;
      }
      
      static constexpr bool references_buffer = false;

      template<typename Writer>
      static void serialize(Writer &w, std::tuple<T...> const &x) {}
      
      template<typename TupOut, typename Reader, typename ...Ptrs>
      static TupOut* deserialize_each(Reader &r, void *spot, Ptrs ...ptrs) {
        return ::new(spot) TupOut(static_cast<typename std::remove_pointer<Ptrs>::type&&>(*ptrs)...);
      }

      static constexpr bool skip_is_fast = true;

      template<typename Reader>
      static void skip(Reader&) {/*nop*/}
    };
  }

  template<typename ...T>
  struct serialization<std::tuple<T...>>:
    detail::serialization_tuple<std::tuple<T...>> {
    
    using deserialized_type = std::tuple<
        typename serialization_traits<T>::deserialized_type...
      >;
    
    template<typename Reader>
    static deserialized_type* deserialize(Reader &r, void *spot) {
      return detail::serialization_tuple<std::tuple<T...>>::template deserialize_each<deserialized_type>(r, spot);
    }
  };

  //////////////////////////////////////////////////////////////////////////////

  template<typename A, typename B>
  struct is_trivially_serializable<std::pair<A,B>> {
    static constexpr bool value = is_trivially_serializable<A>::value &&
                                  is_trivially_serializable<B>::value;
  };
  
  template<typename A, typename B>
  struct serialization<std::pair<A,B>> {
    static constexpr bool is_serializable =
      serialization_traits<A>::is_serializable &&
      serialization_traits<B>::is_serializable;

    template<typename Prefix>
    static auto ubound(Prefix pre, std::pair<A,B> const &x)
      UPCXX_RETURN_DECLTYPE(
        pre.cat_ubound_of(x.first).cat_ubound_of(x.second)
      ) {
      return pre.cat_ubound_of(x.first).cat_ubound_of(x.second);
    }
    
    template<typename Writer>
    static void serialize(Writer &w, std::pair<A,B> const &x) {
      w.template write<A>(x.first);
      w.template write<B>(x.second);
    }

    static constexpr bool references_buffer =
      serialization_traits<A>::references_buffer ||
      serialization_traits<B>::references_buffer;

    using A1 = typename serialization_traits<A>::deserialized_type;
    using B1 = typename serialization_traits<B>::deserialized_type;
    
    using deserialized_type = std::pair<A1,B1>;
    
    template<typename Reader>
    static deserialized_type* deserialize(Reader &r, void *raw) {
      A1 a = r.template read<A>();
      B1 b = r.template read<B>();
      return ::new(raw) std::pair<A1,B1>{std::move(a), std::move(b)};
    }

    static constexpr bool skip_is_fast =
      serialization_traits<A>::skip_is_fast &&
      serialization_traits<B>::skip_is_fast;

    template<typename Reader>
    static void skip(Reader &r) {
      r.template skip<A>();
      r.template skip<B>();
    }
  };

  //////////////////////////////////////////////////////////////////////////////

  #ifndef UPCXX_CREDUCE_SLIM
  template<typename T, std::size_t n>
  struct is_trivially_serializable<std::array<T,n>>:
    is_trivially_serializable<T> {
  };
  
  template<typename T, std::size_t n>
  struct serialization<std::array<T,n>> {
    static constexpr bool is_serializable = serialization_traits<T>::is_serializable;

    template<typename Prefix>
    static constexpr auto ubound(Prefix pre, std::array<T,n> const &x)
      UPCXX_RETURN_DECLTYPE(
        pre.cat(serialization_traits<T>::static_ubound.template arrayed<n>())
      ) {
      return pre.cat(serialization_traits<T>::static_ubound.template arrayed<n>());
    }
    
    template<typename Writer>
    static void serialize(Writer &w, std::array<T,n> const &x) {
      w.write_sequence(&x[0], &x[0] + n, n);
    }

    static constexpr bool references_buffer = serialization_traits<T>::references_buffer;

    using T1 = typename serialization_traits<T>::deserialized_type;
    
    using deserialized_type = std::array<T1,n>;
    
    template<typename Reader>
    static deserialized_type* deserialize(Reader &r, void *raw) {
      return reinterpret_cast<std::array<T1,n>*>(r.template read_sequence_into<T>(raw, n));
    }

    static constexpr bool skip_is_fast = detail::serialization_reader::template skip_sequence_is_fast<T>();
    
    template<typename Reader>
    static void skip(Reader &r) {
      r.template skip_sequence<T>(n);
    }
  };
  #endif
  
  //////////////////////////////////////////////////////////////////////////////

  template<typename T, std::size_t n>
  struct is_trivially_serializable<T[n]>:
    is_trivially_serializable<T> {
  };
  
  template<typename T, std::size_t n>
  struct serialization<T[n]> {
    static constexpr bool is_serializable = serialization_traits<T>::is_serializable;

    template<typename Prefix>
    static constexpr auto ubound(Prefix pre, T const(&x)[n])
      UPCXX_RETURN_DECLTYPE(
        pre.cat(serialization_traits<T>::static_ubound.template arrayed<n>())
      ) {
      return pre.cat(serialization_traits<T>::static_ubound.template arrayed<n>());
    }
    
    template<typename Writer>
    static void serialize(Writer &w, T const(&x)[n]) {
      w.write_sequence(&x[0], &x[0] + n, n);
    }

    static constexpr bool references_buffer = serialization_traits<T>::references_buffer;

    using T1 = typename serialization_traits<T>::deserialized_type;
    
    using deserialized_type = T1[n];
    
    template<typename Reader>
    static deserialized_type* deserialize(Reader &r, void *raw) {
      return reinterpret_cast<T1(*)[n]>(r.template read_sequence_into<T>(raw, n));
    }

    static constexpr bool skip_is_fast = detail::serialization_reader::template skip_sequence_is_fast<T>();
    
    template<typename Reader>
    static void skip(Reader &r) {
      r.template skip_sequence<T>(n);
    }
  };

  //////////////////////////////////////////////////////////////////////////////

  #ifndef UPCXX_CREDUCE_SLIM

  /* This is where we hardcode certain builtin c++ std types which semantically
   * *ought* to be Serializable and make them so. Our strategy is to provide a
   * different specialization for upcxx::serialization that instructs the dispatch
   * machinery to "bless" the default strategy of trivial serialization in the
   * case where no user provided mechanism was found.
   *
   * Q) Does this work if the user specializes upcxx::serialization?
   * A) Yes. Their specialization will be a better fit than ours and thus chosen
   *    unamibguously by the compiler and so nothing we do here will matter.
   *    Example:
   *
   *   namespace upcxx {
   *      // user specialization
   *      template<> struct serialization<std::equal_to<user_type>> {...};
   *      // is a tighter fit than ours:
   *      template<typename T> struct serialization<std::equal_to<T>> {...};
   *   }
   *
   * Q) Does this work if the user nests serialization into the class via
   *    subclass or macros?
   * A) Yes. detail::serialization_dispatch1 will find that and ignore the value
   *    we're pushing here for bless_trivial_default, which is only utilized
   *    when no serialization was found nested in class.
   *
   * Q) Does this work if user specializes is_trivially_serializable<T> to true?
   * A) Yes. serialization_traits<T> first looks at is_trivially_serializable<T>
   *    before anything else, so again, nothing we do here will matter.
   */

  template<typename T>
  struct serialization<std::allocator<T>>: detail::serialization_dispatch1<std::allocator<T>, /*bless_trivial_default=*/true> {};
  
  template<typename T>
  struct serialization<std::less<T>>: detail::serialization_dispatch1<std::less<T>, /*bless_trivial_default=*/true> {};
  template<typename T>
  struct serialization<std::less_equal<T>>: detail::serialization_dispatch1<std::less_equal<T>, /*bless_trivial_default=*/true> {};
  template<typename T>
  struct serialization<std::greater<T>>: detail::serialization_dispatch1<std::greater<T>, /*bless_trivial_default=*/true> {};
  template<typename T>
  struct serialization<std::greater_equal<T>>: detail::serialization_dispatch1<std::greater_equal<T>, /*bless_trivial_default=*/true> {};
  template<typename T>
  struct serialization<std::equal_to<T>>: detail::serialization_dispatch1<std::equal_to<T>, /*bless_trivial_default=*/true> {};
  template<typename T>
  struct serialization<std::not_equal_to<T>>: detail::serialization_dispatch1<std::not_equal_to<T>, /*bless_trivial_default=*/true> {};

  #define UPCXX_HARDCODE_STD_HASH(type) \
    template<> \
    struct serialization<std::hash<type>>: detail::serialization_dispatch1<std::hash<type>, /*bless_trivial_default=*/true> {};

  UPCXX_HARDCODE_STD_HASH(bool)
  UPCXX_HARDCODE_STD_HASH(char)
  UPCXX_HARDCODE_STD_HASH(signed char)
  UPCXX_HARDCODE_STD_HASH(unsigned char)
  #if __cpp_char8_t
    UPCXX_HARDCODE_STD_HASH(char8_t)
  #endif
  UPCXX_HARDCODE_STD_HASH(char16_t)
  UPCXX_HARDCODE_STD_HASH(char32_t)
  UPCXX_HARDCODE_STD_HASH(wchar_t)
  UPCXX_HARDCODE_STD_HASH(short)
  UPCXX_HARDCODE_STD_HASH(unsigned short)
  UPCXX_HARDCODE_STD_HASH(int)
  UPCXX_HARDCODE_STD_HASH(unsigned int)
  UPCXX_HARDCODE_STD_HASH(long)
  UPCXX_HARDCODE_STD_HASH(long long)
  UPCXX_HARDCODE_STD_HASH(unsigned long)
  UPCXX_HARDCODE_STD_HASH(unsigned long long)
  UPCXX_HARDCODE_STD_HASH(float)
  UPCXX_HARDCODE_STD_HASH(double)
  UPCXX_HARDCODE_STD_HASH(long double)
  #if __cplusplus >= 201700L
    UPCXX_HARDCODE_STD_HASH(std::nullptr_t)
  #endif

  #undef UPCXX_HARDCODE_STD_HASH
  
  template<typename T>
  struct serialization<std::hash<T*>>:
    detail::serialization_dispatch1<
      std::hash<T*>, /*bless_trivial_default=*/true
    > {
  };
  
  template<typename CharT, typename Traits, typename Alloc>
  struct serialization<std::hash<std::basic_string<CharT, Traits, Alloc>>>:
    detail::serialization_dispatch1<
      std::hash<std::basic_string<CharT, Traits, Alloc>>, /*bless_trivial_default=*/true
    > {
  };

  template<typename T, typename Del>
  struct serialization<std::hash<std::unique_ptr<T,Del>>>:
    detail::serialization_dispatch1<
      std::hash<std::unique_ptr<T,Del>>, /*bless_trivial_default=*/true
    > {
  };
  
  template<typename T>
  struct serialization<std::hash<std::shared_ptr<T>>>:
    detail::serialization_dispatch1<
      std::hash<std::shared_ptr<T>>, /*bless_trivial_default=*/true
    > {
  };
  
  #endif
  
  //////////////////////////////////////////////////////////////////////////////

  #ifndef UPCXX_CREDUCE_SLIM
  template<typename CharT, typename Traits, typename Alloc>
  struct serialization<std::basic_string<CharT, Traits, Alloc>> {
    static_assert(std::is_trivial<CharT>::value, "Bad string character type.");
    
    static constexpr bool is_serializable = serialization_traits<Alloc>::is_serializable;

    using Str = std::basic_string<CharT,Traits,Alloc>;
    
    template<typename Prefix>
    static auto ubound(Prefix pre, Str const &s)
      UPCXX_RETURN_DECLTYPE(
        pre.template cat_ubound_of<Alloc>(std::declval<Alloc>())
           .template cat_ubound_of<std::size_t>(1)
           .cat(storage_size_of<CharT>().arrayed(1))
      ) {
      std::size_t n = s.size();
      return pre.template cat_ubound_of<Alloc>(s.get_allocator())
                .template cat_ubound_of<std::size_t>(n)
                .cat(storage_size_of<CharT>().arrayed(n));
    }

    template<typename Writer>
    static void serialize(Writer &w, Str const &s) {
      std::size_t n = s.size();
      w.template write<Alloc>(s.get_allocator());
      w.template write<std::size_t>(n);
      w.write_sequence(&s[0], &s[0] + n, n);
    }

    static constexpr bool references_buffer = serialization_traits<Alloc>::references_buffer;
    
    template<typename Reader>
    static Str* deserialize(Reader &r, void *raw) {
      Alloc a = r.template read<Alloc>();
      std::size_t n = r.template read<std::size_t>();
      CharT const *p = (CharT const*)r.unplace(storage_size_of<CharT>().arrayed(n));
      return ::new(raw) Str(p, n, std::move(a));
    }

    static constexpr bool skip_is_fast = serialization_traits<Alloc>::skip_is_fast;

    template<typename Reader>
    static void skip(Reader &r) {
      r.template skip<Alloc>();
      std::size_t n = r.template read<std::size_t>();
      r.unplace(storage_size_of<CharT>().arrayed(n));
    }
  };
  #endif
  
  //////////////////////////////////////////////////////////////////////////////

  #ifndef UPCXX_CREDUCE_SLIM
  namespace detail {
    template<typename Bag, typename=void>
    struct reserve_if_supported {
      void operator()(Bag&, std::size_t) {/*nop*/}
    };
    template<typename Bag>
    struct reserve_if_supported<Bag, decltype((std::declval<Bag&>().reserve(1), void()))> {
      void operator()(Bag &bag, std::size_t n) {
        bag.reserve(n);
      }
    };

    template<typename Bag, typename=void>
    struct inserter {
      std::insert_iterator<Bag> operator()(Bag &bag) {
        return std::insert_iterator<Bag>(bag, bag.end());
      }
    };
    template<typename Bag>
    struct inserter<Bag, decltype((std::declval<Bag&>().push_back(), void()))> {
      std::back_insert_iterator<Bag> operator()(Bag &bag) {
        return std::back_insert_iterator<Bag>(bag);
      }
    };

    template<typename BagIn, typename BagOut,
             typename T0 = typename BagIn::value_type,
             typename T1 = typename BagOut::value_type>
    struct serialization_container_sequence {
      static constexpr bool is_serializable = serialization_traits<typename BagIn::allocator_type>::is_serializable &&
                                              serialization_traits<T0>::is_serializable;
      
      template<typename Prefix>
      static auto ubound(Prefix pre, BagIn const &bag)
        UPCXX_RETURN_DECLTYPE(
          pre.template cat_ubound_of<typename BagIn::allocator_type>(std::declval<typename BagIn::allocator_type>())
             .template cat_ubound_of<std::size_t>(1)
             .cat(serialization_traits<T0>::static_ubound.arrayed(1))
        ) {
        std::size_t n = bag.size();
        return pre.template cat_ubound_of<typename BagIn::allocator_type>(bag.get_allocator())
                  .template cat_ubound_of<std::size_t>(n)
                  .cat(serialization_traits<T0>::static_ubound.arrayed(n));
      }

      template<typename Writer>
      static void serialize(Writer &w, BagIn const &bag) {
        std::size_t n = bag.size();
        w.template write<typename BagIn::allocator_type>(bag.get_allocator());
        w.write_trivial(n);
        w.write_sequence(bag.begin(), bag.end(), n);
      }

      static constexpr bool references_buffer = serialization_traits<typename BagIn::allocator_type>::references_buffer ||
                                                serialization_traits<T0>::references_buffer;

      using deserialized_type = BagOut;

      template<typename Reader>
      static BagOut* deserialize(Reader &r, void *raw) {
        typename BagOut::allocator_type a = r.template read<typename BagIn::allocator_type>();
        std::size_t n = r.template read_trivial<std::size_t>();
        BagOut *bag = ::new(raw) BagOut(std::move(a));
        detail::template reserve_if_supported<BagOut>()(*bag, n);
        r.template read_sequence_into_iterator<T0>(detail::template inserter<BagOut>()(*bag), n);
        return bag;
      }

      template<typename Reader>
      static void skip(Reader &r) {
        r.template skip<typename BagIn::allocator_type>();
        std::size_t n = r.template read_trivial<std::size_t>();
        r.template skip_sequence<T0>(n);
      }

      static constexpr bool skip_is_fast = serialization_traits<typename BagIn::allocator_type>::skip_is_fast &&
                                           serialization_reader::template skip_sequence_is_fast<T0>();
    };

    template<typename BagIn, typename BagOut,
             typename T0 = typename BagIn::value_type,
             typename T1 = typename BagOut::value_type>
    struct serialization_container_ordered {
      static constexpr bool is_serializable = serialization_traits<typename BagIn::allocator_type>::is_serializable &&
                                              serialization_traits<typename BagIn::key_compare>::is_serializable &&
                                              serialization_traits<T0>::is_serializable;
      
      template<typename Prefix>
      static auto ubound(Prefix pre, BagIn const &bag)
        UPCXX_RETURN_DECLTYPE(
          pre.template cat_ubound_of<typename BagIn::allocator_type>(std::declval<typename BagIn::allocator_type>())
             .template cat_ubound_of<typename BagIn::key_compare>(std::declval<typename BagIn::key_compare>())
             .template cat_ubound_of<std::size_t>(1)
             .cat(serialization_traits<T0>::static_ubound.arrayed(1))
        ) {
        std::size_t n = bag.size();
        return pre.template cat_ubound_of<typename BagIn::allocator_type>(bag.get_allocator())
                  .template cat_ubound_of<typename BagIn::key_compare>(bag.key_comp())
                  .template cat_ubound_of<std::size_t>(n)
                  .cat(serialization_traits<T0>::static_ubound.arrayed(n));
      }

      template<typename Writer>
      static void serialize(Writer &w, BagIn const &bag) {
        std::size_t n = bag.size();
        w.template write<typename BagIn::allocator_type>(bag.get_allocator());
        w.template write<typename BagIn::key_compare>(bag.key_comp());
        w.write_trivial(n);
        w.write_sequence(bag.begin(), bag.end(), n);
      }

      static constexpr bool references_buffer = serialization_traits<typename BagIn::allocator_type>::references_buffer ||
                                                serialization_traits<typename BagIn::key_compare>::references_buffer ||
                                                serialization_traits<T0>::references_buffer;

      using deserialized_type = BagOut;

      template<typename Reader>
      static BagOut* deserialize(Reader &r, void *raw) {
        typename BagOut::allocator_type a = r.template read<typename BagIn::allocator_type>();
        typename BagOut::key_compare k = r.template read<typename BagIn::key_compare>();
        std::size_t n = r.template read_trivial<std::size_t>();
        BagOut *bag = ::new(raw) BagOut(std::move(k), std::move(a));
        detail::template reserve_if_supported<BagOut>()(*bag, n);
        r.template read_sequence_into_iterator<T0>(detail::template inserter<BagOut>()(*bag), n);
        return bag;
      }

      template<typename Reader>
      static void skip(Reader &r) {
        r.template skip<typename BagIn::allocator_type>();
        r.template skip<typename BagIn::key_compare>();
        std::size_t n = r.template read_trivial<std::size_t>();
        r.template skip_sequence<T0>(n);
      }

      static constexpr bool skip_is_fast = serialization_traits<typename BagIn::allocator_type>::skip_is_fast &&
                                           serialization_traits<typename BagIn::key_compare>::skip_is_fast &&
                                           serialization_reader::template skip_sequence_is_fast<T0>();
    };

    template<typename BagIn, typename BagOut,
             typename T0 = typename BagIn::value_type,
             typename T1 = typename BagOut::value_type>
    struct serialization_container_unordered {
      static constexpr bool is_serializable = serialization_traits<typename BagIn::allocator_type>::is_serializable &&
                                              serialization_traits<typename BagIn::key_equal>::is_serializable &&
                                              serialization_traits<typename BagIn::hasher>::is_serializable &&
                                              serialization_traits<T0>::is_serializable;
      
      template<typename Prefix>
      static auto ubound(Prefix pre, BagIn const &bag)
        UPCXX_RETURN_DECLTYPE(
          pre.template cat_ubound_of<typename BagIn::allocator_type>(std::declval<typename BagIn::allocator_type>())
             .template cat_ubound_of<typename BagIn::key_equal>(std::declval<typename BagIn::key_equal>())
             .template cat_ubound_of<typename BagIn::hasher>(std::declval<typename BagIn::hasher>())
             .template cat_ubound_of<std::size_t>(1)
             .cat(serialization_traits<T0>::static_ubound.arrayed(1))
        ) {
        std::size_t n = bag.size();
        return pre.template cat_ubound_of<typename BagIn::allocator_type>(bag.get_allocator())
                  .template cat_ubound_of<typename BagIn::key_equal>(bag.key_eq())
                  .template cat_ubound_of<typename BagIn::hasher>(bag.hash_function())
                  .template cat_ubound_of<std::size_t>(n)
                  .cat(serialization_traits<T0>::static_ubound.arrayed(n));
      }

      template<typename Writer>
      static void serialize(Writer &w, BagIn const &bag) {
        std::size_t n = bag.size();
        w.template write<typename BagIn::allocator_type>(bag.get_allocator());
        w.template write<typename BagIn::key_equal>(bag.key_eq());
        w.template write<typename BagIn::hasher>(bag.hash_function());
        w.write_trivial(n);
        w.write_sequence(bag.begin(), bag.end(), n);
      }

      static constexpr bool references_buffer = serialization_traits<typename BagIn::allocator_type>::references_buffer ||
                                                serialization_traits<typename BagIn::key_equal>::references_buffer ||
                                                serialization_traits<typename BagIn::hasher>::references_buffer ||
                                                serialization_traits<T0>::references_buffer;

      using deserialized_type = BagOut;

      template<typename Reader>
      static BagOut* deserialize(Reader &r, void *raw) {
        typename BagOut::allocator_type a = r.template read<typename BagIn::allocator_type>();
        typename BagOut::key_equal k = r.template read<typename BagIn::key_equal>();
        typename BagOut::hasher h = r.template read<typename BagIn::hasher>();
        std::size_t n = r.template read_trivial<std::size_t>();
        BagOut *bag = ::new(raw) BagOut(n, std::move(h), std::move(k), std::move(a));
        detail::template reserve_if_supported<BagOut>()(*bag, n);
        r.template read_sequence_into_iterator<T0>(detail::template inserter<BagOut>()(*bag), n);
        return bag;
      }

      template<typename Reader>
      static void skip(Reader &r) {
        r.template skip<typename BagIn::allocator_type>();
        r.template skip<typename BagIn::key_equal>();
        r.template skip<typename BagIn::hasher>();
        std::size_t n = r.template read_trivial<std::size_t>();
        r.template skip_sequence<T0>(n);
      }

      static constexpr bool skip_is_fast = serialization_traits<typename BagIn::allocator_type>::skip_is_fast &&
                                           serialization_traits<typename BagIn::key_equal>::skip_is_fast &&
                                           serialization_traits<typename BagIn::hasher>::skip_is_fast &&
                                           serialization_reader::template skip_sequence_is_fast<T0>();
    };
  }

  template<typename T, typename Alloc>
  struct serialization<std::vector<T,Alloc>>:
    detail::serialization_container_sequence<
      std::vector<T, Alloc>,
      std::vector<typename serialization_traits<T>::deserialized_type, typename serialization_traits<Alloc>::deserialized_type>
    > {
  };
  template<typename T, typename Alloc>
  struct serialization<std::deque<T,Alloc>>:
    detail::serialization_container_sequence<
      std::deque<T, Alloc>,
      std::deque<typename serialization_traits<T>::deserialized_type, typename serialization_traits<Alloc>::deserialized_type>
    > {
  };
  template<typename T, typename Alloc>
  struct serialization<std::list<T,Alloc>>:
    detail::serialization_container_sequence<
      std::list<T, Alloc>,
      std::list<typename serialization_traits<T>::deserialized_type, typename serialization_traits<Alloc>::deserialized_type>
    > {
  };

  template<typename T, typename Cmp, typename Alloc>
  struct serialization<std::set<T,Cmp,Alloc>>:
    detail::serialization_container_ordered<
      std::set<T,Cmp,Alloc>,
      std::set<typename serialization_traits<T>::deserialized_type, Cmp, typename serialization_traits<Alloc>::deserialized_type>
    > {
  };
  template<typename T, typename Cmp, typename Alloc>
  struct serialization<std::multiset<T,Cmp,Alloc>>:
    detail::serialization_container_ordered<
      std::multiset<T,Cmp,Alloc>,
      std::multiset<typename serialization_traits<T>::deserialized_type, Cmp, typename serialization_traits<Alloc>::deserialized_type>
    > {
  };

  template<typename T, typename Hash, typename Eq, typename Alloc>
  struct serialization<std::unordered_set<T,Hash,Eq,Alloc>>:
    detail::serialization_container_unordered<
      std::unordered_set<T,Hash,Eq,Alloc>,
      std::unordered_set<typename serialization_traits<T>::deserialized_type, Hash, Eq, typename serialization_traits<Alloc>::deserialized_type>
    > {
  };
  template<typename T, typename Hash, typename Eq, typename Alloc>
  struct serialization<std::unordered_multiset<T,Hash,Eq,Alloc>>:
    detail::serialization_container_unordered<
      std::unordered_multiset<T,Hash,Eq,Alloc>,
      std::unordered_multiset<typename serialization_traits<T>::deserialized_type, Hash, Eq, typename serialization_traits<Alloc>::deserialized_type>
    > {
  };

  template<typename K, typename V, typename Cmp, typename Alloc>
  struct serialization<std::map<K,V,Cmp,Alloc>>:
    detail::serialization_container_ordered<
      std::map<K,V,Cmp,Alloc>,
      std::map<
        typename serialization_traits<K>::deserialized_type,
        typename serialization_traits<V>::deserialized_type, Cmp, typename serialization_traits<Alloc>::deserialized_type>
    > {
  };
  template<typename K, typename V, typename Cmp, typename Alloc>
  struct serialization<std::multimap<K,V,Cmp,Alloc>>:
    detail::serialization_container_ordered<
      std::multimap<K,V,Cmp,Alloc>,
      std::multimap<
        typename serialization_traits<K>::deserialized_type,
        typename serialization_traits<V>::deserialized_type, Cmp, typename serialization_traits<Alloc>::deserialized_type>
    > {
  };

  template<typename K, typename V, typename Hash, typename Eq, typename Alloc>
  struct serialization<std::unordered_map<K,V,Hash,Eq,Alloc>>:
    detail::serialization_container_unordered<
      std::unordered_map<K,V,Hash,Eq,Alloc>,
      std::unordered_map<
        typename serialization_traits<K>::deserialized_type,
        typename serialization_traits<V>::deserialized_type, Hash, Eq, typename serialization_traits<Alloc>::deserialized_type>
    > {
  };
  template<typename K, typename V, typename Hash, typename Eq, typename Alloc>
  struct serialization<std::unordered_multimap<K,V,Hash,Eq,Alloc>>:
    detail::serialization_container_unordered<
      std::unordered_multimap<K,V,Hash,Eq,Alloc>,
      std::unordered_multimap<
        typename serialization_traits<K>::deserialized_type,
        typename serialization_traits<V>::deserialized_type, Hash, Eq, typename serialization_traits<Alloc>::deserialized_type>
    > {
  };

  template<typename T, typename Alloc>
  struct serialization<std::forward_list<T,Alloc>> {
    using T0 = T;
    using T1 = typename serialization_traits<T>::deserialized_type;

    static constexpr bool is_serializable = serialization_traits<Alloc>::is_serializable &&
                                            serialization_traits<T0>::is_serializable;

    // no ubound
    
    template<typename Writer>
    static void serialize(Writer &w, std::forward_list<T0,Alloc> const &bag) {
      w.write(bag.get_allocator());
      void *n_spot = w.place(storage_size_of<std::size_t>());
      std::size_t n = w.write_sequence(bag.begin(), bag.end());
      ::new(n_spot) std::size_t(n);
    }

    static constexpr bool references_buffer = serialization_traits<Alloc>::references_buffer ||
                                              serialization_traits<T0>::references_buffer;

    using deserialized_type = std::forward_list<T1, typename serialization_traits<Alloc>::deserialized_type>;

    template<typename Reader>
    static deserialized_type* deserialize(Reader &r, void *raw) {
      auto a = r.template read<Alloc>();
      std::size_t n = r.template read_trivial<std::size_t>();
      deserialized_type *ans = ::new(raw) deserialized_type(std::move(a));
      if(n != 0) {
        ans->push_front(r.template read<T0>());
        n--;
        auto last = ans->begin();
        while(n--)
          last = ans->insert_after(last, r.template read<T0>());
      }
      return ans;
    }

    template<typename Reader>
    static void skip(Reader &r) {
      r.template skip<Alloc>();
      std::size_t n = r.template read_trivial<std::size_t>();
      r.template skip_sequence<T>(n);
    }

    static constexpr bool skip_is_fast = serialization_traits<Alloc>::skip_is_fast &&
                                         detail::serialization_reader::template skip_sequence_is_fast<T>();
  };
  #endif
}
#endif
