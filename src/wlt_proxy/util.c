/*
 * Copyright (c) 2024, Intel Corporation.
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms and conditions of the GNU General Public License,
 * version 2, as published by the Free Software Foundation.
 *
 * This program is distributed in the hope it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
 * more details.
 * 
 * Author: Noor ul Mubeen <noor.u.mubeen@intel.com>
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <math.h>
#include <err.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
#include <fcntl.h>
#include <sys/file.h>
#include <sys/types.h>
#include <sys/time.h>
#include <sys/stat.h>
#include <errno.h>
#include <assert.h>
#include <stdbool.h>
#include <linux/perf_event.h>
#include <asm/unistd.h>
#include "lpmd.h"
#include "wlt_proxy_common.h"
#include "wlt_proxy.h"
#include "cpu_group.h"
#include "perf_msr.h"
#include "../weights/weights_common.h"

#define PERF_API 1

perf_stats_t *perf_stats;
struct group_util grp;
uint32_t update_epp(int fd, uint64_t epp);
uint32_t update_epb(int fd, uint64_t epb);
void update_state_epp(enum lp_state_idx state);
void update_state_epb(enum lp_state_idx state);
void clamp_to_turbo(enum lp_state_idx for_state);

static float soc_mw;

struct thread_data {
	unsigned long long tsc;
	unsigned long long aperf;
	unsigned long long mperf;
	unsigned long long pperf; 
} *thread_even, *thread_odd;

int grp_c0_breach(void)
{
	return (A_GT_B(grp.c0_max, UTIL_NEAR_FULL)
		|| A_GT_B(grp.delta, DELTA_THRESHOLD));
}

int get_msr_fd(int cpu)
{
	return perf_stats[cpu].dev_msr_fd;
}

static int read_perf_counter_info(const char *const path, const char *const parse_format, void *value_ptr)
{
	int fdmt;
	int bytes_read;
	char buf[64];
	int ret = -1;

	fdmt = open(path, O_RDONLY, 0);
	if (fdmt == -1) {
		//if (debug)
			fprintf(stderr, "Failed to parse perf counter info %s\n", path);
		ret = -1;
		goto cleanup_and_exit;
	}

	bytes_read = read(fdmt, buf, sizeof(buf) - 1);
	if (bytes_read <= 0 || bytes_read >= (int)sizeof(buf)) {
		//if (debug)
			fprintf(stderr, "Failed to parse perf counter info %s\n", path);
		ret = -1;
		goto cleanup_and_exit;
	}

	buf[bytes_read] = '\0';

	if (sscanf(buf, parse_format, value_ptr) != 1) {
		//if (debug)
			fprintf(stderr, "Failed to parse perf counter info %s\n", path);
		ret = -1;
		goto cleanup_and_exit;
	}

	ret = 0;

cleanup_and_exit:
    if (fdmt >= 0)
    	close(fdmt);
	return ret;
}

static unsigned int read_perf_counter_info_n(const char *const path, const char *const parse_format)
{
	unsigned int v;
	int status;

	status = read_perf_counter_info(path, parse_format, &v);
	if (status)
		v = -1;

	return v;
}
int read_pperf_config(void)
{
	const char *const path = "/sys/bus/event_source/devices/msr/events/pperf";
	const char *const format = "event=%x";

	return read_perf_counter_info_n(path, format);
}
unsigned int read_aperf_config(void)
{
	const char *const path = "/sys/bus/event_source/devices/msr/events/aperf";
	const char *const format = "event=%x";

	return read_perf_counter_info_n(path, format);	
}

unsigned int read_mperf_config(void)
{
	const char *const path = "/sys/bus/event_source/devices/msr/events/mperf";
	const char *const format = "event=%x";

	return read_perf_counter_info_n(path, format);
}
unsigned int read_tsc_config(void)
{
	const char *const path = "/sys/bus/event_source/devices/msr/events/tsc";
	const char *const format = "event=%x";

	return read_perf_counter_info_n(path, format);
}


static unsigned int read_msr_type(void)
{
	const char *const path = "/sys/bus/event_source/devices/msr/type";
	const char *const format = "%u";

	return read_perf_counter_info_n(path, format);
}

static long perf_event_open(struct perf_event_attr *hw_event, pid_t pid, int cpu, int group_fd, unsigned long flags)
{
	return syscall(__NR_perf_event_open, hw_event, pid, cpu, group_fd, flags);
}

static long open_perf_counter(int cpu, unsigned int type, unsigned int config, int group_fd, __u64 read_format)
{
	struct perf_event_attr attr;
	const pid_t pid = -1;
	const unsigned long flags = 0;

	memset(&attr, 0, sizeof(struct perf_event_attr));

	attr.type = type;
	attr.size = sizeof(struct perf_event_attr);
	attr.config = config;
	attr.disabled = 0;
	attr.sample_type = PERF_SAMPLE_IDENTIFIER;
	attr.read_format = read_format;

	const int fd = perf_event_open(&attr, pid, cpu, group_fd, flags);

	return fd;
}

void open_amperf_fd(int cpu)
{
	const unsigned int msr_type = read_msr_type();
	const unsigned int aperf_config = read_aperf_config();
	const unsigned int mperf_config = read_mperf_config();
	const unsigned int pperf_config = read_pperf_config();	

	perf_stats[cpu].aperf_fd = open_perf_counter(cpu, msr_type, aperf_config, -1, PERF_FORMAT_GROUP);
	perf_stats[cpu].mperf_fd = open_perf_counter(cpu, msr_type, mperf_config, perf_stats[cpu].aperf_fd, PERF_FORMAT_GROUP);	
	perf_stats[cpu].pperf_fd = open_perf_counter(cpu, msr_type, pperf_config, perf_stats[cpu].aperf_fd, PERF_FORMAT_GROUP);	
}

int get_amperf_fd(int cpu)
{
	if (perf_stats[cpu].aperf_fd)
		return perf_stats[cpu].aperf_fd;

	open_amperf_fd(cpu);

	return perf_stats[cpu].aperf_fd;
}


unsigned long long rdtsc(void)
{
	unsigned int low, high;

	asm volatile ("rdtsc":"=a" (low), "=d"(high));

	return low | ((unsigned long long)high) << 32;
}


/* Read APERF, MPERF and TSC using the perf API. */
int read_aperf_mperf_tsc_perf(struct thread_data *t, int cpu)
{
	union {
		struct {
			unsigned long nr_entries;
			unsigned long aperf;
			unsigned long mperf;
			unsigned long pperf; 
		};

		unsigned long as_array[4];
	} cnt;

	const int fd_amperf = get_amperf_fd(cpu);

	/*
	 * Read the TSC with rdtsc, because we want the absolute value and not
	 * the offset from the start of the counter.
	 */
	t->tsc = rdtsc();

	const int n = read(fd_amperf, &cnt.as_array[0], sizeof(cnt.as_array));

	if (n != sizeof(cnt.as_array))
		return -2;

	t->aperf = cnt.aperf;
	t->mperf = cnt.mperf;
	t->pperf = cnt.pperf;

	return 0;
}

