#!/bin/bash
#Purpose: build script

echo "List of all arg: $@"

POSITIONAL_ARGS=()

#-s for skip setup
SKIP_SETUP=0

while [[ $# -gt 0 ]]; do
  case $1 in
    -s|--skip_setup)
      shift # past argument
      SKIP_SETUP=1
      ;;
    -s|--searchpath)
      SEARCHPATH="$2"
      shift # past argument
      shift # past value
      ;;
    -*|--*)
      echo "Unknown option $1"
      exit 1
      ;;
    *)
      POSITIONAL_ARGS+=("$1") # save positional arg
      shift # past argument
      ;;
  esac
done

set -- "${POSITIONAL_ARGS[@]}" # restore positional parameters

RED='\033[0;31m'
NC='\033[0m'
function print_error() {
    echo -e "${RED}*************** !!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!! ****************"
    echo -e "*************** WARNING: Error in build Process ****************"
    echo -e "*************** !!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!! ****************${NC}"
    exit 1
}

#BASEDIR=$(dirname $0)
BASEDIR="$( cd "$( dirname "$0" )" && pwd )"
echo "BASEDIR = $BASEDIR"

########################## build environment setup - BEGIN ####################################
if [ $SKIP_SETUP -eq 0 ]; then
    echo "running setup"
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
else
    echo "skipped setup"
fi
########################## build environment setup - END ####################################

########################## build program - BEGIN ##############################################
#BUILD_DIR="build"
#mkdir -p $BUILD_DIR
#cd $BUILD_DIR && cmake -G"Unix Makefiles" ../.. -DCMAKE_BUILD_TYPE=Release && make -j $(nproc) || print_error
BUILDSCRIPT_PATH=$BASEDIR/../../
pushd $BUILDSCRIPT_PATH
sudo apt install -y dos2unix
dos2unix *.sh
dos2unix configure.ac
./autogen.sh prefix=/
make
#sudo make install
popd
########################## build program - END ##############################################
