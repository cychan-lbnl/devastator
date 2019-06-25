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

namespace deva {
namespace pdes {
  struct event_context {
    std::int32_t cd;
    std::uint64_t time, subtime;
  };
  struct execute_context: event_context {
    template<typename Event>
    void send(std::int32_t rank, std::int32_t cd, std::uint64_t time, Event e);
  };

  // Set these to determine how frequently and where drain should print global
  // statistics such as gvt and efficiency.
  extern int chitter_secs; // non-positive disables chitter io
  extern std::ostream *chitter_io;
  
  void init(std::int32_t cds_this_rank);

  template<typename T>
  void register_state(std::int32_t cd, T *address);
  
  /* drain: Collective wrt all arguments. Advances the simulation by processing
   * all events with a timestamp strictly less than `t_end`. If `rewindable=true`
   * then `pdes::rewind(true|false)` must be called afterwards to indicate
   * whether or not to rewind to the state as it was upon entering drain.
   */
  uint64_t drain(std::uint64_t t_end = ~std::uint64_t(0), bool rewindable=false);

  /* rewind: Collective wrt all arguments. After a call to `pdes::drain(rewindable=true)`,
   * this must be called to either:
   *  do_rewind=false: no-op.
   *  do_rewind=true: revert state via unexecute's as it was upon entering last drain.
   */
  void rewind(bool do_rewind);

  /* finalize: Destroy all unprocessed events and reset pdes state to requiring
   * `pdes::init()` to be called again (if desired).
   */
  void finalize();

  // root_event: Insert an event into a CD on this rank.
  template<typename Event>
  void root_event(std::int32_t cd, std::uint64_t time, Event e);

  struct statistics {
    std::uint64_t executed_n = 0;
    std::uint64_t committed_n = 0;
    bool deterministic = true;
    
