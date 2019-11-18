#include <devastator/diagnostic.hxx>
#include <devastator/threads/epoch_allocator.hxx>

#include <random>
#include <map>
#include <deque>
#include <vector>

using namespace std;

template<int epochs>
void test() {
  deva::threads::epoch_allocator<epochs> ma;
  ma.init(nullptr, 1<<20);
  
  map<uintptr_t, uintptr_t> liveset;
  deque<vector<uintptr_t>> live_at(epochs);
  default_random_engine rng(0);
  
  for(int e=0; e < 10000; e++) {
    int q = rng() % 10;
    for(int m=0; m < q; m++) {
      size_t sz = 1 + (rng() % 256);
      uintptr_t p = reinterpret_cast<uintptr_t>(ma.allocate(sz, 1));

      auto bef = liveset.lower_bound(p);
      auto aft = liveset.lower_bound(p + sz);
      DEVA_ASSERT_ALWAYS(bef == aft, "p="<<p<<" "<<p+sz<<" bef="<<bef->first<<' '<<bef->first+bef->second<<" aft="<<aft->first);
      if(bef != liveset.begin()) {
        --bef;
        DEVA_ASSERT_ALWAYS(bef->first + bef->second <= p);
      }
      
      live_at.back().push_back(p);
      liveset[p] = sz;
    }

    ma.bump_epoch();
    
    for(uintptr_t p: live_at[0]) {
      DEVA_ASSERT_ALWAYS(liveset.count(p));
      liveset.erase(p);
    }
    live_at.pop_front();
    live_at.push_back({});
  }
}

int main() {
  test<2>();
  test<3>();
  test<4>();
  test<5>();
  std::cout<<"SUCCESS\n";
  return 0;
}
