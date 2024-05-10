#!/bin/bash

#Copyright (C) 2024 Intel Corporation
#This software and the related documents are Intel copyrighted materials, and your use of them is governed by the express license under which they were provided to you ("License"). 
#Unless the License provides otherwise, you may not use, modify, copy, publish, distribute, disclose or transmit this software or the related documents without Intel's prior written permission.
#This software and the related documents are provided as is, with no express or implied warranties, other than those that are expressly stated in the License.

#Purpose: copy tuned profiles and restart; activate profile
#Version:
#Parameters

echo on

#check the platform, only MTL is supported
MTL=0
output=$(grep -oP "model name\\K.*" /proc/cpuinfo | head -1)
#output=": Core Ultra 9 125U" #test
#echo $output

if [ ! -z "$output" ]; then
	SUB='Ultra'
	if echo "$output" | grep -q -E "$SUB"; then
		model=${output##*:}
		if [ ! -z "$model" ]; then
			gen=${model##*" "} 
			if [ ! -z "$model" ]; then
				#echo "gen is $gen"
				firstchar="$(echo $gen | head -c 1)"
				#echo "firstchar is $firstchar"
				if [ ! -z "$firstchar" ]; then
					firstchar=$((firstchar+0))
							
					if [ $firstchar -ge 1 ]; then
						MTL=1 
					fi
				fi		
			fi
		fi 		
	else 
		MTL=9
	fi			
fi

if [ $MTL = 9 ]; then
	echo "*********Error: The platform is not supported and the installation process will be aborted*********"
	exit 1
elif [ $MTL = 0 ]; then
	echo "*********Warning: The platform is not supported but the installation process will proceed*********"
fi


OS=""
if [ -f /etc/os-release ]; then
	. /etc/os-release
	OS=$NAME
	#echo $OS
fi

activeprofile=""

if test -f /usr/sbin/tuned-adm; then
    output=$(echo $(tuned-adm active) | grep -o ":")
    if [ ! -z "$output" ]; then 
        activeprofile=$(echo $(tuned-adm active) | cut -d ":" -f 2)        
        if echo $activeprofile | grep -q "intel-best"; then
        	activeprofile=""
        fi        
    fi   
else 
	#tuned not installed, notify user
	echo "Tuned is not detected, please install tuned and run the installation again"
	exit 1	
fi

echo "**************** executing tasks ***************************"
BASEDIR="$( cd "$( dirname "$0" )" && pwd )"
#echo $BASEDIR

echo "**************** copy binary ***************************"
#Copy binaries
cp intel_lpmd /usr/bin/
cp intel_lpmd_control /usr/bin/

# copy man
if [ ! -d "/usr/local/share/man/man5" ]; then
    mkdir -p /usr/local/share/man/man5
fi    
cp intel_lpmd_config.xml.5 /usr/local/share/man/man5/

if [ ! -d "/usr/local/share/man/man8" ]; then
    mkdir -p /usr/local/share/man/man8
fi    
cp intel_lpmd.8 /usr/local/share/man/man8/

#copy config
if [ ! -d "/etc/intel_lpmd" ]; then
    mkdir -p /etc/intel_lpmd
fi 
cp intel_lpmd_config.xml /etc/intel_lpmd/

if [ ! -d "/etc/dbus-1/system.d/" ]; then
    mkdir -p /etc/dbus-1/system.d/
fi
cp org.freedesktop.intel_lpmd.conf /etc/dbus-1/system.d/

#copy dbus service
if [ ! -d "/usr/local/share/dbus-1/system-services" ]; then
    mkdir -p /usr/local/share/dbus-1/system-services
fi
cp org.freedesktop.intel_lpmd.service /usr/local/share/dbus-1/system-services/

#copy service
cp intel_lpmd.service /usr/lib/systemd/system/

#echo "**************** copy license and user guide information ***************************"
#cp $BASEDIR/*.txt $BINDIR/
#cp $BASEDIR/*.pdf $BINDIR/

echo "**************** copy profiles ***************************"
sudo chmod +x $BASEDIR/tuned-profile/intel*/*.sh
sudo cp -r $BASEDIR/tuned-profile/intel* /etc/tuned/
chown root:root /etc/tuned/intel*
chown root:root /etc/tuned/intel*/*

echo "**************** PPD ***************************"
sudo systemctl stop power-profiles-daemon
sudo systemctl mask power-profiles-daemon

echo "**************** re-start daemon ***************************"
sudo systemctl restart tuned

#make tuned persistent across reboots.
sudo systemctl enable --now tuned

echo "**************** activate profile ***************************"
tuned-adm list | grep "intel*"

if [ ! -z "$activeprofile" ]; then
	#if the activeprofile already contains the intel profile, set to intel-best_performance_mode
	if echo "$activeprofile" | grep -q "intel"; then
		tuned-adm profile intel_hepo
	else #add to the active profiles
		tuned-adm profile intel_hepo $activeprofile
	fi
else 
	tuned-adm profile intel_hepo
fi 

#echo "**************** 4. verify ***************************"
tuned-adm active

sudo systemctl mask power-profiles-daemon

echo "**************** done ***************************"
