#!/bin/bash

#Copyright (C) 2024 Intel Corporation
#SPDX-License-Identifier: GPL-3.0-only

ppdstatus=$(echo $(sudo systemctl status power-profiles-daemon | grep "active (running)"))
if [ ! -z "$ppdstatus" ]; then 
    installasservice=1
else 
    installasservice=0
fi

#function cleanup
cleanup()
{
    #remove the profiles    
	if [ -d "/etc/tuned/intel_ileo" ]; then	
		sudo rm -r /etc/tuned/intel*        
	fi
		
	rm /usr/bin/intel_lpmd
    rm /usr/bin/intel_lpmd_control

    rm /usr/local/share/man/man5/intel_lpmd_config.xml.5
    rm /usr/local/share/man/man8/intel_lpmd.8

    sudo rm -r /etc/intel_lpmd/
    rm /etc/dbus-1/system.d/org.freedesktop.intel_lpmd.conf

    rm /usr/local/share/dbus-1/system-services/org.freedesktop.intel_lpmd.service

    rm /usr/lib/systemd/system/intel_lpmd.service	
}

if test -f /usr/lib/systemd/system/intel_lpmd.service; then
	sudo systemctl stop intel_lpmd.service >/dev/null 2>&1
fi
      
#if tuned profiles were installed, remove profiles and unmask the ppd service	  
if [ -d "/etc/tuned/intel_ileo" ]; then	      
	tunedstatus=$(echo $(sudo systemctl status tuned | grep -o "active (running)"))
	if [ ! -z "$tunedstatus" ]; then
		#turn off active profile 
		tuned-adm off >/dev/null 2>&1   
	fi 
              
	sudo systemctl restart tuned       
	
    if test -f /usr/lib/systemd/system/power-profiles-daemon.service; then
        sudo systemctl unmask power-profiles-daemon >/dev/null 2>&1
    fi
fi  

sudo systemctl stop intel_lpmd >/dev/null 2>&1 &
	
cleanup 
sudo systemctl daemon-reload    
    
    
