#include <upcxx/serialization.hpp>

using namespace upcxx;

//extern "C" void __asan_poison_memory_region(void const volatile *addr, size_t size);

void upcxx::detail::serialization_writer<false>::grow(std::size_t size0, std::size_t size1) {
  std::size_t hunk_sz;

  if(size1 <= 4096-64-sizeof(hunk_t) && size1-(size0 & -64) <= 256-64-sizeof(hunk_t))
    hunk_sz = 256-64;
  else if(size1-(size0 & -64) <= 4096-64-sizeof(hunk_t))
    hunk_sz = 4096-64;
  else {
    hunk_sz = size1-(size0 & -64) + sizeof(hunk_t) + alignof(hunk_t) + 64;
    hunk_sz = ((hunk_sz + 4096-1) & -4096)-64;
  }

  hunk_t *h; {
    void *p = detail::alloc_aligned(hunk_sz, 64);
    h = ::new((char*)p + hunk_sz - sizeof(hunk_t)) hunk_t;
    h->front = p;
  }

  h->next = nullptr;
  h->size0 = size0;
  
  (head_ == nullptr ? head_ : tail_->next) = h;
  tail_ = h;
  
  edge_ = (size0 & -64) + hunk_sz - sizeof(hunk_t);
  base_ = reinterpret_cast<std::uintptr_t>(h->front) - (size0 & -64);

  UPCXX_ASSERT_ALWAYS(reinterpret_cast<char*>(base_ + edge_) == (char*)h);
  UPCXX_ASSERT_ALWAYS(size1 <= edge_);
}

void upcxx::detail::serialization_writer<false>::compact_and_invalidate_(void *buf) {
  hunk_t *h = head_;
  std::size_t size_end = size_;
  std::size_t size0 = 0;

  while(h != nullptr) {
    hunk_t *h1 = h->next;
    std::size_t size1 = h1 ? h1->size0 : size_end;
    std::memcpy(
      (char*)buf + size0,
      (char*)h->front + size0%64,
      std::min<std::size_t>(size1-size0, (char*)h - (char*)h->front - size0%64)
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
