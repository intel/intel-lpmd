#!/bin/bash
#purpose: script to setup build environment

#sed -i -e 's/\r$//' *.sh

echo "executing setup_ubuntu script ..."

#BASEDIR=$(dirname $0)
#BASEDIR="$( cd "$( dirname "$0" )" && pwd )"
#echo "BASEDIR = $BASEDIR"

#auto config for software source packages
sudo apt install -y autoconf 
#autoconf-archive

#compilier
sudo apt install -y gcc

#dbus communiation dbus-1 dbus-glib-1
sudo apt -y -- install libglib2.0-dev libdbus-1-dev libdbus-glib-1-dev

#xml parsing
sudo apt -y -- install libxml2-dev

#netlink protocol based Linux kernel interfaces
sudo apt -y -- install libnl-3-dev libnl-genl-3-dev

#systemd interface
sudo apt -y -- install libsystemd-dev

#docs
sudo apt -y -- install gtk-doc-tools

#cmake build
sudo apt install -y cmake

#build framework
sudo apt -y -- install libcap-dev libssl-dev

#build plugins
#sudo apt install -y nlohmann-json3-dev

#bundle as zip, tar
sudo apt install -y zip tar

sudo apt install -y dos2unix

sudo apt autoremove

#cd $BASEDIR
