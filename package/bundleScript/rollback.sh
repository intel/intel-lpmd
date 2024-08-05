#!/bin/bash

#Copyright (C) 2024 Intel Corporation
#This software and the related documents are Intel copyrighted materials, and your use of them is governed by the express license under which they were provided to you ("License"). 
#Unless the License provides otherwise, you may not use, modify, copy, publish, distribute, disclose or transmit this software or the related documents without Intel's prior written permission.
#This software and the related documents are provided as is, with no express or implied warranties, other than those that are expressly stated in the License.

#function cleanup
cleanup()
{
    #remove the profiles    
	if [[ "$installasservice" -eq 0 ]]; then	
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
       
if test -f /etc/tuned/inte_hepo; then	   
	#turn off active profile 
	tuned-adm off >/dev/null 2>&1                 
	sudo systemctl restart tuned            
    if test -f /usr/lib/systemd/system/power-profiles-daemon.service; then
        sudo systemctl unmask power-profiles-daemon >/dev/null 2>&1
    fi
fi  
	
cleanup 
sudo systemctl daemon-reload    
    
    
