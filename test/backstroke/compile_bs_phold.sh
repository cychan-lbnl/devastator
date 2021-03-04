#!/bin/bash

set -e

: ${BS_TOP="${HOME}/projects/external/backstroke"}
BS_SRCDIR="${BS_TOP}/src"

cp phold-bs-state.hxx backstroke_phold-bs-state.hxx
TARGET=phold-bs-state.cxx ./bs.sh 

export PP_DIRS="${BS_SRCDIR}/rtss"
export CXX_CGFLAGS="-fpermissive"
export LIB_DIRS="${BS_SRCDIR}/rtss"
export LIB_NAMES="rtss"

: ${world=gasnet}
: ${procs=1}
: ${workers=1}
: ${debug=0}

export world procs workers debug

python3 "${BRUTAL}/tool.py" exe phold-bs.cxx
echo
echo
