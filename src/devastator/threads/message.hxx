#ifndef _4c4b7bd29c144176a52f5a915a928431
#define _4c4b7bd29c144176a52f5a915a928431

#ifndef DEVA_THREADS_SPSC
  #define DEVA_THREADS_SPSC 0
#endif

#ifndef DEVA_THREADS_MPSC
  #define DEVA_THREADS_MPSC 0
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

////////////////////////////////////////////////////////////////////////////////
// Active messages over threads::message

namespace deva {
namespace threads {
  struct active_message: message {
    void(*execute_and_destruct)(active_message*);
  };

  template<typename Fn>
  struct active_message_impl final: active_message {
    Fn fn;

    static void the_execute_and_destruct(active_message *m) {
      auto *me = static_cast<active_message_impl<Fn>*>(m);
      static_cast<Fn&&>(me->fn)();
      me->fn.~Fn();
    }
    
    active_message_impl(Fn &&fn):
      fn(static_cast<Fn&&>(fn)) {
      this->execute_and_destruct = the_execute_and_destruct;
    }
  };

  template<int wn, int rn, channels_r<rn>(*chan_r)[wn], typename Fn>
  void send_am(channels_w<wn,rn,chan_r> &chan_w, int c, Fn &&fn) {
    auto *m = new(
        ::operator new(sizeof(active_message_impl<Fn>))
      ) active_message_impl<Fn>{static_cast<Fn&&>(fn)};
    chan_w.send(c, m);
  }

  template<int rn>
  bool receive_ams(channels_r<rn> &chan_r) {
    return chan_r.receive(
      [](message *m) {
        auto *am = static_cast<active_message*>(m);
        am->execute_and_destruct(am);
      }
    );
  }
}}
#endif