extern int max_util;
int update_perf_diffs(float *sum_norm_perf, int stat_init_only)
{
	int fd, min_cpu, maxed_cpu = -1;
	float min_load = 100.0, min_s0 = 1.0, next_s0 = 1.0;
	float _sum_nperf = 0, nperf = 0;
	float max_load = 0, max_2nd_load = 0, max_3rd_load = 0, next_load = 0;
	uint64_t aperf_raw, mperf_raw, pperf_raw, tsc_raw, poll_cpu_us = 0;

	int t, min_s0_cpu = 0, first_pass = 1;

	for (t = 0; t < get_max_online_cpu(); t++) {
		if (!cpu_applicable(t, get_cur_state()))
			continue;

#ifdef PERF_API
        /*reading through perf api*/
		struct thread_data tdata;
		read_aperf_mperf_tsc_perf(&tdata , t);
        lpmd_log_debug("from api pperf_raw %lld\n", tdata.pperf);
		/*perf_stats[t].pperf_diff*/ uint64_t pperf = cpu_get_diff_pperf(tdata.pperf, t);
        lpmd_log_debug("from api pperf_diff %ld\n", pperf);
		perf_stats[t].pperf_diff = pperf;

        lpmd_log_debug("from api aperf_raw %lld\n", tdata.aperf);
		/*perf_stats[t].aperf_diff*/  uint64_t aperf = cpu_get_diff_aperf(tdata.aperf, t);
        lpmd_log_debug("from api aperf_diff %ld\n", aperf);
        perf_stats[t].aperf_diff = aperf;

        lpmd_log_debug("from api mperf_raw %lld\n", tdata.mperf);
		/*perf_stats[t].mperf_diff*/  uint64_t mperf = cpu_get_diff_mperf(tdata.mperf, t);
        lpmd_log_debug("from api mperf_diff %ld\n", mperf);
        perf_stats[t].mperf_diff = mperf;

        lpmd_log_debug("from api tsc_raw %lld\n", tdata.tsc);
		/*perf_stats[t].tsc_diff*/ uint64_t tsc = cpu_get_diff_tsc(tdata.tsc, t);
        lpmd_log_debug("from api tsc_diff %ld\n\n", tsc);
        perf_stats[t].tsc_diff = tsc;
#else		
		/* reading through driver*/
		fd = perf_stats[t].dev_msr_fd;

		read_msr(fd, (uint32_t) MSR_IA32_PPERF, &pperf_raw);
		read_msr(fd, (uint32_t) MSR_IA32_APERF, &aperf_raw);
		read_msr(fd, (uint32_t) MSR_IA32_MPERF, &mperf_raw);
		read_msr(fd, (uint32_t) MSR_IA32_TSC, &tsc_raw); 		

        lpmd_log_debug("from driver pperf_raw %ld\n", pperf_raw);
		perf_stats[t].pperf_diff = cpu_get_diff_pperf(pperf_raw, t);
		lpmd_log_debug("****from driver pperf_diff %ld\n", perf_stats[t].pperf_diff);

        lpmd_log_debug("from driver aperf_raw %ld\n", aperf_raw);
		perf_stats[t].aperf_diff = cpu_get_diff_aperf(aperf_raw, t);
        lpmd_log_debug("****from driver aperf_diff %ld\n", perf_stats[t].aperf_diff);

        lpmd_log_debug("from driver mperf_raw %ld\n", mperf_raw);
		perf_stats[t].mperf_diff = cpu_get_diff_mperf(mperf_raw, t);
        lpmd_log_debug("****from driver mperf_diff %ld\n", perf_stats[t].mperf_diff);

        lpmd_log_debug("from driver tsc_raw %ld\n", tsc_raw);
		perf_stats[t].tsc_diff = cpu_get_diff_tsc(tsc_raw, t);
        lpmd_log_debug("****from driver tsc_diff %ld\n\n", perf_stats[t].tsc_diff);*/
#endif

		if (stat_init_only)
			continue;

		poll_cpu_us = perf_stats[t].tsc_diff / cpu_hfm_mhz;
		/*
		 * Normalized perf metric defined as pperf per load per time.
		 * The rationale is detailed here:
		 * github.com/intel/psst >whitepapers >Generic_perf_per_watt.pdf
		 * Given that delta_load = delta_mperf/delta_tsc, we can rewrite
		 * as given below.
		 */
		if (perf_stats[t].mperf_diff) {
			if (poll_cpu_us != 0)
				nperf = (float)perf_stats[t].pperf_diff / poll_cpu_us;
			nperf = (float)nperf *perf_stats[t].tsc_diff;
			nperf = (float)nperf / (perf_stats[t].mperf_diff);
			perf_stats[t].nperf = (uint64_t) nperf;
			_sum_nperf += (float)nperf;
		}
		if (perf_stats[t].tsc_diff) {
			next_load =
			    (float)100 *perf_stats[t].mperf_diff /
			    perf_stats[t].tsc_diff;
			perf_stats[t].l0 = next_load;
		}

		if (A_LTE_B(max_load, next_load)) {
			max_load = next_load;
			maxed_cpu = perf_stats[t].cpu;
		} else if (A_LTE_B(max_2nd_load, next_load)) {
			max_2nd_load = next_load;
		} else if (A_LTE_B(max_3rd_load, next_load)) {
			max_3rd_load = next_load;
		}
		/* min scalability */
		if (perf_stats[t].aperf_diff) {
			next_s0 = (float)perf_stats[t].pperf_diff /
			    perf_stats[t].aperf_diff;
			/* since aperf/pperf are not read oneshot, ratio > 1 is not ruled out */
			next_s0 = (next_s0 >= 1) ? (1 - EPSILON) : next_s0;
		}
		if (A_LTE_B(next_s0, min_s0) || first_pass) {
			min_s0 = next_s0;
			min_s0_cpu = perf_stats[t].cpu;
		}

		if (A_GT_B(min_load, next_load)) {
			min_load = next_load;
			min_cpu = perf_stats[t].cpu;
			/*
			 * min cpu unused for now. 
			 */
			(void)(min_cpu);
		}
		first_pass = 0;
	}
	if (stat_init_only)
		return 0;
	if (perf_stats[maxed_cpu].mperf_diff)
		perf_stats[maxed_cpu].f0 =
		    (float)perf_stats[maxed_cpu].aperf_diff * 0.01 /
		    perf_stats[maxed_cpu].mperf_diff * cpu_hfm_mhz;
	grp.worst_stall = min_s0;
	grp.worst_stall_cpu = min_s0_cpu;

	grp.c0_max = max_load;
	grp.c0_2nd_max = max_2nd_load;
	grp.c0_3rd_max = max_3rd_load;
	grp.c0_min = min_load;
	*sum_norm_perf = _sum_nperf;
	if (poll_cpu_us != 0)
		soc_mw = (float)rapl_ediff_pkg0(read_rapl_pkg0()) * 1000 / poll_cpu_us;

	return maxed_cpu;
}

