#ifndef _af3a33ad_9fc7_4d84_990b_b9caa94daf02
#define _af3a33ad_9fc7_4d84_990b_b9caa94daf02

#include <upcxx/utility.hpp>

#include <array>
#include <algorithm>
#include <cstdint>
#include <deque>
#include <forward_list>
#include <list>
#include <map>
#include <set>
#include <tuple>
#include <type_traits>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace upcxx {
  template<typename T>
  struct serialization;

  template<typename T>
  struct serialization_complete;

  namespace detail {
    template<typename T, typename=std::false_type>
    struct serialization_is_specialized;
  };
  
  template<typename T>
  struct is_definitely_trivially_serializable {
    static constexpr bool value = std::is_trivially_copyable<T>::value && !detail::serialization_is_specialized<T>::value;
  };

  template<typename T>
  struct is_definitely_serializable {
    static constexpr bool value = serialization_complete<T>::is_definitely_serializable;
  };

  template<typename T>
  struct deserialized_type_of {
    using type = typename serialization_complete<T>::deserialized_type;
  };
  template<typename T>
  using deserialized_type_of_t = typename serialization_complete<T>::deserialized_type;

  namespace detail {
    template<typename T>
    struct serialization_references_buffer_not {
      static constexpr bool value = !serialization_complete<T>::references_buffer;
    };
  }
  
  namespace detail {
    template<std::size_t s_size, std::size_t s_align>
    struct storage_size_base;

    template<>
    struct storage_size_base<std::size_t(-1), std::size_t(-1)> {
      static constexpr bool is_valid = false, is_static = false;
      static constexpr std::size_t static_size = -1;
      static constexpr std::size_t static_align = -1;
      static constexpr std::size_t static_align_ub = -1;
      static constexpr std::size_t size = -1, align = -1;
      
      constexpr storage_size_base(std::size_t dyn_size, std::size_t dyn_align) {}

      template<std::size_t s_size1, std::size_t s_align1>
      constexpr storage_size_base(storage_size_base<s_size1,s_align1> const &that) {}
    };
    
    template<std::size_t s_align_ub>
    struct storage_size_base<std::size_t(-2), s_align_ub> {
      static constexpr bool is_valid = true, is_static = false;
      static constexpr std::size_t static_size = -2;
      static constexpr std::size_t static_align = -2;
      static constexpr std::size_t static_align_ub = s_align_ub;
      
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
      static constexpr bool is_valid = true, is_static = true;
      static constexpr std::size_t static_size = s_size;
      static constexpr std::size_t static_align = s_align;
      static constexpr std::size_t static_align_ub = s_align;
      static constexpr std::size_t size = s_size, align = s_align;
      
      constexpr storage_size_base(std::size_t dyn_size, std::size_t dyn_align) {}
    };
  }

  template<std::size_t s_size=std::size_t(-2),
           std::size_t s_align=std::size_t(-2)>
  struct storage_size;

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
      std::size_t a = min_align > this->align ? min_align : this->align;
      return (this->size + a-1) & -a;
    }

    constexpr storage_size<
        s_size >= std::size_t(-2) || s_align >= std::size_t(-2) ? std::size_t(-1) : s_size,
        s_size >= std::size_t(-2) || s_align >= std::size_t(-2) ? std::size_t(-1) : s_align
      >
    static_otherwise_invalid() const {
      return {s_size, s_align};
    }

  private:
    template<std::size_t s_size1, std::size_t s_align1>
    constexpr auto cat_help(std::size_t size1, std::size_t align1) const
      -> storage_size<
        /*s_size = */(
          s_size >= std::size_t(-2) || s_size1 >= std::size_t(-2)
            ? /*max*/(s_size > s_size1 ? s_size : s_size1)
            : ((s_size + s_align1-1) & -s_align1) + s_size1
        ),
        /*s_align = max*/(
          s_align > s_align1 ? s_align : s_align1
        )
      > {
      return {
        ((this->size + align1-1) & -align1) + size1,
        this->align > align1 ? this->align : align1
      };
    }

  public:
    template<std::size_t s_size1, std::size_t s_align1>
    constexpr auto cat(storage_size<s_size1, s_align1> that) const
      -> decltype(this->template cat_help<s_size1, s_align1>(that.size, that.align)) {
      return this->template cat_help<s_size1, s_align1>(that.size, that.align);
    }

    template<std::size_t s_size1, std::size_t s_align1>
    constexpr auto cat()
      -> decltype(this->template cat_help<s_size1, s_align1>(s_size1, s_align1)) {
      return this->template cat_help<s_size1, s_align1>(s_size1, s_align1);
    }
    constexpr auto cat(std::size_t size1, std::size_t align1)
      -> decltype(this->template cat_help<std::size_t(-2),std::size_t(-2)>(size1, align1)) {
      return this->template cat_help<std::size_t(-2),std::size_t(-2)>(size1, align1);
    }
    
    template<typename T>
    constexpr auto cat_size_of() const
      -> decltype(this->cat(storage_size_of<T>())) {
      return this->cat(storage_size_of<T>());
    }

    template<typename T>
    constexpr auto cat_ubound_of(T const &x) const
      -> decltype(serialization_complete<T>::ubound(*this, x)) {
      return serialization_complete<T>::ubound(*this, x);
    }

    constexpr auto array(std::size_t n) const
      -> storage_size<
        -s_size == 1 ? std::size_t(-1) : std::size_t(-2),
        -s_size == 1 ? std::size_t(-1) : s_align
      > {
      return {n*this->size, n == 0 ? 1 : this->align};
    }

    template<std::size_t n>
    constexpr auto array() const
      -> storage_size<
        // size_ub
        n==0 ? 0 :
        -s_size==1 ? std::size_t(-1) :
        -s_size==2 ? std::size_t(-2) :
        n*s_size,

        // align_ub
        n==0 ? 1 :
        -s_align==1 ? std::size_t(-1) :
        s_align
      > {
      return {n*this->size, n==0 ? 1 : this->align};
    }
  };

  template<typename T>
  constexpr storage_size<sizeof(T), alignof(T)> storage_size_of() {
    return {sizeof(T), alignof(T)};
  }

  constexpr storage_size<0,1> empty_storage_size(0,1);

  using invalid_storage_size_t = storage_size<std::size_t(-1), std::size_t(-1)>;
  constexpr invalid_storage_size_t invalid_storage_size(-1,-1);

  namespace detail {
    template<typename Iter,
           typename T = typename std::remove_cv<typename std::iterator_traits<Iter>::value_type>::type>
    struct is_iterator_contiguous:
      std::integral_constant<bool,
        std::is_same<Iter, T*>::value ||
        std::is_same<Iter, T const*>::value ||

        std::is_same<Iter, typename std::array<T,1>::iterator>::value ||
        std::is_same<Iter, typename std::array<T,1>::const_iterator>::value ||

        (!std::is_same<T,bool>::value && (
          std::is_same<Iter, typename std::vector<T>::iterator>::value ||
          std::is_same<Iter, typename std::vector<T>::const_iterator>::value
        ))
      > {
    };
    
    template<bool bounded>
    class serialization_writer;

    template<>
    class serialization_writer</*bounded=*/true> {
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
        size_ = (size_ + obj_align-1) & -obj_align;
        void *spot = reinterpret_cast<void*>(buf_ + size_);
        size_ += obj_size;
        align_ = obj_align > align_ ? obj_align : align_;
        return spot;
      }
      void* place(storage_size<> obj) {
        return this->place(obj.size, obj.align);
      }

      template<typename T>
      T* place_new() {
        static_assert(std::is_trivially_copyable<T>::value, "T must be TriviallyCopyable");
        return ::new(this->place(sizeof(T), alignof(T))) T;
      }
      template<typename T>
      T* place_new(T val) {
        static_assert(std::is_trivially_copyable<T>::value, "T must be TriviallyCopyable");
        return ::new(this->place(sizeof(T), alignof(T))) T(val);
      }

      template<typename T>
      void push(T const &x) {
        upcxx::template serialization_complete<T>::serialize(*this, x);
      }

      template<typename T>
      void push_trivial(T const &x) {
        void *spot = this->place(storage_size_of<T>());
        detail::template memcpy_aligned<alignof(T)>(spot, &x, sizeof(T));
      }

    private:
      template<typename T, typename Iter>
      std::size_t push_sequence_(Iter beg, Iter end, std::true_type trivial_and_contiguous) {
        std::size_t n = std::distance(beg, end);
        void *spot = this->place(storage_size_of<T>().array(n));
        detail::template memcpy_aligned<alignof(T)>(spot, &*beg, n*sizeof(T));
        return n;
      }
      
      template<typename T, typename Iter>
      std::size_t push_sequence_(Iter beg, Iter end, std::false_type trivial_and_contiguous) {
        std::size_t n = 0;
        for(Iter x=beg; x != end; ++x, ++n)
          upcxx::template serialization_complete<T>::serialize(*this, *x);
        return n;
      }

    public:
      template<typename Iter>
      std::size_t push_sequence(Iter beg, Iter end, std::size_t n=-1) {
        using T = typename std::remove_cv<
            typename std::iterator_traits<Iter>::value_type
          >::type;
        
        return this->template push_sequence_<T,Iter>(beg, end,
          /*trivial_and_contiguous=*/std::integral_constant<bool,
              serialization_complete<T>::is_actually_trivially_serializable &&
              is_iterator_contiguous<Iter>::value
            >()
        );
      }
    };

    template<>
    class serialization_writer</*bounded=*/false> {
      std::uintptr_t base_;
      std::size_t edge_;
      std::size_t size_, align_;

      struct hunk_t {
        hunk_t *next;
        void *front;
        std::size_t size0;
      };
      hunk_t *head_, *tail_;
      
      void grow(std::size_t size0, std::size_t size1);
      void compact_and_invalidate_(void *buf);
      
    public:
      serialization_writer(void *initial_buf, std::size_t initial_capacity):
        base_(reinterpret_cast<std::uintptr_t>(initial_buf)),
        edge_((initial_capacity & -alignof(hunk_t)) - sizeof(hunk_t)),
        size_(0), align_(1),
        head_(::new((char*)initial_buf + edge_) hunk_t),
        tail_(head_) {

        UPCXX_ASSERT(sizeof(hunk_t) <= initial_capacity);
        
        head_->next = nullptr;
        head_->front = initial_buf;
        head_->size0 = 0;
      }

      ~serialization_writer() {
        hunk_t *h = head_ ? head_->next : nullptr;
        while(h != nullptr) {
          hunk_t *h1 = h->next;
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

      void* place(storage_size<> obj) {
        return this->place(obj.size, obj.align);
      }

      template<typename T>
      T* place_new() {
        static_assert(std::is_trivially_copyable<T>::value, "T must be TriviallyCopyable");
        return ::new(this->place(sizeof(T), alignof(T))) T;
      }
      template<typename T>
      T* place_new(T val) {
        static_assert(std::is_trivially_copyable<T>::value, "T must be TriviallyCopyable");
        return ::new(this->place(sizeof(T), alignof(T))) T(val);
      }
      
      template<typename T>
      void push(T const &x) {
        upcxx::template serialization_complete<T>::serialize(*this, x);
      }

      template<typename T>
      void push_trivial(T const &x) {
        void *spot = this->place(storage_size_of<T>());
        detail::template memcpy_aligned<alignof(T)>(spot, &x, sizeof(T));
      }

    private:
      template<typename T, typename Iter>
      Iter push_elts_bounded_(Iter xs, std::size_t n, std::true_type trivial_and_contiguous) {
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
      Iter push_elts_bounded_(Iter xs, std::size_t n, std::false_type trivial_and_contiguous) {
        detail::template serialization_writer</*bounded=*/true> w1(reinterpret_cast<void*>(base_));
        w1.size_ = size_;
        w1.align_ = align_;
        while(n--) {
          upcxx::template serialization_complete<T>::serialize(w1, *xs);
          ++xs;
        }
        size_ = w1.size_;
        align_ = w1.align_;
        return xs;
      }
      
      template<typename T, typename Iter, bool n_is_valid, typename Size>
      std::size_t push_sequence_(Iter beg, Iter end, std::integral_constant<bool,n_is_valid>, std::size_t n, Size elt_ub) {
        constexpr auto trivial_and_contiguous = std::integral_constant<bool,
            serialization_complete<T>::is_actually_trivially_serializable &&
            is_iterator_contiguous<Iter>::value
          >();
        
        if(n_is_valid || std::is_same<std::random_access_iterator_tag, typename std::iterator_traits<Iter>::iterator_category>::value) {
          if(!n_is_valid)
            n = std::distance(beg, end);
          
          std::size_t size0 = size_;
          size0 = (size0 + elt_ub.align-1) & -elt_ub.align;
          
          std::size_t n0 = (edge_ - size0)/elt_ub.size;
          n0 = n < n0 ? n : n0;
          
          beg = this->template push_elts_bounded_<T,Iter>(beg, n0, trivial_and_contiguous);
          n -= n0;
          
          if(n != 0) {
            size0 = size_;
            std::size_t size1 = size0 + n*elt_ub.size;
            this->grow(size0, size1);
            
            this->template push_elts_bounded_<T,Iter>(beg, n, trivial_and_contiguous);
          }
        }
        else {
          n = 0;
          while(beg != end) {
            upcxx::template serialization_complete<T>::serialize(*this, *beg);
            ++beg;
            ++n;
          }
        }
        
        return n;
      }

      template<typename T, typename Iter, bool n_is_valid>
      std::size_t push_sequence_(Iter beg, Iter end, std::integral_constant<bool,n_is_valid>, std::size_t n, invalid_storage_size_t elt_ub) {
        if(n_is_valid) {
          std::size_t n1 = n;
          while(n1-- != 0) {
            upcxx::template serialization_complete<T>::serialize(*this, *beg);
            ++beg;
          }
        }
        else {
          n = 0;
          while(beg != end) {
            upcxx::template serialization_complete<T>::serialize(*this, *beg);
            ++beg;
            ++n;
          }
        }
        return n;
      }
      
    public:
      template<typename Iter>
      std::size_t push_sequence(Iter beg, Iter end) {
        using T = typename std::remove_cv<
            typename std::iterator_traits<Iter>::value_type
          >::type;
        
        return this->template push_sequence_<T,Iter>(beg, end, std::false_type(), 0, serialization_complete<T>::static_ubound);
      }
      
      template<typename Iter>
      std::size_t push_sequence(Iter beg, Iter end, std::size_t n) {
        using T = typename std::remove_cv<
            typename std::iterator_traits<Iter>::value_type
          >::type;
        
        return this->template push_sequence_<T,Iter>(beg, end, std::true_type(), n, serialization_complete<T>::static_ubound);
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
      
      template<typename T, typename T1 = typename serialization_complete<T>::deserialized_type>
      T1 pop() {
        detail::raw_storage<T1> raw;
        upcxx::template serialization_complete<T>::deserialize(*this, &raw);
        return raw.value_and_destruct();
      }

      template<typename T, typename T1 = typename serialization_complete<T>::deserialized_type>
      T1* pop_into(void *raw) {
        return upcxx::template serialization_complete<T>::deserialize(*this, raw);
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
        serialization_complete<T>::skip(*this);
      }

      template<typename T>
      T* pop_trivial_into(void *raw) {
        return detail::template construct_trivial<T>(raw, this->unplace(storage_size_of<T>()));
      }

      template<typename T>
      T pop_trivial() {
        detail::raw_storage<T> raw;
        T ans = std::move(*this->template pop_trivial_into<T>(&raw));
        raw.destruct();
        return ans;
      }

    private:
      template<typename T, typename T1>
      T1* pop_sequence_into_(void *raw, std::size_t n, std::true_type trivial_serz) {
        auto ss = storage_size_of<T1>().array(n);
        return detail::template construct_trivial<T1>(raw, this->unplace(ss), n);
      }

      template<typename T, typename T1>
      T1* pop_sequence_into_(void *raw, std::size_t n, std::false_type trivial_serz) {
        T1 *ans = reinterpret_cast<T1*>(raw);
        for(std::size_t i=0; i != n; i++) {
          T1 *elt = this->template pop_into<T>(reinterpret_cast<T1*>(raw) + i);
          if(i == 0) ans = elt;
        }
        return ans;
      }

    public:
      template<typename T,
               typename T1 = typename serialization_complete<T>::deserialized_type>
      T1* pop_sequence_into(void *raw, std::size_t n) {
        return this->template pop_sequence_into_<T,T1>(raw, n,
            std::integral_constant<bool, serialization_complete<T>::is_actually_trivially_serializable>()
          );
      }
      
      template<typename T, typename OutIter>
      void pop_sequence_into(OutIter into, std::size_t n) {
        while(n--) {
          *into = this->template pop<T>();
          ++into;
        }
      }

      template<typename T>
      static constexpr bool skip_sequence_is_fast() {
        return serialization_complete<T>::static_ubound_t::is_valid;
      }
      
      template<typename T>
      void skip_sequence(std::size_t n) {
        if(n != 0) {
          this->template skip<T>();
          n -= 1;
          if(serialization_complete<T>::static_ubound_t::is_valid)
            this->unplace(serialization_complete<T>::static_ubound.array(n)); // skip rest
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
      static constexpr auto ubound(Prefix pre, T const&) ->
        decltype(pre.template cat_size_of<T>()) {
        return pre.template cat_size_of<T>();
      }

      template<typename Writer>
      static void serialize(Writer &w, T const &x) {
        w.template push_trivial<T>(x);
      }

      static constexpr bool references_buffer = false;
      using deserialized_type = T;
      
      template<typename Reader>
      static T* deserialize(Reader &r, void *raw) {
        return r.template pop_trivial_into<T>(raw);
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

    #define UPCXX_SERIALIZED_FIELDS(...) \
      auto upcxx_serialized_fields() -> \
        decltype(std::forward_as_tuple(__VA_ARGS__)) { \
        return std::forward_as_tuple(__VA_ARGS__); \
      }

    template<typename TupRefs,
             int i = 0,
             int n = std::tuple_size<TupRefs>::value>
    struct serialization_fields_each;
    
    template<typename TupRefs, int i, int n>
    struct serialization_fields_each {
      using Ti = typename std::remove_reference<typename std::tuple_element<i, TupRefs>::type>::type;

      static_assert(
        std::is_same<Ti, typename serialization_complete<Ti>::deserialized_type>::value,
        "Serialization via UPCXX_SERIALIZED_FIELDS(...) requires that all "
        "fields serialize and deserialize as the same type."
      );

      template<typename Prefix>
      static auto ubound(Prefix pre, TupRefs const &refs) ->
        decltype(
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
        w.push(std::template get<i>(refs));
        serialization_fields_each<TupRefs, i+1, n>::serialize(w, refs);
      }

      static constexpr bool references_buffer = serialization_complete<Ti>::references_buffer
                                             && serialization_fields_each<TupRefs, i+1, n>::references_buffer;

      template<typename Reader>
      static void deserialize(Reader &r, TupRefs refs) {
        Ti *spot = &std::template get<i>(refs);
        detail::template destruct<Ti>(*spot);
        r.template pop_into<Ti>(spot);
        
        serialization_fields_each<TupRefs, i+1, n>::deserialize(r, refs);
      }

      static constexpr bool skip_is_fast = serialization_complete<Ti>::skip_is_fast
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
      
      template<typename Reader>
      static void deserialize(Reader &r, TupRefs refs) {}

      static constexpr bool skip_is_fast = true;
      
      template<typename Reader>
      static void skip(Reader &r) {}
    };
    
    template<typename T>
    struct serialization_fields {
      using refs_tup_type = decltype(std::declval<T&>().upcxx_serialized_fields());
      
      template<typename Prefix>
      static auto ubound(Prefix pre, T const &x) ->
        decltype(
          serialization_fields_each<refs_tup_type>::ubound(pre, const_cast<T&>(x).upcxx_serialized_fields())
        ) {
        return serialization_fields_each<refs_tup_type>::ubound(pre, const_cast<T&>(x).upcxx_serialized_fields());
      }

      template<typename Writer>
      static void serialize(Writer &w, T const &x) {
        serialization_fields_each<refs_tup_type>::serialize(w, const_cast<T&>(x).upcxx_serialized_fields());
      }

      using deserialized_type = T;

      static constexpr bool references_buffer = serialization_fields_each<refs_tup_type>::references_buffer;
      
      template<typename Reader>
      static deserialized_type* deserialize(Reader &r, void *raw) {
        T *rec = ::new(raw) T;
        serialization_fields_each<refs_tup_type>::deserialize(r, rec->upcxx_serialized_fields());
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

    // ...otherwise checks if has nested subclass T::serialization
    template<typename T, typename=void>
    struct serialization_dispatch1;

    // ...otherwise checks if has UPCXX_SERIALIZED_FIELDS
    // ...finally otherwise dispatch to trivial serialization
    template<typename T, typename=void>
    struct serialization_dispatch2;

    ////////////////////////////////////////////////////////////////////////////

    template<typename T>
    struct serialization_dispatch1<T,
        typename std::conditional<true, void, typename T::serialization>::type
      >: T::serialization {
    };

    template<typename T, typename>
    struct serialization_dispatch1: serialization_dispatch2<T> {};

    template<typename T>
    struct serialization_dispatch2<T,
        decltype((std::declval<T&>().upcxx_serialized_fields(), void()))
      >:
      serialization_fields<T> {
      static constexpr bool is_definitely_serializable = true;
    };

    template<typename T, typename>
    struct serialization_dispatch2:
      serialization_trivial<T> {
      static constexpr bool is_specialized = false;
      static constexpr bool is_definitely_serializable = false;
    };

    template<typename T, typename>
    struct serialization_is_specialized: std::true_type {};

    template<typename T>
    struct serialization_is_specialized<T,
        std::integral_constant<bool, false & serialization<T>::is_specialized>
      >: std::integral_constant<bool, serialization<T>::is_specialized>  {
    };
  }

  template<typename T>
  struct serialization: detail::serialization_dispatch1<T> {};

  namespace detail {
    template<typename T, typename=std::false_type>
    struct serialization_complete_definitely_serializable {
      static constexpr bool is_definitely_serializable = true;
    };
    template<typename T>
    struct serialization_complete_definitely_serializable<T,
        std::integral_constant<bool, false & serialization<T>::is_definitely_serializable>
      > {
    };

    template<typename T, typename=std::false_type>
    struct serialization_complete_actually_trivially_serializable {
      static constexpr bool is_actually_trivially_serializable = false;
    };
    template<typename T>
    struct serialization_complete_actually_trivially_serializable<T,
        std::integral_constant<bool, false & serialization<T>::is_actually_trivially_serializable>
      > {
    };

    template<typename T, typename=std::false_type>
    struct serialization_complete_skip_is_fast {
      static constexpr bool skip_is_fast = false;
    };
    template<typename T>
    struct serialization_complete_skip_is_fast<T,
        std::integral_constant<bool, false & serialization<T>::skip_is_fast>
      > {
    };

    template<typename T, typename=void>
    struct serialization_complete_ubound {
      template<typename Prefix>
      static invalid_storage_size_t ubound(Prefix pre, T const&) {
        return invalid_storage_size;
      }
    };
    template<typename T>
    struct serialization_complete_ubound<T,
        decltype((
          serialization<T>().ubound(empty_storage_size, std::declval<T const&>()),
          void()
        ))
      > {
    };
    
    template<typename T, typename=std::false_type>
    struct serialization_complete_references_buffer {
      static constexpr bool references_buffer = false;
    };
    template<typename T>
    struct serialization_complete_references_buffer<T,
        std::integral_constant<bool, false & serialization<T>::references_buffer>
      > {
    };

    template<typename T, typename=void>
    struct serialization_complete_deserialized_type {
      using deserialized_type = typename std::remove_pointer<
          decltype(
            serialization<T>::deserialize(std::declval<detail::serialization_reader&>(), nullptr)
          )
        >::type;
    };
    template<typename T>
    struct serialization_complete_deserialized_type<T,
        typename std::conditional<true, void, typename serialization<T>::deserialized_type>::type
      > {
    };
    
    template<typename T, bool is_def_triv_serz = is_definitely_trivially_serializable<T>::value>
    struct serialization_complete1;

    template<typename T>
    struct serialization_complete1<T, /*is_def_triv_serz=*/true>:
      serialization_trivial<T> {
      static constexpr bool is_definitely_serializable = true;
    };
    
    template<typename T>
    struct serialization_complete1<T, /*is_def_triv_serz=*/false>:
      detail::serialization_complete_definitely_serializable<T>,
      detail::serialization_complete_actually_trivially_serializable<T>,
      detail::serialization_complete_skip_is_fast<T>,
      detail::serialization_complete_ubound<T>,
      detail::serialization_complete_references_buffer<T>,
      detail::serialization_complete_deserialized_type<T>,
      serialization<T> {
    };
  }
  
  template<typename T>
  struct serialization_complete: detail::serialization_complete1<T> {
    using static_ubound_t = decltype(
      detail::serialization_complete1<T>::ubound(
          empty_storage_size, std::declval<T const&>()
        ).static_otherwise_invalid()
    );
    
    static constexpr static_ubound_t static_ubound = static_ubound_t(static_ubound_t::static_size, static_ubound_t::static_align);
  };

  template<typename T>
  constexpr typename serialization_complete<T>::static_ubound_t serialization_complete<T>::static_ubound;
  
  template<typename T>
  struct serialization_complete<T const>: serialization_complete<T> {};
  
  //////////////////////////////////////////////////////////////////////////////

  namespace detail {
    struct serialization_not_supported {
      static constexpr bool is_definitely_serializable = false;
    };
  }
  
  template<typename T>
  struct serialization<T&>: detail::serialization_not_supported {};
  template<typename T>
  struct serialization<T&&>: detail::serialization_not_supported {};

  template<typename T>
  struct serialization<T const>: serialization<T> {};
  
  //////////////////////////////////////////////////////////////////////////////

  template<typename R, typename ...A>
  struct serialization<R(&)(A...)> {
    static constexpr bool is_definitely_serializable = true;
    static constexpr bool references_buffer = false;

    using deserialized_type = std::reference_wrapper<R(A...)>;

    template<typename Prefix>
    static constexpr auto ubound(Prefix pre, R(&)(A...)) ->
      decltype(pre.template cat_size_of<deserialized_type>()) {
      return pre.template cat_size_of<deserialized_type>();
    }

    template<typename Writer>
    static void serialize(Writer &w, R(&fn)(A...)) {
      w.push_trivial(deserialized_type(fn));
    }

    template<typename Reader>
    static deserialized_type* deserialize(Reader &r, void *raw) {
      return r.template pop_trivial_into<deserialized_type>(raw);
    }

    static constexpr bool skip_is_fast = true;

    template<typename Reader>
    static void skip(Reader &r) {
      r.unplace(storage_size_of<deserialized_type>());
    }
  };

  //////////////////////////////////////////////////////////////////////////////

  template<>
  struct is_definitely_trivially_serializable<std::tuple<>>: std::true_type {};

  template<typename T0, typename ...Ts>
  struct is_definitely_trivially_serializable<std::tuple<T0,Ts...>> {
    static constexpr bool value =
      is_definitely_trivially_serializable<T0>::value &&
      is_definitely_trivially_serializable<std::tuple<Ts...>>::value;
  };
  
  namespace detail {
    template<typename Tup, int i=0, int n=std::tuple_size<Tup>::value>
    struct serialization_tuple;

    template<typename ...T, int i, int n>
    struct serialization_tuple<std::tuple<T...>, i, n> {
      using Ti = typename std::tuple_element<i, std::tuple<T...>>::type;
      using recurse_tail = serialization_tuple<std::tuple<T...>, i+1, n>;
      
      static constexpr bool is_definitely_serializable =
        serialization_complete<Ti>::is_definitely_serializable &&
        recurse_tail::is_definitely_serializable;

      template<typename Prefix>
      static auto ubound(Prefix pre, std::tuple<T...> const &x) ->
        decltype(
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
        serialization_complete<Ti>::references_buffer ||
        recurse_tail::references_buffer;

      template<typename Writer>
      static void serialize(Writer &w, std::tuple<T...> const &x) {
        w.template push<Ti>(std::template get<i>(x));
        recurse_tail::serialize(w, x);
      }

      template<typename Reader, typename Storage, typename Pointers>
      static void deserialize_each(Reader &r, Storage &raws, Pointers &ptrs) {
        std::template get<i>(ptrs) = r.template pop_into<Ti>(&std::template get<i>(raws));
        recurse_tail::deserialize_each(r, raws, ptrs);
      }

      static constexpr bool skip_is_fast =
        serialization_complete<Ti>::skip_is_fast &&
        recurse_tail::skip_is_fast;
      
      template<typename Reader>
      static void skip(Reader &r) {
        r.template skip<Ti>();
        recurse_tail::skip(r);
      }
    };
    
    template<typename ...T, int n>
    struct serialization_tuple<std::tuple<T...>, n, n> {
      static constexpr bool is_definitely_serializable = true;

      template<typename Prefix>
      static Prefix ubound(Prefix pre, std::tuple<T...> const &x) {
        return pre;
      }
      
      static constexpr bool references_buffer = false;

      template<typename Writer>
      static void serialize(Writer &w, std::tuple<T...> const &x) {}

      template<typename Reader, typename Storage, typename Pointers>
      static void deserialize_each(Reader &r, Storage &raws, Pointers &ptrs) {/*nop*/}

      static constexpr bool skip_is_fast = true;

      template<typename Reader>
      static void skip(Reader&) {/*nop*/}
    };
  }

  template<typename ...T>
  struct serialization<std::tuple<T...>>:
    detail::serialization_tuple<std::tuple<T...>> {
    
    using deserialized_raws = std::tuple<typename std::aligned_storage<
        sizeof(typename serialization_complete<T>::deserialized_type),
        alignof(typename serialization_complete<T>::deserialized_type)
      >::type...>;

    using deserialized_ptrs = std::tuple<
        typename serialization_complete<T>::deserialized_type*...
      >;
    
    using deserialized_type = std::tuple<
        typename serialization_complete<T>::deserialized_type...
      >;

    template<typename Ti>
    static Ti take_one(Ti *x) {
      Ti tmp = std::move(*x);
      detail::destruct(*x);
      return tmp;
    }
    
    template<int ...i>
    static deserialized_type* take_all(
        deserialized_ptrs &ptrs,
        void *into,
        detail::index_sequence<i...>
      ) {
      return ::new(into) deserialized_type(take_one(std::template get<i>(ptrs))...);
    }

    template<typename Reader>
    static deserialized_type* deserialize(Reader &r, void *raw) {
      deserialized_raws raws;
      deserialized_ptrs ptrs;
      detail::serialization_tuple<std::tuple<T...>>::deserialize_each(r, raws, ptrs);
      return take_all(ptrs, raw, detail::make_index_sequence<sizeof...(T)>());
    }
  };

  //////////////////////////////////////////////////////////////////////////////

  template<typename A, typename B>
  struct is_definitely_trivially_serializable<std::pair<A,B>> {
    static constexpr bool value = is_definitely_trivially_serializable<A>::value &&
                                  is_definitely_trivially_serializable<B>::value;
  };

  template<typename A, typename B>
  struct serialization<std::pair<A,B>> {
    static constexpr bool is_definitely_serializable =
      serialization_complete<A>::is_definitely_serializable &&
      serialization_complete<B>::is_definitely_serializable;

    template<typename Prefix>
    static auto ubound(Prefix pre, std::pair<A,B> const &x) ->
      decltype(pre.cat_ubound_of(x.first).cat_ubound_of(x.second)) {
      return pre.cat_ubound_of(x.first).cat_ubound_of(x.second);
    }
    
    template<typename Writer>
    static void serialize(Writer &w, std::pair<A,B> const &x) {
      w.template push<A>(x.first);
      w.template push<B>(x.second);
    }

    static constexpr bool references_buffer =
      serialization_complete<A>::references_buffer ||
      serialization_complete<B>::references_buffer;

    using A1 = typename serialization_complete<A>::deserialized_type;
    using B1 = typename serialization_complete<B>::deserialized_type;
    
    using deserialized_type = std::pair<A1,B1>;
    
    template<typename Reader>
    static deserialized_type* deserialize(Reader &r, void *raw) {
      A1 a = r.template pop<A>();
      B1 b = r.template pop<B>();
      return ::new(raw) std::pair<A1,B1>{std::move(a), std::move(b)};
    }

    static constexpr bool skip_is_fast =
      serialization_complete<A>::skip_is_fast &&
      serialization_complete<B>::skip_is_fast;

    template<typename Reader>
    static void skip(Reader &r) {
      r.template skip<A>();
      r.template skip<B>();
    }
  };

  //////////////////////////////////////////////////////////////////////////////

  template<typename T, std::size_t n>
  struct is_definitely_trivially_serializable<std::array<T,n>>:
    is_definitely_trivially_serializable<T> {
  };

  template<typename T, std::size_t n>
  struct serialization<std::array<T,n>> {
    static constexpr bool is_definitely_serializable =
      serialization_complete<T>::is_definitely_serializable;

    template<typename Prefix>
    static constexpr auto ubound(Prefix pre, std::array<T,n> const &x) ->
      decltype(pre.cat(serialization_complete<T>::static_ubound.template array<n>())) {
      return pre.cat(serialization_complete<T>::static_ubound.template array<n>());
    }
    
    template<typename Writer>
    static void serialize(Writer &w, std::array<T,n> const &x) {
      w.push_sequence(&x[0], &x[0] + n, n);
    }

    static constexpr bool references_buffer = serialization_complete<T>::references_buffer;

    using T1 = typename serialization_complete<T>::deserialized_type;
    
    using deserialized_type = std::array<T1,n>;
    
    template<typename Reader>
    static deserialized_type* deserialize(Reader &r, void *raw) {
      return reinterpret_cast<std::array<T1,n>*>(r.template pop_sequence_into<T>(raw, n));
    }

    static constexpr bool skip_is_fast = detail::serialization_reader::template skip_sequence_is_fast<T>();
    
    template<typename Reader>
    static void skip(Reader &r) {
      r.template skip_sequence<T>(n);
    }
  };
  
  //////////////////////////////////////////////////////////////////////////////

  template<typename T, std::size_t n>
  struct is_definitely_trivially_serializable<T[n]>:
    is_definitely_trivially_serializable<T> {
  };

  template<typename T, std::size_t n>
  struct serialization<T[n]> {
    static constexpr bool is_definitely_serializable =
      serialization_complete<T>::is_definitely_serializable;

    template<typename Prefix>
    static constexpr auto ubound(Prefix pre, T const(&x)[n]) ->
      decltype(pre.cat(serialization_complete<T>::static_ubound.template array<n>())) {
      return pre.cat(serialization_complete<T>::static_ubound.template array<n>());
    }
    
    template<typename Writer>
    static void serialize(Writer &w, T const(&x)[n]) {
      w.push_sequence(&x[0], &x[0] + n, n);
    }

    static constexpr bool references_buffer = serialization_complete<T>::references_buffer;

    using T1 = typename serialization_complete<T>::deserialized_type;
    
    using deserialized_type = T1[n];
    
    template<typename Reader>
    static deserialized_type* deserialize(Reader &r, void *raw) {
      return reinterpret_cast<T1(*)[n]>(r.template pop_sequence_into<T>(raw, n));
    }

    static constexpr bool skip_is_fast = detail::serialization_reader::template skip_sequence_is_fast<T>();
    
    template<typename Reader>
    static void skip(Reader &r) {
      r.template skip_sequence<T>(n);
    }
  };

  //////////////////////////////////////////////////////////////////////////////

  template<typename CharT, typename Traits, typename Alloc>
  struct serialization<std::basic_string<CharT, Traits, Alloc>> {
    static_assert(std::is_trivial<CharT>::value, "Bad string character type.");
    
    static constexpr bool is_definitely_serializable = true;

    using Str = std::basic_string<CharT,Traits,Alloc>;
    
    template<typename Prefix>
    static auto ubound(Prefix pre, Str const &s) ->
      decltype(
        pre.template cat_ubound_of<std::size_t>(1)
           .cat(storage_size_of<CharT>().array(1))
      ) {
      std::size_t n = s.size();
      return pre.template cat_ubound_of<std::size_t>(n)
                .cat(storage_size_of<CharT>().array(n));
    }

    template<typename Writer>
    static void serialize(Writer &w, Str const &s) {
      std::size_t n = s.size();
      w.template push<std::size_t>(n);
      w.push_sequence(&s[0], &s[0] + n, n);
    }

    static constexpr bool references_buffer = false;
    
    template<typename Reader>
    static Str* deserialize(Reader &r, void *raw) {
      std::size_t n = r.template pop<std::size_t>();
      CharT const *p = (CharT const*)r.unplace(storage_size_of<CharT>().array(n));
      return ::new(raw) Str(p, n);
    }

    static constexpr bool skip_is_fast = true;

    template<typename Reader>
    static void skip(Reader &r) {
      std::size_t n = r.template pop<std::size_t>();
      r.unplace(storage_size_of<CharT>().array(n));
    }
  };
  
  //////////////////////////////////////////////////////////////////////////////

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
    struct serialization_container {
      static constexpr bool is_definitely_serializable = serialization_complete<T0>::is_definitely_serializable;
      
      template<typename Prefix>
      static auto ubound(Prefix pre, BagIn const &bag) ->
        decltype(
          pre.template cat_ubound_of<std::size_t>(1)
             .cat(serialization_complete<T0>::static_ubound.array(1))
        ) {
        std::size_t n = bag.size();
        return pre.template cat_ubound_of<std::size_t>(n)
                  .cat(serialization_complete<T0>::static_ubound.array(n));
      }

      template<typename Writer>
      static void serialize(Writer &w, BagIn const &bag) {
        std::size_t n = bag.size();
        w.push_trivial(n);
        w.push_sequence(bag.begin(), bag.end(), n);
      }

      static constexpr bool references_buffer = serialization_complete<T0>::references_buffer;

      using deserialized_type = BagOut;

      template<typename Reader>
      static BagOut* deserialize(Reader &r, void *raw) {
        std::size_t n = r.template pop_trivial<std::size_t>();
        BagOut *bag = ::new(raw) BagOut;
        detail::template reserve_if_supported<BagOut>()(*bag, n);
        r.template pop_sequence_into<T0>(detail::template inserter<BagOut>()(*bag), n);
        return bag;
      }

      template<typename Reader>
      static void skip(Reader &r) {
        std::size_t n = r.template pop_trivial<std::size_t>();
        r.template skip_sequence<T0>(n);
      }

      static constexpr bool skip_is_fast = serialization_reader::template skip_sequence_is_fast<T0>();
    };
  }

  template<typename T, typename Alloc>
  struct serialization<std::vector<T,Alloc>>:
    detail::serialization_container<
      std::vector<T, Alloc>,
      std::vector<typename serialization_complete<T>::deserialized_type, Alloc>
    > {
  };
  template<typename T, typename Alloc>
  struct serialization<std::deque<T,Alloc>>:
    detail::serialization_container<
      std::deque<T, Alloc>,
      std::deque<typename serialization_complete<T>::deserialized_type, Alloc>
    > {
  };
  template<typename T, typename Alloc>
  struct serialization<std::list<T,Alloc>>:
    detail::serialization_container<
      std::list<T, Alloc>,
      std::list<typename serialization_complete<T>::deserialized_type, Alloc>
    > {
  };

  template<typename T, typename Cmp, typename Alloc>
  struct serialization<std::set<T,Cmp,Alloc>>:
    detail::serialization_container<
      std::set<T,Cmp,Alloc>,
      std::set<typename serialization_complete<T>::deserialized_type, Cmp, Alloc>
    > {
  };
  template<typename T, typename Cmp, typename Alloc>
  struct serialization<std::multiset<T,Cmp,Alloc>>:
    detail::serialization_container<
      std::multiset<T,Cmp,Alloc>,
      std::multiset<typename serialization_complete<T>::deserialized_type, Cmp, Alloc>
    > {
  };

  template<typename T, typename Hash, typename Eq, typename Alloc>
  struct serialization<std::unordered_set<T,Hash,Eq,Alloc>>:
    detail::serialization_container<
      std::unordered_set<T,Hash,Eq,Alloc>,
      std::unordered_set<typename serialization_complete<T>::deserialized_type, Hash, Eq, Alloc>
    > {
  };
  template<typename T, typename Hash, typename Eq, typename Alloc>
  struct serialization<std::unordered_multiset<T,Hash,Eq,Alloc>>:
    detail::serialization_container<
      std::unordered_multiset<T,Hash,Eq,Alloc>,
      std::unordered_multiset<typename serialization_complete<T>::deserialized_type, Hash, Eq, Alloc>
    > {
  };

  template<typename K, typename V, typename Cmp, typename Alloc>
  struct serialization<std::map<K,V,Cmp,Alloc>>:
    detail::serialization_container<
      std::map<K,V,Cmp,Alloc>,
      std::map<
        typename serialization_complete<K>::deserialized_type,
        typename serialization_complete<V>::deserialized_type, Cmp, Alloc>
    > {
  };
  template<typename K, typename V, typename Cmp, typename Alloc>
  struct serialization<std::multimap<K,V,Cmp,Alloc>>:
    detail::serialization_container<
      std::multimap<K,V,Cmp,Alloc>,
      std::multimap<
        typename serialization_complete<K>::deserialized_type,
        typename serialization_complete<V>::deserialized_type, Cmp, Alloc>
    > {
  };

  template<typename K, typename V, typename Hash, typename Eq, typename Alloc>
  struct serialization<std::unordered_map<K,V,Hash,Eq,Alloc>>:
    detail::serialization_container<
      std::unordered_map<K,V,Hash,Eq,Alloc>,
      std::unordered_map<
        typename serialization_complete<K>::deserialized_type,
        typename serialization_complete<V>::deserialized_type, Hash, Eq, Alloc>
    > {
  };
  template<typename K, typename V, typename Hash, typename Eq, typename Alloc>
  struct serialization<std::unordered_multimap<K,V,Hash,Eq,Alloc>>:
    detail::serialization_container<
      std::unordered_multimap<K,V,Hash,Eq,Alloc>,
      std::unordered_multimap<
        typename serialization_complete<K>::deserialized_type,
        typename serialization_complete<V>::deserialized_type, Hash, Eq, Alloc>
    > {
  };

  template<typename T, typename Alloc>
  struct serialization<std::forward_list<T,Alloc>> {
    using T0 = T;
    using T1 = typename serialization_complete<T>::deserialized_type;

    static constexpr bool is_definitely_serializable = serialization_complete<T0>::is_definitely_serializable;

    // no ubound
    
    template<typename Writer>
    static void serialize(Writer &w, std::forward_list<T0,Alloc> const &bag) {
      void *n_spot = w.place(storage_size_of<std::size_t>());
      std::size_t n = w.push_sequence(bag.begin(), bag.end());
      ::new(n_spot) std::size_t(n);
    }

    static constexpr bool references_buffer = serialization_complete<T0>::references_buffer;

    using deserialized_type = std::forward_list<T1,Alloc>;

    template<typename Reader>
    static deserialized_type* deserialize(Reader &r, void *raw) {
      std::size_t n = r.template pop_trivial<std::size_t>();
      deserialized_type *ans = ::new(raw) deserialized_type;
      if(n != 0) {
        ans->push_front(r.template pop<T0>());
        
        auto last = ans->begin();
        while(n--)
          last = ans->insert_after(last, r.template pop<T0>());
      }
      return ans;
    }

    template<typename Reader>
    static void skip(Reader &r) {
      std::size_t n = r.template pop_trivial<std::size_t>();
      r.template skip_sequence<T>(n);
    }

    static constexpr bool skip_is_fast = detail::serialization_reader::template skip_sequence_is_fast<T>();
  };
}
#endif
