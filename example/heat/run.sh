#!/bin/bash

export debug=1
export ranks=4

source build.sh

export cells=101
export alpha=0.01
export value_thresh=0.1
export local_delta_t=1
export sim_end_time=1000
export sim_conf="source"

export verbose=0

${exe}
