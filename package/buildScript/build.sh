#!/bin/bash
#Purpose: build script

RED='\033[0;31m'
NC='\033[0m'
function print_error() {
    echo -e "${RED}*************** !!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!! ****************"
    echo -e "*************** WARNING: Error in build Process ****************"
    echo -e "*************** !!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!! ****************${NC}"
    exit 1
}

########################## build program - BEGIN ##############################################

BUILD_DIR="build"
mkdir -p $BUILD_DIR

cd $BUILD_DIR && cmake -G"Unix Makefiles" ../.. -DCMAKE_BUILD_TYPE=Release && make -j $(nproc) || print_error

########################## build program - END ##############################################
