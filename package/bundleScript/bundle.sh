#!/bin/bash
#purpose: master script to bundle all types.


RED='\033[0;31m'
NC='\033[0m'

print_error() {
    echo "${RED}*************** !!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!! ****************"
    echo "*************** WARNNING: Error in Packaging Process ****************"
    echo "*************** !!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!! ****************${NC}"
    exit 1
}
trap '[ $? -eq 0 ] && exit 0 || print_error' EXIT


echo "bundle start"

BASEDIR="$( cd "$( dirname "$0" )" && pwd )"
echo $BASEDIR

sed -i -e 's/\r$//' $BASEDIR/*.sh
chmod +x $BASEDIR/*.sh

sed -i -e 's/\r$//' $BASEDIR/bundle-zip_tar.sh
source $BASEDIR/bundle-zip_tar.sh

#check distro
if test -f /etc/lsb-release; then
	echo "ubuntu!"
	source $BASEDIR/bundle-deb.sh || print_error
fi
if test -f /etc/redhat-release; then
	echo "redhat!"
	source $BASEDIR/bundle-rpm.sh || print_error
fi


echo "bundle done"



