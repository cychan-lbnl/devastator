#!/bin/bash

export debug=1
export ranks=4
export verbose=0

# export cells=100
# export sim_end_time=1000
export cells=100
export sim_end_time=1000

export alpha=0.01
export value_thresh=0.1
export local_delta_t=1

source build.sh

${exe}