#define MAX_INJECT	(90)
#define LIMIT_INJECT(i)	(i > MAX_INJECT ? MAX_INJECT:(i < 0 ? 1 : i))

int idle_inject_feature = IDLE_INJECT_FEATURE;
int inject_update = UNDEFINED;
int irq_rebalance = 0;
static int record = 0;

/* 
 * simple moving average (sma), event count based - not time. 
 * updated for upto top 3 max util streams.
 * exact cpu # is not tracked; only the max since continuum of task
 * keeps switching cpus anyway.
 * array implementation with SMA_LENGTH number of values.
 * XXX: reimplement dynamic for advance tunable SMA_LENGTH
 */
#define SMA_LENGTH	(25)
#define SMA_CPU_COUNT	(3)
static int sample[3][SMA_LENGTH];
void sma_init()
{
	for (int i = 0; i < SMA_CPU_COUNT; i++) {
		grp.sma_sum[i] = -1;
		for (int j = 0; j < SMA_LENGTH; j++)
			sample[i][j] = 0;
	}
	grp.sma_pos = -1;
}

int do_sum(int *sam, int len)
{
	int sum = 0;
	for (int i = 0; i < len; i++)
		sum += sam[i];
	return sum;
}


#define SCALE_DECIMAL (100)
int state_max_avg()
{
	grp.sma_pos += 1;

	int v1 = (int)round(grp.c0_max * SCALE_DECIMAL);
	int v2 = (int)round(grp.c0_2nd_max * SCALE_DECIMAL);
	int v3 = (int)round(grp.c0_3rd_max * SCALE_DECIMAL);
	if (grp.sma_pos == SMA_LENGTH)
		grp.sma_pos = 0;
	if (grp.sma_sum[0] == -1) {
		sample[0][grp.sma_pos] = v1;
		sample[1][grp.sma_pos] = v2;
		sample[2][grp.sma_pos] = v3;
		if (grp.sma_pos == SMA_LENGTH - 1) {
			grp.sma_sum[0] = do_sum(sample[0], SMA_LENGTH);
			grp.sma_sum[1] = do_sum(sample[1], SMA_LENGTH);
			grp.sma_sum[2] = do_sum(sample[2], SMA_LENGTH);
		}
	} else {
		grp.sma_sum[0] = grp.sma_sum[0] - sample[0][grp.sma_pos] + v1;
		grp.sma_sum[1] = grp.sma_sum[1] - sample[1][grp.sma_pos] + v2;
		grp.sma_sum[2] = grp.sma_sum[2] - sample[2][grp.sma_pos] + v3;
		sample[0][grp.sma_pos] = v1;
		sample[1][grp.sma_pos] = v2;
		sample[2][grp.sma_pos] = v3;
	}

	grp.sma_avg1 =
	    (int)round((double)grp.sma_sum[0] / (double)(SMA_LENGTH * SCALE_DECIMAL));
	grp.sma_avg2 =
	    (int)round((double)grp.sma_sum[1] / (double)(SMA_LENGTH * SCALE_DECIMAL));
	grp.sma_avg3 =
	    (int)round((double)grp.sma_sum[2] / (double)(SMA_LENGTH * SCALE_DECIMAL));

	return 1;
}

