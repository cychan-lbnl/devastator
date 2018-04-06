#include "diagnostic.hxx"
#include "gvt.hxx"
#include "pdes.hxx"
#include "intrusive_map.hxx"
#include "intrusive_min_heap.hxx"
#include "queue.hxx"
#include "reduce.hxx"

#include <atomic>
#include <chrono>
#include <memory>
#include <utility>

using namespace pdes;
using namespace std;

std::ostream* pdes::chitter_io = &std::cout;
int pdes::chitter_secs = 3;

thread_local unsigned pdes::event::far_id_bump = 0;

namespace {
  thread_local int cds_on_rank = -1;
  
  thread_local statistics local_stats_;
  
  class global_status_state {
    static constexpr int hist_len = 16; // must be pow2
    
    bool prev_sign = true; // true=pos, false=neg
    unsigned round = 0;
    uint64_t hist_exec_n[hist_len] = {/*0...*/};
    uint64_t hist_comm_n[hist_len] = {/*0...*/};
    uint64_t hist_exec_sum = 0;
    uint64_t hist_comm_sum = 0;
    
    std::chrono::time_point<std::chrono::steady_clock>
      last_chit_tick = std::chrono::steady_clock::now();
    uint64_t io_exec_sum = 0;
    uint64_t io_comm_sum = 0;
    
  public:
    uint64_t look_dt = 1;
    uint64_t look_t_ub = 1;

    void update(uint64_t gvt, uint64_t exec_n, uint64_t comm_n);
  };

  constexpr event_tdig end_of_time = {uint64_t(-1), uint64_t(-1)};

  inline pair<int,unsigned> far_origin_id_of(event_on_creator *e) {
    return {e->far_origin, e->far_id};
  }
  inline size_t far_origin_id_hash(const pair<int,unsigned> &xy) {
    return size_t(xy.first)*0x9e3779b97f4a7c15u + xy.second;
  }
  
  struct local_event {
    event *e;
    event_tdig tdig;
    
    static int& future_ix_of(local_event le) {
      return le.e->future_ix;
    }
    static event_tdig tdig_of(local_event le) {
      return le.tdig;
    }
  };

  // causality domain (logical process)
  struct cd_state {
    int cd_ix;
    queue<local_event> past_events;
    
    intrusive_min_heap<
        local_event, event_tdig,
        local_event::future_ix_of, local_event::tdig_of>
      future_events;
    
    cd_state *anti_next; // next cd with events to annihilate
    event *anti_least = nullptr; // least event to annihilate

    int by_now_ix, by_dawn_ix;
    
    event_tdig was() const {
      return past_events.back_or({nullptr,{0,0}}).tdig;
    }
    uint64_t now() const {
      return future_events.least_key_or(end_of_time).first;
    }
    uint64_t now_after_future_insert() const {
      return future_events.least_key().first;
    }
    uint64_t dawn() const {
      return past_events.front_or({nullptr, end_of_time}).tdig.first;
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
        event_on_creator, std::pair<int,unsigned>,
        event_on_creator::far_next_of,
        far_origin_id_of,
        far_origin_id_hash>
      from_far;
  };
  
  thread_local sim_state sim_me;

  void arrive_far_anti(int origin, unsigned far_id);
  void arrive_near(int cd_ix, event *e, event_tdig tdig, int existence_delta);
  void rollback(cd_state *cd, event_tdig new_now);
  
