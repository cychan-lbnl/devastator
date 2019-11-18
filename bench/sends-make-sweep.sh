#!/bin/bash

BRUTAL_KEEP_ENV=1 . ${BRUTAL_SITE}/sourceme

function loudly() {
  echo "$@" 1>&2
  "$@"
}

wall_secs=3

echo "#!/bin/bash" > sends-sweep.sh
echo "#SBATCH --qos=regular" >> sends-sweep.sh
echo "#SBATCH --constraint=haswell" >> sends-sweep.sh
echo "#SBATCH --nodes=1" >> sends-sweep.sh
echo "#SBATCH --time=$((1 + 4*2*2*3*4*(8 + wall_secs)/60))" >> sends-sweep.sh
echo >> sends-sweep.sh

echo "export wall_secs=$wall_secs" >> sends-sweep.sh

for c in 8 16 24 32; do
  for tmsg in spsc mpsc; do
    exe=$(loudly brutal ranks=$c tmsg=$tmsg exe sends.cxx)
    for msgs in 16 128; do
    for kbs in 8 32 128; do
    for miss in 0 16 32 64; do
      env="msg_per_rank=$msgs kbs_per_rank=$kbs false_misses=$miss"
      echo echo $env srun -N 1 -n 1 -c $c $exe >> sends-sweep.sh
      echo      $env srun -N 1 -n 1 -c $c $exe >> sends-sweep.sh
    done; done; done
  done
done

chmod a+x sends-sweep.sh
