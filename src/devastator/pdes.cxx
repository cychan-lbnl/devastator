#include <devastator/diagnostic.hxx>
#include <devastator/gvt.hxx>
#include <devastator/pdes.hxx>
#include <devastator/intrusive_map.hxx>
#include <devastator/intrusive_min_heap.hxx>
#include <devastator/queue.hxx>
#include <devastator/os_env.hxx>

#include <atomic>
#include <chrono>
#include <memory>
#include <utility>
#include <vector>
#include <fstream>

namespace gvt = deva::gvt;
namespace pdes = deva::pdes;

using namespace pdes;
using namespace pdes::detail;
using namespace std;

std::ostream* pdes::chitter_io = &std::cout;
int pdes::chitter_secs = 3;

constexpr detail::sent_far_record::vtable detail::sent_far_one::the_vtbl;

__thread uint64_t pdes::detail::far_id_bumper;
uint64_t pdes::detail::seq_id_delta;

#if DEBUG
__thread std::int64_t pdes::detail::live_event_balance = 0;
#endif

namespace {
  const int64_t static_look_dt = deva::os_env<int64_t>("deva_static_look_dt", -1);
}
#if DRAIN_TIMER
namespace {
constexpr auto default_sim_interval = static_cast<uint64_t>(15*60*1000) << 33; // 15 minutes in milliseconds, assuming 33 bits of subtime
const double wall_interval_s = std::max(0.001, deva::os_env<double>("deva_drain_timer_wall_interval", 10)); // 10 seconds
}

const uint64_t pdes::DrainTimer::sim_interval = std::max(1ul, os_env<uint64_t>("deva_drain_timer_sim_interval" , default_sim_interval));
const std::chrono::steady_clock::duration pdes::DrainTimer::wall_interval =
        std::chrono::milliseconds(static_cast<int>(wall_interval_s * 1000));
#endif // DRAIN_TIMER

namespace {
  bool flag_heartbeat = deva::os_env<bool>("pdes_heartbeat", true);

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

  inline pair<uint64_t,uint64_t> far_id_time_of(event_on_creator *e) {
    return {e->far_id, e->time};
  }
  inline size_t far_id_time_hash(pair<uint64_t,uint64_t> const &xy) {
    return xy.first ^ xy.second;
  }

  struct cd_state {
    deva::intrusive_min_heap<
        stamped_event, stamped_event,
        stamped_event::future_ix_of, deva::identity<stamped_event>>
      future_events;
    
    deva::queue<stamped_event> past_events;
    int32_t cd_ix;
    int32_t by_now_ix, by_dawn_ix;
    int undo_n_hi=0, undo_n_lo=0;
    uint64_t seq_id_bumper, seq_id_bumper_rewind;
    fridge *fridge_head = nullptr;

    #if DEBUG
    std::function<std::uint64_t()> checksummer;
    #endif
    
    std::pair<std::uint64_t/*time+1*/,std::uint64_t/*subtime*/>
      last_commit_t, rewind_commit_t;
    
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

    uint64_t next_seq_id(int n);
  };

  template<int32_t cd_state::*ix>
  struct cd_by {
    cd_state *cd;
    uint64_t key;
    static int32_t& ix_of(cd_by by) { return by.cd->*ix; }
    static uint64_t key_of(cd_by by) { return by.key; }
  };
  
  struct sim_state {
    int32_t local_cd_n = -1;
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
        event_on_creator, pair<uint64_t,uint64_t>,
        event_on_creator::far_next_of,
        far_id_time_of, far_id_time_hash>
      from_far;

    // time-sorted list of all locally created events which were sent away
    deva::intrusive_min_heap<
        event*, uint64_t,
        event::sent_near_ix_of, event::time_of>
      sent_near;

    event *anni_near_cold_head = nullptr;
    event *anni_near_hot_head = nullptr;

    bool has_rewind = false;
    std::vector<event*> rewind_roots; // roots targeted at us
    std::vector<event*> rewind_created_near; // roots we created but sent away near

    statistics stats;

    #if DRAIN_TIMER
      bool spinning_empty = false;
      bool spinning_look = false;
      DrainTimer drain_timer;

