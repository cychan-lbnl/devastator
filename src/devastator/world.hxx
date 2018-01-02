#if WORLD_THREADS
  #include "world_threads.hxx"
#elif WORLD_GASNET
  #include "world_gasnet.hxx"
#endif

#include "reduce.hxx"
