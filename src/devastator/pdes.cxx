#include <devastator/diagnostic.hxx>
#include <devastator/gvt.hxx>
#include <devastator/pdes.hxx>
#include <devastator/intrusive_map.hxx>
#include <devastator/intrusive_min_heap.hxx>
#include <devastator/queue.hxx>
#include <devastator/reduce.hxx>

#include <atomic>
#include <chrono>
#include <memory>
#include <utility>
#include <vector>

namespace gvt = deva::gvt;
namespace pdes = deva::pdes;

using namespace pdes;
using namespace pdes::detail;
using namespace std;

std::ostream* pdes::chitter_io = &std::cout;
int pdes::chitter_secs = 3;

thread_local unsigned pdes::detail::event::far_id_bump = 0;

#if DEBUG
thread_local int64_t event::live_n = 0;
#endif

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

    void reset();
    void update(uint64_t exec_n, uint64_t comm_n);
    uint64_t calc_look_t_ub(uint64_t gvt, uint64_t t_end);
  };

  inline pair<int,unsigned> far_origin_id_of(event_on_creator *e) {
    return {e->far_origin, e->far_id};
  }
  inline size_t far_origin_id_hash(const pair<int,unsigned> &xy) {
    return size_t(xy.first)*0x9e3779b97f4a7c15u + xy.second;
  }

  struct cd_state {
    deva::intrusive_min_heap<
        local_event, local_event,
        local_event::future_ix_of, local_event::identity>
      future_events;
    
    deva::queue<local_event> past_events;
    int cd_ix;
    int by_now_ix, by_dawn_ix;
    int undo_n_hi=0, undo_n_lo=0;
    fridged_event *fridge_head = nullptr;
    
    uint64_t now() const {
      return future_events.least_key_or({nullptr, end_of_time, end_of_time}).time;
    }
    uint64_t now_after_future_insert() const {
      return future_events.least_key().time;
    }
    uint64_t dawn() const {
      return past_events.front_or({nullptr, end_of_time, end_of_time}).time;
    }
    uint64_t dawn_after_past_insert() const {
      return past_events.at_forwards(0).time;
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
    
    deva::intrusive_min_heap<
        cd_by<&cd_state::by_now_ix>,
        uint64_t,
        cd_by<&cd_state::by_now_ix>::ix_of,
        cd_by<&cd_state::by_now_ix>::key_of>
      cds_by_now;
    
    deva::intrusive_min_heap<
        cd_by<&cd_state::by_dawn_ix>,
        uint64_t,
        cd_by<&cd_state::by_dawn_ix>::ix_of,
        cd_by<&cd_state::by_dawn_ix>::key_of>
      cds_by_dawn;

    deva::intrusive_map<
        event_on_creator, std::pair<int,unsigned>,
        event_on_creator::far_next_of,
        far_origin_id_of,
        far_origin_id_hash>
      from_far;

    // time-sorted list of all locally created events which were sent away
    deva::intrusive_min_heap<
        event*, std::uint64_t,
        event::sent_near_ix_of, event::time_of>
      sent_near;

    event *anni_near_cold_head = nullptr;
    event *anni_near_hot_head = nullptr;

    std::vector<event*> rewind_roots; // roots targeted at us
    std::vector<event*> rewind_created_near; // roots we created but sent away near
  };
  
  thread_local sim_state sim_me;
  
  void arrive_far_anti(int origin, unsigned far_id);
  template<int charge>
  void arrive_near(int cd_ix, local_event e);
  
  void insert_past(cd_state *cd, local_event ins);
  void remove_past(cd_state *cd, local_event rem);
  void rollback(cd_state *cd, int undo_n);

  void global_status_state::reset() {
    io_exec_sum = 0;
    io_comm_sum = 0;
    last_chit_tick = std::chrono::steady_clock::now();
  }
  
  void global_status_state::update(uint64_t exec_n, uint64_t comm_n) {
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
  }

  uint64_t global_status_state::calc_look_t_ub(uint64_t gvt, uint64_t t_end) {
    if(pdes::chitter_secs > 0) {
      auto period = std::chrono::seconds(chitter_secs);
      auto now = std::chrono::steady_clock::now();
      
      if(deva::rank_me() == 0 && now - last_chit_tick > period) {
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
    
    uint64_t look_t_ub = gvt + look_dt;
    if(look_t_ub < gvt)
      look_t_ub = uint64_t(-1);
    if(look_t_ub > t_end)
      look_t_ub = t_end;
    
    return look_t_ub;
  }
}

void pdes::init(int cds_this_rank) {
  cds_on_rank = cds_this_rank;
  cd_state *cds = new cd_state[cds_on_rank];

  DEVA_ASSERT_ALWAYS(!sim_me.cds);
  sim_me.cds.reset(cds);
  sim_me.cds_by_dawn.resize(cds_on_rank);
  sim_me.cds_by_now.resize(cds_on_rank);
  
  for(int i=0; i < cds_on_rank; i++) {
    cds[i].cd_ix = i;
    sim_me.cds_by_dawn.insert({&cds[i], cds[i].dawn()});
    sim_me.cds_by_now.insert({&cds[i], cds[i].now()});
  }
  
  local_stats_ = {};
}

pdes::statistics pdes::local_stats() {
  return local_stats_;
}

pair<size_t, size_t> pdes::get_total_event_counts() {
  auto ans = deva::reduce_sum(local_stats_);
  return make_pair(ans.executed_n, ans.committed_n);
}

namespace {
  constexpr event_vtable anti_vtable = {
    /*destruct_and_delete*/nullptr,
    /*execute*/nullptr,
    /*unexecute*/nullptr,
    /*commit*/nullptr,
    /*refrigerate*/nullptr
  };
}

void detail::root_event(int cd_ix, event *e) {
  e->created_here = true;
  e->rewind_root = false;
  e->existence = 1;
  e->future_not_past = true;
  
  cd_state *cd = &sim_me.cds[cd_ix];
  cd->future_events.insert({e, e->time, e->subtime});
  sim_me.cds_by_now.decreased({cd, cd->now_after_future_insert()});
}

void detail::arrive_far(int origin, unsigned far_id, event *e) {
  sim_me.from_far.visit(
    {origin, far_id},
    [&](event_on_creator *o)->event_on_creator* {
      if(o == nullptr) {
        // insert event
        e->created_here = true;
        e->rewind_root = false;
        arrive_near<+1>(e->target_cd, local_event{e, e->time, e->subtime});
        return e;
      }
      else {
        DEVA_ASSERT(o->vtbl_on_creator == &anti_vtable);
        delete o;
        e->vtbl_on_creator->destruct_and_delete(e);
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
          DEVA_ASSERT(o->vtbl_on_creator != &anti_vtable);
          // late anninilation
          auto *e = static_cast<event*>(o);
          arrive_near<-1>(e->target_cd, local_event{e, e->time, e->subtime});
          e->vtbl_on_creator->destruct_and_delete(e);
          return nullptr;
        }
        else {
          // insert anti-event
          o = new event_on_creator;
          o->vtbl_on_creator = &anti_vtable;
          o->far_origin = origin;
          o->far_id = far_id;
          return o;
        }
      }
    );
  }

  template<int charge>
  void arrive_near(int cd_ix, local_event le) {
    sim_state &sim_me = ::sim_me;
    cd_state *cd = &sim_me.cds[cd_ix];
    
    le.e->existence += charge;
    
    switch(charge) {
    case +1:
      // positive events go in future regardless of that requires rollback (handled later)
      if(le.e->existence == 1) {
        le.e->future_not_past = true;
        cd->future_events.insert(le);
        sim_me.cds_by_now.decreased({cd, cd->now_after_future_insert()});
      }
      break;
      
    case -1:
      // negative events do not go into future/past
      if(le.e->existence == 0) {
        if(le.e->future_not_past) {
          cd->future_events.erase(le);
          sim_me.cds_by_now.increased({cd, cd->now()});
        }
        else
          remove_past(cd, le);
      }
      break;
    }
  }
}

namespace {
  void insert_past(cd_state *cd, local_event ins) {
    int n = cd->past_events.size();
    cd->past_events.push_back({});
    
    int j = 0;
    while(j < n) {
      local_event le = cd->past_events.at_backwards(j+1);
      cd->past_events.at_backwards(j) = le;
      if(le < ins)
        break;
      j += 1;
    }
    
    cd->past_events.at_backwards(j) = ins;

    sim_me.cds_by_dawn.decreased({cd, cd->dawn_after_past_insert()});
    
    rollback(cd, j);
  }

  void remove_past(cd_state *cd, local_event rem) {
    int j = 0;
    while(rem.e != cd->past_events.at_backwards(j).e)
      j += 1;
    
    rem.e->remove_after_undo = true;
    
    rollback(cd, j+1);
  }

  void rollback(cd_state *cd, int undo_n) {
    sim_state &sim_me = ::sim_me;
    const int rank_me = deva::rank_me();
    
    std::vector<cd_state*> undos_all, undos_fresh;
    undos_all.reserve(32);
    undos_fresh.reserve(32);
    
    undos_all.push_back(cd);
    cd->undo_n_hi = undo_n;

    event *del_head = nullptr; // list of deferred deletes

    // increase cd->undo_n_{lo|hi} until fixed-point 
    // and send anti-messages
    do {
      int i = cd->undo_n_lo;
      while(i < cd->undo_n_hi) {
        local_event le = cd->past_events.at_backwards(i++);
        event *e = le.e;
        
        // walk far-sent events
        while(!e->sent_far.empty()) {
          far_event_id far = e->sent_far.front();
          e->sent_far.pop_front();

          unsigned far_id = far.id;
          gvt::send(
            far.rank, /*local=*/deva::cfalse3, far.time,
            [=]() { arrive_far_anti(rank_me, far_id); }
          );
        }

        // walk near-sent events
        event *sent = e->sent_near_head;
        while(sent != nullptr) {
          event *sent_next = sent->sent_near_next;
          local_event sent_le = {sent, sent->time, sent->subtime};
          
          if(sent->target_rank != rank_me) { // event sent to near-remote
            // remove from sent_near
            sim_me.sent_near.erase(sent);
            
            // add to anni_near
            sent->anni_near_next = sim_me.anni_near_hot_head;
            sim_me.anni_near_hot_head = sent;
            
            // send anti-message
            int target_cd = sent->target_cd;
            gvt::send(
              sent->target_rank, /*local=*/deva::ctrue3, sent->time,
              [=]() { arrive_near<-1>(target_cd, sent_le); }
            );
          }
          else { // sent event to myself
            cd_state *cd1 = &sim_me.cds[sent->target_cd];

            if(sent->future_not_past) {
              // can remove now, hasn't executed
              cd1->future_events.erase(sent_le);
              sim_me.cds_by_now.increased({cd1, cd1->now()});
              // add to deferred delete list
              sent->sent_near_next = del_head;
              del_head = sent;
            }
            else {
              sent->remove_after_undo = true;
              
              if(cd1->undo_n_hi == 0 || sent_le < cd1->past_events.at_backwards(cd1->undo_n_hi-1)) {
                DEVA_ASSERT(cd1 != cd);

                bool already_fresh = cd1->undo_n_hi != cd1->undo_n_lo;
                bool already_all = cd1->undo_n_hi != 0;
                DEVA_ASSERT(!already_fresh || already_all);
                
                int j = cd1->undo_n_hi;
                while(sent != cd1->past_events.at_backwards(j).e)
                  j += 1;
                cd1->undo_n_hi = j + 1;

                if(!already_fresh) {
                  undos_fresh.push_back(cd1);
                  if(!already_all)
                    undos_all.push_back(cd1);
                }
              }
            }
          }
          
          sent = sent_next;
        }
      }

      cd->undo_n_lo = i;

      if(undos_fresh.empty())
        break;
      cd = undos_fresh.back();
      undos_fresh.pop_back();
    }
    while(true);

    // walk all cds and issue unexecute's
    for(cd_state *cd: undos_all) {
      int n = cd->undo_n_hi;
      bool inserted_future = false;
      
      for(int i=0; i < n; i++) {
        local_event le = cd->past_events.at_backwards(i);
        bool should_delete = le.e->remove_after_undo;
        
        if(!should_delete) {
          le.e->future_not_past = true;
          cd->future_events.insert(le);
          inserted_future = true;
          // handled outside loop:
          // sim_me.cds_by_now.decreased({cd, cd->now_after_future_insert()});
        }
        
        le.e->vtbl_on_target->unexecute(le.e, should_delete);
      }

      cd->undo_n_hi = 0;
      cd->undo_n_lo = 0;
      cd->past_events.chop_back(n);
      
      if(inserted_future)
        sim_me.cds_by_now.decreased({cd, cd->now_after_future_insert()});

      if(cd->past_events.size() == 0)
        sim_me.cds_by_dawn.increased({cd, end_of_time});
    }

    // reap deferred deletes
    while(del_head != nullptr) {
      event *next = del_head->sent_near_next;
      del_head->vtbl_on_creator->destruct_and_delete(del_head);
      del_head = next;
    }
  }
}

uint64_t pdes::drain(uint64_t t_end, bool rewindable) {
  sim_state &sim_me = ::sim_me;
  const int rank_me = deva::rank_me();

  //////////////////////////////////////////////////////////////////////////////

  DEVA_ASSERT_ALWAYS(
    sim_me.rewind_roots.empty() && sim_me.rewind_created_near.empty(),
    "Lingering rewind state must be rewound via pdes::rewind(true|false) before calling pdes::drain()."
  );
  
  if(rewindable) {
    for(int cd_ix=0; cd_ix < cds_on_rank; cd_ix++) {
      cd_state *cd = &sim_me.cds[cd_ix];
      
      // Anything in future upon entry to drain is a root. Since might be rewinding
      // mark them as non-deletable.
      int n = cd->future_events.size();
      for(int i=0; i < n; i++) {
        local_event le = cd->future_events.at(i);
        le.e->rewind_root = true; // prevents deleting rewind roots
        sim_me.rewind_roots.push_back(le.e);
      }
    }
    
    { // also prevents deleting rewind roots
      int n = sim_me.sent_near.size();
      for(int i=0; i < n; i++) {
        event *e = sim_me.sent_near.at(i);
        sim_me.rewind_created_near.push_back(e);
      }
      sim_me.sent_near.clear();
    }

    // rewind roots definitely won't be annihilated so we can remove from from_far
    for(auto *es: std::initializer_list<std::vector<event*>*>{
        &sim_me.rewind_roots, // requires we check for `created_here`
        &sim_me.rewind_created_near
      }) {
      for(event *e: *es) {
        if(e->created_here && e->far_next != reinterpret_cast<event_on_creator*>(0x1)) {
          sim_me.from_far.remove(e);
          e->far_next = reinterpret_cast<event_on_creator*>(0x1);
        }
      }
    }
  }

  //////////////////////////////////////////////////////////////////////////////

  uint64_t gvt_returned;
  
  uint64_t executed_n = 0;
  uint64_t committed_n = 0;
  
  uint64_t total_executed_n = local_stats_.executed_n;
  uint64_t total_committed_n = local_stats_.committed_n;

  thread_local global_status_state the_global_status;
  global_status_state &global_status = the_global_status;
  global_status.reset();

  gvt::reducibles rxs_acc = {0,0};
  uint64_t look_t_ub;
  {
    uint64_t lvt = sim_me.cds_by_now.least_key();
    uint64_t gvt0 = deva::reduce_min(lvt);

    gvt::init(gvt0, {0, 0});
    gvt::epoch_begin(lvt, {0, 0});

    look_t_ub = global_status.calc_look_t_ub(gvt0, t_end);
  }
  
  while(true) {
    deva::progress();
    
    { // nurse gvt
      uint64_t lvt = sim_me.cds_by_now.least_key();
      uint64_t gvt_old = gvt::epoch_gvt();

      gvt::advance();
      
      if(gvt::epoch_ended()) {
        uint64_t gvt_new = gvt::epoch_gvt();
        
        rxs_acc.reduce_with(gvt::epoch_reducibles());
        
        { // delete locally created events from previous epoch
          while(sim_me.sent_near.least_key_or(uint64_t(-1)) < gvt_old) {
            event *e = sim_me.sent_near.pop_least();
            e->vtbl_on_creator->destruct_and_delete(e);
          }
          
          // and delete annihilated events from previous epoch
          event *e = sim_me.anni_near_cold_head;
          // hot moved to cold list
          sim_me.anni_near_cold_head = sim_me.anni_near_hot_head;
          // hot list now empty
          sim_me.anni_near_hot_head = nullptr;
          
          while(e != nullptr) {
            event *e_next = e->anni_near_next;
            e->vtbl_on_creator->destruct_and_delete(e);
            e = e_next;
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
              if(le.time >= gvt_new)
                break;

              bool should_delete = le.e->created_here && !le.e->rewind_root;
              
              if(should_delete) {
                if(le.e->far_next != reinterpret_cast<event_on_creator*>(0x1))
                  sim_me.from_far.remove(le.e);
              }
              
              commit_n += 1;
              if(rewindable) {
                fridged_event *f = le.e->vtbl_on_target->commit_and_refrigerate(le.e, le.e->rewind_root, should_delete);
                f->next = cd->fridge_head;
                cd->fridge_head = f;
              }
              else
                le.e->vtbl_on_target->commit(le.e, should_delete);
            }
            
            if(commit_n == 0)
              break;
            
            committed_n += commit_n;
            total_committed_n += commit_n;
            cd->past_events.chop_front(commit_n);
            sim_me.cds_by_dawn.increased({cd, cd->dawn()});
          }
          
          // update global status
          global_status.update(rxs_acc.sum1, rxs_acc.sum2);
          rxs_acc = {0,0};
          look_t_ub = global_status.calc_look_t_ub(gvt_new, t_end);
        }
        else if(t_end <= gvt_old) {
          //say()<<"drain done gvt="<<gvt_old;
          gvt_returned = gvt_old;
          
          if(gvt_old == uint64_t(-1))
            goto drain_completed;
          else
            goto drain_paused;
        }
        
        // begin new epoch
        gvt::epoch_begin(lvt, {executed_n, committed_n});
        executed_n = 0;
        committed_n = 0;
      }
    }
    
    { // execute one event
      cd_state *cd = sim_me.cds_by_now.peek_least().cd;
      local_event le = cd->future_events.peek_least_or({nullptr, end_of_time, end_of_time});
      
      if(le.time < look_t_ub) {
        DEVA_ASSERT(le.e->future_not_past);
        le.e->future_not_past = false;
                
        cd->future_events.pop_least();
        sim_me.cds_by_now.increased({cd, cd->now()});
        
        if(le > cd->past_events.back_or({nullptr, 0, 0})) {
          cd->past_events.push_back(le);
          if(1 == cd->past_events.size())
            sim_me.cds_by_dawn.decreased({cd, le.time});
        }
        else
          insert_past(cd, le);
        
        event *sent_near; {
          execute_context_impl cxt;
          cxt.cd = cd->cd_ix;
          cxt.time = le.time;
          cxt.sent_far = &le.e->sent_far;
          
          le.e->vtbl_on_target->execute(le.e, cxt);
          
          le.e->sent_near_head = cxt.sent_near_head;
          sent_near = cxt.sent_near_head;
          
          executed_n += 1;
          total_executed_n += 1;
        }
        
        { // walk the `sent_near` list of the event's execution
          event *sent = sent_near;
          while(sent != nullptr) {
            local_event sent_le = {sent, sent->time, sent->subtime};
            int sent_cd_ix = sent->target_cd;
            
            if(sent->target_rank != rank_me) {
              sent->created_here = false;
              sent->rewind_root = false;
              sent->existence = 0;
              sent->future_not_past = false; // garbage would be fine
              sent->remove_after_undo = false;
              
              gvt::send(
                sent->target_rank, /*local=*/deva::ctrue3, sent->time,
                [=]() { arrive_near<+1>(sent_cd_ix, sent_le); }
              );
              
              sim_me.sent_near.insert(sent);
            }
            else {
              sent->created_here = true;
              sent->rewind_root = false;
              sent->existence = 1;
              sent->future_not_past = true;
              sent->remove_after_undo = false;
              
              cd_state *sent_cd = &sim_me.cds[sent_cd_ix];
              sent_cd->future_events.insert(sent_le);
              sim_me.cds_by_now.decreased({sent_cd, sent_cd->now_after_future_insert()});
            }
            
            sent = sent->sent_near_next;
          }
        }
      }
    }
  }

drain_completed:
  DEVA_ASSERT_ALWAYS(sim_me.from_far.size() == 0);
  DEVA_ASSERT_ALWAYS(sim_me.sent_near.size() == 0, "sim_me.sent_near.size() = "<<sim_me.sent_near.size()<<", expected 0");
  
drain_paused:
  DEVA_ASSERT_ALWAYS(sim_me.anni_near_cold_head == nullptr);
  DEVA_ASSERT_ALWAYS(sim_me.anni_near_hot_head == nullptr);
  local_stats_.executed_n = total_executed_n;
  local_stats_.committed_n = total_committed_n;
  return gvt_returned;
}

