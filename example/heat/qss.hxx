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
#include "actor_space.hxx"

// simulation ends after this many seconds
const Time sim_end_time = deva::os_env<unsigned>("sim_end_time", 100);
// control verbosity
const bool flag_verbose = deva::os_env<bool>("verbose", false);

//////////////////////
// Polynomial Class //
//////////////////////

// T is type of input/output/coefficients
// N is order of polynomial (N+1 coefficients)
template <class T, int N>
struct Poly
{
  std::array<T, N+1> coeffs {}; // value-initialize array to zeros
  Time effective = 0;           // input to polynomial is relative to this time

  // include all object data fields in this macro
  SERIALIZED_FIELDS(coeffs, effective);

  T eval (Time t) const; // evaluate at time t
};

template <class T, int N>
T Poly<T, N>::eval (Time t) const
{
  Time dt = t - effective;
  T result = coeffs[N];
  for (int i = N - 1; i >= 0; --i) {
    result = result * dt + coeffs[i];
  }
  return result;
}


template <class T, int N>
std::ostream & operator<< (std::ostream & os, const Poly<T, N> & x)
{
  os << "Poly({";
  bool flag_first = true;
  for (int i = 0; i < N + 1; ++i) {
    if (!flag_first) {
      os << ", ";
    }
    os << x.coeffs[i];
    flag_first = false;
  }
  os << "}, eff = " << x.effective << ")";
  return os;
}

////////////////////
// QSSActor Class //
////////////////////

template <class State>
class QSSActor
{
public:

  using NeighborState = typename State::NeighborState;

  QSSActor () = default;
  QSSActor (int i) : id(i) {}

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

  // event that sets a destination QSSActor's neighbor value
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
  State & get_state () { return state; }
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
};

// advance cell state to time t
template <class State>
typename QSSActor<State>::AdvanceInfo
QSSActor<State>::advance (Time t)
{
  AdvanceInfo info {time, state}; // save old state in case of rollback
  state.advance(time, t, neighbors);
  time = t;
  if (flag_verbose) {
    AllPrint() << "Time " << t << ": QSSActor " << id << ": advance() from time " << info.saved_time << " with value: " << info.saved_state << " -> " << state << std::endl;
  }
  return info;
}

template <class State>
void QSSActor<State>::unadvance (AdvanceInfo info)
{
  time  = info.saved_time;
  state = info.saved_state;
}

template <class State>
typename State::NeighborState
QSSActor<State>::set_neighbor (int nid, typename State::NeighborState nbr)
{
  if (flag_verbose) {
    AllPrint() << "Time " << time << ": QSSActor " << id << ": set_neighbor() cell " << nid << " to " << nbr << std::endl;
  }
  NeighborState old_nbr = neighbors[nid];
  neighbors[nid] = nbr;
  return old_nbr;
}

template <class State>
typename State::NeighborState
QSSActor<State>::send_share_state_events (deva::pdes::execute_context & cxt)
{
  NeighborState saved_last_sent = last_sent;

  if (state.check_need_update(time, last_sent)) {
    NeighborState my_nbr_state = state.gen_neighbor_state(time, neighbors);
    if (flag_verbose) {
      AllPrint() << "Time " << time << ": QSSActor " << id << ": send_share_state_events(): " << my_nbr_state << std::endl;
    }
    const auto & as = *get_actor_space<QSSActor<State>>();
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
Time QSSActor<State>::reschedule (deva::pdes::execute_context & cxt)
{
  Time saved_next_advance_time = next_advance_time;
  if (next_advance_time <= time) {
    next_advance_time = std::numeric_limits<Time>::max();
  }

  // estimate when value will change enough to send to neighbors
  Time est_time = state.estimate_next_send_time(time, neighbors, last_sent);

  // only reschedule if est_time is earlier than currently scheduled advance 
  if (est_time > time && est_time < std::min(next_advance_time, sim_end_time)) {
    const auto & as = *get_actor_space<QSSActor<State>>();
    int rank = as.actor_id_to_rank(id);
    int cd   = as.actor_id_to_idx(id);
    if (flag_verbose) {
      AllPrint() << "Time " << time << ": QSSActor " << id << ": reschedule() for future time: " << est_time << std::endl;
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
typename QSSActor<State>::ShareState::UC
QSSActor<State>::ShareState::execute (deva::pdes::execute_context & cxt)
{
  auto & as = *get_actor_space<QSSActor<State>>();
  QSSActor<State> & cell = as.get_actor(dst_actor_id);
  // advance to given time using old neighbor values
  AdvanceInfo adv_info = cell.advance(event_time);
  // set the neighbor with new values from event
  NeighborState old_nbr = cell.set_neighbor(src_actor_id, nbr);
  // reschedule advance given new neighbor values
  Time saved_next_advance_time = cell.reschedule(cxt);

  return ShareState::UC {adv_info, old_nbr, saved_next_advance_time};
}

template <class State>
void QSSActor<State>::ShareState::UC::unexecute (deva::pdes::event_context & cxt, QSSActor<State>::ShareState & me)
{
  // restore the old state
  auto & as = *get_actor_space<QSSActor<State>>();
  QSSActor<State> & cell = as.get_actor(me.dst_actor_id);
  cell.unadvance(std::move(adv_info));
  cell.set_neighbor(me.src_actor_id, old_nbr);
  cell.set_next_advance_time(saved_next_advance_time);
}

//////////////////////////////////
// Advance Event Implementation //
//////////////////////////////////

template <class State>
typename QSSActor<State>::Advance::UC
QSSActor<State>::Advance::execute (deva::pdes::execute_context & cxt)
{
  auto & as = *get_actor_space<QSSActor<State>>();
  QSSActor<State> & cell = as.get_actor(actor_id);
  AdvanceInfo adv_info = cell.advance(event_time);
  // share state with neighbors
  NeighborState saved_last_sent = cell.send_share_state_events(cxt);
  // reschedule next advance
  Time saved_next_advance_time = cell.reschedule(cxt);
  return Advance::UC {std::move(adv_info), saved_last_sent, saved_next_advance_time};
}

template <class State>
void QSSActor<State>::Advance::UC::unexecute (deva::pdes::event_context & cxt, QSSActor<State>::Advance & me) {
  // restore the old state
  auto & as = *get_actor_space<QSSActor<State>>();
  QSSActor<State> & cell = as.get_actor(me.actor_id);
  cell.unadvance(std::move(adv_info));
  cell.set_last_sent(saved_last_sent);
  cell.set_next_advance_time(saved_next_advance_time);
};
