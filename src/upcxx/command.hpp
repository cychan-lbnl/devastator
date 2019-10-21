#ifndef _3589ecdd_36fa_4240_ade2_c807b184ac7f
#define _3589ecdd_36fa_4240_ade2_c807b184ac7f

#include <upcxx/diagnostic.hpp>
#include <upcxx/global_fnptr.hpp>
#include <upcxx/serialization.hpp>

// Commands are callable objects that have been packed into a parcel.

namespace upcxx {
namespace detail {
  struct command {
    using executor_wire_t = global_fnptr<void(detail::serialization_reader&, char const*)>;

    template<typename Fn>
    static void the_executor(detail::serialization_reader &r, char const *head_before) {
      detail::raw_storage<typename serialization_traits<Fn>::deserialized_type> fn;
      serialization_traits<Fn>::deserialize(r, &fn);
      char const *head_after = r.head();
      static_cast<Fn&&>(fn.value())(head_before, head_after - head_before);
      fn.destruct();
    }

    static void execute(detail::serialization_reader &r) {
      char const *head_before = r.head();
      executor_wire_t exec = r.template pop_trivial<executor_wire_t>();
      UPCXX_ASSERT(exec.u_ != 0);
      exec.fnptr_non_null()(r, head_before);
    }

    // Update an upper-bound on the size needed to accomadate adding the
    // given callable as a command.
    template<typename Fn1, typename SS,
             typename Fn = typename std::decay<Fn1>::type>
    static constexpr auto ubound(SS ub0, Fn1 &&fn)
      -> decltype(
        ub0.template cat_size_of<executor_wire_t>()
           .template cat_ubound_of<Fn>(fn)
      ) {
      return ub0.template cat_size_of<executor_wire_t>()
                .template cat_ubound_of<Fn>(fn);
    }

    // Serialize the given callable and reader actions onto the writer.
    template<typename Fn1, typename Writer>
    static void serialize(Writer &w, std::size_t size_ub, Fn1 &&fn) {
      using Fn = typename std::decay<Fn1>::type;
      
      executor_wire_t exec = executor_wire_t(&the_executor<Fn>);
      
      w.template push_trivial<executor_wire_t>(exec);
      
      serialization_traits<Fn>::serialize(w, fn);
      
      UPCXX_ASSERT(
        size_ub == 0 || w.size() <= size_ub,
        "Overflowed serialization buffer: buffer="<<size_ub<<", packed="<<w.size()
      ); // blame serialization<Fn>::ubound()
    }
  };
}}
#endif
