#ifndef _409355b8303d41628b1284d487a6d766
#define _409355b8303d41628b1284d487a6d766

#include <devastator/gvt.hxx>
#include <devastator/world.hxx>
#include <devastator/intrusive_min_heap.hxx>
#include <devastator/queue.hxx>

#include <cstdint>
#include <forward_list>
#include <iostream>
#include <utility>

#define DEVA_PDES_CD_SPECIALIZE 1

namespace deva {
namespace pdes {
  struct execute_context {
    int cd;
    std::uint64_t time, subtime;
    
    template<typename Event>
    void send(int rank, int cd, std::uint64_t time, Event e);
  };

  // Set these to determine how frequently and where drain should print global
  // statistics such as gvt and efficiency.
  extern int chitter_secs; // non-positive disables chitter io
  extern std::ostream *chitter_io;
  
  void init(int cds_this_rank);
  
  /* init_cd: sets the behavior of the cd. `CdOps` must be a type like:
   *   struct CdOps {
   *     static bool commutes(event_view a, event_view b);
   *   };
   * 
   * A cd that hasn't been init'd with this call receives the default behaviors.
   * (where `commutes` always return false).
   */
  template<class CdOps>
  void init_cd(int cd_id);
  
  void drain();

  template<typename Event>
  void root_event(int cd_ix, std::uint64_t time, Event e);

  struct statistics {
    std::uint64_t executed_n = 0;
    std::uint64_t committed_n = 0;
    
    statistics& operator+=(statistics x) {
      this->executed_n += x.executed_n;
      this->committed_n += x.committed_n;
      return *this;
    }
  };
  
  // The non-reduced statistics for this rank pertaining to the last invocation
  // of `drain()`.
  statistics local_stats();
  
  // Use and reduce `local_stats` yourself please.
  [[deprecated]]
  std::pair<size_t, size_t> get_total_event_counts();

  //////////////////////////////////////////////////////////////////////////////
  // internal
  
  namespace detail {
    struct event;
    
    constexpr std::uint64_t end_of_time = std::uint64_t(-1);
  }
  
  //////////////////////////////////////////////////////////////////////////////
  // public
  
  class event_view {
    detail::event *e_;
    std::uint64_t time_;
  
  public:
    constexpr event_view(detail::event *e, std::uint64_t time):
      e_(e),
      time_(time) {
    }
  
  public:
    constexpr std::uint64_t time() const { return time_; }
    
    template<typename Event>
    Event* try_cast() const;
    
    void* type_id() const;
  };
  
  //////////////////////////////////////////////////////////////////////
  // internal
  
  namespace detail {
    struct far_event_id {
      int rank;
      unsigned id;
      std::uint64_t time;
    };

    struct event_on_creator;
    struct event_on_target;
    struct event;
    
    void root_event(int cd_ix, event *e);
    void arrive_far(int far_origin, unsigned far_id, event *e);
    
    struct event_vtable {
      void(*destruct_and_delete)(event *me);
      void(*execute)(event *me, execute_context &cxt);
      void(*unexecute)(event *me);
      void(*commit)(event *me);
    };

    struct alignas(64) event_on_creator {
      event_vtable const *vtbl_on_creator;
      std::uint64_t time;
      std::uint64_t subtime;
      union {
        int target_rank;
        int far_origin;
      };
      int target_cd;
      unsigned far_id;
      int remote_near_ix = -1;
      event *sent_near_next = nullptr;
      event_on_creator *far_next = reinterpret_cast<event_on_creator*>(0x1); // 0x1 == not from far

      static event_on_creator*& far_next_of(event_on_creator *me) {
        return me->far_next;
      }
    };
    
    struct alignas(64) event_on_target {
      event_vtable const *vtbl_on_target;
      event *sent_near_head = nullptr;
      std::forward_list<far_event_id> sent_far;
      int creator_rank; // rank which allocated this object (not necessarily sender of event)
      std::int8_t existence = 0; // -1,0,+1
      
