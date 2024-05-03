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


#BASEDIR=$(dirname $0)
BASEDIR="$( cd "$( dirname "$0" )" && pwd )"
echo $BASEDIR

echo "Prepare..."
sudo apt install -y zip tar

PKG_NAME="EPPprofile"
PKG_CATEGORY="OPT"
BASE=pkg.$PKG_CATEGORY.$PKG_NAME
MAJOR="2"
MINOR="01"
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

echo "Create..."
# make required directories
rm -fr $SOURCEFOLDER
mkdir -p $SOURCEFOLDER/profiles

# copy profiles
cp -r $BASEDIR/../profiles/intel* $SOURCEFOLDER/profiles
chmod +x $SOURCEFOLDER/profiles/intel*/*.sh

#build source code 
BUILDSCRIPT_PATH=$BASEDIR/../buildScript
if [ -d "$BUILDSCRIPT_PATH" ]; then
    echo "building: $BUILDSCRIPT_PATH"

	pushd $BUILDSCRIPT_PATH
    chmod +x build.sh
    sed -i -e 's/\r$//' build.sh
    source build.sh || print_error
	popd 
	
    echo "copy binaries to target folder ..."
    BUILDOUT_PATH=$BUILDSCRIPT_PATH/build/bin

    cp -r $BUILDOUT_PATH/* $SOURCEFOLDER/    
fi

#Copy binaries
#cp $PKG_BUILD_DIR/bin/eco_linux_x86_64 $SOURCEFOLDER/


#copy deploy and rollback script.
cp $BASEDIR/../deploy.sh $SOURCEFOLDER/
cp $BASEDIR/../rollback.sh $SOURCEFOLDER/
chmod +x $SOURCEFOLDER/*.sh

#copy license, user guide
cp $BASEDIR/../release_notes.txt $SOURCEFOLDER/
cp $BASEDIR/../userGuide.txt $SOURCEFOLDER/
cp $BASEDIR/../*.pdf $SOURCEFOLDER/

#zip/tar folder
mkdir -p $BASEDIR/bundle
tar -czvf $FILENAME.tar.gz $SOURCEFOLDER
sha512sum $FILENAME.tar.gz > $BASEDIR/bundle/$FILENAME.tar.gz.sha512sum.txt
mv $FILENAME.tar.gz $BASEDIR/bundle/

zip -vr $FILENAME.zip $SOURCEFOLDER/
sha512sum $FILENAME.zip > $BASEDIR/bundle/$FILENAME.zip.sha512sum.txt
mv $FILENAME.zip $BASEDIR/bundle/

echo "cleanup..."
rm -fr $SOURCEFOLDER

echo "done"