void pdes::finalize() {
  deva::barrier();
  
  for(int cd_ix=0; cd_ix < cds_on_rank; cd_ix++) {
    cd_state *cd = &sim_me.cds[cd_ix];
    DEVA_ASSERT(cd->past_events.size() == 0);
    
    fridged_event *f = cd->fridge_head;
    cd->fridge_head = nullptr;
    while(f != nullptr) {
      fridged_event *f_next = f->next;
      f->just_delete();
      f = f_next;
    }

    { // delete locally created non-roots that were sent here
      int n = cd->future_events.size();
      for(int i=0; i < n; i++) {
        local_event le = cd->future_events.at(i);
        if(le.e->created_here && !le.e->rewind_root)
          le.e->vtbl_on_target->destruct_and_delete(le.e);
      }
      cd->future_events.clear();
    }
  }

  deva::barrier();

  { // delete locally created non-roots that were sent away
    int n = sim_me.sent_near.size();
    for(int i=0; i < n; i++) {
      event *e = sim_me.sent_near.at(i);
      e->vtbl_on_creator->destruct_and_delete(e);
    }
    sim_me.sent_near.clear();
    sim_me.from_far.clear();
  }

  // delete locally created roots
  for(event *e: sim_me.rewind_roots) {
    if(e->created_here)
      e->vtbl_on_creator->destruct_and_delete(e);
  }
  for(event *e: sim_me.rewind_created_near)
    e->vtbl_on_creator->destruct_and_delete(e);
  sim_me.rewind_created_near.clear();
  sim_me.rewind_roots.clear();
  
  sim_me.cds_by_dawn.clear();
  sim_me.cds_by_now.clear();
  sim_me.cds.reset();
  cds_on_rank = 0;

  DEVA_ASSERT(event::live_n == 0, "live_n="<<event::live_n);
}

