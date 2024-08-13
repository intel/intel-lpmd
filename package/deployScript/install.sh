#!/bin/bash
#purpose: Script to do system requirements check, sync github source code, setup build environment, build from source and install on target machine. cleanup after installation.
#Version: 0.2
#Parameters:

#Copyright (C) 2024 Intel Corporation
#SPDX-License-Identifier: GPL-3.0-only

set -e

RED='\033[0;31m'
NC='\033[0m'
function print_error() {
    echo -e "${RED}*************** !!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!! ****************"
    echo -e "*************** WARNING: Error in Build Process ****************"
    echo -e "*************** !!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!! ****************${NC}"
    exit 1
}

MINIMUM_STORAGE_REQUIRED=$((10*1024*1024)) #10MB

DISK_SIZE_FREE_BEGIN=$(df -kh . | tail -n1 | awk '{print $4}')
echo "Available storage = $DISK_SIZE_FREE_BEGIN"

DISK_SIZE_FREE_BEGIN_NUMERIC=$(echo $DISK_SIZE_FREE_BEGIN | sed 's/[^0-9.]//g')
echo "Available storage in numeric = $DISK_SIZE_FREE_BEGIN_NUMERIC"

if [ $DISK_SIZE_FREE_BEGIN_NUMERIC -ge $MINIMUM_STORAGE_REQUIRED ]; then
	echo "Requirements not met."
fi

BASEDIR="$( cd "$( dirname "$0" )" && pwd )"
echo $BASEDIR

#create working dir.
WORKING_FOLDER="$BASEDIR/src_tmp"
mkdir -p $WORKING_FOLDER

#step: sync source
pushd $WORKING_FOLDER
git clone https://github.com/intel/intel-lpmd
#extract the repo name
REPO=$(basename "https://github.com/intel/intel-lpmd" .git)
cd "$REPO"
git checkout tags/v0.0.6 -b main

PROJECT_FOLDER=$WORKING_FOLDER/intel-lpmd
[ ! -d "$PROJECT_FOLDER" ] && echo "Directory $PROJECT_FOLDER DOES NOT exists."

pushd $PROJECT_FOLDER

#step: setup env & build
echo "set up development environment & build ..."
BUILDSCRIPT_PATH=$PROJECT_FOLDER/package/buildScript/
pushd $BUILDSCRIPT_PATH #fix otherwise it messes with basedir in other script.
dos2unix *.sh
chmod +x *.sh
source build.sh || print_error
popd #BUILDSCRIPT_PATH

[ ! -d "$BUILDSCRIPT_PATH/out" ] && echo "Directory $BUILDSCRIPT_PATH/out DOES NOT exists."

#step: bundle tar
echo "bundle binaries ..."
BUNDLESCRIPT_PATH=$PROJECT_FOLDER/package/bundleScript/
pushd $BUNDLESCRIPT_PATH
dos2unix *.sh
chmod +x *.sh
source bundle-zip_tar.sh || print_error
popd #BUNDLESCRIPT_PATH

#step: Deploy binaries
echo "deploy binaries ..."
DEPLOYSCRIPT_PATH=$WORKING_FOLDER/intel-lpmd/package/bundleScript/
pushd $DEPLOYSCRIPT_PATH
dos2unix *.sh
chmod +x *.sh
source deploy.sh || print_error
popd #DEPLOYSCRIPT_PATH

popd #PROJECT_FOLDER

popd #WORKING_FOLDER

#delete working dir
rm -fr $WORKING_FOLDER

DISK_SIZE_FREE_END=$(df -kh . | tail -n1 | awk '{print $4}')
echo "Available storage = $DISK_SIZE_FREE_END"

echo "Used storage = $DISK_SIZE_FREE_END - $DISK_SIZE_FREE_BEGIN"
