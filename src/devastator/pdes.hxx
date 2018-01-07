#ifndef _409355b8303d41628b1284d487a6d766
#define _409355b8303d41628b1284d487a6d766

#include "gvt.hxx"
#include "world.hxx"

#include <cstdint>
#include <forward_list>
#include <utility>

namespace pdes {
  struct execute_context {
    std::uint64_t time, id;
    
    template<typename E>
    void send(int rank, int cd, std::uint64_t time, std::uint64_t id, E ev);
  };

  void init(int cds_this_rank);
  void drain();

  template<typename E>
  void root_event(int cd_ix, std::uint64_t time, std::uint64_t id, E ev);

  //////////////////////////////////////////////////////////////////////
  // internal

  using event_tid = std::pair<std::uint64_t, std::uint64_t>;

  struct far_event_tid {
    int rank;
    int cd;
    event_tid tid;
  };

  struct event_on_creator;
  struct event_on_target;
  struct event;
  
  struct event_vtable {
    void(*destruct_and_delete)(event *me);
    void(*execute)(event *me, execute_context &cxt);
    void(*unexecute)(event *me);
    void(*commit)(event *me);
  };

  struct alignas(64) event_on_creator {
    event_vtable const *vtbl1;
    std::uint64_t time;
    std::uint64_t id;
    int target_rank, target_cd;
    int remote_near_ix = -1;
    event *sent_near_next = nullptr;
    event_on_creator *far_next;
    
    event_tid tid() const {
      return {time, id};
    }

    static event_tid tid_of(event_on_creator *e) {
      return {e->time, e->id};
    }
  };
  
  struct alignas(64) event_on_target {
    event_vtable const *vtbl2;
    event *sent_near_head = nullptr;
    std::forward_list<far_event_tid> sent_far;
    int creator_rank;
    std::int8_t existence = 0; // -1,0,+1
    int future_ix = -1;
  };

  struct event: event_on_creator, event_on_target {
    event(event_vtable const *vtbl) {
      this->vtbl1 = vtbl;
      this->vtbl2 = vtbl;
      this->creator_rank = world::rank_me();
    }

    static std::uint64_t time_of(event *e) {
      return e->time;
    }

    static int& remote_near_ix_of(event *e) {
      return e->remote_near_ix;
    }
  };

  struct execute_context_impl: execute_context {
    event *sent_near_head = nullptr;
    std::forward_list<far_event_tid> *sent_far;
  };

  template<typename E>
  struct event_impl final: event {
    using Unex = decltype(std::declval<E>().execute(std::declval<execute_context&>()));
    E exec;
    union { Unex unex; };
    
    static void destruct_and_delete(event *me) {
      delete static_cast<event_impl<E>*>(me);
    }
    static void execute(event *me1, execute_context &cxt) {
      auto *me = static_cast<event_impl<E>*>(me1);
      new(&me->unex) Unex{me->exec.execute(cxt)};
    }
    static void unexecute(event *me1) {
      auto *me = static_cast<event_impl<E>*>(me1);
      me->unex(me->exec);
    }
    static void commit(event *me1) {
      auto *me = static_cast<event_impl<E>*>(me1);
      me->unex.~Unex();
      me->exec.commit();
    }
    
    static constexpr event_vtable the_vtable = {
      &event_impl<E>::destruct_and_delete,
      &event_impl<E>::execute,
      &event_impl<E>::unexecute,
      &event_impl<E>::commit
    };
    
    event_impl(E exec):
      event{&the_vtable},
      exec{std::move(exec)} {
    }
  };

  template<typename E>
  constexpr event_vtable event_impl<E>::the_vtable;

  void arrive_far(int cd_ix, event *e, event_tid e_tid, int existence_delta);
  
  template<typename E>
  void execute_context::send(
      int rank, int cd,
      std::uint64_t time,
      std::uint64_t id,
      E ev
    ) {
    auto *me = static_cast<execute_context_impl*>(this);

    if(world::rank_is_local(rank)) {
      auto *e = new event_impl<E>{std::move(ev)};
      e->target_rank = rank;
      e->target_cd = cd;
      e->time = time;
      e->id = id;
      
      e->sent_near_next = me->sent_near_head;
      me->sent_near_head = e;
    }
    else {
      gvt::send_remote(rank, time,
        world::bind(
          [=](E &ev) {
            auto *e = new event_impl<E>{std::move(ev)};
            e->target_rank = rank;
            e->target_cd = cd;
            e->time = time;
            e->id = id;
            
            pdes::arrive_far(cd, e, {time,id}, 1);
          },
          std::move(ev)
        )
      );

      me->sent_far->push_front({rank, cd, {time,id}});
    }
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
