#ifndef _567ce379de154fd2ba980fb2d490297f
#define _567ce379de154fd2ba980fb2d490297f

#include <devastator/diagnostic.hxx>

#include <cstdint>

namespace deva {
namespace threads {
  template<int epochs>
  class message_allocator {
    static constexpr std::intptr_t grain_size = 32;
    std::uint32_t capacity_;
    
    std::uint32_t ed_, bump_, wall_;
    std::int8_t lo_[epochs];
    std::uint32_t edge_[2*epochs];
    
  public:
    message_allocator(std::size_t capacity);
    std::intptr_t allocate(std::size_t size, std::size_t align);
    void bump_epoch();
  };

  template<int epochs>
  message_allocator<epochs>::message_allocator(std::size_t capacity) {
    capacity_ = std::min<std::size_t>(std::uint32_t(-1), capacity/grain_size);
    
    for(int ed=0; ed < 2*epochs; ed++)
      edge_[ed] = 0;

    ed_ = 2*epochs-1;
    bump_ = 0;
    wall_ = capacity_;
    for(int e=0; e < epochs; e++)
      lo_[e] = e;
  }

  template<int epochs>
  std::intptr_t message_allocator<epochs>::allocate(std::size_t size, std::size_t align) {
    size = (size + grain_size-1)/grain_size;
    align = (align + grain_size-1)/grain_size;

    std::uint32_t bump1 = (bump_ + align-1) & -align;
    if(bump1 + size > wall_) {
      if(wall_ == capacity_)
        return -1;
      
      edge_[ed_] = bump_;
      wall_ = capacity_;
      ed_ = 2*epochs-1;
      bump_ = edge_[ed_-1];
      bump1 = (bump_ + align-1) & -align;
    }
    
    std::intptr_t ans = std::intptr_t(bump1)*grain_size;
    //deva::say()<<"allocd "<<bump1*grain_size;
    DEVA_ASSERT(bump_ <= bump1 + size && bump1 + size <= wall_);
    bump_ = bump1 + size;
    return ans;
  }

  template<int epochs>
  void message_allocator<epochs>::bump_epoch() {
    edge_[ed_] = bump_;
    
    const int lo = lo_[0];
    const int hi = epochs; // int hi = hi_[0];
    
    std::uint32_t sz_lo = edge_[lo] - (lo == 0 ? 0 : edge_[lo-1]);
    std::uint32_t sz_hi = edge_[hi] - edge_[hi-1];
    
    // pick edge delimiting smallest region as one to remove
    int rem;
    if(lo+1 == hi || sz_lo < sz_hi) {
      rem = lo;
      ed_ = hi-1; // -1 accounts for edge renumbering since lo is removed
    }
    else {
      ed_ = lo;
      rem = hi;
    }

    // if hi edge is equal to maximal edge, decrease all maximal edges to previous highest
    if(edge_[hi] == edge_[2*epochs-1]) {
      for(int ed=hi; ed < 2*epochs; ed++)
        edge_[ed] = edge_[hi-1];
    }

    // remove edge "rem" by shifting down
    for(int ed=rem+1; ed < 2*epochs; ed++)
      edge_[ed-1] = edge_[ed];
    
    // update pointers to reflect removal renumbering
    for(int e=1; e < epochs; e++)
      lo_[e-1] = lo_[e] - (rem <= lo_[e] ? 1 : 0);
    
    bump_ = ed_ == 0 ? 0 : edge_[ed_-1];
    wall_ = edge_[ed_];
    
    lo_[epochs-1] = ed_;
    
    #if 0
    {
      std::cout<<"after removal: bump="<<grain_size*bump_<<" wall="<<grain_size*wall_<<" | ";
      bool ordered = true;
      for(int ed=0; ed < 2*epochs; ed++) {
        ordered &= (ed==0 ? 0 : edge_[ed-1]) <= edge_[ed];
        std::cout<<grain_size*edge_[ed]<<' ';
      }
      DEVA_ASSERT_ALWAYS(ordered);
      std::cout<<std::endl;
    }
    #endif
  }
}}
#endif
