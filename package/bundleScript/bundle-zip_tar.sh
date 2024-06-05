#!/bin/bash
# Script to generate zip/tar deployment package

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
MINOR="05"
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

#build source code 
BUILDSCRIPT_PATH=$BUNDLEDIR/../buildScript
if [ -d "$BUILDSCRIPT_PATH" ]; then
    echo "building: $BUILDSCRIPT_PATH"

	pushd $BUILDSCRIPT_PATH
    chmod +x build.sh
    sed -i -e 's/\r$//' build.sh
    source build.sh || print_error
	popd 
	
    #echo "copy binaries to target folder ..."
    #BUILDOUT_PATH=$BUILDSCRIPT_PATH/build/bin

    #cp -r $BUILDOUT_PATH/* $SOURCEFOLDER/    
fi

echo "after build, BUNDLEDIR: "
echo $BUNDLEDIR

echo "Create..."
# make required directories
rm -fr $SOURCEFOLDER
mkdir -p $SOURCEFOLDER/tuned-profile

# copy tuned-profile
cp -r $BUNDLEDIR/../tuned-profile/intel* $SOURCEFOLDER/tuned-profile
chmod +x $SOURCEFOLDER/tuned-profile/intel*/*.sh

#Copy binaries
cp $BUNDLEDIR/../../intel_lpmd $SOURCEFOLDER/
cp $BUNDLEDIR/../../tools/intel_lpmd_control $SOURCEFOLDER/

# copy man
cp $BUNDLEDIR/../../man/intel_lpmd_config.xml.5 $SOURCEFOLDER/
cp $BUNDLEDIR/../../man/intel_lpmd.8 $SOURCEFOLDER/

#copy config
cp $BUNDLEDIR/../data/platform_mtl.xml $SOURCEFOLDER/intel_lpmd_config.xml
cp $BUNDLEDIR/../../data/org.freedesktop.intel_lpmd.conf $SOURCEFOLDER/

#copy dbus service
cp $BUNDLEDIR/../../data/org.freedesktop.intel_lpmd.service $SOURCEFOLDER/

#copy service
cp $BUNDLEDIR/../../data/intel_lpmd.service $SOURCEFOLDER/

#copy deploy and rollback script.
cp $BUNDLEDIR/deploy.sh $SOURCEFOLDER/
cp $BUNDLEDIR/rollback.sh $SOURCEFOLDER/
chmod +x $SOURCEFOLDER/*.sh

#copy license, user guide
cp $BUNDLEDIR/../release-notes* $SOURCEFOLDER/
cp $BUNDLEDIR/../license/readme-license* $SOURCEFOLDER/
cp $BUNDLEDIR/../license/*.pdf $SOURCEFOLDER/
cp $BUNDLEDIR/../user-guide* $SOURCEFOLDER/

#zip/tar folder
sudo apt install -y tar
if [ -d "$BUNDLEDIR/bundle" ]; then
	rm -rf $BUNDLEDIR/bundle
fi
mkdir -p $BUNDLEDIR/bundle
 
tar -czvf $FILENAME.tar.gz $SOURCEFOLDER
sha512sum $FILENAME.tar.gz > $BUNDLEDIR/bundle/$FILENAME.tar.gz.sha512sum.txt
mv $FILENAME.tar.gz $BUNDLEDIR/bundle/

#sudo apt install -y zip
#zip -vr $FILENAME.zip $SOURCEFOLDER/
#sha512sum $FILENAME.zip > $BUNDLEDIR/bundle/$FILENAME.zip.sha512sum.txt
#mv $FILENAME.zip $BUNDLEDIR/bundle/

echo "cleanup..."
rm -fr $SOURCEFOLDER

echo "done"