enum lp_state_idx nearest_supported(enum lp_state_idx from_state, enum lp_state_idx to_state)
{
	enum lp_state_idx state;
	int operator =	(from_state < to_state) ? (+1) : (-1);
	for (state = to_state; (state >= 0) && (state < MAX_MODE); state = state + operator) {
		/* RESP mode is special and not good idea to step into as backup state */
		if (state == RESP_MODE)
			continue;
		if (is_state_valid(state))
			return state;
	}
	assert(0);

}

static int get_state_mapping(enum lp_state_idx state){

    //read the battery connection status 
    bool AC_CONNECTED = is_ac_powered_power_supply_status() == 0  ? false: true; //unknown is considered as ac powered.
    
    switch(state){
	case PERF_MODE:
    	return WLT_BURSTY;
	
	case RESP_MODE:
	    if (AC_CONNECTED){	
		lpmd_log_info("AC_CONNECTED: WLT_SUSTAINED\n");
            return WLT_SUSTAINED;
		}
        else{
		lpmd_log_info("Powered by battery: WLT_SUSTAINED_BAT\n");			
            return WLT_SUSTAINED_BAT;
		}
            
	case MDRT4E_MODE:
	case MDRT3E_MODE:
	case MDRT2E_MODE:
	case NORM_MODE:
	    if (AC_CONNECTED){
			lpmd_log_info("AC_CONNECTED: WLT_BATTERY_LIFE\n");
    	    return WLT_BATTERY_LIFE;
		}
    	else {
			lpmd_log_info("Powered by battery: WLT_BATTERY_LIFE_BAT\n");			
        	return WLT_BATTERY_LIFE_BAT;
		}	
	case DEEP_MODE:							
        return WLT_IDLE; 
	
	//there is no corresponding wlt for INIT_MODE, it goes away quickly.
	//use WLT_SUSTAINED as default type	
	case INIT_MODE:
		return WLT_SUSTAINED;
		
	//we don't allow invalid wlt type, flag and use WLT_SUSTAINED		
	default:
		lpmd_log_error("unknown work load type\n");
	    return WLT_SUSTAINED;	
    }    
}

