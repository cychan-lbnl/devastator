#!/bin/bash

export debug=1
export ranks=4

if true; then
  export cells=100
  export alpha=0.01
  export value_thresh=0.1
  export local_delta_t=1
  export max_advance_delta_t=1000
  export sim_end_time=1000
else
  export cells=2
  export alpha=0.01
  export value_thresh=0.1
  export local_delta_t=1
  export max_advance_delta_t=10
  export sim_end_time=10
fi

source build.sh

${exe}