#ifndef _ce62bf80793b40888f7e463dfa811c3d
#define _ce62bf80793b40888f7e463dfa811c3d

#ifndef DEVA_WORLD_THREADS
  #define DEVA_WORLD_THREADS 0
#endif

#ifndef DEVA_WORLD_GASNET
  #define DEVA_WORLD_GASNET 0
#endif

#include <devastator/opnew.hxx>
#include <devastator/utility.hxx>

#include <upcxx/utility.hpp>

namespace deva {
  //////////////////////////////////////////////////////////////////////////////
  // Public API

  #if 0 // no way to forward decalre constexpr variables
    constexpr int rank_n;
    constexpr int log2up_rank_n;
    constexpr int process_n;
    constexpr int worker_n;
  #endif

  #if 0 // no way to forward decalre #define's
    #define SERIALIZED_FIELDS(...)
  #endif

  void run(upcxx::detail::function_ref<void()> fn);
  void run_and_die(upcxx::detail::function_ref<void()> fn);
  
  int rank_me();
  int rank_me_local();
  
  bool rank_is_local(int rank);
  
  int process_me();
  constexpr int process_rank_lo(int proc = process_me());
  constexpr int process_rank_hi(int proc = process_me());
  
  void progress(bool spinning=false, bool deaf=false);

  void barrier(bool deaf=false);
  
  template<typename Fn, typename ...Arg>
  void send(int rank, Fn &&fn, Arg &&...arg);
  
  template<bool3 local, typename Fn, typename ...Arg>
  void send(int rank, cbool3<local>, Fn &&fn, Arg &&...arg);

  template<typename Fn, typename ...Arg>
  void send_local(int rank, Fn &&fn, Arg &&...arg);
  
  template<typename Fn, typename ...Arg>
  void send_remote(int rank, Fn &&fn, Arg &&...arg);

  template<typename ProcFn>
  void bcast_procs(ProcFn &&proc_fn);
  
  #if 0 // no way to forward decalare computed return type
    template<typename Fn, typename ...B>
    auto bind(Fn fn, B ...b);
  #endif
}

#if DEVA_WORLD_THREADS
  #include <devastator/world/world_threads.hxx>
#elif DEVA_WORLD_GASNET
  #include <devastator/world/world_gasnet.hxx>
#endif

#include <devastator/world/reduce.hxx>

#endif
