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

PKG_CATEGORY="OPT"
PKG_FOLDERNAME="HEPO"
BASE=pkg.$PKG_CATEGORY.$PKG_FOLDERNAME
MAJOR="0"
MINOR="04"
ARCH="x86_64"
BUILD_DATE=$(date +'%y%m%d')
#DISTRO_CODENAME=$(cat /etc/lsb-release |grep DISTRIB_CODENAME|cut -d"=" -f2)
#VERSION=$MAJOR.$MINOR.$BUILD_DATE-$DISTRO_CODENAME
VERSION=$MAJOR.$MINOR.$BUILD_DATE
FILENAME="$BASE-$VERSION-$ARCH"
PKG_BUILD_DIR=../buildScript/build

if [ $(uname -r | sed -n 's/.*\( *Microsoft *\).*/\1/ip') ];
then
    #WSL VM
    SOURCEFOLDER=/home/$USER/$FILENAME
else
	#native linux 
    SOURCEFOLDER=$FILENAME
fi
echo "Target folder: $SOURCEFOLDER"

#create directory
#rpmdev-setuptree
mkdir -p rpmbuild/{BUILD,BUILDROOT,RPMS,SOURCES,SPECS,SRPMS}

#cd
cd rpmbuild

#copy files
cp hepo.spec SPECS/
cp $BUNDLEDIR/bundle/*.tar.gz SOURCES/

rpmbuild -bb -v SPECS/hepo.spec
ls RPMS/noarch/*


echo "cleanup..."
rm -fr $SOURCEFOLDER

echo "done"

