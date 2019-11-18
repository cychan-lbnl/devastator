#ifndef _4c4b7bd29c144176a52f5a915a928431
#define _4c4b7bd29c144176a52f5a915a928431

#ifndef DEVA_THREADS_SPSC
  #define DEVA_THREADS_SPSC 0
#endif

#ifndef DEVA_THREADS_MPSC
  #define DEVA_THREADS_MPSC 0
#endif

#ifndef DEVA_THREADS_ALLOC_EPOCH
  #define DEVA_THREADS_ALLOC_EPOCH 0
#endif

////////////////////////////////////////////////////////////////////////////////
// Message & channel forward declaration

namespace deva {
namespace threads {
  struct message;

  template<int>
  class channels_r;
  
  template<int wn, int rn, channels_r<rn>(*chan_r)[wn]>
  class channels_w;
}}

#if DEVA_THREADS_SPSC
  #include <devastator/threads/message_spsc.hxx>
#elif DEVA_THREADS_MPSC
  #include <devastator/threads/message_mpsc.hxx>
#endif

#endif
