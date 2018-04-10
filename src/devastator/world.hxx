#if WORLD_THREADS
  #include <devastator/world_threads.hxx>
#elif WORLD_GASNET
  #include <devastator/world_gasnet.hxx>
#endif

#include <devastator/reduce.hxx>
