/*
 * wlt_proxy_def.c: Intel Low Power Daemon WLT proxy
 *
 * Copyright (C) 2023 Intel Corporation. All rights reserved.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
 *
 * This file contains the Workload type detection proxy. This file logic
 * is specific to a CPU model and can be customized.
 */

#include "lpmd.h"
#include "cpu_group.h"
#include "wlt_proxy_common.h"
#include "wlt_proxy.h"

#define DEFAULT_ACTION_INTERVAL	1
#define CPU_UTIL_SUSTAIN		50
#define CPU_UTIL_IDLE			10
#define CPU_UTIL_BATTERY_LIFE	20
#define BURST_COUNT_ABOVE_TDP	3

static int tdp_mw;
static int burst_count;
static int burst_len;
static bool proxy_initialized = false;
int action_interval = DEFAULT_ACTION_INTERVAL;

static lpmd_config_t *lpmd_config;
//lpmd_config_t *lpmd_config;
extern int next_proxy_poll;

struct _threshold {
	int cpu_util_sustain; /* Above this utilization the workload will be identified as sustained */
	int cpu_util_idle; /* Below this util the workload will be identified as idle */
	int cpu_util_battery_life; /* Above this util the workload will be identified as semi_active */
	int burst_count; /* Number of burst above TDP */
};

static struct _threshold thereshold = {
		.cpu_util_sustain = CPU_UTIL_SUSTAIN,
		.cpu_util_idle = CPU_UTIL_IDLE,
		.cpu_util_battery_life = CPU_UTIL_BATTERY_LIFE,
		.burst_count = BURST_COUNT_ABOVE_TDP
};

static int current_workload_type;

#define MAX_SAMPLES 3

static int cpu_usage_list[MAX_SAMPLES];
static int cpu_usage_list_index = -1;
static int cpu_usage_count;

static unsigned long prev_idle, prev_total;

#define PATH_PROC_STAT "/proc/stat"

static int sysfs_read(const char *path, char *buf, int len) {
	int fd;
	int ret = 0;

	if (!buf || !len)
		return -EINVAL;

	fd = open(path, O_RDONLY);
	if (fd < 0)
		return -errno;

	ret = read(fd, buf, len);
	if (ret > 0 && ret < len)
		buf[ret] = '\0';
	close(fd);
	buf[len - 1] = '\0';

	return ret;
}

static int get_busy(void) {
	unsigned int cpu_usage = 0;
	char buffer[512];
	size_t size = 0;
	FILE *filep;
	char *eol;
	int ret;

	ret = sysfs_read("/proc/stat", buffer, sizeof(buffer));
	if (ret < 0) {
		fprintf(stderr, "Fatal error, exit\n");
		return -1;
	}

	eol = strchr(buffer, '\n');
	if (eol) {
		unsigned int user, nice, system, idle, iowait, irq, softirq, steal,
				guest, guest_nice;
		unsigned long idle_time, total;

		eol[0] = '\0';
		sscanf(&buffer[4], "%u%u%u%u%u%u%u%u%u%u", &user, &nice, &system, &idle,
				&iowait, &irq, &softirq, &steal, &guest, &guest_nice);

		idle_time = idle + iowait;
		total = idle_time + user + nice + system + irq + softirq + steal;
		lpmd_log_debug("idle_time %lu %lu\n", idle_time, total);

		if (prev_idle && prev_total) {
			unsigned int _idle, _total;

			_idle = idle_time - prev_idle;
			_total = total - prev_total;
			if (_total != 0)
    			cpu_usage = (_total - _idle) * 100 / _total;
			lpmd_log_debug("cpu_usage: %d %d %d\n", _idle, _total, cpu_usage);
		}

		prev_idle = idle_time;
		prev_total = total;
	}

	return cpu_usage;
}

