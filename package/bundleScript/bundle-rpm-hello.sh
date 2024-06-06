#!/bin/bash
# Script to generate rpm deployment package

set -e

RED='\033[0;31m'
NC='\033[0m'

print_error() {
    echo -e "${RED}*************** !!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!! ****************"
    echo "*************** WARNNING: Error in Packaging Process ****************"
    echo -e "*************** !!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!! ****************${NC}"
    exit 1
}

trap '[ $? -eq 0 ] && exit 0 || print_error' EXIT


#BUNDLEDIR=$(dirname $0)
BUNDLEDIR="$( cd "$( dirname "$0" )" && pwd )"
echo $BUNDLEDIR

echo "Prepare..."

if [ $(uname -r | sed -n 's/.*\( *Microsoft *\).*/\1/ip') ];
then
    #WSL VM
	SOURCEFOLDER=/home/$USER
else
	#native linux 
	SOURCEFOLDER=$BUNDLEDIR
fi
echo "Target folder: $SOURCEFOLDER"

#create directory
rm -fr $SOURCEFOLDER/rpmbuild
mkdir -p $SOURCEFOLDER
cd $SOURCEFOLDER

rpmdev-setuptree #creates folder
#mkdir -p $SOURCEFOLDER/rpmbuild/{BUILD,BUILDROOT,RPMS,SOURCES,SPECS,SRPMS}

dos2unix $BUNDLEDIR/*.spec
rpmlint $BUNDLEDIR/hello-world.spec

echo "copy files...."
cp $BUNDLEDIR/hello-world.spec $SOURCEFOLDER/rpmbuild/SPECS/

cd $SOURCEFOLDER/rpmbuild

rpmbuild -bb -v --target=noarch $SOURCEFOLDER/rpmbuild/SPECS/hello-world.spec
ls -l $SOURCEFOLDER/rpmbuild/RPMS/noarch
cp $SOURCEFOLDER/rpmbuild/RPMS/noarch/*.rpm $BUNDLEDIR/bundle/

echo "cleanup..."
rm -fr $SOURCEFOLDER/rpmbuild

echo "done"

