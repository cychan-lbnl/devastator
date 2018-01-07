#include "diagnostic.hxx"
#include "gvt.hxx"
#include "pdes.hxx"
#include "intrusive_map.hxx"
#include "intrusive_min_heap.hxx"
#include "queue.hxx"

#include <atomic>
#include <memory>

using namespace pdes;
using namespace std;

namespace {
  thread_local int cds_on_rank = -1;
  
  class lookahead_state {
    static constexpr int hist_len = 16; // must be pow2
    
    bool prev_sign = true; // true=pos, false=neg
    unsigned round = 0;
    uint64_t hist_exec_n[hist_len] = {/*0...*/};
    uint64_t hist_comm_n[hist_len] = {/*0...*/};
    uint64_t hist_exec_sum = 0;
    uint64_t hist_comm_sum = 0;

  public:
    uint64_t dt = 1;
    uint64_t t_ub = 1;

    void update(uint64_t gvt, uint64_t exec_n, uint64_t comm_n);
  };

  constexpr event_tid end_of_time = {uint64_t(-1), uint64_t(-1)};

  struct event_tid_cd {
    uint64_t time, id;
    int cd;

    friend bool operator==(event_tid_cd a, event_tid_cd b) {
      return a.time == b.time && a.id == b.id && a.cd == b.cd;
    }
    static event_tid_cd of(event_on_creator *e) {
      return {e->time, e->id, e->target_cd};
    }
    static size_t hash_of(event_tid_cd const &x) {
      return 0x9e3779b97f4a7c15u*(0x9e3779b97f4a7c15u*x.time + x.id) + x.cd;
    }
  };
  
  struct local_event {
    event *e;
    event_tid tid;
    
    static int& future_ix_of(local_event le) {
      return le.e->future_ix;
    }
    static event_tid tid_of(local_event le) {
      return le.tid;
    }
  };

  // causality domain (logical process)
  struct cd_state {
    queue<local_event> past_events;
    
    intrusive_min_heap<
        local_event, event_tid,
        local_event::future_ix_of, local_event::tid_of>
      future_events;
    
    cd_state *anti_next; // next cd with events to annihilate
    event *anti_least = nullptr; // least event to annihilate

    int by_now_ix, by_dawn_ix;
    
    event_tid was() const {
      return past_events.back_or({nullptr,{0,0}}).tid;
    }
    uint64_t now() const {
      return future_events.least_key_or(end_of_time).first;
    }
    uint64_t now_after_future_insert() const {
      return future_events.least_key().first;
    }
    uint64_t dawn() const {
      return past_events.front_or({nullptr, end_of_time}).tid.first;
    }
  };
  
  template<int cd_state::*ix>
  struct cd_by {
    cd_state *cd;
    uint64_t key;
    static int& ix_of(cd_by by) { return by.cd->*ix; }
    static uint64_t key_of(cd_by by) { return by.key; }
  };
  
  struct sim_state {
    unique_ptr<cd_state[]> cds;
    
    intrusive_min_heap<
        cd_by<&cd_state::by_now_ix>,
        uint64_t,
        cd_by<&cd_state::by_now_ix>::ix_of,
        cd_by<&cd_state::by_now_ix>::key_of>
      cds_by_now;
    
    intrusive_min_heap<
        cd_by<&cd_state::by_dawn_ix>,
        uint64_t,
        cd_by<&cd_state::by_dawn_ix>::ix_of,
        cd_by<&cd_state::by_dawn_ix>::key_of>
      cds_by_dawn;

    intrusive_map<
        event_on_creator, event_tid_cd,
        &event_on_creator::far_next,
        event_tid_cd::of,
        event_tid_cd::hash_of>
      from_far;
  };
  
  thread_local sim_state sim_me;

  thread_local bool specialed = false;

  void arrive_near(int cd_ix, event *e, event_tid tid, int existence_delta);
  void rollback(cd_state *cd, event_tid new_now);
  
