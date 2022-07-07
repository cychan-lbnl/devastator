#pragma once

#include <devastator/diagnostic.hxx>
#include <devastator/world.hxx>
#include <devastator/pdes.hxx>
#include <devastator/os_env.hxx>

#include <cstdint>
#include <vector>
#include <unordered_map>

#include "time.hxx"
#include "print.hxx"

// force advance at least this many seconds
const Time max_advance_delta_t = deva::os_env<unsigned>("max_advance_delta_t", 100);
// simulation ends after this many seconds
const Time sim_end_time = deva::os_env<unsigned>("sim_end_time", 100);
// control verbosity
const bool flag_verbose = deva::os_env<bool>("verbose", false);

// basic implementation of an actor space
// uses a simple block partitioning across ranks
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
        actors.insert({actor_id, Actor(actor_id)});
      }
    }
    deva::barrier();
  }

  // derived class implements this
  // use Actor::set_neighbors() to connect actors
  virtual void connect () {}

  void root_events () const
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

/////////////////
// Actor Class //
/////////////////

template <class State>
class Actor
{
public:

  using NeighborState = typename State::NeighborState;

  Actor () = default;
  Actor (int i) : id(i) {}

  ///////////////////
  // Advance event //
  ///////////////////

  struct AdvanceInfo
  {
    Time  saved_time = 0;
    State saved_state;
  };

  // event that advance's a actor's state to a given time
  struct Advance
  {
    Time event_time = 0;
    int actor_id = 0;

    // include all object data fields in this macro
    SERIALIZED_FIELDS(event_time, actor_id);

    // helper struct to unexecute or commit event
    struct UC
    {
      // saved data to restore state
      AdvanceInfo adv_info;
      NeighborState saved_last_sent;
      Time saved_next_advance_time;

      void unexecute (deva::pdes::event_context & cxt, Advance & me);
      void commit (deva::pdes::event_context & cxt, Advance & me) {} // does nothing
    };

    // main event execute function
    UC execute (deva::pdes::execute_context & cxt);
  };

  ///////////////////////
  // ShareState event //
  ///////////////////////

  // event that sets a destination Actor's neighbor value
  struct ShareState
  {
    Time event_time = 0;
    int src_actor_id = 0;
    int dst_actor_id = 0;
    NeighborState nbr;

    // include all object data fields in this macro
    SERIALIZED_FIELDS(event_time, src_actor_id, dst_actor_id, nbr);

    // helper struct to unexecute or commit event
    struct UC
    {
      AdvanceInfo adv_info;
      NeighborState old_nbr;
      Time saved_next_advance_time;

      void unexecute (deva::pdes::event_context & cxt, ShareState & me);
      void commit (deva::pdes::event_context & cxt, ShareState & me) {} // does nothing
    };

    // main event execute function
    UC execute (deva::pdes::execute_context & cxt);
  };

  ////////////////////
  // Public methods //
  ////////////////////

  AdvanceInfo advance (Time t);
  void unadvance (AdvanceInfo info);
  NeighborState set_neighbor (int nid, NeighborState nbr);
  NeighborState send_share_state_events (deva::pdes::execute_context & cxt); // share state with neighbors
  Time reschedule (deva::pdes::execute_context & cxt); // self-schedule an advance

  const State & get_state () const { return state; }
  void set_neighbors (std::unordered_map<int, NeighborState> nbrs) { neighbors = std::move(nbrs); }
  void set_last_sent (NeighborState nbr) { last_sent = nbr; }
  void set_next_advance_time (Time t) { next_advance_time = t; }

private:

  /////////////////////////////////
  // Private members and methods //
  /////////////////////////////////

  int id = 0;    // my ID
  Time time = 0; // current time
  State state;   // current state
  std::unordered_map<int, NeighborState> neighbors; // neighbor states
  NeighborState last_sent; // last sent value to neighbors
  Time next_advance_time = 0; // next scheduled advance

  // include all object data fields in this macro
  SERIALIZED_FIELDS(id, time, state, neighbors, last_sent, next_advance_time);

  std::pair<Time, State> estimate_next_send_time () const;
};

// advance cell state to time t
template <class State>
typename Actor<State>::AdvanceInfo
Actor<State>::advance (Time t)
{
  // save old state in case of rollback
  AdvanceInfo info {time, state};

  auto always_false = [] (Time t, const State & s) { return false; };
  state.local_integrator(time, t, state, neighbors, always_false);
  if (flag_verbose) {
    AllPrint() << "Time " << t << ": Actor " << id << ": advance() from time " << info.saved_time << " with value: " << info.saved_state << " -> " << state << std::endl;
  }
  DEVA_ASSERT(time == t);

  return info;
}

template <class State>
void Actor<State>::unadvance (AdvanceInfo info)
{
  time  = info.saved_time;
  state = info.saved_state;
}

template <class State>
typename State::NeighborState
Actor<State>::set_neighbor (int nid, typename State::NeighborState nbr)
{
  if (flag_verbose) {
    AllPrint() << "Time " << time << ": Actor " << id << ": set_neighbor() cell " << nid << " to " << nbr << std::endl;
  }
  NeighborState old_nbr = neighbors[nid];
  neighbors[nid] = nbr;
  return old_nbr;
}

