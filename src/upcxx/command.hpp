#ifndef _3589ecdd_36fa_4240_ade2_c807b184ac7f
#define _3589ecdd_36fa_4240_ade2_c807b184ac7f

#include <upcxx/diagnostic.hpp>
#include <upcxx/global_fnptr.hpp>
#include <upcxx/packing.hpp>

// Commands are callable objects that have been packed into a parcel.

namespace upcxx {
  template<typename...>
  class command;
  
  template<>
  class command<> {
    using executor_t = global_fnptr<void(parcel_reader&)>;

    template<typename Fn>
    static void the_executor(parcel_reader &r) {
      raw_storage<unpacked_of_t<Fn>> fn;

      unpacking<Fn>::unpack(r, &fn, std::false_type());

      fn.value.operator()();
      fn.destruct();
    }

  public:
    // Knowing the next thing on the reader was packed by command<>::pack, this
    // will pop it off and execute it leaving the reader pointing to just after
    // the command.
    static void execute(parcel_reader &r) {
      executor_t exec = r.pop_trivial_aligned<executor_t>();
      exec.fnptr_non_null()(r);
    }

    // Update an upper-bound on the parcel size needed to accomadate adding the
    // given callable as a command.
    template<typename Fn1, typename Ub,
             typename Fn = typename std::decay<Fn1>::type>
    static constexpr auto ubound(Ub ub0, Fn1 &&fn)
      -> decltype(
        packing<Fn>::ubound(ub0.template trivial_added<executor_t>(), fn, std::false_type())
      ) {
      return packing<Fn>::ubound(ub0.template trivial_added<executor_t>(), fn, std::false_type());
    }

    // Pack the given callable onto the writer.
    template<typename Fn1,
             typename Fn = typename std::decay<Fn1>::type>
    static void pack(parcel_writer &w, std::size_t size_ub, Fn1 &&fn) {
      w.template put_trivial_aligned<executor_t>(executor_t(&the_executor<Fn>));

      packing<Fn>::pack(w, fn, std::false_type());
      
      UPCXX_ASSERT(
        size_ub == 0 || w.size() <= size_ub,
        "Overflowed parcel buffer: buffer="<<size_ub<<", packed="<<w.size()
      ); // blame packing<Fn>::ubound()
    }
  };
}
#endif