  void lookahead_state::update(uint64_t gvt, uint64_t exec_n, uint64_t comm_n) {
    unsigned r = round++;
    uint64_t comm0 = hist_comm_sum;

    hist_exec_sum -= hist_exec_n[r % hist_len];
    hist_comm_sum -= hist_comm_n[r % hist_len];
    hist_exec_n[r % hist_len] = exec_n;
    hist_comm_n[r % hist_len] = comm_n;
    hist_exec_sum += exec_n;
    hist_comm_sum += comm_n;

    uint64_t comm1 = hist_comm_sum;
    
    double eff_num = hist_comm_sum;
    double eff_den = hist_exec_sum;
    
    if(eff_num < .33*eff_den) {
      dt *= 1/2.0;
      prev_sign = false;
    }
    else if(eff_num > .99*eff_den) {
      dt *= 2.0;
      prev_sign = true;
    }
    else {
      bool sign = comm1 >= comm0 ? prev_sign : !prev_sign;
      prev_sign = sign;
      
      if(sign)
        dt = 1 + uint64_t(1.01*double(dt));
      else
        dt = uint64_t((1/1.01)*double(dt-1));
    }

    dt = std::min<uint64_t>(1ull<<58, dt);
    dt = std::max<uint64_t>(1, dt);
    
    #if 0
      static thread_local int skips = 0;
      if(world::rank_me() == 0 && skips++ == 100) {
        skips = 0;
        std::cout<<"lookahead = "<<float(dt)<<'\n';
        std::cout<<"commit = "<<hist_comm_sum<<'\n';
        std::cout<<"efficiency = "<<eff_num/eff_den<<'\n';
        std::cout<<'\n';
      }
    #endif

    t_ub = gvt + dt;
    if(t_ub < gvt)
      t_ub = uint64_t(-1);
  }
}

void pdes::init(int cds_this_rank) {
  cds_on_rank = cds_this_rank;
  cd_state *cds = new cd_state[cds_on_rank];
  sim_me.cds.reset(cds);

  for(int i=0; i < cds_on_rank; i++) {
    sim_me.cds_by_dawn.insert({&cds[i], cds[i].dawn()});
    sim_me.cds_by_now.insert({&cds[i], cds[i].now()});
  }
  
  gvt::init({0, 0});
  gvt::epoch_begin(0, {0, 0});
}

void pdes::root_event(int cd_ix, event *e) {
  e->existence = 1;
  cd_state *cd = &sim_me.cds[cd_ix];
  cd->future_events.insert({e, e->tid()});
  sim_me.cds_by_now.decreased({cd, cd->now_after_future_insert()});
}

namespace {
  constexpr event_vtable anti_vtable = {
    /*destruct_and_delete*/nullptr,
    /*execute*/nullptr,
    /*unexecute*/nullptr,
    /*commit*/nullptr
  };
}

void pdes::arrive_far(int cd_ix, event *e, event_tid e_tid, int existence_delta) {
  event_on_creator *e_add = nullptr;
  
  sim_me.from_far.visit(
    event_tid_cd{e_tid.first, e_tid.second, cd_ix},
    [&](event_on_creator *o)->event_on_creator* {
      if(o == nullptr) {
        if(existence_delta == 1) {
          // insert event
          arrive_near(cd_ix, e, e_tid, existence_delta);
          return e;
        }
        else { // insert anti-event
          o = new event_on_creator;
          o->vtbl1 = &anti_vtable;
          o->time = e_tid.first;
          o->id = e_tid.second;
          o->target_rank = 0xdeadbeef;
          o->target_cd = cd_ix;
          return o;
        }
      }
      else {
        if(o->vtbl1 == &anti_vtable && existence_delta == 1) {
          // early annihilation (NOT present with bug)
          delete o;
          e->vtbl1->destruct_and_delete(e);
          return nullptr;
        }
        else if(o->vtbl1 != &anti_vtable && existence_delta == -1) {
          // late anninilation
          arrive_near(cd_ix, static_cast<event*>(o), e_tid, existence_delta);
          o->vtbl1->destruct_and_delete(static_cast<event*>(o));
          return nullptr;
        }
        else { // multiplicity (NOT present with bug)
          if(existence_delta == 1) {
            // event multiplicity
            arrive_near(cd_ix, e, e_tid, existence_delta);
            e_add = e;
            return o;
          }
          else {
            // anti-event multiplicity
            e_add = new event_on_creator;
            e_add->vtbl1 = &anti_vtable;
            e_add->time = e_tid.first;
            e_add->id = e_tid.second;
            e_add->target_rank = 0xdeadbeef;
            e_add->target_cd = cd_ix;
            return o;
          }
        }
      }
    }
  );

  if(e_add) {
    sim_me.from_far.insert(e_add);
  }
}

