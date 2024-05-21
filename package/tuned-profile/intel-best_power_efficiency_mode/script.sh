#!/bin/sh

#Copyright (C) 2024 Intel Corporation
#SPDX-License-Identifier: GPL-2.0-or-later

#EPP_PATH "/sys/devices/system/cpu/cpu$i/cpufreq/energy_performance_preference"
#EPB_PATH "/sys/devices/system/cpu/cpu$i/power/energy_perf_bias"

. /usr/lib/tuned/functions

start() {
    n=$(nproc)
    i=0
    while [ "$i" -lt $n ]; do
        echo 178 | sudo tee  /sys/devices/system/cpu/cpu$i/cpufreq/energy_performance_preference
        echo 8 | sudo tee  /sys/devices/system/cpu/cpu$i/power/energy_perf_bias
        i=$(( i + 1 ))
    done 
    return 0
}

stop() {
    n=$(nproc)
    i=0
    while [ "$i" -lt $n ]; do
        echo "balance_performance" | sudo tee  /sys/devices/system/cpu/cpu$i/cpufreq/energy_performance_preference
        echo 6 | sudo tee  /sys/devices/system/cpu/cpu$i/power/energy_perf_bias
        i=$(( i + 1 ))
    done     
    return 0
}

process $@