int state_demote = 0;
int next_proxy_poll = 2000; 
int prep_state_change(enum lp_state_idx from_state, enum lp_state_idx to_state,
		      int reset)
{
	if (is_state_disabled(to_state))
		to_state = nearest_supported(from_state, to_state);

	if (!reset && state_has_ppw(to_state))
		inject_update = ACTIVATED;
	else
		inject_update = PAUSE;

	/* state entry: p-state ctl if applicable*/
	if (!reset && state_support_freq_ctl(to_state))
		clamp_to_turbo(to_state);
	/* state exit: p-state reset back */
	if (!reset && state_support_freq_ctl(from_state))
		unclamp_default_freq(from_state);
#if 0
	//epp and epb are updated in the main 
	update_state_epp(to_state);
	update_state_epb(to_state);
#endif
    //switch(to_state)
    //do to_state to WLT mapping
    int type = get_state_mapping((int)to_state); 
    lpmd_log_debug("proxy WLT state value :%d\n", type);
	set_workload_hint(type);

	set_cur_state(to_state);
	set_state_reset();
	set_last_maxutil(DEACTIVATED);
	irq_rebalance = 1;

	if (to_state < from_state)
		state_demote = 1;
	
	//proxy: apply state change and get poll of different states
	apply_state_change(); 
	if (likely(is_state_valid(to_state))) {
		next_proxy_poll = get_state_poll(max_util, to_state);
	}//proxy: change end

	return 1;
}

int staytime_to_staycount(enum lp_state_idx state)
{
	int stay_count = 0;
	if (unlikely((state < 0) || (state >= MAX_MODE)))
		assert(0);
	switch (state)
	{
		case MDRT2E_MODE:
		case MDRT3E_MODE:
		case MDRT4E_MODE:
			stay_count = (int)MDRT_MODE_STAY/get_poll_ms(MDRT3E_MODE);
			break;
		case PERF_MODE:
			stay_count = (int)PERF_MODE_STAY/get_poll_ms(PERF_MODE);
			break;
		case RESP_MODE:
		case INIT_MODE:
		case NORM_MODE:
		case DEEP_MODE:
		case BYPS_MODE:
		//case MAX_MODE:
			/* undefined */
			assert(0);
			break;
	}
	return stay_count;
}

int prepare_deomote_bypass(enum lp_state_idx to_state, int epp_high)
{
	if (!epp_high) {
		update_state_epp(NORM_MODE);
		update_state_epb(NORM_MODE);
	} else {
		update_state_epp(PERF_MODE);
		update_state_epb(PERF_MODE);
	}

	set_cur_state(to_state);
	set_state_reset();
	set_last_maxutil(DEACTIVATED);
	irq_rebalance = 1;
	return 1;
}

void unclamp_default_freq(enum lp_state_idx for_state)
{
	char path[MAX_STR_LENGTH];
	int min_freq, max_freq;

	for (int i = 0; i < get_max_online_cpu(); i++) {
		if (cpu_applicable(i, for_state)) {
			min_freq = get_min_freq(i);
			if (min_freq == -1) {
				snprintf(path, sizeof(path),
					 "/sys/devices/system/cpu/cpufreq/policy%d/cpuinfo_min_freq",
					 i);
				fs_read_int(path, &min_freq);
			}

			max_freq = get_turbo_freq(i);
			snprintf(path, sizeof(path),
				 "/sys/devices/system/cpu/cpufreq/policy%d/scaling_max_freq",
				 i);
			fs_write_int(path, max_freq);
			snprintf(path, sizeof(path),
				 "/sys/devices/system/cpu/cpufreq/policy%d/scaling_min_freq",
				 i);
			fs_write_int(path, min_freq);
		}
	}
}