static int check_cpu_busy(int power_mw, int tdp) {
	int cpu_usage, avg_cpu_usage, i;

	cpu_usage = get_busy();
	if (cpu_usage == 0)
		return 0;

	if (cpu_usage_list_index == -1) {
		cpu_usage_list[0] = cpu_usage;
		cpu_usage_list_index = 1;
	} else {
		cpu_usage_list[cpu_usage_list_index] = cpu_usage;
		cpu_usage_list_index = (cpu_usage_list_index + 1) % MAX_SAMPLES;
	}
	++cpu_usage_count;

	if (cpu_usage_count > MAX_SAMPLES) {
		cpu_usage_count = MAX_SAMPLES;
	}

	avg_cpu_usage = 0;
	for (i = 0; i < cpu_usage_count; ++i) {
		avg_cpu_usage += cpu_usage_list[i];
	}

	avg_cpu_usage /= cpu_usage_count;

	lpmd_log_debug("%d -> %d:%d:%d avg_cpu_usage: %d \n", cpu_usage_count,
			cpu_usage_list[0], cpu_usage_list[1], cpu_usage_list[2],
			avg_cpu_usage);

	return avg_cpu_usage;
}

void set_workload_hint(int type) {
	lpmd_log_debug("proxy WLT hint :%d\n", type);
	periodic_util_update(lpmd_config, type);
}

/*this is replaced by state_machine_auto*/
static void action_loop(int power_mw, int tdp_mw) {
	static int interval;
	int avg_cpu_usage;

	++interval;

	avg_cpu_usage = check_cpu_busy(power_mw, tdp_mw);

	if (interval == action_interval) {
		if (avg_cpu_usage > thereshold.cpu_util_sustain) {
			set_workload_hint(WLT_SUSTAINED);
		} else if (avg_cpu_usage > thereshold.cpu_util_battery_life) {
			set_workload_hint(WLT_BATTERY_LIFE);
		} else if (avg_cpu_usage <= thereshold.cpu_util_idle) {
			set_workload_hint(WLT_IDLE);
		} else if (burst_count > thereshold.burst_count) {
			set_workload_hint(WLT_BURSTY);
		} else {
			set_workload_hint(WLT_INVALID);
		}

		interval = 0;
		burst_len = 0;
		burst_count = 0;
	}
}

/* Called at the configured interval to take action */
void wlt_proxy_action_loop(void) {
	static int last_energy;
	static int burst_seen;
	int energy, power_mw;
	char buffer[256];
	int ret;

	ret = sysfs_read(
					"/sys/class/powercap/intel-rapl/intel-rapl:0/intel-rapl:0:0/energy_uj",
					buffer, sizeof(buffer));
	if (ret < 0) {
		lpmd_log_error("Fatal error, exit\n");
		return;
	}

	energy = atoi(buffer);
	if (!last_energy) {
		last_energy = energy;
		return;
	}

	power_mw = (energy - last_energy) / 1000;
	last_energy = energy;

	if (power_mw > tdp_mw) {
		++burst_len;
		burst_seen = 1;
	}

	if (burst_seen && power_mw < tdp_mw) {
		++burst_count;
		burst_len = 0;
		burst_seen = 0;
	}

	//action_loop(power_mw, tdp_mw);
	if (proxy_initialized){
		lpmd_log_debug("\n\nwlt_proxy_action_loop, proxy initialzied\n");
		state_machine_auto(get_cur_state());
		lpmd_log_debug("wlt_proxy_action_loop, handled states\n");		
	}
}

static int get_tdp() {
	char buffer[256];
	int ret;

	ret = sysfs_read(
					"/sys/class/powercap/intel-rapl-mmio/intel-rapl-mmio:0/constraint_0_max_power_uw",
					buffer, sizeof(buffer));
	if (ret < 0) {
		lpmd_log_error("Fatal error, can't read TDP\n");
		return LPMD_FATAL_ERROR;
	}

	tdp_mw = atoi(buffer);
	tdp_mw /= 1000;

	return 0;
}

/* Return non zero if the proxy is not present for a platform */
int wlt_proxy_init(lpmd_config_t *_lpmd_config) {
	int ret;

    init_cpu_proxy();
    util_init_proxy();

	/* Check model check and fail */
	/* TODO */
	lpmd_config = _lpmd_config;

	ret = get_tdp();
	if (ret)
		return ret;

	proxy_initialized = true; 
	next_proxy_poll = 2000;
	return LPMD_SUCCESS;
}

/*make sure all resource are properly released and clsoed*/
void wlt_proxy_uninit(void){
    //if proxy is enabled, make sure we close all open fd. 
    if (lpmd_config->wlt_proxy_enable){
    	exit_state_change();
        close_all_fd();    
       	uninit_delta_vars();        
        uninit_cpu_proxy();
       	perf_stat_uninit(); 
    } 	
}
