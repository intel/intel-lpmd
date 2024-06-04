#!/bin/bash
# purpose: script to setup build environment

#sudo yum install gcc gcc-c++ kernel-devel make
#sudo dnf group install "Development Tools"

cat /etc/redhat-release

sudo dnf install -y automake autoconf-archive
sudo dnf install -y gcc
sudo dnf install -y glib-devel dbus-glib-devel
sudo dnf install -y libxml2-devel
sudo dnf install -y libnl3-devel
sudo dnf install -y systemd-devel
sudo dnf install -y gtk-doc

sudo dnf install -y clang cmake 
#sudo dnf install llvm-toolset

sudo dnf install -y dos2unix

#sudo dnf install libcap-devel openssl-devel nlohmann-json3-devel
sudo dnf install -y libcap-devel openssl-devel
sudo dnf install -y json-devel

#bundle
sudo dnf install -y zip tar

#rpm build
sudo dnf install -y rpmdevtools rpmlint