void clamp_to_freq(enum lp_state_idx for_state, int to_freq)
{
	char path_min[MAX_STR_LENGTH];
	char path_max[MAX_STR_LENGTH];
	to_freq = to_freq * 100000;
	int cur_max_freq;
	if (!state_support_freq_ctl(for_state))
		return;
	for (int i = 0; i < get_max_online_cpu(); i++) {
		if (cpu_applicable(i, for_state)) {
			if (to_freq > get_turbo_freq(i))
				log_err("cpu%d reqested freq %d > turbo %d\n",
					i, to_freq, get_turbo_freq(i));
			snprintf(path_max, sizeof(path_max),
				 "/sys/devices/system/cpu/cpufreq/policy%d/scaling_max_freq",
				 i);
			snprintf(path_min, sizeof(path_min),
				 "/sys/devices/system/cpu/cpufreq/policy%d/scaling_min_freq",
				 i);

			fs_read_int(path_max, &cur_max_freq);

			if (cur_max_freq > to_freq) {
				/* reducing max freq. so, ok to set min first */
				fs_write_int(path_min, to_freq);
				fs_write_int(path_max, to_freq);
			} else {
				fs_write_int(path_max, to_freq);
				fs_write_int(path_min, to_freq);
			}
		}
	}
}

void clamp_to_turbo(enum lp_state_idx for_state)
{
	char path[MAX_STR_LENGTH];
	int turbo_freq;
	for (int i = 0; i < get_max_online_cpu(); i++) {
		if (cpu_applicable(i, for_state)) {
			turbo_freq = get_turbo_freq(i);
			snprintf(path, sizeof(path),
				 "/sys/devices/system/cpu/cpu%d/cpufreq/scaling_max_freq",
				 i);
			fs_write_int(path, turbo_freq);
			snprintf(path, sizeof(path),
				 "/sys/devices/system/cpu/cpu%d/cpufreq/scaling_min_freq",
				 i);
			fs_write_int(path, turbo_freq);
		}
	}
}

float get_cur_freq(int cpu)
{
	return perf_stats[cpu].f0;
}

float get_cur_scalability(int cpu)
{
	return perf_stats[cpu].s0;
}

int max_mt_detected(enum lp_state_idx state)
{
	for (int t = 0; t < get_max_online_cpu(); t++) {
		if (!cpu_applicable(t, state))
			continue;
		if A_LTE_B
			(perf_stats[t].l0, (UTIL_LOW))
			    return 0;
	}
	return 1;
}

uint64_t diff_ms(struct timespec *ts_then, struct timespec *ts_now);
static struct timespec ts_start, ts_prev;
static struct timespec ts_init = { 0, 0 };

