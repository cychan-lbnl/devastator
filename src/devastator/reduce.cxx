#include <devastator/reduce.hxx>

thread_local int deva::rdxn_incoming = 0;
thread_local void *deva::rdxn_acc;
thread_local void *deva::rdxn_ans;
