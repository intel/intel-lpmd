#!/bin/sh

#Copyright (C) 2024 Intel Corporation
#SPDX-License-Identifier: GPL-2.0-or-later

. /usr/lib/tuned/functions

start() {
    
    #start lpmd application
    sudo /usr/bin/intel_lpmd --loglevel=info --no-daemon >/dev/null 2>&1 &
    
    #sleep 0.2
    
    #allows low power mode based on system utilization
	#sudo dbus-send --system --dest=org.freedesktop.intel_lpmd --print-reply /org/freedesktop/intel_lpmd org.freedesktop.intel_lpmd.LPM_AUTO
    
    return 0
}

stop() {

    #Terminate
    #sudo dbus-send --system --dest=org.freedesktop.intel_lpmd --print-reply /org/freedesktop/intel_lpmd org.freedesktop.intel_lpmd.Terminate
    
    #sleep 0.2
        
    #stop lpmd application
    sudo pkill -f intel_lpmd >/dev/null 2>&1 &
    
    return 0
}

process $@
