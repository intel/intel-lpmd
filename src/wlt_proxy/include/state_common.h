/*
 * state_common.h: Intel Linux Energy Optimizer proxy detection common header file
 *
 * Copyright (C) 2024 Intel Corporation. All rights reserved.
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
 */

#ifndef _WLT_PROXY_COMMON_H_
#define _WLT_PROXY_COMMON_H_

/* threshold (%) for instantaneous utilizations */
#define UTIL_LOWEST              1
#define UTIL_LOWER               2
#define UTIL_LOW                10
#define UTIL_FILL_START         35
#define UTIL_BELOW_HALF         40
#define UTIL_HALF               50
#define UTIL_ABOVE_HALF         70
#define UTIL_NEAR_FULL          90

/* floating point comparison */
#define EPSILON    (0.01)
#define A_LTE_B(A,B)    (((B-A) >= EPSILON) ? 1 : 0 )
#define A_GTE_B(A,B)    (((A-B) >= EPSILON) ? 1 : 0 )
#define A_GT_B(A,B)    (((A-B) > EPSILON) ? 1 : 0 )

/* state indexs for  WLT proxy detection based cpu usage high to low */
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

/* feature states */
#define DEACTIVATED  (-1)
#define UNDEFINED    (0)
#define RUNNING      (1)
#define ACTIVATED    (2)
#define PAUSE        (3)

/* state_manager.c */
void uninit_state_manager(void);

enum state_idx get_cur_state(void);

int get_last_poll(void);
int get_poll_ms(enum state_idx);
int get_state_poll(int, enum state_idx);

int set_stay_count(enum state_idx, int);
int get_stay_count(enum state_idx);

int staytime_to_staycount(enum state_idx state);
int prep_state_change(enum state_idx, enum state_idx, int);

int do_countdown(enum state_idx);

/* state_util.c */
int util_init_proxy(void);
void util_uninit_proxy(void);

int state_max_avg();
int update_perf_diffs(float *, int);

int max_mt_detected(enum state_idx);

/* state_machine.c */
int state_machine_auto();

/* spike_mgmt.c */
int add_spike_time(int);
int add_non_spike_time(int);
int get_spike_rate(void);
int get_burst_rate_per_min(void);
int fresh_burst_response(int initial_val);
int burst_rate_breach(void);
int strikeout_once(int);

#endif /* _WLT_PROXY_COMMON_H_ */
