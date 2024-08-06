
#!/bin/sh
# purpose: script to generate deb installation package

#Copyright (C) 2024 Intel Corporation
#SPDX-License-Identifier: GPL-3.0-only

set -e

RED='\033[0;31m'
NC='\033[0m'

print_error() {
    echo -e "${RED}*************** !!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!! ****************"
    echo "*************** WARNING: Error in Packaging Process ****************"
    echo -e "*************** !!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!! ****************${NC}"
    exit 1
}

trap '[ $? -eq 0 ] && exit 0 || print_error' EXIT

#BASEDIR=$(dirname $0)
BASEDIR="$( cd "$( dirname "$0" )" && pwd )"
echo $BASEDIR

echo "Prepare..."

PKG_CATEGORY="OPT"
PKG_FOLDERNAME="ILEO"
BASE=pkg.$PKG_CATEGORY.$PKG_FOLDERNAME
MAJOR="0"
MINOR="10"
ARCH="x86_64"
BUILD_DATE=$(date +'%y%m%d')
#DISTRO_CODENAME=$(cat /etc/lsb-release |grep DISTRIB_CODENAME|cut -d"=" -f2)
#VERSION=$MAJOR.$MINOR.$BUILD_DATE-$DISTRO_CODENAME
VERSION=$MAJOR.$MINOR.$BUILD_DATE

FILENAME="$BASE-$VERSION-$ARCH"

if [ $(uname -r | sed -n 's/.*\( *Microsoft *\).*/\1/ip') ];
then
    #WSL VM
    SOURCEFOLDER=/home/$USER/$FILENAME
else
	#native linux 
    SOURCEFOLDER=$BASEDIR/$FILENAME
fi

# make required directories
rm -fr $SOURCEFOLDER
mkdir -p $SOURCEFOLDER

#moved to use the script.
PACK_BINARY_IN_DEB=1
if [ -z "$1" ]; then
	echo "no input param"
else
	PACK_BINARY_IN_DEB=0
fi

#mkdir -p $SOURCEFOLDER/usr/share/ia_pkg/$PKG_FOLDERNAME

# copy debian files, this project have all preinst, prerm, postinst, postrm files
mkdir -p $SOURCEFOLDER/DEBIAN

#generate control file programatically.
set -o noclobber
echo "Section: base" > $SOURCEFOLDER/DEBIAN/control
echo "Priority: optional" >> $SOURCEFOLDER/DEBIAN/control
echo "Package: debkit-"$BASE >> $SOURCEFOLDER/DEBIAN/control
echo "Version: "$VERSION >> $SOURCEFOLDER/DEBIAN/control
echo "Maintainer: intel.com" >> $SOURCEFOLDER/DEBIAN/control
echo "Architecture: amd64" >> $SOURCEFOLDER/DEBIAN/control
#echo "Depends: tuned" >> $SOURCEFOLDER/DEBIAN/control

if [ -d "$BASEDIR/DEBIAN" ]; then
	#copy post and pre install/remove scripts if any
	cp -r $BASEDIR/DEBIAN/* $SOURCEFOLDER/DEBIAN/
else
	if [ $PACK_BINARY_IN_DEB -eq 0 ]; then
	
	echo "Description: Package sync, build and install ILEO which dynamically changes soc configuration parameters [EPP/EPB] to optimize the device performance and battery life" >> $SOURCEFOLDER/DEBIAN/control
	
		cat <<EOT >> $SOURCEFOLDER/DEBIAN/postinst
#script to run post install
/usr/share/ia_pkg/$PKG_FOLDERNAME/install.sh &
EOT
		chmod 755 $SOURCEFOLDER/DEBIAN/postinst
	else
		echo "Description: Package that dynamically changes soc configuration parameters [EPP/EPB] to optimize the device performance and battery life" >> $SOURCEFOLDER/DEBIAN/control
	fi
fi

#cat $SOURCEFOLDER/DEBIAN/control
chmod 755 $SOURCEFOLDER/DEBIAN/*

#Copy tar file, bundle-zip_tar.sh
if [ -d "$BASEDIR/bundle" ]; then 
	TAR_FILE=$(find $BASEDIR/bundle -type f -iname *.tar.gz)
	echo "tar file = $TAR_FILE"
	#to avoid multiple .gz file exist in the installation directory, copy to a unified name 
	cp $TAR_FILE $BASEDIR/bundle/pkg.opt.hepo.x86_64.tar.gz
	
	mkdir -p $SOURCEFOLDER/usr/share/ia_pkg/hepo
	#cp $TAR_FILE $SOURCEFOLDER/usr/share/ia_pkg/hepo/
	cp $BASEDIR/bundle/pkg.opt.hepo.x86_64.tar.gz $SOURCEFOLDER/usr/share/ia_pkg/hepo/
	#remove the copied one so not impacting the tar file 
	rm $BASEDIR/bundle/pkg.opt.hepo.x86_64.tar.gz	
	#cp bundle/*.gz $SOURCEFOLDER/usr/share/ia_pkg/hepo/pkg.opt.hepo.x86_64.tar.gz
fi

#build deb
#mkdir -p $BASEDIR/bundle/
dpkg-deb --build $SOURCEFOLDER/
sha512sum $SOURCEFOLDER.deb > $BASEDIR/bundle/$FILENAME.deb.sha512sum.txt
mv $SOURCEFOLDER.deb $BASEDIR/bundle/

#clean
rm -fr $SOURCEFOLDER

echo "Program exited"
