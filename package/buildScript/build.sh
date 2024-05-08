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

########################## build environment setup - BEGIN ####################################
BASEDIR="$( cd "$( dirname "$0" )" && pwd )"
echo $BASEDIR
SETUPSCRIPT_PATH=$BASEDIR/../setupScript
if [ -d "$SETUPSCRIPT_PATH" ]; then
    echo "Setup: $SETUPSCRIPT_PATH"
	pushd $SETUPSCRIPT_PATH
    
    chmod +x *.sh
    sed -i -e 's/\r$//' *.sh
    
    #check distro
    if test -f /etc/lsb-release; then
        echo "ubuntu!"
        source $SETUPSCRIPT_PATH/setup_ubuntu.sh || print_error
    fi
    if test -f /etc/redhat-release; then
        echo "redhat!"
        source $SETUPSCRIPT_PATH/setup_redhat.sh || print_error
    fi
    
	popd
    echo "Setup: Done"
fi
########################## build environment setup - END ####################################

########################## build program - BEGIN ##############################################
#BUILD_DIR="build"
#mkdir -p $BUILD_DIR
#cd $BUILD_DIR && cmake -G"Unix Makefiles" ../.. -DCMAKE_BUILD_TYPE=Release && make -j $(nproc) || print_error
BUILDSCRIPT_PATH=$BASEDIR/../../
pushd $BUILDSCRIPT_PATH
./autogen.sh prefix=/
make
#sudo make install
popd
########################## build program - END ##############################################
