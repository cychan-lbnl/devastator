#!/bin/bash

# run this file from the devastator/test/heat directory

# initialize build system
pushd ../../; source sourceme; popd

# compile
exe=$(brutal ranks=4 exe heat_qss1.cxx)
