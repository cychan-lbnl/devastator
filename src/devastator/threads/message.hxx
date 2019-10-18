#ifndef _4c4b7bd29c144176a52f5a915a928431
#define _4c4b7bd29c144176a52f5a915a928431

////////////////////////////////////////////////////////////////////////////////
// Message & channel forward declaration

namespace deva {
namespace threads {
  struct message;

  template<int>
  class channels_r;
  
  template<int wn, typename Chans_r, Chans_r(*chan_r)[wn]>
  class channels_w;
}}

#if DEVA_THREADS_MESSAGE_SPSC
  #include <devastator/threads/message_spsc.hxx>
#elif DEVA_THREADS_MESSAGE_MPSC
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

  template<int rn>
  struct active_channels_r;

  template<int wn, int rn, active_channels_r<rn>(*achan_r)[wn]>
  struct active_channels_w:
    channels_w<wn, active_channels_r<rn>, achan_r> {
    
    template<typename Fn>
    void send(int c, Fn &&fn) {
      auto *m = new(
          ::operator new(sizeof(active_message_impl<Fn>))
        ) active_message_impl<Fn>{static_cast<Fn&&>(fn)};

      channels_w<wn, active_channels_r<rn>, achan_r>::send(c, m);
    }
  };

  template<int rn>
  struct active_channels_r: channels_r<rn> {
    bool receive() {
      return channels_r<rn>::receive(
        [](message *m) {
          auto *am = static_cast<active_message*>(m);
          am->execute_and_destruct(am);
        }
      );
    }
  };
}}
#endif