void pdes::rewind(bool do_rewind) {
  deva::barrier();

  auto &sim_me = ::sim_me;

  if(do_rewind) {
    for(int cd_ix=0; cd_ix < cds_on_rank; cd_ix++) {
      cd_state *cd = &sim_me.cds[cd_ix];

      { // unexecute fridge
        fridged_event *f = cd->fridge_head;
        cd->fridge_head = nullptr;
        while(f != nullptr) {
          fridged_event *f_next = f->next;
          f->unexecute_and_delete();
          f = f_next;
        }
      }
      
      { // delete locally created non-roots that were sent here
        int n = cd->future_events.size();
        for(int i=0; i < n; i++) {
          local_event le = cd->future_events.at(i);
          if(le.e->created_here && !le.e->rewind_root)
            le.e->vtbl_on_target->destruct_and_delete(le.e);
        }
        cd->future_events.clear();
      }
    }

    deva::barrier();

    { // delete locally created non-roots that were sent away
      int n = sim_me.sent_near.size();
      for(int i=0; i < n; i++) {
        event *e = sim_me.sent_near.at(i);
        e->vtbl_on_creator->destruct_and_delete(e);
      }
      sim_me.sent_near.clear();
      sim_me.from_far.clear();
    }

    // move the rewind roots into future
    for(event *e: sim_me.rewind_roots) {
      cd_state *cd = &sim_me.cds[e->target_cd];
      e->rewind_root = false;
      e->future_not_past = true;
      cd->future_events.insert(local_event{e, e->time, e->subtime});
    }
    for(event *e: sim_me.rewind_created_near)
      sim_me.sent_near.insert(e);
    sim_me.rewind_roots.clear();
    sim_me.rewind_created_near.clear();
    
    for(int cd_ix=0; cd_ix < cds_on_rank; cd_ix++) {
      cd_state *cd = &sim_me.cds[cd_ix];
      sim_me.cds_by_now.changed({cd, cd->now()});
    }
  }
  else {
    for(int cd_ix=0; cd_ix < cds_on_rank; cd_ix++) {
      cd_state *cd = &sim_me.cds[cd_ix];

      { // delete fridge
        fridged_event *f = cd->fridge_head;
        cd->fridge_head = nullptr;
        while(f != nullptr) {
          fridged_event *f_next = f->next;
          f->just_delete();
          f = f_next;
        }
      }
    }
    
    for(event *e: sim_me.rewind_roots) {
      if(e->created_here && !e->future_not_past)
        e->vtbl_on_creator->destruct_and_delete(e);
    }
    sim_me.rewind_roots.clear();
    
    deva::barrier();
    
    for(event *e: sim_me.rewind_created_near) {
      if(!e->future_not_past)
        e->vtbl_on_creator->destruct_and_delete(e);
    }
    sim_me.rewind_created_near.clear();
  }
}