  void global_status_state::update(uint64_t gvt, uint64_t exec_n, uint64_t comm_n) {
    unsigned r = round++;
    uint64_t comm0 = hist_comm_sum;

    hist_exec_sum -= hist_exec_n[r % hist_len];
    hist_comm_sum -= hist_comm_n[r % hist_len];
    hist_exec_n[r % hist_len] = exec_n;
    hist_comm_n[r % hist_len] = comm_n;
    hist_exec_sum += exec_n;
    hist_comm_sum += comm_n;

    io_exec_sum += exec_n;
    io_comm_sum += comm_n;
    
    uint64_t comm1 = hist_comm_sum;
    
    double eff_num = hist_comm_sum;
    double eff_den = hist_exec_sum;
    
    if(eff_num < .66*eff_den) {
      if(eff_num < .33*eff_den)
        look_dt /= 4;
      else
        look_dt /= 2;
      prev_sign = false;
    }
    else if(eff_num > .95*eff_den) {
      look_dt *= 2;
      prev_sign = true;
    }
    else {
      bool sign = comm1 >= comm0 ? prev_sign : !prev_sign;
      prev_sign = sign;
      
      if(sign)
        look_dt = 1 + uint64_t(1.01*double(look_dt));
      else
        look_dt = uint64_t((1/1.01)*double(look_dt-1));
    }

    look_dt = std::min<uint64_t>(1ull<<58, look_dt);
    look_dt = std::max<uint64_t>(1, look_dt);
    
    if(pdes::chitter_secs > 0) {
      auto period = std::chrono::seconds(chitter_secs);
      auto now = std::chrono::steady_clock::now();
      
      if(world::rank_me() == 0 && now - last_chit_tick > period) {
        (*pdes::chitter_io)
          <<"pdes::drain() status:\n"
          <<"  gvt = "<<double(gvt)<<'\n'
          <<"  lookahead = "<<float(look_dt)<<'\n'
          <<"  commits/sec = "<<double(io_comm_sum)/std::chrono::duration<double>(now - last_chit_tick).count()<<'\n'
          <<"  efficiency = "<<double(io_comm_sum)/double(io_exec_sum)<<'\n'
          <<'\n';
        pdes::chitter_io->flush();
        
        last_chit_tick = now;
        io_exec_sum = 0;
        io_comm_sum = 0;
      }
    }
    
    look_t_ub = gvt + look_dt;
    if(look_t_ub < gvt)
      look_t_ub = uint64_t(-1);
  }
}

void pdes::init(int cds_this_rank) {
  cds_on_rank = cds_this_rank;
  cd_state *cds = new cd_state[cds_on_rank];
  sim_me.cds.reset(cds);

  for(int i=0; i < cds_on_rank; i++) {
    cds[i].cd_ix = i;
    sim_me.cds_by_dawn.insert({&cds[i], cds[i].dawn()});
    sim_me.cds_by_now.insert({&cds[i], cds[i].now()});
  }
  
  local_stats_ = {};
  
  gvt::init({0, 0});
  gvt::epoch_begin(0, {0, 0});
}

void pdes::root_event(int cd_ix, event *e) {
  e->existence = 1;
  cd_state *cd = &sim_me.cds[cd_ix];
  cd->future_events.insert({e, e->tdig()});
  sim_me.cds_by_now.decreased({cd, cd->now_after_future_insert()});
}

pdes::statistics pdes::local_stats() {
  return local_stats_;
}

pair<size_t, size_t> pdes::get_total_event_counts() {
  auto total_global = world::reduce_sum(total_local);
  return make_pair(total_global.executed_n, total_global.committed_n);
}

namespace {
  constexpr event_vtable anti_vtable = {
    /*destruct_and_delete*/nullptr,
    /*execute*/nullptr,
    /*unexecute*/nullptr,
    /*commit*/nullptr
  };
}

void pdes::arrive_far(int origin, unsigned far_id, event *e) {
  sim_me.from_far.visit(
    {origin, far_id},
    [&](event_on_creator *o)->event_on_creator* {
      if(o == nullptr) {
        // insert event
        arrive_near(e->target_cd, e, e->tdig(), /*existence_delta=*/1);
        return e;
      }
      else {
        ASSERT(o->vtbl1 == &anti_vtable);
        // early annihilation (NOT present with bug)
        delete o;
        e->vtbl1->destruct_and_delete(e);
        return nullptr;
      }
    }
  );
}

namespace {
  void arrive_far_anti(int origin, unsigned far_id) {
    sim_me.from_far.visit(
      {origin, far_id},
      [&](event_on_creator *o)->event_on_creator* {
        if(o != nullptr) {
          ASSERT(o->vtbl1 != &anti_vtable);
          // late anninilation
          auto *o1 = static_cast<event*>(o);
          arrive_near(o1->target_cd, o1, o1->tdig(), /*existence_delta=*/-1);
          o1->vtbl1->destruct_and_delete(o1);
          return nullptr;
        }
        else {
          // insert anti-event
          o = new event_on_creator;
          o->vtbl1 = &anti_vtable;
          o->far_origin = origin;
          o->far_id = far_id;
          return o;
        }
      }
    );
  }

  void arrive_near(int cd_ix, event *e, event_tdig e_tdig, int existence_delta) {
    sim_state &sim_me = ::sim_me;
    cd_state *cd = &sim_me.cds[cd_ix];

    int8_t existence0 = e->existence;
    int8_t existence1 = e->existence + existence_delta;

    if(e_tdig <= cd->was()) {
      if(existence0 >= 0 && existence1 >= 0)
        rollback(cd, e_tdig);
    }

    e->existence = existence1;
    
    if(existence0 >= 0 && existence1 >= 0) {
      if(existence1 > 0) {
        cd->future_events.insert({e, e_tdig});
        sim_me.cds_by_now.decreased({cd, cd->now_after_future_insert()});
      }
      else {
        cd->future_events.erase({e, e_tdig});
        sim_me.cds_by_now.increased({cd, cd->now()});
      }
    }
  }
  
