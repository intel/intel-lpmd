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
rpmlint $BUNDLEDIR/hepo.spec

echo "copy files...."
cp $BUNDLEDIR/hepo.spec $SOURCEFOLDER/rpmbuild/SPECS/
#cp $BUNDLEDIR/bundle/pkg.OPT.HEPO-0.05.240605-x86_64.tar.gz $SOURCEFOLDER/rpmbuild/SOURCES/
#cd $SOURCEFOLDER/rpmbuild/SOURCES/

TAR_FILE=$(find $BUNDLEDIR/bundle -type f -iname *.tar.gz)
echo "tar file = $TAR_FILE"

rm -fr $BUNDLEDIR/tmp/
mkdir -p $BUNDLEDIR/tmp/
cd $BUNDLEDIR/tmp/

tar -xvf $TAR_FILE
cp -r ./pkg*/* $SOURCEFOLDER/rpmbuild/BUILD/
chmod +x $SOURCEFOLDER/rpmbuild/BUILD/*.sh
dos2unix $SOURCEFOLDER/rpmbuild/BUILD/*.sh
#ls $SOURCEFOLDER/rpmbuild/BUILD/

cd $SOURCEFOLDER/rpmbuild
rpmbuild -bb -v --target=noarch $SOURCEFOLDER/rpmbuild/SPECS/hepo.spec
ls -l $SOURCEFOLDER/rpmbuild/RPMS/noarch
cp $SOURCEFOLDER/rpmbuild/RPMS/noarch/*.rpm $BUNDLEDIR/bundle/

echo "cleanup..."
rm -fr $SOURCEFOLDER/rpmbuild
rm -fr $BUNDLEDIR/tmp/

echo "done"