//extern bool plotting;
float max_Qperf = 1;;
float max_Wattage = 1;
float max_QperW = 1;
static int util_main(enum slider_value sld)
{
	int max_util, present_state;
	int next_freq;
	float dummy;

	int last_max = get_last_maxutil();

	update_perf_diffs(&dummy, 0);
	max_util = (int)round(grp.c0_max);

	if (last_max > 0)
		grp.delta = max_util - last_max;
	else
		last_max = 0;

	present_state = get_cur_state();
	/*
	 * we do not want to track avg util for following cases:
	 * a) bypass mode where the solution temporary bypassed
	 * b) Responsive transit mode (fast poll can flood avg leading to incorrect decisions)
	 */
	if ((present_state != BYPS_MODE) && (present_state != RESP_MODE))
		state_max_avg();

	switch (sld) {
	case performance:
	case balance_performance:
		state_machine_perf(present_state);
		break;
	case power_saver:
	case balance_power:
		state_machine_power(present_state);
		break;
	case balanced:
                state_machine_auto(present_state);
                break;
	case unknown:
	case MAX_SLIDER:
		exit_state_change();
		exit(0);
		break;
	}

	if (state_has_ppw(get_cur_state())) {
		/* 
		 * clamping to higher freq in PPW is needed for following reasons:
		 * - race to halt within the ON portion of idle-inject
		 * - compensates for idle-inject response lag (e.g avoid mouse jitter)
		 * XXX discover some value close to turbo. hardcoded for now. 
		 */
		next_freq = 24;
		clamp_to_freq(get_cur_state(), next_freq);

		if (IDLE_INJECT_FEATURE) {
			if (A_GT_B((max_util + INJ_BUF_PCT), UTIL_NEAR_FULL)) {
				/* stop idle injection as long has max util exceeds     */
				inject_update = PAUSE;
				/* XXX check if freq and inject are already expected state */
				process_cpu_powerclamp_exit();
				log_debug
				    (" maxutil:%d poll: %d idle_pct: PAUSED sma: %d\n",
				     max_util, get_state_poll(max_util,
							      get_cur_state()),
				     grp.sma_avg1);
			} else {
				if (inject_update != RUNNING) {
					process_cpu_powerclamp_enter
					    (get_state_poll
					     (max_util, get_cur_state()) * DURATION_SPILL,
					     MAX_IDLE_INJECT);
					log_debug
					    (" maxutil:%d poll: %d idle_pct: %d sma:%d BACK.\n",
					     max_util, get_state_poll(max_util,
								      get_cur_state
								      ()),
					     MAX_IDLE_INJECT, grp.sma_avg1);
				}
				inject_update = RUNNING;
			}
		}
	}

	int spike_rate = get_spike_rate();
	int poll = get_state_poll(max_util, get_cur_state());
//	if ((present_state != get_cur_state())
//	    || ( diff_ms(&ts_prev, &ts_start) > 200 ))
	 {
		if (!(record % RECORDS_PER_HEADER)
		    && (ts_start.tv_sec - ts_prev.tv_sec) > 10) {
			log_info
			    ("\n  time.ms, sldr, state, sma1, sma2, sma3, 1stmax, 2ndmax, 3rdmax, nx_poll, nx_st, Qperf,    Watt,     PPW, min_s0, cpu_s0, SpkRt, Rcnt, brst_pm\n");
		}
		log_info
		     ("%05ld.%03ld,   %2d,  %4d,  %3d,  %3d,  %3d, %6.2f, %6.2f, %6.2f,  %6d,  %4d, %5.1f,  %6.2f,  %6.2f,   %.2f,    %3d,  %3d,   %3d,  %3d  %d\n",
		     ts_start.tv_sec - ts_init.tv_sec,
		     ts_start.tv_nsec / 1000000, sld, present_state,
		     grp.sma_avg1, grp.sma_avg2, grp.sma_avg3, grp.c0_max,
		     grp.c0_2nd_max, grp.c0_3rd_max, poll,
		     get_cur_state(), dummy / 550, soc_mw / 280,
		     dummy * 10 / soc_mw,
		     grp.worst_stall, grp.worst_stall_cpu,
		     spike_rate,
		     get_stay_count(MDRT3E_MODE),
		     get_stay_count(PERF_MODE),
		     get_burst_rate_per_min());

		record++;
		/*if (plotting) {
			float Qperf = dummy/max_Qperf;
			float Wattage = soc_mw/max_Wattage;
			float QperW = Qperf/Wattage;

			update_plot(Qperf, QperW, Wattage, grp.c0_max/100, present_state);
			if (dummy > max_Qperf)
				max_Qperf = dummy;
			if (soc_mw > max_Wattage)
				max_Wattage = soc_mw;
			if (QperW > max_QperW)
				max_QperW = QperW;
		}*/
		ts_prev = ts_start;
	}


	if (last_max != DEACTIVATED)
		set_last_maxutil(max_util);
	set_last_poll(poll);

	/* XXX if there was stage change do re-evaluate max util in new state */
	return max_util;
}

int inject_active()
{
	switch (inject_update) {
	case ACTIVATED:
	case RUNNING:
		return 1;
		break;
	default:
		return 0;
	}
}

#define MSEC_PER_SEC (1000)
#define NSEC_PER_MSEC (1000000)
uint64_t diff_ms(struct timespec *ts_then, struct timespec *ts_now)
{
	uint64_t ns_sum = 0;
	int64_t diff = 0;
	if (ts_now->tv_sec > ts_then->tv_sec) {
		diff = (ts_now->tv_sec - ts_then->tv_sec) * MSEC_PER_SEC;
		ns_sum = (ts_then->tv_nsec + ts_now->tv_nsec)/NSEC_PER_MSEC;
		if (ns_sum < diff) // case where diff is >= 2
			diff = diff - (ts_then->tv_nsec + ts_now->tv_nsec)/NSEC_PER_MSEC;
		else // i.e case where diff is < 2
			diff = (ts_then->tv_nsec + ts_now->tv_nsec)/NSEC_PER_MSEC - diff;
	} else {
		diff += (ts_now->tv_nsec - ts_then->tv_nsec)/NSEC_PER_MSEC;
	}
	return diff;
}

