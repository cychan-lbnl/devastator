#pragma once

#include <devastator/diagnostic.hxx>
#include <devastator/world.hxx>
#include <devastator/pdes.hxx>

#include <cstdint>
#include <vector>
#include <unordered_map>

#include "time.hxx"
#include "print.hxx"

template <class Actor>
class ActorSpace
{
public:

  void init (int a_n)
  {
    // allocate actors in shared storage to prepare for concurrent access
    if (deva::rank_me_local() == 0) {
      actor_n = a_n;
      for (int actor_id = 0; actor_id < actor_n; ++actor_id) {
        if (deva::rank_is_local(actor_id_to_rank(actor_id))) {
          get_shared_actor(actor_id) = Actor(actor_id);
        }
      }
    }
    deva::barrier();
  }

  virtual void connect () {}

  void root_events ()
  {
    // insert a root advance event for each actor owned by this rank
    for (int actor_id : rank_to_actor_ids(deva::rank_me())) {
      int actor_idx = actor_id_to_idx(actor_id);
      using Advance = typename Actor::Advance;
      deva::pdes::root_event(actor_idx, // use index as causality domain
                             get_timestamp(0, 0), // event time stamp
                             Advance{0, actor_id}); // advance event
    }
  }

  bool has_actor (int actor_id) const
  {
    return actor_id >= 0 && actor_id < actor_n;
  }

  // which rank a actor is assigned to
  int actor_id_to_rank (int actor_id)
  {
    DEVA_ASSERT(has_actor(actor_id));
    return actor_id / actor_per_rank();
  }

  // returns actor's index within rank
  int actor_id_to_idx (int actor_id)
  {
    DEVA_ASSERT(has_actor(actor_id));
    return actor_id % actor_per_rank();
  }

  // which actors are assigned to a rank
  std::vector<int> rank_to_actor_ids (int rank)
  {
    int lb = std::min(actor_n,  deva::rank_me()      * actor_per_rank());
    int ub = std::min(actor_n, (deva::rank_me() + 1) * actor_per_rank());
    
    std::vector<int> result(ub - lb);
    for (int idx = 0; idx < ub - lb; idx++) {
      result[idx] = lb + idx;
    }
    return result;
  }

  // return Actor from local storage
  Actor & get_actor (int actor_id)
  {
    DEVA_ASSERT(actor_id_to_rank(actor_id) == deva::rank_me());
    return actors[actor_id];
  }

  // return Actor from local storage (any actor on process)
  Actor & get_shared_actor (int actor_id)
  {
    DEVA_ASSERT(deva::rank_is_local(actor_id_to_rank(actor_id)));
    return actors[actor_id];
  }

  void print () const
  {
    if (deva::rank_me() == 0) {
      std::vector<int> actor_ids;
      for (const auto & p : actors) {
        actor_ids.push_back(p.first);
      }
      std::sort(actor_ids.begin(), actor_ids.end());
      Print() << "Actors : [";
      bool flag_first = true;
      for (int actor_id : actor_ids) {
        if (!flag_first) {
          Print() << ',';
        }
        Print() << actors.at(actor_id);
        flag_first = false;
      }
      Print() << ']' << std::endl;
    }
  }

private:

  // global number of actors
  int actor_n = 0;

  // here's where the actor actors are stored
  std::unordered_map<int, Actor> actors;

  int actor_per_rank () const {
    return (actor_n + deva::rank_n-1) / deva::rank_n;
  }
};
