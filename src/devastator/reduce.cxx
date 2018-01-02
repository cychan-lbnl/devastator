#include "reduce.hxx"

thread_local int world::rdxn_incoming = 0;
thread_local void *world::rdxn_acc;
thread_local void *world::rdxn_ans;
