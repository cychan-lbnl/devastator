#include <devastator/gvt.hxx>

using namespace std;

namespace gvt = deva::gvt;

namespace deva {
  namespace gvt {
    __thread coll_status_e coll_status_[2];
    __thread reducibles coll_rxs_[2];
    __thread uint64_t epoch_gvt_[2];
    
    __thread unsigned epoch_;
    __thread uint64_t epoch_lvt_[2];
    __thread uint64_t epoch_lsend_[2];
    __thread uint64_t epoch_lrecv_[3];
  }
}

namespace {
  __thread int rdxn_incoming;
  __thread uint64_t rdxn_gsend, rdxn_grecv;
  __thread uint64_t rdxn_gvt_acc;
  __thread gvt::reducibles rdxn_rxs_acc;
  
  void rdxn_up(uint64_t lvt, uint64_t lsend, uint64_t lrecv, gvt::reducibles rxs);
  void rdxn_down(int to_ub, uint64_t gvt, gvt::reducibles rxs);
}

void deva::gvt::init(uint64_t gvt0, gvt::reducibles rxs0) {
  coll_status_[0] = coll_status_e::non_quiesced;
  coll_status_[1] = coll_status_e::non_quiesced;
  coll_rxs_[0] = rxs0;
  coll_rxs_[1] = rxs0;
  epoch_gvt_[0] = gvt0;
  epoch_gvt_[1] = gvt0;
  
  epoch_ = 0;
  epoch_lvt_[0] = gvt0;
  epoch_lvt_[1] = gvt0;
  epoch_lsend_[0] = 0;
  epoch_lsend_[1] = 0;
  epoch_lrecv_[0] = 0;
  epoch_lrecv_[1] = 0;
  epoch_lrecv_[2] = 0;
  
  rdxn_incoming = 0;
  rdxn_gvt_acc = 0;
  
  deva::barrier();
}

void deva::gvt::coll_begin(std::uint64_t lvt, gvt::reducibles rxs) {
  DEVA_ASSERT(coll_status_[0] != coll_status_e::reducing &&
              coll_status_[0] == coll_status_[1]);
  
  if(coll_status_[0] == coll_status_e::quiesced) {
    epoch_ += 1;
    
    epoch_lvt_[0] = std::min(lvt, gvt::epoch_lvt_[1]);
    epoch_lvt_[1] = ~uint64_t(0);
    
    epoch_lsend_[0] = gvt::epoch_lsend_[1];
    epoch_lsend_[1] = 0;
    
    epoch_lrecv_[0] = gvt::epoch_lrecv_[1];
    epoch_lrecv_[1] = gvt::epoch_lrecv_[2];
    epoch_lrecv_[2] = 0;
  }
  
  coll_status_[0] = coll_status_e::reducing;
  coll_status_[1] = coll_status_e::reducing;
  
  rdxn_up(epoch_lvt_[0], epoch_lsend_[0], epoch_lrecv_[0], rxs);
}

namespace {
  void rdxn_up(uint64_t lvt, uint64_t lsend, uint64_t lrecv, deva::gvt::reducibles rxs) {
    const int rank_me = deva::rank_me();
    const int rank_n = deva::rank_n;
    
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
        deva::send(parent, [=]() {
          rdxn_up(lvt, gsend, grecv, rxs);
        });
      }
    }
  }
  
  void rdxn_down(int to_ub, uint64_t gvt, deva::gvt::reducibles grxs) {
    const bool quiesced = to_ub < 0;
    to_ub ^= quiesced ? -1 : 0;
    
    const int rank_me = deva::rank_me();
    
    while(true) {
      int mid = rank_me + (to_ub - rank_me)/2;
      if(mid == rank_me) break;

      int to_ub1 = to_ub ^ (quiesced ? -1 : 0);
      deva::send(mid, [=]() {
        rdxn_down(to_ub1, gvt, grxs);
      });
      
      to_ub = mid;
    }
    
    gvt::coll_status_[1] = quiesced ? gvt::coll_status_e::quiesced : gvt::coll_status_e::non_quiesced;
    gvt::coll_rxs_[1] = grxs;
    if(quiesced) {
      DEVA_ASSERT(gvt::epoch_gvt_[1] <= gvt);
      gvt::epoch_gvt_[1] = gvt;
    }
  }
}
