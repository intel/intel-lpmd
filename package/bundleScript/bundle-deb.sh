
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
PKG_FOLDERNAME="HEPO"
BASE=pkg.$PKG_CATEGORY.$PKG_FOLDERNAME
MAJOR="0"
MINOR="01"
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
    SOURCEFOLDER=$FILENAME
fi

# make required directories
rm -fr $SOURCEFOLDER
mkdir -p $SOURCEFOLDER
#mkdir -p $SOURCEFOLDER/usr/share/ia_pkg/$PKG_FOLDERNAME

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
echo "Depends: tuned" >> $SOURCEFOLDER/DEBIAN/control
echo "Description: Package that dynamically changes the TuneD profile to optimize the device performance and battery life" >> $SOURCEFOLDER/DEBIAN/control

#cat $SOURCEFOLDER/DEBIAN/control
chmod 755 $SOURCEFOLDER/DEBIAN/*

#Copy binaries
mkdir -p $SOURCEFOLDER/usr/bin
cp ../../intel_lpmd $SOURCEFOLDER/usr/bin/
cp ../../tools/intel_lpmd_control $SOURCEFOLDER/usr/bin/

# copy profiles
mkdir -p $SOURCEFOLDER/etc/tuned
cp -r ../tuned-profile/* $SOURCEFOLDER/etc/tuned/

# copy man
mkdir -p $SOURCEFOLDER/usr/local/share/man/man5
cp ../../man/intel_lpmd_config.xml.5 $SOURCEFOLDER/usr/local/share/man/man5
mkdir -p $SOURCEFOLDER/usr/local/share/man/man8
cp ../../man/intel_lpmd.8 $SOURCEFOLDER/usr/local/share/man/man8

#copy config
mkdir -p $SOURCEFOLDER/etc/intel_lpmd
cp ../../data/intel_lpmd_config.xml $SOURCEFOLDER/etc/intel_lpmd/

mkdir -p $SOURCEFOLDER/etc/dbus-1/system.d
cp ../../data/org.freedesktop.intel_lpmd.conf $SOURCEFOLDER/etc/dbus-1/system.d/

#copy dbus service
mkdir -p $SOURCEFOLDER/usr/local/share/dbus-1/system-services
cp ../../data/org.freedesktop.intel_lpmd.service $SOURCEFOLDER/usr/local/share/dbus-1/system-services

#copy service
mkdir -p $SOURCEFOLDER/usr/lib/systemd/system
cp ../../data/intel_lpmd.service $SOURCEFOLDER/usr/lib/systemd/system

#copy license, user guide

mkdir -p $BASEDIR/bundle/
dpkg-deb --build $SOURCEFOLDER/
sha512sum $SOURCEFOLDER.deb > $BASEDIR/bundle/$FILENAME.deb.sha512sum.txt
mv $SOURCEFOLDER.deb $BASEDIR/bundle/

rm -fr $SOURCEFOLDER

