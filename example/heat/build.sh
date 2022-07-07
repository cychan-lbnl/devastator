#!/bin/bash

# run this file from the devastator/test/heat directory

# initialize build system
pushd ../../; source sourceme; popd

# compile
: "${debug:=1}"
: "${ranks:=1}"
exe=$(brutal debug=${debug} ranks=${ranks} exe heat.cxx)
