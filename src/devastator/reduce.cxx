#include <devastator/reduce.hxx>

__thread int deva::detail::reduce_incoming = 0;
__thread void *deva::detail::reduce_acc;
__thread void *deva::detail::reduce_ans;

__thread int deva::detail::scan_reduce_received = 0;
__thread void *deva::detail::scan_reduce_accs;
__thread void *deva::detail::scan_reduce_ans;