// compute when to send updated value to neighbors
template <class State>
std::pair<Time, State> Actor<State>::estimate_next_send_time () const
{
  Time est_time = time;
  State est_state = state;
  auto checker = [this] (Time t, const State & s) {
    return state.check_need_update(last_sent, t, s);
  };

  // estimate when we would need to update neighbors via local integration
  state.local_integrator(est_time, time + max_advance_delta_t, est_state, neighbors, checker);

  return {est_time, est_state};
}

template <class State>
typename State::NeighborState
Actor<State>::send_share_state_events (deva::pdes::execute_context & cxt)
{
  NeighborState saved_last_sent = last_sent;

  if (state.check_need_update(last_sent, time, state)) {
    if (flag_verbose) {
      AllPrint() << "Time " << time << ": Actor " << id << ": send_share_state_events() with value: " << state << std::endl;
    }
    NeighborState my_nbr_state = state.gen_neighbor_state(time, neighbors);
    const auto & as = *get_actor_space<Actor<State>>();
    for (const auto & p : neighbors) {
      int nbr_id = p.first;
      if (as.has_actor(nbr_id)) {
        int nbr_rank = as.actor_id_to_rank(nbr_id);
        int nbr_cd   = as.actor_id_to_idx(nbr_id);
        ShareState event{time, id, nbr_id, my_nbr_state};
        cxt.send(nbr_rank, nbr_cd, get_timestamp(time, 1), event);
      }
    }
    last_sent = my_nbr_state;
  }

  return saved_last_sent;
}

template <class State>
Time Actor<State>::reschedule (deva::pdes::execute_context & cxt)
{
  Time saved_next_advance_time = next_advance_time;
  if (next_advance_time <= time) {
    next_advance_time = std::numeric_limits<Time>::max();
  }

  // estimate when value will change enough to send to neighbors
  Time est_time; State est_state;
  std::tie(est_time, est_state) = estimate_next_send_time();

  // only reschedule if est_time is earlier than currently scheduled advance 
  if (est_time > time && est_time < std::min(next_advance_time, sim_end_time)) {
    const auto & as = *get_actor_space<Actor<State>>();
    int rank = as.actor_id_to_rank(id);
    int cd   = as.actor_id_to_idx(id);
    if (flag_verbose) {
      AllPrint() << "Time " << time << ": Actor " << id << ": reschedule() for future time: " << est_time << std::endl;
    }
    cxt.send(rank, cd, get_timestamp(est_time, 0), Advance {est_time, id});
    next_advance_time = est_time;
  }

  return saved_next_advance_time;
}

/////////////////////////////////////
// ShareState Event Implementation //
/////////////////////////////////////

template <class State>
typename Actor<State>::ShareState::UC
Actor<State>::ShareState::execute (deva::pdes::execute_context & cxt)
{
  auto & as = *get_actor_space<Actor<State>>();
  Actor<State> & cell = as.get_actor(dst_actor_id);
  // advance to given time using old neighbor values
  AdvanceInfo adv_info = cell.advance(event_time);
  // set the neighbor with new values from event
  NeighborState old_nbr = cell.set_neighbor(src_actor_id, nbr);
  DEVA_ASSERT(nbr.effective > old_nbr.effective);
  // reschedule advance given new neighbor values
  Time saved_next_advance_time = cell.reschedule(cxt);

  return ShareState::UC {adv_info, old_nbr, saved_next_advance_time};
}

template <class State>
void Actor<State>::ShareState::UC::unexecute (deva::pdes::event_context & cxt, Actor<State>::ShareState & me)
{
  // restore the old state
  auto & as = *get_actor_space<Actor<State>>();
  Actor<State> & cell = as.get_actor(me.dst_actor_id);
  cell.unadvance(std::move(adv_info));
  cell.set_neighbor(me.src_actor_id, old_nbr);
  cell.set_next_advance_time(saved_next_advance_time);
}

//////////////////////////////////
// Advance Event Implementation //
//////////////////////////////////

template <class State>
typename Actor<State>::Advance::UC
Actor<State>::Advance::execute (deva::pdes::execute_context & cxt)
{
  auto & as = *get_actor_space<Actor<State>>();
  Actor<State> & cell = as.get_actor(actor_id);
  AdvanceInfo adv_info = cell.advance(event_time);
  // share state with neighbors
  NeighborState saved_last_sent = cell.send_share_state_events(cxt);
  // reschedule next advance
  Time saved_next_advance_time = cell.reschedule(cxt);
  return Advance::UC {std::move(adv_info), saved_last_sent, saved_next_advance_time};
}

template <class State>
void Actor<State>::Advance::UC::unexecute (deva::pdes::event_context & cxt, Actor<State>::Advance & me) {
  // restore the old state
  auto & as = *get_actor_space<Actor<State>>();
  Actor<State> & cell = as.get_actor(me.actor_id);
  cell.unadvance(std::move(adv_info));
  cell.set_last_sent(saved_last_sent);
  cell.set_next_advance_time(saved_next_advance_time);
};
