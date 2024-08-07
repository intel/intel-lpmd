#!/bin/sh

#Copyright (C) 2024 Intel Corporation
#SPDX-License-Identifier: GPL-2.0-or-later

. /usr/lib/tuned/functions

start() {

	service="intel_lpmd"
	# is service installed
	#if [ -f "/etc/init.d/$service" ]; then
	if [ -f "/usr/lib/systemd/system/intel_lpmd.service" ]; then
		#start energy optimizer as service with default configuration file.	
		#sudo systemctl enable $service
		sudo systemctl start $service
		return 0
	fi

    return 1
}

stop() {

	service="intel_lpmd"
	sudo systemctl is-active $service
	if [ $? = 0 ]; then
		#Terminate
		sudo dbus-send --system --dest=org.freedesktop.intel_lpmd --print-reply /org/freedesktop/intel_lpmd org.freedesktop.intel_lpmd.Terminate >/dev/null 2>&1 &
		sudo systemctl stop $service >/dev/null 2>&1 &
		
		#To disable the service from starting automatically
		sudo systemctl disable $service
	fi
	
    return 0
}

process $@
