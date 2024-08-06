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

#ifndef _WLT_PROXY_COMMON_H_
#define _WLT_PROXY_COMMON_H_

#define _GNU_SOURCE
#include <math.h>
#include <stdint.h>
#include <sched.h>
#include <stdbool.h>

/*
 * If polling is too fast some of the stats (such as util)
 * could be momentarily high owing to state change disturbances.
 * avoid unexpected decision due to this as it may not be tied to workload per-se.
 * any setting below, say 15ms, needs careful assessment.
 */
#define MIN_POLL_PERIOD 15

/*
 * stall scalability refer to non-stallable percentage of utilization.
 * e.g due to memory or other depenency. If work is reasonably scaling well,
 * values in 80 to 90+% is expected
 */
#define STALL_SCALE_LOWER_MARK	70

/* threshold (%) for instantaneous utilizations */
#define UTIL_LOWEST              1
#define UTIL_LOWER               2
#define UTIL_LOW                10
#define UTIL_FILL_START         35
#define UTIL_BELOW_HALF		40
#define UTIL_HALF		50
#define UTIL_ABOVE_HALF		70
#define UTIL_NEAR_FULL          90

/* threshold (%) for sustained (avg) utilizations */
#define SUS_LOWEST		 1
#define SUS_LOWER		 2
#define SUS_LOW_RANGE_START	 4
#define SUS_LOW_RANGE_END	25

/* hold period (ms) before moving to deeper state */
#define MDRT_MODE_STAY		(15000)
#define PERF_MODE_STAY		(300000)

#define BURST_COUNT_THRESHOLD	3
/* 
 * DELTA_THRESHOLD is extent of spike that exits low-power mode 
 * too small - means we exit too often 
 * too large - means we may take a hit on legit responsiveness
 */
#define DELTA_THRESHOLD		(70.0)

#define BASE_POLL_RESP		  96
#define BASE_POLL_MT		 100
#define BASE_POLL_PERF		 280
#define BASE_POLL_MDRT4E	 600	// e.g., 4E cores of a module
#define BASE_POLL_MDRT3E	 800	// e.g., 3E cores of a module
#define BASE_POLL_MDRT2E	1000	// e.g., 2E cores of a module
#define BASE_POLL_NORM		1200
#define BASE_POLL_DEEP		1800

#define PPW_EFFICIENCY_FEATURE    (1)
#define IDLE_INJECT_FEATURE       (0)
#define MAX_UTIL_INJECT		 (70)
#define MAX_IDLE_INJECT		 (100 - MAX_UTIL_INJECT)
#define	INJ_BUF_PCT		 (UTIL_NEAR_FULL - MAX_UTIL_INJECT)

/* 
 * DURATION_SPILL is work-around for current re-arm implementation problem:
 * given that idle injection (off time) occurs first followed by normal work (on time)
 * if programmed idle duration is <= this program's current poll period, a 
 * new inject cycle would begin even before we re-evaluate and re-arm.
 * therefore, program total duration safely beyond poll period so we 
 * can always re-arm with correct new value.
 * if inject portion was tail-end we would not need this.
 */
#define DURATION_SPILL		(1.2)

#define EPB_AC			(6)
#define EPB_DC			(8)
#define POWERSAVE_EPP_PCT	(70)
#define POWERSAVE_EPP 		((uint)ceil(POWERSAVE_EPP_PCT*256/100))
#define BALANCED_EPP_PCT	(50)
#define BALANCED_EPP 		((uint)ceil(BALANCED_EPP_PCT*256/100))
#define PERFORMANCE_EPP_PCT	(25)
#define PERFORMANCE_EPP 	((uint)ceil(PERFORMANCE_EPP_PCT*256/100))

#define MAX_STR_LENGTH		256
#define CLSS_SOCKET_TMPFS	"/run/eco"

/* floating point comparison */
#define EPSILON	(0.01)
#define A_LTE_B(A,B)	(((B-A) >= EPSILON) ? 1 : 0 )
#define A_GTE_B(A,B)	(((A-B) >= EPSILON) ? 1 : 0 )
#define A_GT_B(A,B)	(((A-B) > EPSILON) ? 1 : 0 )

#define RECORDS_PER_HEADER	(30)
extern int slider;
enum slider_value {
	unknown,
	performance,
	balance_performance,
	balance_power,
	balanced,
	power_saver,
	MAX_SLIDER,
};

enum lp_state_idx {
	INIT_MODE,	// 0
	BYPS_MODE,	// 1
	PERF_MODE,	// 2
	MDRT4E_MODE,	// 3
	MDRT3E_MODE,	// 4
	MDRT2E_MODE,	// 5
	RESP_MODE,	// 6
	NORM_MODE,	// 7
	DEEP_MODE	// 8
//	MAX_MODE,	// 9
};
#define	MAX_MODE 9

