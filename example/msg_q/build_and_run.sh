#!/bin/bash

# run this file from the devastator/example/msg_q directory

# initialize build system
pushd ../../; source sourceme; popd

# compile
exe=$(brutal ranks=4 exe msg_q.cxx)

# run
$exe