  void rollback(cd_state *cd, event_tdig new_now) {
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
          if(le.tdig < new_now)
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
          //say()<<"le.t="<<le.tdig.first;

          // walk far-sent events
          while(!le.e->sent_far.empty()) {
            far_event_id far = le.e->sent_far.front();
            le.e->sent_far.pop_front();

            int origin = world::rank_me();
            unsigned far_id = far.id;
            gvt::send_remote(far.rank, far.time, [=]() {
              arrive_far_anti(origin, far_id);
            });
          }
          
          // walk near-sent events
          event *sent = le.e->sent_near_head;
          while(sent != nullptr) {
            event *sent_next = sent->sent_near_next;
            event_tdig sent_tdig = sent->tdig();
            
            if(sent->target_rank != world::rank_me()) { // event sent to near-remote
              // send anti-message
              int target_cd = sent->target_cd;
              gvt::send_local(sent->target_rank, sent->time, [=]() {
                arrive_near(target_cd, sent, sent_tdig, -1);
              });
            }
            else { // event self-sent to this rank
              cd_state *cd1 = &sim_me.cds[sent->target_cd];
              
              if(sent_tdig <= cd1->was()) {
                if(cd != cd1) {
                  // since target cd is different we remember to rollback later
                  if(cd1->anti_least == nullptr) {
                    cd1->anti_least = sent;
                    // register cd1 as needing rollback
                    cd1->anti_next = cds_anti_head;
                    cds_anti_head = cd1;
                  }
                  else if(sent_tdig < cd1->anti_least->tdig())
                    cd1->anti_least = sent;
                }
                
                sent->existence = -1; // mark event for removal
              }
              else { // can remove now, hasn't executed
                cd1->future_events.erase({sent, sent_tdig});
                sim_me.cds_by_now.increased({cd1, cd1->now()});
                sent->vtbl1->destruct_and_delete(sent);
              }
            }
            
            sent = sent_next;
          }

          if(le.e->existence == -1) {
            ASSERT(le.e->far_next == reinterpret_cast<event_on_creator*>(0x1) &&
                   le.e->target_rank == world::rank_me() &&
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
      new_now = cd->anti_least->tdig();
      cd->anti_least = nullptr;
      goto rollback_cd;
    }
  }
}

void pdes::drain() {
  sim_state &sim_me = ::sim_me;
  
  uint64_t executed_n = 0;
  uint64_t committed_n = 0;
  
  uint64_t total_executed_n = 0;
  uint64_t total_committed_n = 0;

  // time-sorted list of all locally created events which were sent away
  intrusive_min_heap<
      event*, std::uint64_t,
      event::remote_near_ix_of, event::time_of>
    remote_near_events;

  gvt::reducibles rxs_acc = {0,0};
  global_status_state global_status;
  
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
              if(le.tdig.first >= gvt_new)
                break;
              
              commit_n += 1;
              le.e->vtbl2->commit(le.e);
              
              if(le.e->creator_rank == world::rank_me()) {
                // We report as the creator of events which were sent from a
                // far since we actually allocated and constructed them.
                if(le.e->far_next != reinterpret_cast<event_on_creator*>(0x1))
                  sim_me.from_far.remove(le.e);
                
                le.e->vtbl2->destruct_and_delete(le.e);
              }
            }
            
            if(commit_n == 0)
              break;
            
            committed_n += commit_n;
            total_committed_n += commit_n;
            cd->past_events.chop_front(commit_n);
            sim_me.cds_by_dawn.increased({cd, cd->dawn()});
          }
          
          // update global status
          global_status.update(gvt_new, rxs_acc.sum1, rxs_acc.sum2);
          rxs_acc = {0,0};
        }
        else if(gvt_old == ~uint64_t(0)) {
          ASSERT(remote_near_events.size() == 0);
          goto drain_complete;
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
      
      if(le.tdig.first < global_status.look_t_ub) {
        cd->future_events.pop_least();
        sim_me.cds_by_now.increased({cd, cd->now()});
        
        cd->past_events.push_back(le);
        if(cd->past_events.size() == 1)
          sim_me.cds_by_dawn.decreased({cd, cd->dawn()});

        event *sent_near; {
          execute_context_impl cxt;
          cxt.cd = cd->cd_ix;
          cxt.time = le.tdig.first;
          cxt.digest = le.tdig.second;
          cxt.sent_far = &le.e->sent_far;
          
          le.e->vtbl2->execute(le.e, cxt);
          
          le.e->sent_near_head = cxt.sent_near_head;
          sent_near = cxt.sent_near_head;
          
          executed_n += 1;
          total_executed_n += 1;
        }
        
        { // walk the `sent_near` list of the event's execution
          event *sent = sent_near;
          while(sent != nullptr) {
            event_tdig sent_tdig = sent->tdig();
            int sent_cd_ix = sent->target_cd;
            
            if(sent->target_rank != world::rank_me()) {
              gvt::send_local(sent->target_rank, sent->time, [=]() {
                arrive_near(sent_cd_ix, sent, sent_tdig, +1);
              });
              
              remote_near_events.insert(sent);
            }
            else {
              sent->existence = 1;
              
              cd_state *sent_cd = &sim_me.cds[sent_cd_ix];

              if(cd != sent_cd && sent_tdig < sent_cd->was())
                rollback(sent_cd, sent_tdig);

              sent_cd->future_events.insert({sent, sent_tdig});
              sim_me.cds_by_now.decreased({sent_cd, sent_cd->now_after_future_insert()});
            }
            
            sent = sent->sent_near_next;
          }
        }
      }
    }
  }

drain_complete:
  ASSERT_ALWAYS(sim_me.from_far.size() == 0);
  ASSERT_ALWAYS(remote_near_events.size() == 0);
  sim_me.cds_by_dawn.clear();
  sim_me.cds_by_now.clear();
  sim_me.cds.reset();
  
  local_stats_.executed_n = total_executed_n;
  local_stats_.committed_n = total_committed_n;
}
