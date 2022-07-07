#pragma once

#include <devastator/diagnostic.hxx>
#include <devastator/world.hxx>
#include <devastator/pdes.hxx>
#include <devastator/os_env.hxx>

/////////////////////////////
// Basic Actor Space Class //
/////////////////////////////

// uses a dense ID space and a simple block partitioning across ranks
template <class Actor>
class BasicActorSpace
{
public:

  // allocate actors in shared storage to prepare for concurrent access
  void init (int a_n)
  {
    if (deva::rank_me_local() == 0) {
      actor_n = a_n;
      for (int actor_id = 0; actor_id < actor_n; ++actor_id) {
        if (deva::rank_is_local(actor_id_to_rank(actor_id))) {
          actors.insert({actor_id, Actor(actor_id)});
        }
      }
    }
    deva::barrier();
  }

  // derived class implements these
  virtual void configure () {}
  virtual void root_events () const {}

  int actor_per_rank () const
  {
    return (actor_n + deva::rank_n-1) / deva::rank_n;
  }

  bool has_actor (int actor_id) const
  {
    return actor_id >= 0 && actor_id < actor_n;
  }

  // which rank a actor is assigned to
  int actor_id_to_rank (int actor_id) const
  {
    DEVA_ASSERT(has_actor(actor_id));
    return actor_id / actor_per_rank();
  }

  // returns actor's index within rank
  int actor_id_to_idx (int actor_id) const
  {
    DEVA_ASSERT(has_actor(actor_id));
    return actor_id % actor_per_rank();
  }

  // which actors are assigned to a rank
  std::vector<int> rank_to_actor_ids (int rank) const
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
        Print() << actors.at(actor_id).get_state();
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
};

// specialize this function to return the Actor's actor space
template <class Actor>
BasicActorSpace<Actor> * get_actor_space ()
{
  return nullptr;
}
