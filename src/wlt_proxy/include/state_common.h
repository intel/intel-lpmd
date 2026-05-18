/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Copyright (C) 2026 Intel Corporation */

#ifndef _WLT_PROXY_COMMON_H_
#define _WLT_PROXY_COMMON_H_

/* threshold (%) for instantaneous utilizations */
#define UTIL_LOWEST		1
#define UTIL_LOWER		2
#define UTIL_LOW		10
#define UTIL_FILL_START		35
#define UTIL_BELOW_HALF		40
#define UTIL_HALF		50
#define UTIL_ABOVE_HALF		70
#define UTIL_NEAR_FULL		90

/* floating point comparison */
#define EPSILON		(0.01)
#define A_LTE_B(A, B)	((((B) - (A)) >= EPSILON) ? 1 : 0)
#define A_GTE_B(A, B)	((((A) - (B)) >= EPSILON) ? 1 : 0)
#define A_GT_B(A, B)	((((A) - (B)) > EPSILON) ? 1 : 0)

/* state indexes for  WLT proxy detection based cpu usage high to low */
enum state_idx {
	INIT_MODE,
	PERF_MODE,
	MDRT4E_MODE,
	MDRT3E_MODE,
	MDRT2E_MODE,
	RESP_MODE,
	NORM_MODE,
	DEEP_MODE
};

extern int state_demote;	/* from state_manager.c */
extern int burst_count;		/* from spike_mgmt.c */
extern struct group_util grp;	/* from state_util.c */
extern int next_proxy_poll;	/* from wlt_proxy.c */
extern int max_util;		/* from state_machine.c */
extern int wlt_type;		/* from wlt_proxy.c */

#define		MAX_MODE 8

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

/* feature states */
#define DEACTIVATED	(-1)
#define UNDEFINED	(0)
#define RUNNING		(1)
#define ACTIVATED	(2)
#define PAUSE		(3)

/* state_manager.c */
void uninit_state_manager(void);

enum state_idx get_cur_state(void);

int get_last_poll(void);
int get_poll_ms(enum state_idx);
int get_state_poll(int util, enum state_idx);

int set_stay_count(enum state_idx, int count);
int get_stay_count(enum state_idx);

int staytime_to_staycount(enum state_idx state);
int prep_state_change(enum state_idx, enum state_idx, int reset);

int do_countdown(enum state_idx);

/* state_util.c */
int util_init_proxy(void);
void util_uninit_proxy(void);

int state_max_avg(void);
int update_perf_diffs(float *sum_norm_perf, int stat_init_only);

int max_mt_detected(enum state_idx);

/* state_machine.c */
int state_machine_auto(void);

/* spike_mgmt.c */
int add_spike_time(int duration);
int add_non_spike_time(int duration);
int get_spike_rate(void);
int get_burst_rate_per_min(void);
int fresh_burst_response(int initial_val);
int burst_rate_breach(void);
int strikeout_once(int n);

#endif /* _WLT_PROXY_COMMON_H_ */
