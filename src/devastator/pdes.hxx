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
  
  /* drain: Collective wrt all arguments. Advances the simulation by processing
   * all events with a timestamp strictly less than `t_end`. If `rewindable=true`
   * then `pdes::rewind(true|false)` must be called afterwards to indicate
   * whether or not to rewind to the state as it was upon entering drain. If
   * `rewindable=false`, then all events will be reaped immediately after being
   * committed.
   */
  uint64_t drain(std::uint64_t t_end = ~std::uint64_t(0), bool rewindable=false);

  /* rewind: Collective wrt all arguments. After a call to `pdes::drain(rewindable=true)`,
   * this must be called to either:
   *  do_rewind=false: invoke `reap` for all events committed last drain.
   *  do_rewind=true: revert state via unexecute's as it was upon entering last drain.
   */
  void rewind(bool do_rewind);

  /* finalize: Destroy all unprocessed events and reset pdes state to requiring
   * `pdes::init()` to be called again (if desired).
   */
  void finalize();

  // root_event: Insert an event into a CD on this rank.
  template<typename Event>
  void root_event(int cd, std::uint64_t time, Event e);

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

    struct fridged_event {
      fridged_event *next;
      virtual void unexecute_and_delete() = 0;
      virtual void reap_and_delete() = 0;
    };

    struct event_vtable {
      //void(*destruct_and_delete)(event *head, event* event::*next_of);
      void(*destruct_and_delete)(event*);
      void(*execute)(event *me, execute_context &cxt);
      void(*unexecute)(event *me);
      void(*commit)(event *me);
      fridged_event*(*refrigerate)(event *me, bool root);
      void(*reap)(event *me);
      #if 0
      event_vtable *all_next;
      event *del_head;
      event *anni_cold_head, *anni_hot_head;
      #endif
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
      int sent_near_ix = -1;
      event_on_creator *far_next = reinterpret_cast<event_on_creator*>(0x1); // 0x1 == not from far
      union {
        event *sent_near_next = nullptr;
        event *anni_near_next; // annihilated list next pointer
        event *del_next;
      };
      
      static event_on_creator*& far_next_of(event_on_creator *me) {
        return me->far_next;
      }
    };
    
    struct /*alignas(64)*/ event_on_target {
      event_vtable const *vtbl_on_target;
      event *sent_near_head = nullptr;
      std::forward_list<far_event_id> sent_far;
      std::int16_t created_here:1, // bool
                   rewind_root:1, // bool
                   existence:2, // -1,0,+1
                   future_not_past:1, // bool
                   remove_after_undo:1; // bool
      int future_ix;

      event_on_target():
        existence(0),
        remove_after_undo(false) {
      }
    };

    struct event: event_on_creator, event_on_target {
      static thread_local unsigned far_id_bump;
      
      #if DEBUG
        static thread_local int64_t live_n;
      #endif
      
      event(event_vtable const *vtbl) {
        this->vtbl_on_creator = vtbl;
        this->vtbl_on_target = vtbl;
        
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

      static int& sent_near_ix_of(event *e) {
        return e->sent_near_ix;
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
      void operator()(E &e, ExecRet &r) const {
        r.unexecute(e);
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
      void operator()(E &e, ExecRet &r) const {
        r(e);
      }
    };
    
    template<typename E, typename ExecRet,
             typename HasCommit = void>
    struct event_commit_dispatch {
      void operator()(E&, ExecRet&) const {}
    };
    
    template<typename E, typename ExecRet>
    struct event_commit_dispatch<E, ExecRet,
        /*HasCommit*/decltype(
          std::declval<ExecRet>().commit(std::declval<E&>()),
          void()
        )
      > {
      void operator()(E &e, ExecRet &r) const {
        r.commit(e);
      }
    };

    template<typename E, typename ExecRet,
             typename HasReap = void>
    struct event_reap_dispatch {
      void operator()(E&, ExecRet&) const {}
    };
    
    template<typename E, typename ExecRet>
    struct event_reap_dispatch<E, ExecRet,
        /*HasReap*/decltype(
          std::declval<ExecRet>().reap(std::declval<E&>()),
          void()
        )
      > {
      void operator()(E &e, ExecRet &r) const {
        r.reap(e);
      }
    };

    template<typename E, typename ExecRet>
    struct fridged_nonroot_impl final: fridged_event {
      E e;
      ExecRet r;
      
      fridged_nonroot_impl(E e, ExecRet r):
        e(std::move(e)),
        r(std::move(r)) {
      }

      void unexecute_and_delete() {
        event_unexecute_dispatch<E,ExecRet>()(e, r);
        delete this;
      }
      
      void reap_and_delete() {
        event_reap_dispatch<E,ExecRet>()(e, r);
        delete this;
      }
    };

    template<typename E, typename ExecRet>
    struct fridged_root_impl final: fridged_event {
      event_impl<E> *e;
      
      fridged_root_impl(event_impl<E> *e): e(e) {}
      
      void unexecute_and_delete() {
        event_unexecute_dispatch<E,ExecRet>()(e->user, e->exec_ret);
        e->exec_ret.~ExecRet();
        delete this;
      }
      
      void reap_and_delete() {
        event_reap_dispatch<E,ExecRet>()(e->user, e->exec_ret);
        e->exec_ret.~ExecRet();
        delete this;
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
      
      static void destruct_and_delete(event *me1) {
        auto *me = static_cast<event_impl<E>*>(me1);
        delete me;
      }
      
      static void execute(event *me1, execute_context &cxt) {
        auto *me = static_cast<event_impl<E>*>(me1);
        ::new(&me->exec_ret) ExecRet{me->user.execute(cxt)};
      }
      
      static void unexecute(event *me1) {
        auto *me = static_cast<event_impl<E>*>(me1);
        event_unexecute_dispatch<E, ExecRet>()(me->user, me->exec_ret);
        me->exec_ret.~ExecRet();
      }
      
      static void commit(event *me1) {
        auto *me = static_cast<event_impl<E>*>(me1);
        event_commit_dispatch<E, ExecRet>()(me->user, me->exec_ret);
      }
      
      static fridged_event* refrigerate(event *me1, bool root) {
        auto *me = static_cast<event_impl<E>*>(me1);
        
        fridged_event *ans;
        if(root)
          ans = new fridged_root_impl<E,ExecRet>(me);
        else {
          ans = new fridged_nonroot_impl<E,ExecRet>(std::move(me->user), std::move(me->exec_ret));
          me->exec_ret.~ExecRet();
        }
        
        return ans;
      }
      
      static void reap(event *me1) {
        auto *me = static_cast<event_impl<E>*>(me1);
        event_reap_dispatch<E, ExecRet>()(me->user, me->exec_ret);
        me->exec_ret.~ExecRet();
      }
      
      static constexpr event_vtable the_vtable = {
        &event_impl<E>::destruct_and_delete,
        &event_impl<E>::execute,
        &event_impl<E>::unexecute,
        &event_impl<E>::commit,
        &event_impl<E>::refrigerate,
        &event_impl<E>::reap
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
      
      gvt::send(rank, /*local=*/deva::cfalse3, time,
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
    detail::root_event(cd_ix, e);
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
  }
} // namespace pdes
} // namespace deva
#endif