      static constexpr int
        phase_future = 0, // event is in `cd_state::future_events` minheap
        phase_past = 1, // event is in `cd_state::past_events` queue
        phase_undo = 2, // event will be undone
        phase_undo_commute_done = 4, // event has been commute-tested with all successors
        phase_undo_remove = 8; // event needs to be deleted after unexecute
      std::uint8_t phase;
      
      bool local;
      
      union {
        int future_ix;
        event *undo_next;
      };
    };

    struct event: event_on_creator, event_on_target {
      static thread_local unsigned far_id_bump;
      
      #if DEBUG
        static thread_local int64_t live_n;
      #endif
      
      event(event_vtable const *vtbl) {
        this->vtbl_on_creator = vtbl;
        this->vtbl_on_target = vtbl;
        this->creator_rank = deva::rank_me();
        
        #if DEBUG
          live_n++;
        #endif
      }
      
      #if DEBUG
        ~event() { live_n--; }
      #endif

      static std::uint64_t time_of(event *e) {
        return e->time;
      }

      static int& remote_near_ix_of(event *e) {
        return e->remote_near_ix;
      }
    };

    struct execute_context_impl: execute_context {
      event *sent_near_head = nullptr;
      std::forward_list<far_event_id> *sent_far;
    };
    
    template<typename E, typename=void>
    struct event_subtime {
      std::uint64_t operator()(E&) const {
        return std::uint64_t(-1);
      }
    };
    template<typename E>
    struct event_subtime<E,
        decltype(
          std::declval<E&>().subtime(),
          void()
        )
      > {
      std::uint64_t operator()(E &e) const {
        return e.subtime();
      }
    };
    
    template<typename E, typename=void>
    struct event_has_commit: std::false_type {};
    
    template<typename E>
    struct event_has_commit<E, decltype(std::declval<E&>().commit(), void())>:
      std::true_type {
    };
    
    template<typename E>
    struct event_impl;
    
    template<typename E, typename ExecRet,
             typename HasUnexecute = void,
             typename HasOpParens = void>
    struct event_unexecute_dispatch;
    
    template<typename E, typename ExecRet>
    struct event_unexecute_dispatch<E, ExecRet,
        /*HasUnexecute*/decltype(
          std::declval<ExecRet>().unexecute(std::declval<E&>()),
          void()
        ),
        /*HasOpParens*/void
      > {
      void operator()(event_impl<E> *me) const {
        me->exec_ret.unexecute(me->user);
      }
    };
    template<typename E, typename ExecRet>
    struct event_unexecute_dispatch<E, ExecRet,
        /*HasUnexecute*/void, 
        /*HasOpParens*/decltype(
          std::declval<ExecRet>().operator()(std::declval<E&>()),
          void()
        )
      > {
      void operator()(event_impl<E> *me) const {
        me->exec_ret.operator()(me->user);
      }
    };
    
    template<typename E, typename ExecRet,
             typename HasCommit = void>
    struct event_commit_dispatch {
      void operator()(event_impl<E>*) const {}
    };
    
    template<typename E, typename ExecRet>
    struct event_commit_dispatch<E, ExecRet,
        /*HasCommit*/decltype(
          std::declval<ExecRet>().unexecute(std::declval<E&>()),
          void()
        )
      > {
      void operator()(event_impl<E> *me) const {
        me->exec_ret.commit(me->user);
      }
    };
    
    template<typename E>
    struct event_impl final: event {
      static_assert(
        !event_has_commit<E>::value,
        "`UserEvent::commit()` is deprecated. Make `commit(UserEvent&)` a member of `UserEvent::execute`'s return type."
      );
      
      using ExecRet = typename std::remove_const<decltype(std::declval<E>().execute(std::declval<execute_context&>()))>::type;
      E user;
      union { ExecRet exec_ret; };
      