    statistics& operator+=(statistics x) {
      this->executed_n += x.executed_n;
      this->committed_n += x.committed_n;
      this->deterministic &= x.deterministic;
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
    constexpr std::uint64_t end_of_time = std::uint64_t(-1);
    
    struct far_event_id {
      std::int32_t rank;
      std::uint32_t id;
      std::uint64_t time;
    };

    struct event_on_creator;
    struct event_on_target;
    struct event;

    extern __thread std::uint32_t far_id_bump;
    
    void root_event(std::int32_t cd_ix, event *e);
    std::uint64_t next_seq_id(std::int32_t cd_ix);
    void arrive_far(std::int32_t origin, std::uint32_t far_id, event *e);
    
    struct event_vtable {
      void(*destruct_and_delete)(event*);
      void(*execute)(event *me, execute_context &cxt);
      void(*unexecute)(event *me, event_context cxt, bool should_delete);
      void(*commit)(event *me, event_context cxt, bool should_delete);
    };

    struct alignas(64) event_on_creator {
      event_vtable const *vtbl_on_creator;
      std::uint64_t time;
      std::uint64_t subtime;
      union {
        std::int32_t target_rank;
        std::int32_t far_origin;
      };
      std::int32_t target_cd;
      std::uint32_t far_id;
      std::int32_t sent_near_ix = -1;
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
      std::int32_t future_ix;
      
      event_on_target():
        existence(0),
        remove_after_undo(false) {
      }
    };

    #if DEBUG
    extern __thread std::int64_t live_event_balance; // num(construct) - num(destructed)
    #endif

    struct event: event_on_creator, event_on_target {
      event(event_vtable const *vtbl) {
        this->vtbl_on_creator = vtbl;
        this->vtbl_on_target = vtbl;
        
        #if DEBUG
          live_event_balance += 1;
        #endif
      }
      
      #if DEBUG
        ~event() { live_event_balance -= 1; }
      #endif

      static std::uint64_t time_of(event *e) {
        return e->time;
      }

      static std::int32_t& sent_near_ix_of(event *e) {
        return e->sent_near_ix;
      }
    };

    struct execute_context_impl: execute_context {
      event *sent_near_head = nullptr;
      std::forward_list<far_event_id> *sent_far;
    };
    
    template<typename E, typename=void>
    struct event_has_commit: std::false_type {};
    
    template<typename E>
    struct event_has_commit<E, decltype(std::declval<E&>().commit(), void())>:
      std::true_type {
    };

    template<typename E, typename HasSubtime=void>
    struct event_subtime {
      std::uint64_t operator()(std::int32_t cd, E &user) const {
        return detail::next_seq_id(cd);
      }
    };
    template<typename E>
    struct event_subtime<E,
        /*HasSubtime*/decltype(std::declval<E>().subtime(), void())
      > {
      std::uint64_t operator()(std::int32_t cd, E &user) {
        return user.subtime();
      }
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
          std::declval<ExecRet>().unexecute(
            std::declval<event_context&>(),
            std::declval<E&>()
          ),
          void()
        ),
        /*HasOpParens*/void
      > {
      void operator()(event_context &cxt, E &user, ExecRet &exec_ret) const {
        exec_ret.unexecute(cxt, user);
      }
    };
    template<typename E, typename ExecRet>
    struct event_unexecute_dispatch<E, ExecRet,
        /*HasUnexecute*/void, 
        /*HasOpParens*/decltype(
          std::declval<ExecRet>().operator()(
            std::declval<event_context&>(),
            std::declval<E&>()
          ),
          void()
        )
      > {
      void operator()(event_context &cxt, E &user, ExecRet &exec_ret) const {
        exec_ret(cxt, user);
      }
    };
    
    template<typename E, typename ExecRet,
             typename HasCommit = void>
    struct event_commit_dispatch {
      void operator()(event_context&, E&, ExecRet&) const {}
    };
    
    template<typename E, typename ExecRet>
    struct event_commit_dispatch<E, ExecRet,
        /*HasCommit*/decltype(
          std::declval<ExecRet>().commit(
            std::declval<event_context&>(),
            std::declval<E&>()
          ),
          void()
        )
      > {
      void operator()(event_context &cxt, E &user, ExecRet &exec_ret) const {
        exec_ret.commit(cxt, user);
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
      
      static void unexecute(event *me1, event_context cxt, bool should_delete) {
        auto *me = static_cast<event_impl<E>*>(me1);
        event_unexecute_dispatch<E, ExecRet>()(cxt, me->user, me->exec_ret);
        me->exec_ret.~ExecRet();

        if(should_delete)
          delete me;
      }
      
      static void commit(event *me1, event_context cxt, bool should_delete) {
        auto *me = static_cast<event_impl<E>*>(me1);
        event_commit_dispatch<E, ExecRet>()(cxt, me->user, me->exec_ret);
        me->exec_ret.~ExecRet();
        
        if(should_delete)
          delete me;
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

      ~event_impl() {}
    };

    template<typename E>
    constexpr event_vtable event_impl<E>::the_vtable;
  }
  
  //////////////////////////////////////////////////////////////////////////////
  
  template<typename Event>
  void execute_context::send(
      int32_t rank, int32_t cd,
      std::uint64_t time,
      Event user
    ) {
    auto *me = static_cast<detail::execute_context_impl*>(this);
    std::uint64_t subtime = detail::event_subtime<Event>()(this->cd, user);
    
    DEVA_ASSERT(
      std::make_pair(me->time, me->subtime) <= std::make_pair(time, subtime),
      "Sent events must have a greater-or-equal (time,subtime) combo. If equal,"
      "then determinacy of committed events is forfeit. In the case of "
      "non-user-provided subtime, this restriction implies that time components "
      "must be strictly increasing."
    );
    
    if(deva::rank_is_local(rank)) {
      auto *e = new detail::event_impl<Event>{std::move(user)};
      e->target_rank = rank;
      e->target_cd = cd;
      e->time = time;
      e->subtime = subtime;
      e->sent_near_next = me->sent_near_head;
      me->sent_near_head = e;
    }
    else {
      std::int32_t origin = deva::rank_me();
      std::uint32_t far_id = detail::far_id_bump++;
      
      gvt::send(rank, /*local=*/deva::cfalse3, time,
        [=](Event &user) {
          auto *e = new detail::event_impl<Event>{std::move(user)};
          e->far_origin = origin;
          e->far_id = far_id;
          e->target_cd = cd;
          e->time = time;
          e->subtime = subtime;
          
          detail::arrive_far(origin, far_id, e);
        },
        std::move(user)
      );

      me->sent_far->push_front({rank, far_id, time});
    }
  }
  
  //////////////////////////////////////////////////////////////////////////////

  namespace detail {
    struct fridge {
      fridge *next;
      virtual ~fridge() = default;
      virtual void capture() = 0;
      virtual void restore() = 0;
      virtual void discard() = 0;
    };

    template<typename T>
    struct fridge_impl final: fridge {
      T *user;
      union { T backup; };
      
      fridge_impl(T *user): user(user) {}

      void capture() {
        ::new(&backup) T(const_cast<T const&>(*user));
      }
      void restore() {
        *user = std::move(backup);
        backup.~T();
      }
      void discard() {
        backup.~T();
      }
    };

    void register_state(int cd, fridge *fr);
  }
  
  template<typename T>
  void register_state(int cd, T *address) {
    detail::register_state(cd, new detail::fridge_impl<T>(address));
  }

  //////////////////////////////////////////////////////////////////////////////
  
  template<typename Event>
  void root_event(int cd_ix, std::uint64_t time, Event user) {
    auto *e = new detail::event_impl<Event>{std::move(user)};
    e->target_rank = deva::rank_me();
    e->target_cd = cd_ix;
    e->time = time;
    e->subtime = detail::event_subtime<Event>()(cd_ix, e->user);
    detail::root_event(cd_ix, e);
  }
  
  //////////////////////////////////////////////////////////////////////////////
  
  namespace detail {
    // Wraps an event pointer and stores its time & subtime redundantly so consumers don't
    // have to reach in to the creator's cache line to see that info.
    struct stamped_event {
      event *e;
      std::uint64_t time;
      std::uint64_t subtime;
      
      static std::int32_t& future_ix_of(stamped_event se) {
        return se.e->future_ix;
      }
      static stamped_event identity(stamped_event e) {
        return e;
      }

      constexpr bool definitely_ordered_wrt(stamped_event that) const {
        return this->time != that.time || this->subtime != that.subtime;
      }
      
      constexpr friend bool operator==(stamped_event a, stamped_event b) {
        return (a.time == b.time) &
               (a.subtime == b.subtime);
      }
      constexpr friend bool operator!=(stamped_event a, stamped_event b) {
        return !(a == b);
      }
      constexpr friend bool operator<(stamped_event a, stamped_event b) {
        bool ans = a.subtime < b.subtime;
        ans &= a.time == b.time;
        ans |= a.time < b.time;
        return ans;
      }
      constexpr friend bool operator>(stamped_event a, stamped_event b) {
        return b < a;
      }
      constexpr friend bool operator<=(stamped_event a, stamped_event b) {
        bool ans = a.subtime <= b.subtime;
        ans &= a.time == b.time;
        ans |= a.time < b.time;
        return ans;
      }
      constexpr friend bool operator>=(stamped_event a, stamped_event b) {
        return b <= a;
      }
    };
  }
} // namespace pdes
} // namespace deva
#endif
