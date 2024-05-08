#!/bin/bash

#Copyright (C) 2024 Intel Corporation
#This software and the related documents are Intel copyrighted materials, and your use of them is governed by the express license under which they were provided to you ("License"). 
#Unless the License provides otherwise, you may not use, modify, copy, publish, distribute, disclose or transmit this software or the related documents without Intel's prior written permission.
#This software and the related documents are provided as is, with no express or implied warranties, other than those that are expressly stated in the License.

folder="/usr/share/ia_pkg"
file="/usr/share/ia_pkg/EPPprofile/EPPprofile.install"

#function cleanup
cleanup()
{
    #remove the profiles    
    sudo rm -r /etc/tuned/intel*        
		
	rm /usr/bin/intel_lpmd
    rm /usr/bin/intel_lpmd_control

    rm /usr/local/share/man/man5/intel_lpmd_config.xml.5
    rm /usr/local/share/man/man8/intel_lpmd.8

    rm /usr/local/etc/intel_lpmd/intel_lpmd_config.xml
    rm /etc/dbus-1/system.d/org.freedesktop.intel_lpmd.conf

    rm /usr/local/share/dbus-1/system-services/org.freedesktop.intel_lpmd.service

    rm /usr/lib/systemd/system/intel_lpmd.service	
}

       
#turn off active profile 
tuned-adm off                 
sudo systemctl restart tuned            

cleanup 
sudo systemctl daemon-reload
sudo systemctl unmask power-profiles-daemon


    
    
    