struct group_util {
	/* top 3 max utils and last (min) util */
	float c0_max;
	float c0_min;
	float worst_stall;
	int worst_stall_cpu;
	float c0_2nd_max;
	float c0_3rd_max;
	int delta;
	/* simple moving average for top 3 utils */
	int sma_sum[3];
	int sma_avg1;
	int sma_avg2;
	int sma_avg3;
	int sma_pos;
};

enum elastic_poll {
	ZEROTH,
	LINEAR,
	QUADRATIC,
	CUBIC,
};

/* feature states */
#define	DEACTIVATED 	(-1)
#define UNDEFINED	(0)
#define RUNNING		(1)
#define ACTIVATED	(2)
#define PAUSE 		(3)


//#ifdef _USE_ECO
/* main.c */
void eco_printf(int level, const char *format, ...);

#define log_err(fmt, ...)			\
do {							\
		eco_printf(0, fmt, ##__VA_ARGS__);	\
} while (0)

#define log_info(fmt, ...)			\
do {							\
		eco_printf(1, fmt, ##__VA_ARGS__);	\
} while (0)

#define log_debug(fmt, ...)				\
do {							\
		eco_printf(2, fmt, ##__VA_ARGS__);	\
} while (0)

#define log_verbose(fmt, ...)			\
do {							\
		eco_printf(3, fmt, ##__VA_ARGS__);	\
} while (0)
	
//#endif

#ifdef __GNUC__
#define likely(x)       __builtin_expect(!!(x), 1)
#define unlikely(x)     __builtin_expect(!!(x), 0)
#else
#define likely(x)       (x)
#define unlikely(x)     (x)
#endif

void print_state(void);
int get_cpu_mode(void);
void set_cur_state(enum lp_state_idx);
int is_state_valid(enum lp_state_idx);

/*mapping the state to wlt proxy workload type*/
int getStateMapping(int state); 

/* irq.c */
int init_irq(void);
int process_irqs_proxy(int);//defined in lpmd
int dump_interrupts(int);
int restore_irq_mask(void);
int update_irqs(void);

/* util.c */
int util_init_proxy(void);//defined in lpmd_util
void set_eco_timeout(int);
int get_msr_fd(int);
int perf_stat_init(void);
void unclamp_default_freq(enum lp_state_idx);
int update_perf_diffs(float *, int);
int revert_orig_epp(void);
int revert_orig_epb(void);
int max_mt_detected(enum lp_state_idx);
int prep_state_change(enum lp_state_idx, enum lp_state_idx, int);
int count_up_breach(int);
int grp_c0_breach(void);
int breach_per_sec(int);
int state_toggle_per_sec(int);
int grp_c0_breach_fast(void);
int staytime_to_staycount(enum lp_state_idx);
void perf_stat_uninit(); 

/* perf_msr.c*/
void uninit_delta_vars(); 

/* helper */
int fs_write_str(const char *name, char *str);
int fs_write_str_append(const char *name, char *str);
int fs_write_int(const char *name, int val);
int fs_open_check(const char *name);
int fs_read_int(const char *name, int *val);
int fs_read_str(const char *name, char *val);
int open_fd(const char *name, int flags);
int close_fd(int fd);
int init_rapl_fd(void);
void close_rapl_fd(void);
long long read_rapl_pkg0(void);
void init_all_fd(void);
void close_all_fd(void);
int write_cgroup_partition(const char *);
int write_cgroup_isolate(const char *);
int write_str_fd(int fd, const char *);
int read_str_fd(int fd, char *);
FILE *open_fs(const char *, char *);
int close_fs(FILE *);
int write_str_fs(FILE *, const char *);

char *get_mode_name(enum lp_state_idx);
int get_mode_cpu_count(enum lp_state_idx);
int get_mode_max(void);

/* slider */
int slider_monitor(void);
int set_slider(int s);
int get_slider(void);

/* state machine */
int state_machine_power(int);
int state_machine_perf(int);
int state_machine_auto(int);

/* spike_mgmt */
int add_spike_time(int);
int add_non_spike_time(int);
int get_spike_rate(void);
int get_burst_rate_per_min(void);
int fresh_burst_response(int initial_val);
int burst_rate_breach(void);

//int set_spike_type(int);
int strikeout_once(int);
int update_burst_count(int);
int update_spike_rate_maxima();
int clear_spike_rate_maxima();

#endif				/* _WLT_PROXY_COMMON_H_ */
