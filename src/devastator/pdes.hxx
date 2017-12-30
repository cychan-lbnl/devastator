#ifndef _409355b8303d41628b1284d487a6d766
#define _409355b8303d41628b1284d487a6d766

#include "world.hxx"

#include <cstdint>
#include <utility>

namespace pdes {
  struct execute_context {
    std::uint64_t time;
    std::uint64_t id;
    
    template<typename E>
    void send(int rank, int cd, std::uint64_t time, std::uint64_t id, E ev);
  };

  void init(int cds_this_rank);
  void drain();

  template<typename E>
  void root_event(int cd_ix, uint64_t time, uint64_t id, E ev);

  //////////////////////////////////////////////////////////////////////
  // internal

  using event_tid = std::pair<std::uint64_t, std::uint64_t>;

  struct event;
  
  struct event_vtable {
    void(*destruct_and_delete)(event *me);
    void(*execute)(event *me, execute_context &cxt);
    void(*unexecute)(event *me);
    void(*commit)(event *me);
  };

  struct event {
    event_vtable const *vtbl1;
    std::uint64_t time;
    std::uint64_t id;
    int target_rank, target_cd;
    int remoted_ix;
    event *sent_next = nullptr;
    
    alignas(64) // -----------------------------------------------------

    event_vtable const *vtbl2;
    event *sent_head = nullptr;
    int creator_rank;
    std::int8_t existence = 0; // -1,0,+1
    int future_ix;
    
    event(event_vtable const *vtbl) {
      this->vtbl1 = vtbl;
      this->vtbl2 = vtbl;
      this->creator_rank = world::rank_me();
    }

    event_tid tid() const {
      return {time, id};
    }
    
    static std::uint64_t time_of(event *e) {
      return e->time;
    }

    static int& remoted_ix_of(event *e) {
      return e->remoted_ix;
    }
  };

  struct execute_context_impl: execute_context {
    event *sent_head = nullptr;
  };

  template<typename E>
  struct event_impl final: event {
    E user;

    static void destruct_and_delete(event *me) {
      delete static_cast<event_impl<E>*>(me);
    }
    static void execute(event *me, execute_context &cxt) {
      static_cast<event_impl<E>*>(me)->user.execute(cxt);
    }
    static void unexecute(event *me) {
      static_cast<event_impl<E>*>(me)->user.unexecute();
    }
    static void commit(event *me) {
      static_cast<event_impl<E>*>(me)->user.commit();
    }

    static constexpr event_vtable the_vtable = {
      event_impl<E>::destruct_and_delete,
      event_impl<E>::execute,
      event_impl<E>::unexecute,
      event_impl<E>::commit
    };

    event_impl(E user):
      event{&the_vtable},
      user{std::move(user)} {
    }
  };

  template<typename E>
  constexpr event_vtable event_impl<E>::the_vtable;
  
  template<typename E>
  void execute_context::send(
      int rank, int cd,
      std::uint64_t time, std::uint64_t id,
      E ev
    ) {
    auto *me = static_cast<execute_context_impl*>(this);
    auto *e = new event_impl<E>{std::move(ev)};
    e->target_rank = rank;
    e->target_cd = cd;
    e->time = time;
    e->id = id;
    e->sent_next = me->sent_head;
    me->sent_head = e;
  }

  void root_event(int cd_ix, event *e);
  
  template<typename E>
  void root_event(int cd_ix, std::uint64_t time, std::uint64_t id, E ev) {
    auto *e = new event_impl<E>{std::move(ev)};
    e->target_rank = world::rank_me();
    e->target_cd = cd_ix;
    e->time = time;
    e->id = id;
    root_event(cd_ix, e);
  }
}

#endif
