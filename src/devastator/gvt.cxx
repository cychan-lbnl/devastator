#include "gvt.hxx"

#include <algorithm>

using namespace std;

namespace gvt {
  thread_local bool epoch_ended_;
  thread_local uint64_t epoch_gvt_;
  thread_local reducibles epoch_rxs_;
  
  thread_local unsigned round_;
  thread_local uint64_t round_lvt_;
  thread_local uint64_t round_lsend_;
  thread_local uint64_t round_lrecv_[3];
}

namespace {
  thread_local bool rdxn_active;
  thread_local int rdxn_incoming;
  thread_local uint64_t rdxn_gsend, rdxn_grecv;
  thread_local uint64_t rdxn_gvt;
  thread_local gvt::reducibles rdxn_rxs;
  
  thread_local uint64_t fresh_gvt;
  thread_local gvt::reducibles fresh_rxs;
  
  void rdxn_up(uint64_t lvt, uint64_t lsend, uint64_t lrecv, gvt::reducibles rxs);
  void rdxn_down(int to_ub, uint64_t gvt, gvt::reducibles rxs);
}

void gvt::init(gvt::reducibles rxs0) {
  gvt::epoch_ended_ = true;
  gvt::epoch_gvt_ = 0;
  gvt::epoch_rxs_ = rxs0;

  gvt::round_ = 0;
  gvt::round_lvt_ = ~uint64_t(0);
  gvt::round_lsend_ = 0;
  gvt::round_lrecv_[0] = 0;
  gvt::round_lrecv_[1] = 0;
  gvt::round_lrecv_[2] = 0;
  
  rdxn_active = false;
  rdxn_incoming = 0;
  rdxn_gvt = 0;
  fresh_gvt = 0;
  fresh_rxs = rxs0;
  
  world::barrier();
}

void gvt::advance() {
  gvt::epoch_ended_ = !rdxn_active;
  gvt::epoch_gvt_ = fresh_gvt;
  gvt::epoch_rxs_ = fresh_rxs;
}

void gvt::epoch_begin(std::uint64_t lvt, gvt::reducibles rxs) {
  ASSERT(!rdxn_active);

  rdxn_active = true;
  gvt::epoch_ended_ = false;
  
  rdxn_up(
    std::min(lvt, gvt::round_lvt_),
    gvt::round_lsend_,
    gvt::round_lrecv_[0],
    rxs
  );

  gvt::round_ += 1;
  gvt::round_lvt_ = ~uint64_t(0);
  gvt::round_lsend_ = 0;
  gvt::round_lrecv_[0] = gvt::round_lrecv_[1];
  gvt::round_lrecv_[1] = gvt::round_lrecv_[2];
  gvt::round_lrecv_[2] = 0;
}

namespace {
  void rdxn_up(uint64_t lvt, uint64_t lsend, uint64_t lrecv, gvt::reducibles rxs) {
    const int rank_me = world::rank_me();
    const int rank_n = world::rank_n;
    
    if(rdxn_incoming == 0) {
      while(true) {
        int kid = rank_me | (1<<rdxn_incoming);
        if(kid == rank_me || rank_n <= kid)
          break;
        rdxn_incoming += 1;
      }
      rdxn_incoming += 1; // add one for self

      rdxn_gvt = ~uint64_t(0);
      rdxn_rxs = rxs;
      rdxn_gsend = 0;
      rdxn_grecv = 0;
    }
    else
      rdxn_rxs.reduce_with(rxs);
    
    rdxn_gvt = std::min(rdxn_gvt, lvt);
    rdxn_gsend += lsend;
    rdxn_grecv += lrecv;
    
    if(0 == --rdxn_incoming) {
      if(rank_me == 0) {
        bool quiesced = rdxn_gsend == rdxn_grecv;

        //say()<<"root gvt="<<rdxn_gvt<<" send="<<rdxn_gsend<<" recv="<<rdxn_grecv;
        
        rdxn_down(rank_n ^ (quiesced ? -1 : 0), rdxn_gvt, rdxn_rxs);
      }
      else {
        //say()<<"sending up";
        int parent = rank_me & (rank_me-1);
        uint64_t gsend = rdxn_gsend;
        uint64_t grecv = rdxn_grecv;
        rxs = rdxn_rxs;
        lvt = rdxn_gvt;
        world::send(parent, [=]() {
          rdxn_up(lvt, gsend, grecv, rxs);
        });
      }
    }
  }
  
  void rdxn_down(int to_ub, uint64_t gvt, gvt::reducibles grxs) {
    const bool quiesced = to_ub < 0;
    to_ub ^= quiesced ? -1 : 0;
    
    const int rank_me = world::rank_me();
    const int rank_n = world::rank_n;
    
    while(true) {
      int mid = rank_me + (to_ub - rank_me)/2;
      if(mid == rank_me) break;

      int to_ub1 = to_ub ^ (quiesced ? -1 : 0);
      world::send(mid, [=]() {
        rdxn_down(to_ub1, gvt, grxs);
      });
      
      to_ub = mid;
    }
    
    //say()<<"epoch bump rxs={"<<grxs.sum1<<" "<<grxs.sum2<<"}";
    rdxn_active = false;
    fresh_rxs = grxs;
    if(quiesced)
      fresh_gvt = gvt;
  }
}
