#include "gvt.hxx"

using namespace std;

namespace gvt {
  __thread bool epoch_ended_;
  __thread uint64_t epoch_gvt_;
  __thread reducibles epoch_rxs_;
  
  __thread unsigned round_;
  __thread uint64_t round_lvt_[2];
  __thread uint64_t round_lsend_[2];
  __thread uint64_t round_lrecv_[3];
}

namespace {
  enum class rdxn_status_e  {
    reducing,
    quiesced,
    non_quiesced,
  };
  
  __thread rdxn_status_e rdxn_status;
  __thread int rdxn_incoming;
  __thread uint64_t rdxn_gsend, rdxn_grecv;
  __thread uint64_t rdxn_gvt_acc;
  __thread gvt::reducibles rdxn_rxs_acc;
  __thread uint64_t rdxn_gvt_ans;
  __thread gvt::reducibles rdxn_rxs_ans;
  
  void rdxn_up(uint64_t lvt, uint64_t lsend, uint64_t lrecv, gvt::reducibles rxs);
  void rdxn_down(int to_ub, uint64_t gvt, gvt::reducibles rxs);
}

void gvt::init(gvt::reducibles rxs0) {
  gvt::epoch_ended_ = true;
  gvt::epoch_gvt_ = 0;
  gvt::epoch_rxs_ = rxs0;

  gvt::round_ = 0;
  gvt::round_lvt_[0] = 0;
  gvt::round_lvt_[1] = 0;
  gvt::round_lsend_[0] = 0;
  gvt::round_lsend_[1] = 0;
  gvt::round_lrecv_[0] = 0;
  gvt::round_lrecv_[1] = 0;
  gvt::round_lrecv_[2] = 0;
  
  rdxn_status = rdxn_status_e::non_quiesced;
  rdxn_incoming = 0;
  rdxn_gvt_acc = 0;
  rdxn_gvt_ans = 0;
  rdxn_rxs_ans = rxs0;
  
  world::barrier();
}

void gvt::advance() {
  gvt::epoch_ended_ = rdxn_status != rdxn_status_e::reducing;
  gvt::epoch_gvt_ = rdxn_gvt_ans;
  gvt::epoch_rxs_ = rdxn_rxs_ans;
}

void gvt::epoch_begin(std::uint64_t lvt, gvt::reducibles rxs) {
  ASSERT(rdxn_status != rdxn_status_e::reducing);

  if(rdxn_status == rdxn_status_e::quiesced) {
    gvt::round_ += 1;
    
    gvt::round_lvt_[0] = gvt::round_lvt_[1];
    gvt::round_lvt_[1] = lvt;
  
    gvt::round_lsend_[0] = gvt::round_lsend_[1];
    gvt::round_lsend_[1] = 0;
    
    gvt::round_lrecv_[0] = gvt::round_lrecv_[1];
    gvt::round_lrecv_[1] = gvt::round_lrecv_[2];
    gvt::round_lrecv_[2] = 0;
  }
  
  gvt::epoch_ended_ = false;
  rdxn_status = rdxn_status_e::reducing;
  
  rdxn_up(
    gvt::round_lvt_[0],
    gvt::round_lsend_[0],
    gvt::round_lrecv_[0],
    rxs
  );
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

      rdxn_gvt_acc = ~uint64_t(0);
      rdxn_rxs_acc = rxs;
      rdxn_gsend = 0;
      rdxn_grecv = 0;
    }
    else
      rdxn_rxs_acc.reduce_with(rxs);
    
    rdxn_gvt_acc = std::min(rdxn_gvt_acc, lvt);
    rdxn_gsend += lsend;
    rdxn_grecv += lrecv;
    
    if(0 == --rdxn_incoming) {
      if(rank_me == 0) {
        bool quiesced = rdxn_gsend == rdxn_grecv;

        //say()<<"root gvt="<<rdxn_gvt_acc<<" send="<<rdxn_gsend<<" recv="<<rdxn_grecv;
        
        rdxn_down(rank_n ^ (quiesced ? -1 : 0), rdxn_gvt_acc, rdxn_rxs_acc);
      }
      else {
        //say()<<"sending up";
        int parent = rank_me & (rank_me-1);
        uint64_t gsend = rdxn_gsend;
        uint64_t grecv = rdxn_grecv;
        rxs = rdxn_rxs_acc;
        lvt = rdxn_gvt_acc;
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
    rdxn_status = quiesced ? rdxn_status_e::quiesced : rdxn_status_e::non_quiesced;
    rdxn_rxs_ans = grxs;
    if(quiesced)
      rdxn_gvt_ans = gvt;
  }
}
