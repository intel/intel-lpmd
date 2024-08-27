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

/* threshold (%) for instantaneous utilizations */
#define UTIL_LOWEST              1
#define UTIL_LOWER               2
#define UTIL_LOW                10
#define UTIL_FILL_START         35
#define UTIL_BELOW_HALF         40
#define UTIL_HALF               50
#define UTIL_ABOVE_HALF         70
#define UTIL_NEAR_FULL          90

/* hold period (ms) before moving to deeper state */
#define MDRT_MODE_STAY        (15000)
#define PERF_MODE_STAY        (300000)

#define BURST_COUNT_THRESHOLD    3
/* 
 * DELTA_THRESHOLD is extent of spike that exits low-power mode 
 * too small - means we exit too often 
 * too large - means we may take a hit on legit responsiveness
 */
#define DELTA_THRESHOLD        (70.0)

#define BASE_POLL_RESP          96
#define BASE_POLL_MT           100
#define BASE_POLL_PERF         280
#define BASE_POLL_MDRT4E       600    // e.g., 4E cores of a module
#define BASE_POLL_MDRT3E       800    // e.g., 3E cores of a module
#define BASE_POLL_MDRT2E      1000    // e.g., 2E cores of a module
#define BASE_POLL_NORM        1200
#define BASE_POLL_DEEP        1800

#define PPW_EFFICIENCY_FEATURE    (1)

#define IDLE_INJECT_FEATURE       (0)
#define MAX_UTIL_INJECT           (70)
#define MAX_IDLE_INJECT           (100 - MAX_UTIL_INJECT)
#define INJ_BUF_PCT               (UTIL_NEAR_FULL - MAX_UTIL_INJECT)

/* 
 * DURATION_SPILL is work-around for current re-arm implementation problem:
 * given that idle injection (off time) occurs first followed by normal work (on time)
 * if programmed idle duration is <= this program's current poll period, a 
 * new inject cycle would begin even before we re-evaluate and re-arm.
 * therefore, program total duration safely beyond poll period so we 
 * can always re-arm with correct new value.
 * if inject portion was tail-end we would not need this.
 */
#define DURATION_SPILL        (1.2)

/* floating point comparison */
#define EPSILON    (0.01)
#define A_LTE_B(A,B)    (((B-A) >= EPSILON) ? 1 : 0 )
#define A_GTE_B(A,B)    (((A-B) >= EPSILON) ? 1 : 0 )
#define A_GT_B(A,B)    (((A-B) > EPSILON) ? 1 : 0 )

#define RECORDS_PER_HEADER    (30)
extern int slider;

enum lp_state_idx {
    INIT_MODE,    //BYPS_MODE,
    PERF_MODE,
    MDRT4E_MODE,
    MDRT3E_MODE,
    MDRT2E_MODE,
    RESP_MODE,
    NORM_MODE,
    DEEP_MODE
};
#define    MAX_MODE 8

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
#define DEACTIVATED  (-1)
#define UNDEFINED    (0)
#define RUNNING      (1)
#define ACTIVATED    (2)
#define PAUSE        (3)

#ifndef __USE_LPMD_IRQ__
/* irq.c */
#endif

/* util.c */
int util_init_proxy(void);//defined in lpmd_util
void util_uninit_proxy(void);

int perf_stat_init(void);
//void perf_stat_uninit();
int state_max_avg();

int prep_state_change(enum lp_state_idx, enum lp_state_idx, int);
int update_perf_diffs(float *, int);

int staytime_to_staycount(enum lp_state_idx);
int max_mt_detected(enum lp_state_idx);

/* state machine */
int state_machine_auto();

/* spike managament */
int add_spike_time(int);
int add_non_spike_time(int);
int get_spike_rate(void);
int get_burst_rate_per_min(void);
int fresh_burst_response(int initial_val);
int burst_rate_breach(void);
int strikeout_once(int);
int update_spike_rate_maxima();
int clear_spike_rate_maxima();

#endif /* _WLT_PROXY_COMMON_H_ */
