#include <upcxx/serialization.hpp>

using namespace upcxx;

//extern "C" void __asan_poison_memory_region(void const volatile *addr, size_t size);

// how much memory to grab when we need a new hunk
constexpr std::uintptr_t hunk_size_small = 512;
constexpr std::uintptr_t hunk_size_large = 8192;

constexpr std::uintptr_t align_max = serialization_align_max; // shorthand

void upcxx::detail::serialization_writer<false>::grow(std::size_t size0, std::size_t size1) {
  static_assert(2*align_max <= hunk_size_small, "Small hunk size (hunk_size_small) not big enough");

  std::size_t hunk_sz;
  
  // if total size so far fits within large hunk
  // AND requested size increase fits within a small hunk...
  if(size1 <= hunk_size_large - sizeof(hunk_footer) &&
     size1-(size0 & -align_max) <= hunk_size_small - sizeof(hunk_footer)
    ) {
    // then use a small hunk
    hunk_sz = hunk_size_small;
  }
  // else if size increase fits in a large hunk...
  else if(size1-(size0 & -align_max) <= hunk_size_large - sizeof(hunk_footer)) {
    // then use a large hunk
    hunk_sz = hunk_size_large;
  }
  // requested size increase exceeded a single large hunk...
  else {
    // round up to a multiple of a large hunk
    hunk_sz = size1-(size0 & -align_max) + sizeof(hunk_footer);
    hunk_sz = (hunk_sz + hunk_size_large-1) & -hunk_size_large;
  }
  
  hunk_footer *h; {
    void *p = detail::alloc_aligned(hunk_sz, align_max);
    h = ::new((char*)p + hunk_sz - sizeof(hunk_footer)) hunk_footer;
    h->front = p;
  }

  h->next = nullptr;
  h->size0 = size0;
  
  (head_ == nullptr ? head_ : tail_->next) = h;
  tail_ = h;
  
  edge_ = (size0 & -align_max) + hunk_sz - sizeof(hunk_footer);
  base_ = reinterpret_cast<std::uintptr_t>(h->front) - (size0 & -align_max);

  UPCXX_ASSERT_ALWAYS(reinterpret_cast<char*>(base_ + edge_) == (char*)h);
  UPCXX_ASSERT_ALWAYS(size1 <= edge_);
}

void upcxx::detail::serialization_writer<false>::compact_and_invalidate_(void *buf) {
  hunk_footer *h = head_;
  std::size_t size_end = size_;
  std::size_t size0 = 0;

  while(h != nullptr) {
    hunk_footer *h1 = h->next;
    std::size_t size1 = h1 ? h1->size0 : size_end;
    std::memcpy(
      (char*)buf + size0,
      (char*)h->front + (size0 % align_max),
      std::min<std::size_t>(size1-size0, (char*)h - (char*)h->front - (size0 % align_max))
    );
    if(h != head_)
      std::free(h->front);
    size0 = size1;
    h = h1;
  }
  
  base_ = 0; edge_ = 0;
  size_ = 0; align_ = 1;
  head_ = tail_ = nullptr;
}