namespace {
  void arrive_near(int cd_ix, event *e, event_tid e_tid, int existence_delta) {
    sim_state &sim_me = ::sim_me;
    cd_state *cd = &sim_me.cds[cd_ix];

    int8_t existence0 = e->existence;
    int8_t existence1 = e->existence + existence_delta;

    if(e_tid <= cd->was()) {
      if(existence0 >= 0 && existence1 >= 0)
        rollback(cd, e_tid);
    }

    e->existence = existence1;
    
    if(existence0 >= 0 && existence1 >= 0) {
      if(existence1 > 0) {
        cd->future_events.insert({e, e_tid});
        sim_me.cds_by_now.decreased({cd, cd->now_after_future_insert()});
      }
      else {
        cd->future_events.erase({e, e_tid});
        sim_me.cds_by_now.increased({cd, cd->now()});
      }
    }
  }
  
  void rollback(cd_state *cd, event_tid new_now) {
    //say()<<"ROLLBACK";
    sim_state &sim_me = ::sim_me;
    
    // accumulates list of cd's needing rollback
    cd_state *cds_anti_head = nullptr;
    
    rollback_cd: {
      int rolled_n;
      
      { // walk backwards to unexecute
        int i = 0;
        int past_n = cd->past_events.size();
        while(i < past_n) {
          local_event le = cd->past_events.at_backwards(i);
          if(le.tid < new_now)
            break;
          le.e->vtbl2->unexecute(le.e);
          i += 1;
        }
        rolled_n = i;
      }
      
      { // walk forwards to remove
        int i = rolled_n;
        while(i-- > 0) {
          local_event le = cd->past_events.at_backwards(i);
          //say()<<"le.t="<<le.tid.first;

          // walk far-sent events
          while(!le.e->sent_far.empty()) {
            far_event_tid far = le.e->sent_far.front();
            le.e->sent_far.pop_front();
            
            gvt::send_remote(far.rank, /*time*/far.tid.second, [=]() {
              arrive_far(far.cd, nullptr, far.tid, -1);
            });
          }
          
          // walk near-sent events
          event *sent = le.e->sent_near_head;
          while(sent != nullptr) {
            event *sent_next = sent->sent_near_next;
            event_tid sent_tid = sent->tid();
            
            if(sent->target_rank != world::rank_me()) { // event sent to near-remote
              // send anti-message
              int target_cd = sent->target_cd;
              gvt::send_local(sent->target_rank, sent->time, [=]() {
                arrive_near(target_cd, sent, sent_tid, -1);
              });
            }
            else { // event self-sent to this rank
              cd_state *cd1 = &sim_me.cds[sent->target_cd];
              
              if(sent_tid <= cd1->was()) {
                if(cd != cd1) {
                  // since target cd is different we remember to rollback later
                  if(cd1->anti_least == nullptr) {
                    cd1->anti_least = sent;
                    // register cd1 as needing rollback
                    cd1->anti_next = cds_anti_head;
                    cds_anti_head = cd1;
                  }
                  else if(sent_tid < cd1->anti_least->tid())
                    cd1->anti_least = sent;
                }
                
                sent->existence = -1; // mark event for removal
              }
              else { // can remove now, hasn't executed
                cd1->future_events.erase({sent, sent_tid});
                sim_me.cds_by_now.increased({cd1, cd1->now()});
                sent->vtbl1->destruct_and_delete(sent);
              }
            }
            
            sent = sent_next;
          }

          if(le.e->existence == -1) {
            ASSERT(le.e->target_rank == world::rank_me() &&
                   &sim_me.cds[le.e->target_cd] == cd);
            le.e->vtbl1->destruct_and_delete(le.e);
          }
          else {
            cd->future_events.insert(le);
            sim_me.cds_by_now.decreased({cd, cd->now_after_future_insert()});
          }
        }
      }
      
      cd->past_events.chop_back(rolled_n);
      if(cd->past_events.size() == 0)
        sim_me.cds_by_dawn.increased({cd, end_of_time.first});
    }
    
    // look for another `cd` that needs rollback
    if(cds_anti_head != nullptr) {
      cd = cds_anti_head;
      cds_anti_head = cd->anti_next;
      new_now = cd->anti_least->tid();
      cd->anti_least = nullptr;
      goto rollback_cd;
    }
  }
}

