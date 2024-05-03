
#!/bin/sh
# Script generate deb installation package

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

PKG_CATEGORY="OPT"
PKG_FOLDERNAME="EPPprofile"
BASE=pkg.$PKG_CATEGORY.$PKG_FOLDERNAME
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

# make required directories
rm -fr $SOURCEFOLDER
mkdir -p $SOURCEFOLDER
mkdir -p $SOURCEFOLDER/usr/share/ia_pkg/$PKG_FOLDERNAME

# copy debian files, this project have all preinst, prerm, postinst, postrm files
mkdir -p $SOURCEFOLDER/DEBIAN

if [ -d "$BASEDIR/DEBIAN" ]; then
	#copy post and pre install/remove scripts if any
	cp -r $BASEDIR/DEBIAN/* $SOURCEFOLDER/DEBIAN/
fi

#generating control file dynamically.
set -o noclobber
echo "Section: base" > $SOURCEFOLDER/DEBIAN/control
echo "Priority: optional" >> $SOURCEFOLDER/DEBIAN/control
echo "Package: debkit-"$BASE >> $SOURCEFOLDER/DEBIAN/control
echo "Version: "$VERSION >> $SOURCEFOLDER/DEBIAN/control
echo "Maintainer: intel.com" >> $SOURCEFOLDER/DEBIAN/control
echo "Architecture: amd64" >> $SOURCEFOLDER/DEBIAN/control
echo "Depends: tuned intel-lpmd" >> $SOURCEFOLDER/DEBIAN/control
echo "Description: Package that dynamically changes the TuneD profile to optimize the device performance and battery life" >> $SOURCEFOLDER/DEBIAN/control

#cat $SOURCEFOLDER/DEBIAN/control
chmod 755 $SOURCEFOLDER/DEBIAN/*

cp $BASEDIR/../EPPprofile.install $SOURCEFOLDER/usr/share/ia_pkg/$PKG_FOLDERNAME/


#Copy binaries
mkdir -p $SOURCEFOLDER/usr/share/ia_pkg/$PKG_FOLDERNAME/
cp $PKG_BUILD_DIR/bin/eco_linux_x86_64 $SOURCEFOLDER/usr/share/ia_pkg/$PKG_FOLDERNAME/

# copy profiles
mkdir -p $SOURCEFOLDER/etc/tuned
cp -r ../profiles/* $SOURCEFOLDER/etc/tuned/
chmod +x $SOURCEFOLDER/etc/tuned/intel*/*.sh

#copy license, user guide
cp $BASEDIR/../release_notes.txt $SOURCEFOLDER/usr/share/ia_pkg/$PKG_FOLDERNAME
cp $BASEDIR/../userGuide.txt $SOURCEFOLDER/usr/share/ia_pkg/$PKG_FOLDERNAME
cp $BASEDIR/../*.pdf $SOURCEFOLDER/usr/share/ia_pkg/$PKG_FOLDERNAME

mkdir -p $BASEDIR/bundle/
dpkg-deb --build $SOURCEFOLDER/
sha512sum $SOURCEFOLDER.deb > $BASEDIR/bundle/$FILENAME.deb.sha512sum.txt
mv $SOURCEFOLDER.deb $BASEDIR/bundle/

rm -fr $SOURCEFOLDER