      static void destruct_and_delete(event *me) {
        delete static_cast<event_impl<E>*>(me);
      }
      static void execute(event *me1, execute_context &cxt) {
        auto *me = static_cast<event_impl<E>*>(me1);
        new(&me->exec_ret) ExecRet{me->user.execute(cxt)};
      }
      static void unexecute(event *me1) {
        auto *me = static_cast<event_impl<E>*>(me1);
        event_unexecute_dispatch<E, ExecRet>()(me);
        me->exec_ret.~ExecRet();
      }
      static void commit(event *me1) {
        auto *me = static_cast<event_impl<E>*>(me1);
        event_commit_dispatch<E, ExecRet>()(me);
        me->exec_ret.~ExecRet();
      }
      
      static constexpr event_vtable the_vtable = {
        &event_impl<E>::destruct_and_delete,
        &event_impl<E>::execute,
        &event_impl<E>::unexecute,
        &event_impl<E>::commit
      };
      
      event_impl(E user):
        event(&the_vtable),
        user(std::move(user)) {
      }
    };

    template<typename E>
    constexpr event_vtable event_impl<E>::the_vtable;
  }
  
  //////////////////////////////////////////////////////////////////////////////
  
  template<typename Event>
  void execute_context::send(
      int rank, int cd,
      std::uint64_t time,
      Event user
    ) {
    auto *me = static_cast<detail::execute_context_impl*>(this);
    
    DEVA_ASSERT(me->time <= time);
    
    if(deva::rank_is_local(rank)) {
      auto *e = new detail::event_impl<Event>{std::move(user)};
      e->target_rank = rank;
      e->target_cd = cd;
      e->time = time;
      e->subtime = detail::event_subtime<Event>()(e->user);
      
      e->sent_near_next = me->sent_near_head;
      me->sent_near_head = e;
    }
    else {
      int origin = deva::rank_me();
      unsigned far_id = detail::event::far_id_bump++;
      
      gvt::send_remote(rank, time,
        [=](Event &user) {
          auto *e = new detail::event_impl<Event>{std::move(user)};
          e->far_origin = origin;
          e->far_id = far_id;
          e->target_cd = cd;
          e->time = time;
          e->subtime = detail::event_subtime<Event>()(e->user);
          
          detail::arrive_far(origin, far_id, e);
        },
        std::move(user)
      );

      me->sent_far->push_front({rank, far_id, time});
    }
  }
  
  //////////////////////////////////////////////////////////////////////////////
  
  template<typename Event>
  void root_event(int cd_ix, std::uint64_t time, Event user) {
    auto *e = new detail::event_impl<Event>{std::move(user)};
    e->target_rank = deva::rank_me();
    e->target_cd = cd_ix;
    e->time = time;
    e->subtime = detail::event_subtime<Event>()(e->user);
    root_event(cd_ix, e);
  }
  
  //////////////////////////////////////////////////////////////////////////////
  
  inline void* event_view::type_id() const {
    return (void*)e_->vtbl_on_target;
  }
  
  template<typename Event>
  Event* event_view::try_cast() const {
    return e_->vtbl_on_target == &detail::event_impl<Event>::the_vtable
      ? &static_cast<detail::event_impl<Event>*>(e_)->user
      : nullptr;
  }
  
  //////////////////////////////////////////////////////////////////////////////
  
  namespace detail {
    // Wraps an event pointer and stores its time & subtime redundantly so consumers don't
    // have to reach in to the creator's cache line to see that info.
    struct local_event {
      event *e;
      std::uint64_t time, subtime;
      
      static int& future_ix_of(local_event le) {
        return le.e->future_ix;
      }
      static local_event identity(local_event e) {
        return e;
      }
      
