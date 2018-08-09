#include <devastator/diagnostic.hxx>
#include <devastator/world.hxx>
#include <devastator/pdes.hxx>

#include <cstdint>
#include <memory>

using namespace std;

namespace pdes = deva::pdes;

using deva::rank_n;
using deva::rank_me;

constexpr int cell_n = 100;
constexpr int cell_per_rank = (cell_n + rank_n-1)/rank_n;
constexpr int iter_n = 4000;
constexpr int nebr_n = 80;

// cell values
thread_local std::unique_ptr<unsigned[]> cells;

// counters of contributions received
thread_local std::unique_ptr<int[]> counts;

struct load_event {
  static thread_local int64_t execute_n;
  static thread_local int64_t commit_n;

  int cell_src, cell_dst;
  
  // optional, pdes will assume a default value if subtime() absent.
  int subtime() const { return 0; }

  struct reverse {
    void unexecute(load_event&) { execute_n -= 1;}
    void commit(load_event&) { commit_n += 1; }
  };

  reverse execute(pdes::execute_context &cxt);
};

thread_local int64_t load_event::execute_n;
thread_local int64_t load_event::commit_n;

struct accum_event {
  unsigned val;
  int cell_dst;
  
  auto execute(pdes::execute_context &cxt) {
    int iter = cxt.time/2;
    
    int c = cell_dst % cell_per_rank;
    cells[c] += val;
    counts[c] += 1;
    
    if(counts[c] >= (iter==0 ? 1 : nebr_n)  && iter < iter_n) {
      counts[c] = 0;
      for(int j=0; j < nebr_n; j++) {
        int cell_src = (cell_dst + iter + j) % cell_n;
        cxt.send(
          cell_src/cell_per_rank, cell_src%cell_per_rank,
          cxt.time + 1,
          load_event{cell_src, cell_dst}
        );
      }
    }
    
    // return the unexecute lambda
    return [=](accum_event &me) {
      int c = me.cell_dst % cell_per_rank;
      
      cells[c] -= me.val;
      
      if(counts[c] == 0)
        counts[c] = (iter==0 ? 1 : nebr_n)-1;
      else
        counts[c] -= 1;
    };
  }
};

load_event::reverse load_event::execute(pdes::execute_context &cxt) {
  execute_n += 1;
  cxt.send(
    cell_dst/cell_per_rank, cell_dst%cell_per_rank,
    cxt.time + 1,
    accum_event{cells[cell_src%cell_per_rank], cell_dst}
  );
  return reverse{};
}

struct cell_cd {
  static bool commutes(pdes::event_view a, pdes::event_view b) {
    //return false;
    bool ans;
    
    // a & b commute iff they are both loads or both accums.
    #if 1
      ans = a.type_id() == b.type_id();
    #else // equivalent
      ans =
        (!!a.try_cast<load_event>() & !!b.try_cast<load_event>()) |
        (!!a.try_cast<accum_event>() & !!b.try_cast<accum_event>());
    #endif
    
    //if(ans) deva::say()<<"COMMUTES";
    return ans;
  }
};

int main() {
  auto doit = []() {
    cells.reset(new unsigned[cell_per_rank](/*0...*/));
    counts.reset(new int[cell_per_rank](/*0...*/));
    
    pdes::init(cell_per_rank);
    
    int lb = rank_me()*cell_per_rank;
    int ub = std::min(cell_n, (rank_me()+1)*cell_per_rank);
    
    for(int cell=lb; cell < ub; cell++) {
      int cd = cell - lb;
      #if 1
        pdes::init_cd<cell_cd>(cd);
      #endif
      pdes::root_event(cd, 0, accum_event{(unsigned)cell, cell});
    }
    
    load_event::commit_n = 0;
    load_event::execute_n = 0;
    
    pdes::drain();
    
    // do the stencils in serial and compare results
    std::unique_ptr<unsigned[]> gcells1(new unsigned[cell_n]);
    std::unique_ptr<unsigned[]> gcells2(new unsigned[cell_n]);
    
    for(int cell=0; cell < cell_n; cell++)
      gcells1[cell] = cell;
    
    for(int i=0; i < iter_n; i++) {
      for(int cell=0; cell < cell_n ; cell++) {
        gcells2[cell] = gcells1[cell];
        for(int j=0; j < nebr_n; j++)
          gcells2[cell] += gcells1[(cell + i + j)%cell_n];
      }
      std::swap(gcells1, gcells2);
    }
      
    for(int cell=lb; cell < ub; cell++) {
      int cd = cell - lb;
      DEVA_ASSERT_ALWAYS(cells[cd] == gcells1[cell]);
    }
    
    DEVA_ASSERT_ALWAYS(load_event::commit_n == load_event::execute_n);
 };

  for(int i=0; i < 1; i++)
    deva::run(doit);

  if(deva::process_me() == 0)
    std::cout<<"Looks good!"<<std::endl;
  return 0;
}