      bool spinning () {
        return spinning_empty || spinning_look;
      }

      void drain_timer_update_spin_or (DrainTimer::Cat or_cat) {
        auto cat = spinning_empty ? DrainTimer::Cat::spin_empty :
                  (spinning_look  ? DrainTimer::Cat::spin_look : or_cat);
        drain_timer.update(cat);
      }
    #endif // DRAIN_TIMER

    #if TIMELINE
      struct Timeline
      {
        struct EventRecord {
          uint64_t time;
          int gen_rank;
          int gen_cd;
          uint64_t gen_time;
        };
        std::vector<std::vector<EventRecord>> events;

        void init (int cd_n) {
          events.resize(cd_n);
        }
        void record_event (int cd, uint64_t time, int gen_rank, int gen_cd, uint64_t gen_time) {
          events[cd].push_back({time, gen_rank, gen_cd, gen_time});
        }
        friend std::ostream & operator<< (std::ostream & os, const Timeline & tl) {
          os << "rank,cd,time,gen_rank,gen_cd,gen_time" << endl;
          for (int cd = 0; cd < tl.events.size(); ++cd) {
            for (const auto & e : tl.events[cd]) {
              os << deva::rank_me() << ',' << cd << ',' << e.time << ',' << e.gen_rank << ',' << e.gen_cd << ',' << e.gen_time << std::endl;
            }
          }
          return os;
        }
      };
      Timeline timeline;
    #endif
  };
  
  thread_local sim_state sim_me;
  
  template<int charge>
  void arrive_near(int32_t cd_ix, stamped_event e);
  
  void insert_past(cd_state *cd, stamped_event ins);
  void remove_past(cd_state *cd, stamped_event rem);
  void rollback(cd_state *cd, int undo_n);

  inline uint64_t cd_state::next_seq_id(int n) {
    uint64_t id = seq_id_bumper;
    seq_id_bumper += n*seq_id_delta;
    return id;
  }
  
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

