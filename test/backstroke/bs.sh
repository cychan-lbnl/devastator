#!/bin/bash

: ${TOOL1:=g++}
: ${TOOL2:=backstroke}
: ${TARGET=phold-bs-state.cxx}
: ${BS_TOP="${HOME}/projects/external/backstroke"}

BS_SRCDIR="${BS_TOP}/src"

BS_INPUT_LANG_STANDARD=c++14
BS_INPUT_LANG_STANDARD_OPTION=-std=c++14
BS_OUTPUT_LANG_STANDARD=c++14
BS_INPUT_LANG_STANDARD_OPTION=-std=c++14

rm -f *.pass.C *.fail.C *.o *.pp.C *.ti

# the bs runtime lib is not used/linked with these tests yet.
#BS_INCLUDE=-I$BACKSTROKE_RUNTIMELIB
BS_INCLUDE=
BS_BACKEND_NR=1
BS_BACKEND=--backend=${BS_BACKEND_NR}
BS_ACCESS_MODE="--access-mode=1"
BACKSTROKE=${BS_SRCDIR}/backstroke
BS_RTLIB_INCLUDE_OPTIONS=-I${BS_SRCDIR}/rtss
BS_STL_SUPPORT=
BS_RESTRICTIONS=--ignore-unions
BS_LIB_INCLUDE_OPTION_CHECK="-include backstroke/rtss.h"
BS_LIB_INCLUDE_OPTION="--rtss-header"
#GCC_OPTIONS="-fabi-version=6" # only required for g++ 4.9
GCC_OPTIONS="-fpermissive"

echo "BS_RTLIB_INCLUDE_OPTIONS: ${BS_RTLIB_INCLUDE_OPTIONS}"

echo "-----------------------------------------------------------------"
echo "Backstroke installation: ${BS_SRCDIR}"
echo "-----------------------------------------------------------------"
echo "C++ input/output language standard: ${BS_INPUT_LANG_STANDARD}/${BS_OUTPUT_LANG_STANDARD}"
echo "-----------------------------------------------------------------"
echo "Note: using g++ options: $GCC_OPTIONS"
echo "-----------------------------------------------------------------"

cmd="$BACKSTROKE --no-preprocessor $BS_RTLIB_INCLUDE_OPTIONS $BS_RESTRICTIONS $BS_BACKEND $BS_ACCESS_MODE $BS_INCLUDE --no-transform ${TARGET} $BS_INPUT_LANG_STANDARD_OPTION"
echo
echo $cmd
echo
$cmd
echo

if [ $? -eq 0 ]; then
  cmd="$BACKSTROKE --preprocessor $BS_LIB_INCLUDE_OPTION $BS_RTLIB_INCLUDE_OPTIONS $BS_RESTRICTIONS $BS_BACKEND $BS_ACCESS_MODE $BS_INCLUDE --stats-csv-file=${TARGET}.csv ${TARGET} $BS_INPUT_LANG_STANDARD_OPTION"
  echo
  echo $cmd
  echo
  $cmd
  echo
fi

cmd="${TOOL1} -c $BS_OUTPUT_LANG_STANDARD_OPTION $BS_RTLIB_INCLUDE_OPTIONS backstroke_${TARGET} -w -Wfatal-errors $BS_STL_SUPPORT $GCC_OPTIONS"
echo
echo $cmd
$cmd
echo
