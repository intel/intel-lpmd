#!/bin/sh

if [ "$(whoami)" != "root" ]
then
	echo "This script must be run as root"
	exit 1
fi

opt_no=
if [ ! -z "$1" ]
then
        opt_no=$1
fi

while true;
do
	if [ -z "$1" ]
	then
		echo "****options****"
		echo "0 : Allow All CPUs"
		echo "1 : Terminate"
		echo "2 : LPM force on"
		echo "3 : LPM force off"
		echo "4 : LPM auto"
		echo "5 : SUV_MODE Enter"
		echo "6 : SUV_MODE Exit"
		echo "7 : Quit"
	        echo -n " Enter choice: "
	        read opt_no
	fi

	case $opt_no in
	0)
		echo "0 : Allow All CPUs"
		echo -n " Enter Maximum CPU number"
		read max_cpu
		sudo systemctl set-property --runtime user.slice AllowedCPUs=0-$max_cpu
		sudo systemctl set-property --runtime system.slice AllowedCPUs=0-$max_cpu
		;;
	1)
		echo "1 : Terminate"
		dbus-send --system --dest=org.freedesktop.intel_lpmd --print-reply /org/freedesktop/intel_lpmd org.freedesktop.intel_lpmd.Terminate
		;;
	2)
		echo "2 : LPM force on"
		dbus-send --system --dest=org.freedesktop.intel_lpmd --print-reply /org/freedesktop/intel_lpmd org.freedesktop.intel_lpmd.LPM_FORCE_ON
		;;
	3)
		echo "3 : LPM force off"
		dbus-send --system --dest=org.freedesktop.intel_lpmd --print-reply /org/freedesktop/intel_lpmd org.freedesktop.intel_lpmd.LPM_FORCE_OFF
		;;
	4)
		echo "4 : LPM auto"
		dbus-send --system --dest=org.freedesktop.intel_lpmd --print-reply /org/freedesktop/intel_lpmd org.freedesktop.intel_lpmd.LPM_AUTO
		;;
	5)
		echo "5 : SUV_MODE Enter"
		dbus-send --system --dest=org.freedesktop.intel_lpmd --print-reply /org/freedesktop/intel_lpmd org.freedesktop.intel_lpmd.SUV_MODE_ENTER
		;;
	6)
		echo "6 : SUV_MODE Exit"
		dbus-send --system --dest=org.freedesktop.intel_lpmd --print-reply /org/freedesktop/intel_lpmd org.freedesktop.intel_lpmd.SUV_MODE_EXIT
		;;
	7)
		exit 0
		;;
	*)
		echo "7 : Quit"
		echo "Invalid option"
	esac
	[ ! -z "$1" ] && break
done