    if (static_look_dt != -1) {
      look_dt = static_look_dt;
      return;
    }

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
    if(pdes::chitter_secs > 0 && pdes::chitter_io != nullptr) {
      auto period = std::chrono::seconds(chitter_secs);
      auto now = std::chrono::steady_clock::now();
      
      if(flag_heartbeat && deva::rank_me() == 0 && now - last_chit_tick > period) {
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

void pdes::init(int32_t local_cd_n) {
  sim_me.local_cd_n = local_cd_n;

  far_id_bumper = deva::rank_me();
  
  uint64_t global_cd_begin, global_cd_n;
  std::tie(global_cd_begin, global_cd_n) = deva::scan_reduce_sum<uint64_t>(local_cd_n);
  
  seq_id_delta = global_cd_n;
  
  cd_state *cds = new cd_state[local_cd_n];

  DEVA_ASSERT_ALWAYS(!sim_me.cds);
  sim_me.cds.reset(cds);
  sim_me.cds_by_dawn.resize(local_cd_n);
  sim_me.cds_by_now.resize(local_cd_n);
  
  for(int32_t i=0; i < local_cd_n; i++) {
    cds[i].cd_ix = i;
    cds[i].seq_id_bumper = global_cd_begin + i;
    cds[i].last_commit_t = {0,0};
    sim_me.cds_by_dawn.insert({&cds[i], cds[i].dawn()});
    sim_me.cds_by_now.insert({&cds[i], cds[i].now()});
  }
  
  sim_me.stats = {};

  #if TIMELINE
    sim_me.timeline.init(local_cd_n);
  #endif
}

pdes::statistics pdes::local_stats() {
  return sim_me.stats;
}

pair<size_t, size_t> pdes::get_total_event_counts() {
  auto ans = deva::reduce_sum(sim_me.stats);
  return make_pair(ans.executed_n, ans.committed_n);
}

namespace {
  constexpr event_vtable anti_vtable = {
    /*destruct_and_delete*/nullptr,
    /*execute*/nullptr,
    /*unexecute*/nullptr,
    /*commit*/nullptr
  };
}

void detail::register_state(int cd_ix, fridge *fr) {
  cd_state *cd = &sim_me.cds[cd_ix];
  fr->next = cd->fridge_head;
  cd->fridge_head = fr;
}

void pdes::register_checksum_if_debug(int32_t cd_ix, std::function<uint64_t()> &&fn) {
#if DEBUG
  cd_state *cd = &sim_me.cds[cd_ix];
  cd->checksummer = std::move(fn);
#endif
}

void detail::root_event(int32_t cd_ix, event *e) {
  DEVA_ASSERT(0 <= cd_ix && cd_ix < sim_me.local_cd_n);

  cd_state *cd = &sim_me.cds[cd_ix];
  e->created_here = true;
  e->rewind_root = false;
  e->existence = 1;
  e->future_not_past = true;
  
  cd->future_events.insert({e, e->time, e->subtime});
  sim_me.cds_by_now.decreased({cd, cd->now_after_future_insert()});
}

std::uint64_t detail::next_seq_id(std::int32_t cd, int n) {
  return sim_me.cds[cd].next_seq_id(n);
}

bool detail::arrive_far(uint64_t far_id, uint64_t time, int32_t cd, event *e) {
  e->far_id = far_id;
  e->time = time;
  e->target_cd = cd;

  bool annihilated = false;
  
  sim_me.from_far.visit({far_id, time},
    [&](event_on_creator *o)->event_on_creator* {
      if(o == nullptr) {
        //deva::say()<<"far insert origin="<<origin<<" id="<<far_id<<" o="<<(event_on_creator*)e;
        // insert event
        e->created_here = true;
        e->rewind_root = false;
        arrive_near<+1>(e->target_cd, stamped_event{e, e->time, e->subtime});
        annihilated = false;
        return e;
      }
      else {
        //deva::say()<<"far anni-+ origin="<<origin<<" id="<<far_id<<" o="<<(void*)o;
        DEVA_ASSERT(o->vtbl_on_creator == &anti_vtable);
        delete o;
        e->vtbl_on_creator->destruct_and_delete(e);
        annihilated = true;
        return nullptr;
      }
    }
  );

  return annihilated;
}

bool detail::arrive_far_anti(uint64_t far_id, uint64_t time) {
  bool annihilated = false;
  
  sim_me.from_far.visit({far_id, time},
    [&](event_on_creator *o)->event_on_creator* {
      if(o != nullptr) {
        //deva::say()<<"far anni+- id="<<far_id<<" o="<<o;
        DEVA_ASSERT(o->vtbl_on_creator != &anti_vtable);
        // late anninilation
        auto *e = static_cast<event*>(o);
        stamped_event se{e, e->time, e->subtime};
        cd_state *cd = &sim_me.cds[e->target_cd];
        if(e->future_not_past) {
          cd->future_events.erase(se);
          sim_me.cds_by_now.increased({cd, cd->now()});
          e->vtbl_on_creator->destruct_and_delete(e);
        }
        else
          remove_past(cd, se);
        annihilated = true;
        return nullptr;
      }
      else {
        // insert anti-event
        o = new event_on_creator;
        o->vtbl_on_creator = &anti_vtable;
        o->far_id = far_id;
        o->time = time;
        annihilated = false;
        return o;
      }
    }
  );

  return annihilated;
}

namespace {
  template<int charge>
  void arrive_near(int32_t cd_ix, stamped_event se) {
    sim_state &sim_me = ::sim_me;
    cd_state *cd = &sim_me.cds[cd_ix];
    
    se.e->existence += charge;
    
    switch(charge) {
    case +1:
      // positive events go in future regardless of that requires rollback (handled later)
      if(se.e->existence == 1) {
        se.e->future_not_past = true;
        cd->future_events.insert(se);
        sim_me.cds_by_now.decreased({cd, cd->now_after_future_insert()});
      }
      break;
      
    case -1:
      // negative events do not go into future/past
      if(se.e->existence == 0) {
        if(se.e->future_not_past) {
          cd->future_events.erase(se);
          sim_me.cds_by_now.increased({cd, cd->now()});
        }
        else
          remove_past(cd, se);
      }
      break;
    }
  }
}

namespace {
  void insert_past(cd_state *cd, stamped_event ins) {
    int n = cd->past_events.size();
    cd->past_events.push_back({});

    int j = 0;
    while(j < n) {
      stamped_event se = cd->past_events.at_backwards(j+1);
      if(se <= ins)
        break;
      cd->past_events.at_backwards(j) = se;
      j += 1;
    }
    
    cd->past_events.at_backwards(j) = ins;

    sim_me.cds_by_dawn.decreased({cd, cd->dawn_after_past_insert()});
    
    if(j != 0)
      rollback(cd, j);
  }

  void remove_past(cd_state *cd, stamped_event rem) {
    int j = 0;
    while(rem.e != cd->past_events.at_backwards(j).e)
      j += 1;
    
    rem.e->remove_after_undo = true;
    
    rollback(cd, j+1);
  }

  void rollback(cd_state *cd, int undo_n) {
    sim_state &sim_me = ::sim_me;
    const int rank_me = deva::rank_me();

    #if DRAIN_TIMER
      sim_me.drain_timer.update(DrainTimer::Cat::rollback);
    #endif // DRAIN_TIMER
    
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
        stamped_event se = cd->past_events.at_backwards(i++);
        event *e = se.e;
        
        { // walk far-sent events
          sent_far_record *far = e->sent_far_head;
          e->sent_far_head = nullptr;
          
          while(far != nullptr) {
            sent_far_record *far_next = far->next;
            int unseq;
            
            if(far->vtbl == &sent_far_one::the_vtbl)
              unseq = sent_far_one::the_send_anti_and_delete(far);
            else
              unseq = far->vtbl->send_anti_and_delete(far);
            
            cd->next_seq_id(-unseq);
            far = far_next;
          }
        }

        // walk near-sent events
        event *sent = e->sent_near_head;
        while(sent != nullptr) {
          cd->next_seq_id(-1);
          
          event *sent_next = sent->sent_near_next;
          stamped_event sent_se{sent, sent->time, sent->subtime};
          
          if(sent->target_rank != rank_me) { // event sent to near-remote
            // remove from sent_near
            sim_me.sent_near.erase(sent);
            
            // add to anni_near
            sent->anni_near_next = sim_me.anni_near_hot_head;
            sim_me.anni_near_hot_head = sent;
            
            // send anti-message
            int32_t target_cd = sent->target_cd;
            gvt::send(
              sent->target_rank, /*local=*/deva::ctrue3, sent->time,
              [=]() { arrive_near<-1>(target_cd, sent_se); }
            );
          }
          else { // sent event to myself
            cd_state *cd1 = &sim_me.cds[sent->target_cd];

            if(sent->future_not_past) {
              // can remove now, hasn't executed
              cd1->future_events.erase(sent_se);
              sim_me.cds_by_now.increased({cd1, cd1->now()});
              // add to deferred delete list
              sent->sent_near_next = del_head;
              del_head = sent;
            }
            else {
              sent->remove_after_undo = true;
              
              if(cd1->undo_n_hi == 0 || sent_se < cd1->past_events.at_backwards(cd1->undo_n_hi-1)) {
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
        stamped_event se = cd->past_events.at_backwards(i);
        bool do_remove = se.e->remove_after_undo;
        bool do_delete = do_remove && se.e->created_here;
        
        if(!do_remove) {
          se.e->future_not_past = true;
          cd->future_events.insert(se);
          inserted_future = true;
          // handled outside loop:
          // sim_me.cds_by_now.decreased({cd, cd->now_after_future_insert()});
        }

        event_context cxt;
        cxt.cd = cd->cd_ix;
        cxt.time = se.time;
        cxt.subtime = se.subtime;
        se.e->vtbl_on_target->unexecute(se.e, cxt, DEVA_DEBUG_ONLY(cd->checksummer,) do_delete);

        #if DRAIN_TIMER
          sim_me.drain_timer.rollback_event(cd->cd_ix);
        #endif // DRAIN_TIMER
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

    #if DRAIN_TIMER
      sim_me.drain_timer_update_spin_or(DrainTimer::Cat::progress);
    #endif // DRAIN_TIMER
  }
}

uint64_t pdes::drain(uint64_t t_end, bool rewindable) {
  sim_state &sim_me = ::sim_me;
  const int32_t rank_me = deva::rank_me();

  #if DRAIN_TIMER
    sim_me.drain_timer.init(sim_me.local_cd_n);
  #endif // DRAIN_TIMER

  //////////////////////////////////////////////////////////////////////////////

  DEVA_ASSERT_ALWAYS(
    !sim_me.has_rewind && sim_me.rewind_roots.empty() && sim_me.rewind_created_near.empty(),
    "Lingering rewind state must be rewound via pdes::rewind(true|false) before calling pdes::drain()."
  );

  if(rewindable) {
    sim_me.has_rewind = true;
    
    for(int32_t cd_ix=0; cd_ix < sim_me.local_cd_n; cd_ix++) {
      cd_state *cd = &sim_me.cds[cd_ix];
      cd->seq_id_bumper_rewind = cd->seq_id_bumper;
      cd->rewind_commit_t = cd->last_commit_t;
      
      // Anything in future upon entry to drain is a root. Since might be rewinding
      // mark them as non-deletable.
      int n = cd->future_events.size();
      for(int i=0; i < n; i++) {
        stamped_event se = cd->future_events.at(i);
        se.e->rewind_root = true; // prevents deleting rewind roots
        sim_me.rewind_roots.push_back(se.e);
      }

      for(fridge *f=cd->fridge_head; f != nullptr; f = f->next)
        f->capture();
    }
    
    { // also prevents deleting rewind roots
      int n = sim_me.sent_near.size();
      for(int i=0; i < n; i++) {
        event *e = sim_me.sent_near.at(i);
        sim_me.rewind_created_near.push_back(e);
      }
      sim_me.sent_near.clear();
    }
  }

  //////////////////////////////////////////////////////////////////////////////

  uint64_t gvt_returned;
  
  uint64_t executed_n = 0;
  uint64_t committed_n = 0;
  
  thread_local global_status_state the_global_status;
  global_status_state &global_status = the_global_status;
  global_status.reset();

  gvt::reducibles rxs_acc = {0,0};
  uint64_t look_t_ub;
  {
    uint64_t lvt = sim_me.cds_by_now.least_key();
    uint64_t gvt0 = deva::reduce_min(lvt);
    
    gvt::init(gvt0, {0, 0});
    gvt::coll_begin(lvt, {0, 0});

    look_t_ub = global_status.calc_look_t_ub(gvt0, t_end);
  }

  #if DRAIN_TIMER
    sim_me.spinning_empty = false;
    sim_me.spinning_look = false;
  #else
    bool spinning = false;
  #endif // DRAIN_TIMER
  
  while(true) {
    #if DRAIN_TIMER
      sim_me.drain_timer_update_spin_or(DrainTimer::Cat::progress);
      deva::progress(sim_me.spinning());
    #else
      deva::progress(spinning);
    #endif // DRAIN_TIMER
    
    { // nurse gvt
      #if DRAIN_TIMER
        sim_me.drain_timer_update_spin_or(DrainTimer::Cat::gvt);
      #endif // DRAIN_TIMER

      uint64_t lvt = sim_me.cds_by_now.least_key();
      uint64_t gvt_old = gvt::epoch_gvt();

      gvt::advance();
      
      if(gvt::coll_ended()) {
        //if(deva::rank_me()==0) deva::say()<<"gvt="<<gvt_old<<" coll";
        rxs_acc.reduce_with(gvt::coll_reducibles());

        // delete near-sent events which were committed in previous collective round
        while(sim_me.sent_near.least_key_or(uint64_t(-1)) < gvt_old) {
          event *e = sim_me.sent_near.pop_least();
          e->vtbl_on_creator->destruct_and_delete(e);
        }
        
        if(gvt::coll_was_epoch()) {
          uint64_t gvt_new = gvt::epoch_gvt();
          
          { // and delete annihilated events from previous epoch
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
                stamped_event se = cd->past_events.at_forwards(commit_n);
                if(se.time >= gvt_new)
                  break;
                
                auto current_t = std::make_pair(se.time+1, se.subtime);
                DEVA_ASSERT(cd->last_commit_t <= current_t);
                sim_me.stats.deterministic &= cd->last_commit_t < current_t;
                cd->last_commit_t = current_t;
                
                bool should_delete = se.e->created_here && !se.e->rewind_root;
                
                if(should_delete) {
                  if(se.e->far_next != reinterpret_cast<event_on_creator*>(0x1)) {
                    //deva::say()<<"committed from_far remove origin="<<se.e->far_origin<<" id="<<se.e->far_id;
                    sim_me.from_far.remove(se.e);
                  }
                }
                
                { // invoke commit()
                  #if DRAIN_TIMER
                    sim_me.drain_timer.update(DrainTimer::Cat::commit);
                  #endif // DRAIN_TIMER

                  event_context cxt;
                  cxt.cd = cd->cd_ix;
                  cxt.time = se.time;
                  cxt.subtime = se.subtime;
                  #if TIMELINE
                    sim_me.timeline.record_event(cxt.cd, cxt.time, se.e->gen_rank, se.e->gen_cd, se.e->gen_time);
                  #endif
                  se.e->vtbl_on_target->commit(se.e, cxt, should_delete);

                  #if DRAIN_TIMER
                    sim_me.drain_timer.commit_event(cd->cd_ix);
                    sim_me.drain_timer_update_spin_or(DrainTimer::Cat::gvt);
                  #endif // DRAIN_TIMER
                }
                commit_n += 1;
              }
              
              if(commit_n == 0)
                break;
              
              committed_n += commit_n;
              sim_me.stats.committed_n += commit_n;
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
        }
        
        // begin new collective
        gvt::coll_begin(lvt, {executed_n, committed_n});
        executed_n = 0;
        committed_n = 0;
      }

      #if DRAIN_TIMER
        sim_me.drain_timer_update_spin_or(DrainTimer::Cat::none);
      #endif // DRAIN_TIMER
    }
    
    { // execute one event
      cd_state *cd = sim_me.cds_by_now.peek_least().cd;
      stamped_event se = cd->future_events.peek_least_or({nullptr, end_of_time, end_of_time});

      #if DRAIN_TIMER
        sim_me.spinning_empty = cd->future_events.size() == 0;
        sim_me.spinning_look  = !sim_me.spinning_empty && se.time >= look_t_ub;
      #else
        spinning = true;
      #endif // DRAIN_TIMER

      if(se.time < look_t_ub) {
        #if DRAIN_TIMER
          DEVA_ASSERT(!sim_me.spinning_empty && !sim_me.spinning_look);
          sim_me.drain_timer.update(DrainTimer::Cat::execute);
        #else
          spinning = false;
        #endif // DRAIN_TIMER
        
        DEVA_ASSERT(se.e->future_not_past);
        se.e->future_not_past = false;
        
        cd->future_events.pop_least();
        sim_me.cds_by_now.increased({cd, cd->now()});
        
        insert_past(cd, se);

        event *sent_near; {
          #if DEBUG
          se.e->entry_checksum = cd->checksummer ? cd->checksummer() : 0;
          #endif
          
          #if DEVA_DUMMY_EXEC
          execute_context cxt_dummy;
          cxt_dummy.cd = cd->cd_ix;
          cxt_dummy.time = se.time;
          cxt_dummy.subtime = se.subtime;
          cxt_dummy.dummy = true;
          se.e->vtbl_on_target->execute(se.e, cxt_dummy);
          se.e->vtbl_on_target->unexecute(se.e, cxt_dummy, DEVA_DEBUG_ONLY(cd->checksummer,) false);
          #endif

          execute_context_impl cxt;
          cxt.cd = cd->cd_ix;
          cxt.time = se.time;
          cxt.subtime = se.subtime;
          
          se.e->vtbl_on_target->execute(se.e, cxt);
          
          se.e->sent_near_head = cxt.sent_near_head;
          sent_near = cxt.sent_near_head;
          se.e->sent_far_head = cxt.sent_far_head;
          
          executed_n += 1;
          sim_me.stats.executed_n += 1;
        }
        
        { // walk the `sent_near` list of the event's execution
          event *sent = sent_near;
          while(sent != nullptr) {
            stamped_event sent_se{sent, sent->time, sent->subtime};
            int32_t sent_cd_ix = sent->target_cd;
            
            if(sent->target_rank != rank_me) {
              sent->created_here = false;
              sent->rewind_root = false;
              sent->existence = 0;
              sent->future_not_past = false; // garbage would be fine
              sent->remove_after_undo = false;
              
              gvt::send(
                sent->target_rank, /*local=*/deva::ctrue3, sent->time,
                [=]() { arrive_near<+1>(sent_cd_ix, sent_se); }
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
              sent_cd->future_events.insert(sent_se);
              sim_me.cds_by_now.decreased({sent_cd, sent_cd->now_after_future_insert()});
            }
            
            sent = sent->sent_near_next;
          }
        }

        #if DRAIN_TIMER
          auto wall_time = sim_me.drain_timer.epoch_begin; // when event began
          auto dt = sim_me.drain_timer.update(DrainTimer::Cat::none, true);
          sim_me.drain_timer.register_event(cd->cd_ix, se.time, wall_time, dt);
        #endif // DRAIN_TIMER
      }
    }
  }

drain_completed:
  #if DRAIN_TIMER
    sim_me.drain_timer.update(DrainTimer::Cat::none);
  #endif //DRAIN_TIMER

  for(int cd_ix=0; cd_ix < sim_me.local_cd_n; cd_ix++) {
    cd_state *cd = &sim_me.cds[cd_ix];
    DEVA_ASSERT_ALWAYS(cd->future_events.size() == 0);
  }
  
  DEVA_ASSERT_ALWAYS(sim_me.from_far.size() == 0);
  DEVA_ASSERT_ALWAYS(sim_me.sent_near.size() == 0, "sim_me.sent_near.size() = "<<sim_me.sent_near.size()<<", expected 0");
  
drain_paused:
  #if DRAIN_TIMER
    sim_me.drain_timer.update(DrainTimer::Cat::none);
  #endif // DRAIN_TIMER

  for(int cd_ix=0; cd_ix < sim_me.local_cd_n; cd_ix++) {
    cd_state *cd = &sim_me.cds[cd_ix];
    DEVA_ASSERT_ALWAYS(cd->past_events.size() == 0);
  }
  
  sim_me.from_far.for_each([&](event_on_creator *o) {
    DEVA_ASSERT_ALWAYS(o->vtbl_on_creator != &anti_vtable, "Lingering from-far anti-message: id="<<o->far_id<<" o="<<o);

    // since we're quiesced we can pretend that events which came from afar didn't
    // since no anti-messages are in the pipes
    o->far_next = reinterpret_cast<event_on_creator*>(0x1);
  });
  sim_me.from_far.clear();

  DEVA_ASSERT_ALWAYS(sim_me.anni_near_cold_head == nullptr);
  DEVA_ASSERT_ALWAYS(sim_me.anni_near_hot_head == nullptr);

  #if DEBUG
  {
    int64_t n = 0;
    for(int32_t cd_ix=0; cd_ix < sim_me.local_cd_n; cd_ix++) {
      cd_state *cd = &sim_me.cds[cd_ix];
      n += cd->future_events.size();
    }
    
    for(event *e: sim_me.rewind_roots) {
      if(!e->future_not_past)
        n += 1;
    }

    n -= live_event_balance;
    n = deva::reduce_sum(n);
    
    DEVA_ASSERT(n == 0, "Events have been leaked!");
  }
  #endif
  
  return gvt_returned;
}

void pdes::finalize() {
  deva::barrier();
  
  for(int cd_ix=0; cd_ix < sim_me.local_cd_n; cd_ix++) {
    cd_state *cd = &sim_me.cds[cd_ix];
    DEVA_ASSERT(cd->past_events.size() == 0);
    
    fridge *f = cd->fridge_head;
    while(f != nullptr) {
      fridge *f1 = f->next;
      if(sim_me.has_rewind)
        f->discard();
      delete f;
      f = f1;
    }
    
    { // delete locally created non-roots that were sent here
      int n = cd->future_events.size();
      for(int i=0; i < n; i++) {
        stamped_event se = cd->future_events.at(i);
        if(se.e->created_here && !se.e->rewind_root)
          se.e->vtbl_on_target->destruct_and_delete(se.e);
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
  sim_me.has_rewind = false;
  
  sim_me.cds_by_dawn.clear();
  sim_me.cds_by_now.clear();
  sim_me.cds.reset();
  sim_me.local_cd_n = 0;

  #if DEBUG
    live_event_balance = deva::reduce_sum(live_event_balance);
    DEVA_ASSERT(live_event_balance == 0, "Events have been leaked!");
  #endif

  #if DRAIN_TIMER
    if (os_env<bool>("dump_drain_timer", false)) {
      // dump per-interval stats
      sim_me.drain_timer.dump();
    }
  #endif // DRAIN_TIMER

  #if TIMELINE
    if (os_env<bool>("dump_timeline", false)) {
      // dump event timelines
      ostringstream oss;
      oss << "timeline." << deva::rank_me() << ".out";
      ofstream ofs(oss.str());
      ofs << sim_me.timeline;
    }
  #endif
}

void pdes::rewind(bool do_rewind) {
  DEVA_ASSERT(sim_me.has_rewind);
  sim_me.has_rewind = false;
  
  deva::barrier();

  auto &sim_me = ::sim_me;

  if(do_rewind) {
    for(int cd_ix=0; cd_ix < sim_me.local_cd_n; cd_ix++) {
      cd_state *cd = &sim_me.cds[cd_ix];
      cd->last_commit_t = cd->rewind_commit_t;
      cd->seq_id_bumper = cd->seq_id_bumper_rewind;
      
      { // restore user state from fridge
        fridge *f = cd->fridge_head;
        while(f != nullptr) {
          f->restore();
          f = f->next;
        }
      }
      
      { // delete locally created non-roots that were sent here
        int n = cd->future_events.size();
        for(int i=0; i < n; i++) {
          stamped_event se = cd->future_events.at(i);
          if(se.e->created_here && !se.e->rewind_root)
            se.e->vtbl_on_target->destruct_and_delete(se.e);
        }
        cd->future_events.clear();
      }
    }

    // ensure everyone is done inspecting metadata of future events before we delete them below
    deva::barrier();

    { // delete locally created non-roots that were sent away
      int n = sim_me.sent_near.size();
      for(int i=0; i < n; i++) {
        event *e = sim_me.sent_near.at(i);
        e->vtbl_on_creator->destruct_and_delete(e);
      }
      sim_me.sent_near.clear();
    }

    // move the rewind roots into future
    for(event *e: sim_me.rewind_roots) {
      cd_state *cd = &sim_me.cds[e->target_cd];
      if(!e->future_not_past)
        sent_far_record::delete_list(e->sent_far_head);
      e->rewind_root = false;
      e->future_not_past = true;
      cd->future_events.insert({e, e->time, e->subtime});
    }
    for(event *e: sim_me.rewind_created_near)
      sim_me.sent_near.insert(e);
    sim_me.rewind_roots.clear();
    sim_me.rewind_created_near.clear();
    
    for(int cd_ix=0; cd_ix < sim_me.local_cd_n; cd_ix++) {
      cd_state *cd = &sim_me.cds[cd_ix];
      sim_me.cds_by_now.changed({cd, cd->now()});
    }
  }
  else {
    for(int cd_ix=0; cd_ix < sim_me.local_cd_n; cd_ix++) {
      cd_state *cd = &sim_me.cds[cd_ix];

      { // discard fridged user state
        fridge *f = cd->fridge_head;
        while(f != nullptr) {
          f->discard();
          f = f->next;
        }
      }
    }
    
    for(event *e: sim_me.rewind_roots) {
      if(e->created_here && !e->future_not_past)
        e->vtbl_on_creator->destruct_and_delete(e);
    }
    sim_me.rewind_roots.clear();

    // ensure everyone is done inspecting metadata of events before we delete them below
    deva::barrier();
    
    for(event *e: sim_me.rewind_created_near) {
      if(!e->future_not_past)
        e->vtbl_on_creator->destruct_and_delete(e);
    }
    sim_me.rewind_created_near.clear();
  }
}