void pdes::drain() {
  sim_state &sim_me = ::sim_me;
  
  uint64_t executed_n = 0;
  uint64_t committed_n = 0;

  // time-sorted list of all locally created events which were sent away
  intrusive_min_heap<
      event*, std::uint64_t,
      event::remote_near_ix_of, event::time_of>
    remote_near_events;

  gvt::reducibles rxs_acc = {0,0};
  lookahead_state lookahead;
  
  while(true) {
    world::progress();
    
    { // nurse gvt
      uint64_t lvt = sim_me.cds_by_now.least_key();
      uint64_t gvt_old = gvt::epoch_gvt();

      gvt::advance();
      
      if(gvt::epoch_ended()) {
        uint64_t gvt_new = gvt::epoch_gvt();
        
        rxs_acc.reduce_with(gvt::epoch_reducibles());
        
        { // delete near-committed events from previous epoch
          while(remote_near_events.least_key_or(uint64_t(-1)) < gvt_old) {
            event *e = remote_near_events.pop_least();
            e->vtbl1->destruct_and_delete(e);
          }
        }
        
        if(gvt_new != gvt_old) {
          // commmit events that have fallen behind new gvt
          while(true) {
            cd_state *cd = sim_me.cds_by_dawn.peek_least().cd;
            int past_n = cd->past_events.size();
            int commit_n = 0;
            
            while(commit_n < past_n) {
              local_event le = cd->past_events.at_forwards(commit_n);
              if(le.tid.first >= gvt_new)
                break;
              
              commit_n += 1;
              le.e->vtbl2->commit(le.e);
              
              if(le.e->creator_rank == world::rank_me()) {
                // We report as the creator of events which were sent from a
                // far since we actually allocated and constructed them.
                sim_me.from_far.remove(le.e);
                
                le.e->vtbl2->destruct_and_delete(le.e);
              }
            }
            
            if(commit_n == 0)
              break;
            
            committed_n += commit_n;
            cd->past_events.chop_front(commit_n);
            sim_me.cds_by_dawn.increased({cd, cd->dawn()});
          }
                    
          // update lookahead
          lookahead.update(gvt_new, rxs_acc.sum1, rxs_acc.sum2);
          rxs_acc = {0,0};
        }
        else if(gvt_old == ~uint64_t(0)) {
          ASSERT(remote_near_events.size() == 0);
          return;
        }
        
        // begin new epoch
        gvt::epoch_begin(lvt, {executed_n, committed_n});
        executed_n = 0;
        committed_n = 0;
      }
    }
    
    { // execute one event
      cd_state *cd = sim_me.cds_by_now.peek_least().cd;
      local_event le = cd->future_events.peek_least_or({nullptr, end_of_time});
      
      if(le.tid.first < lookahead.t_ub) {
        cd->future_events.pop_least();
        sim_me.cds_by_now.increased({cd, cd->now()});
        
        cd->past_events.push_back(le);
        if(cd->past_events.size() == 1)
          sim_me.cds_by_dawn.decreased({cd, cd->dawn()});

        event *sent_near; {
          execute_context_impl cxt;
          cxt.time = le.tid.first;
          cxt.id = le.tid.second;
          cxt.sent_far = &le.e->sent_far;
          
          le.e->vtbl2->execute(le.e, cxt);
          
          le.e->sent_near_head = cxt.sent_near_head;
          sent_near = cxt.sent_near_head;
          
          executed_n += 1;
        }
        
        { // walk the `sent_near` list of the event's execution
          event *sent = sent_near;
          while(sent != nullptr) {
            event_tid sent_tid = sent->tid();
            int sent_cd_ix = sent->target_cd;
            
            if(sent->target_rank != world::rank_me()) {
              gvt::send_local(sent->target_rank, sent->time, [=]() {
                arrive_near(sent_cd_ix, sent, sent_tid, +1);
              });
              
              remote_near_events.insert(sent);
            }
            else {
              sent->existence = 1;
              
              cd_state *sent_cd = &sim_me.cds[sent_cd_ix];
              
              if(cd != sent_cd && sent_tid < sent_cd->was())
                rollback(sent_cd, sent_tid);

              sent_cd->future_events.insert({sent, sent_tid});
              sim_me.cds_by_now.decreased({sent_cd, sent_cd->now_after_future_insert()});
            }
            
            sent = sent->sent_near_next;
          }
        }
      }
    }
  }
}
