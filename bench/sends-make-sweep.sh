#!/bin/bash

BRUTAL_KEEP_ENV=1 . ${BRUTAL_SITE}/sourceme

function loudly() {
  echo "$@" 1>&2
  "$@"
}

> sends-sweep.sh

for c in {1..16}; do
  c=$((2*c))
  for tmsg in spsc mpsc; do
    exe=$(loudly brutal ranks=$c tmsg=$tmsg exe sends.cxx)
    for msgs in 16 64 128; do
    for kbs in 8 16 32 64 128; do
    for miss in {0..8}; do
      miss=$((8*miss))
      env="msg_per_rank=$msgs kbs_per_rank=$kbs false_misses=$miss"
      echo echo $env srun -n 1 -c $c $exe >> sends-sweep.sh
      echo      $env srun -n 1 -c $c $exe >> sends-sweep.sh
    done; done; done
  done
done