      constexpr friend bool operator==(local_event a, local_event b) {
        return (a.time == b.time) &
               (a.subtime == b.subtime) &
               (a.e == b.e);
      }
      constexpr friend bool operator!=(local_event a, local_event b) {
        return !(a == b);
      }
      constexpr friend bool operator<(local_event a, local_event b) {
        bool ans = reinterpret_cast<std::uintptr_t>(a.e) < reinterpret_cast<std::uintptr_t>(b.e);
        ans &= a.subtime == b.subtime;
        ans |= a.subtime < b.subtime;
        ans &= a.time == b.time;
        ans |= a.time < b.time;
        return ans;
      }
      constexpr friend bool operator>(local_event a, local_event b) {
        return b < a;
      }
      constexpr friend bool operator<=(local_event a, local_event b) {
        bool ans = reinterpret_cast<std::uintptr_t>(a.e) <= reinterpret_cast<std::uintptr_t>(b.e);
        ans &= a.subtime == b.subtime;
        ans |= a.subtime < b.subtime;
        ans &= a.time == b.time;
        ans |= a.time < b.time;
        return ans;
      }
      constexpr friend bool operator>=(local_event a, local_event b) {
        return b <= a;
      }
    };
    
    struct cd_state;
    
    struct cd_vtable {
      // readonly
      void(*insert_past)(cd_state*, local_event);
      void(*remove_past)(cd_state*, local_event);
      void(*close_undos_wrt_commute)(cd_state *head, event **undo_wrt_send);
      
      // thread_local mutable
      cd_vtable *undo_next;// = reinterpret_cast<cd_vtable*>(0x1);
      cd_state *undo_head;// = nullptr;
      
      template<class CdOps>
      static void the_insert_past(cd_state*, local_event);
      template<class CdOps>
      static void the_remove_past(cd_state*, local_event);
      template<class CdOps>
      static void the_close_undos_wrt_commute(cd_state *head, event **undo_wrt_send);
    };
    
    struct cd_ops_trivial {
      static bool commutes(event_view, event_view) {
        return false;
      }
    };
    
    template<class CdOps>
    struct the_cd_vtable {
      static thread_local cd_vtable the_vtable;
    };
    
    template<class CdOps>
    thread_local cd_vtable the_cd_vtable<CdOps>::the_vtable = {
      cd_vtable::the_insert_past<CdOps>,
      cd_vtable::the_remove_past<CdOps>,
      cd_vtable::the_close_undos_wrt_commute<CdOps>,
      /*undo_next*/reinterpret_cast<cd_vtable*>(0x1),
      /*undo_head*/nullptr
    };
    
    struct cd_state {
      cd_vtable *vtbl;
      
      deva::queue<local_event> past_events;
      
      deva::intrusive_min_heap<
          local_event, local_event,
          local_event::future_ix_of, local_event::identity>
        future_events;
      
      int cd_ix;
      int by_now_ix, by_dawn_ix;
      
      int undo_least = -1;
      local_event undo_least_wrt_commute = {nullptr, end_of_time, end_of_time};
      cd_state *undo_all_next = reinterpret_cast<cd_state*>(0x1);
      cd_state *undo_next = reinterpret_cast<cd_state*>(0x1);
      
      uint64_t now() const {
        return future_events.least_key_or({nullptr, end_of_time, end_of_time}).time;
      }
      uint64_t now_after_future_insert() const {
        return future_events.least_key().time;
      }
      uint64_t dawn() const {
        return past_events.front_or({nullptr, end_of_time, end_of_time}).time;
      }
    };
    
    extern thread_local cd_state *the_cds;
  }
  
  //////////////////////////////////////////////////////////////////////////////
  
  template<class CdOps>
  void init_cd(int cd) {
    detail::the_cds[cd].vtbl = &detail::the_cd_vtable<CdOps>::the_vtable;
  }
  
  //////////////////////////////////////////////////////////////////////////////
  
  namespace detail {
    cd_vtable* close_undos_wrt_send(event *undo_head);
    void rollback_closure(cd_state *cd_all_head, event *e_head);
    