//extern enum slider_value sld;
void *state_handler(void)
{
	int next_poll;
	int util_max;
	enum lp_state_idx next_state;
//	int latency_ms;
	clockid_t clk = CLOCK_MONOTONIC;

	initialize_state_mask();
	sma_init();

	if (!ts_init.tv_sec && (clock_gettime(clk, &ts_init)))
		perror("clock_gettime init");
	for (;;) {
		if (clock_gettime(clk, &ts_start))
			perror("clock_gettime start");

		//util_max = util_main(sld);
		util_max = util_main(0);//todo: update slider value 

		if (check_reset_status()) {
			apply_state_change();
//                      if (clock_gettime(clk, &ts_end))
//                                perror("clock_gettime end");
//                      latency_ms = diff_ns(&ts_start, &ts_end)/1000000;
//                      printf("latency: %d ms\n", latency_ms);
		}

		next_state = get_cur_state();
		if (likely(is_state_valid(next_state))) {
			next_poll = get_state_poll(util_max, next_state);
			log_debug(" max_util %d next state:%d Poll:%4d \n",
				  util_max, next_state, next_poll);
			usleep(next_poll * 1000);
		} else {
			lpmd_log_info("unknown state %d", next_state);
			break;
		}
	}
	return NULL;
}

void update_state_epp(enum lp_state_idx state)
{
	for (int t = 0; t < get_max_online_cpu(); t++) {
		update_epp(perf_stats[t].dev_msr_fd,
			   (uint64_t) get_state_epp(state));
	}
}

void update_state_epb(enum lp_state_idx state)
{
	for (int t = 0; t < get_max_online_cpu(); t++) {
		update_epb(perf_stats[t].dev_msr_fd,
			   (uint64_t) get_state_epb(state));
	}
}

int perf_stat_init(void)
{
	int max_cpus = get_max_cpus();
	perf_stats = NULL; 
	perf_stats = malloc(sizeof(perf_stats_t) * max_cpus);
    if ( !perf_stats ) {
        return 0;
    }

	for (int t = 0; t < max_cpus; t++) {
        memset( &perf_stats[t], 0, sizeof(perf_stats_t));

		if (!is_cpu_online(t))
			continue;
		perf_stats[t].cpu = t;
		perf_stats[t].dev_msr_fd = initialize_dev_msr(t);
		perf_stats[t].orig_epp =
		    update_epp(perf_stats[t].dev_msr_fd,
			       (uint64_t) PERFORMANCE_EPP);
		perf_stats[t].orig_epb =
		    update_epb(perf_stats[t].dev_msr_fd, (uint64_t) EPB_AC);
	}
	return 1;
}

void perf_stat_uninit(){
    int max_cpus = get_max_cpus();
	if (perf_stats) {
	    for (size_t i = 0; i < max_cpus; ++i) {
            memset( &perf_stats[i], 0, sizeof(perf_stats_t));	        	    
    	}
    	free(perf_stats); 
	}

}

/* EP BIAS. XXX switch to sysfs */

uint32_t update_epb(int fd, uint64_t new_value)
{
	uint64_t orig_value;
	read_msr(fd, (uint32_t) MSR_EPB, &orig_value);
	write_msr(fd, (uint32_t) MSR_EPB, &new_value);
	return (uint32_t) orig_value;
}

int revert_orig_epb(void)
{
	for (int t = 0; t < get_max_cpus(); t++) {
		if (!is_cpu_online(t))
			continue;
		write_msr(perf_stats[t].dev_msr_fd, (uint32_t) MSR_EPB,
			  &perf_stats[t].orig_epb);
	}
	return 1;
}

/* EP Preference. XXX switch to sysfs */
uint32_t update_epp(int fd, uint64_t new_value)
{
	uint64_t orig_value;
	read_msr(fd, (uint32_t) MSR_HWP, &orig_value);
	new_value = (((orig_value << 40) >> 40) | (new_value << 24));
	write_msr(fd, (uint32_t) MSR_HWP, &new_value);
	return (uint32_t) orig_value;
}

int revert_orig_epp(void)
{
	for (int t = 0; t < get_max_cpus(); t++) {
		if (!is_cpu_online(t))
			continue;
		write_msr(perf_stats[t].dev_msr_fd, (uint32_t) MSR_HWP,
			  &perf_stats[t].orig_epp);
	}
	return 1;
}

/*defined in lpmd_util*/
int util_init_proxy(void)
{
	float dummy;

	if (IDLE_INJECT_FEATURE)
		check_cpu_powerclamp_support();

	init_all_fd();

	init_delta_vars(get_max_online_cpu());

	update_perf_diffs(&dummy, 1);

	initialize_cpu_hfm_mhz(perf_stats[0].dev_msr_fd);

	//state_handler();
    initialize_state_mask();
    sma_init();


	//close_all_fd();
	return 1;
}