    #if DEVA_PDES_CD_SPECIALIZE
    template<>
    void cd_vtable::the_insert_past<cd_ops_trivial>(cd_state *cd, local_event ins);
    
    template<>
    void cd_vtable::the_remove_past<cd_ops_trivial>(cd_state *cd, local_event rem);
    
    template<>
    void cd_vtable::the_close_undos_wrt_commute<cd_ops_trivial>(cd_state *cd_head, event **undo_fresh_head);
    #endif
    
    template<class CdOps>
    void cd_vtable::the_insert_past(cd_state *cd, local_event ins) {
      event *undo_head = nullptr;
      
      int n = cd->past_events.size();
      cd->past_events.push_back({});
      
      int i_next = -1;
      int j = 0;
      while(j < n) {
        local_event le = cd->past_events.at_backwards(j+1);
        cd->past_events.at_backwards(j) = le;
        
        if(le < ins)
          break;
        
        if(!CdOps::commutes(event_view(ins.e, ins.time),
                            event_view(le.e, le.time))
          ) {
          le.e->phase = event_on_target::phase_undo;
          
          le.e->undo_next = undo_head;
          undo_head = le.e;
          
          i_next = j;
          cd->undo_least = j;
        }
        
        j += 1;
      }
      
      cd->past_events.at_backwards(j) = ins;
      
      if(i_next != -1) {
        cd->undo_all_next = nullptr; // make cd head of "all" list
        cd->undo_next = nullptr;
        cd->undo_least_wrt_commute = cd->past_events.at_backwards(i_next);
        the_close_undos_wrt_commute<CdOps>(cd, &undo_head);
        rollback_closure(cd, undo_head);
      }
    }
    
    template<class CdOps>
    void cd_vtable::the_remove_past(cd_state *cd, local_event rem) {
      event *undo_head = nullptr;
      
      rem.e->phase = event_on_target::phase_undo | event_on_target::phase_undo_remove;
      
      rem.e->undo_next = undo_head;
      undo_head = rem.e;
      
      cd->undo_all_next = nullptr; // make cd head of "all" list
      cd->undo_next = nullptr;
      cd->undo_least_wrt_commute = rem;
      the_close_undos_wrt_commute<CdOps>(cd, &undo_head);
      rollback_closure(cd, undo_head);
    }
    
    template<class CdOps>
    void cd_vtable::the_close_undos_wrt_commute(cd_state *cd_head, event **undo_fresh_head) {
      cd_state *cd = cd_head;
      do {
        local_event ie = cd->undo_least_wrt_commute;
        cd->undo_least_wrt_commute = {nullptr, end_of_time, end_of_time};
        
        while(true) {
          int i_next = -1;
          ie.e->phase |= event_on_target::phase_undo_commute_done;
          
          for(int j=0; true; j++) {
            local_event je = cd->past_events.at_backwards(j);
            if(je.e == ie.e) {
              if(j > cd->undo_least)
                cd->undo_least = j;
              break;
            }
            
            if(je.e->phase == event_on_target::phase_past &&
               !CdOps::commutes(event_view(ie.e, ie.time),
                                event_view(je.e, je.time))
              ) {
              je.e->phase = event_on_target::phase_undo;
              
              // subscribe for send processing
              je.e->undo_next = *undo_fresh_head;
              *undo_fresh_head = je.e;
            }
            
            if(event_on_target::phase_undo == (je.e->phase & (event_on_target::phase_undo | event_on_target::phase_undo_commute_done)))
              i_next = j;
          }
          
          if(i_next == -1) break;
          ie = cd->past_events.at_backwards(i_next);
        }
        
        cd_state *cd_next = cd->undo_next;
        cd->undo_next = reinterpret_cast<cd_state*>(0x1);
        cd = cd_next;
      }
      while(cd != nullptr);
    }
  }
} // namespace pdes
} // namespace deva
#endif
