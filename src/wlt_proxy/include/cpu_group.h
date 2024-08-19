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
 * some functions are from intel_lpmd project
 * Author: Noor ul Mubeen <noor.u.mubeen@intel.com>
 */
#ifndef _CPU_GROUP_H_
#define _CPU_GROUP_H_

#define _GNU_SOURCE
#include <math.h>
#include <stdint.h>
#include <sched.h>
#include "wlt_proxy_common.h"

/*
 * If polling is too fast some of the stats (such as util)
 * could be momentarily high owing to state change disturbances.
 * avoid unexpected decision due to this as it may not be tied to workload per-se.
 * any setting below, say 15ms, needs careful assessment.
 */
#define MIN_POLL_PERIOD 15

#define MAX_CPUS_NUM        1024
#define MAX_CPUMASK_SIZE    MAX_CPUS_NUM / 8

#define MAX_MDRT4E_LP_CPU    (4)
#define MAX_MDRT3E_LP_CPU    (3)
#define MAX_MDRT2E_LP_CPU    (2)
#define MAX_RESP_LP_CPU        (2)
#define MAX_NORM_LP_CPU        (2)
#define MAX_DEEP_LP_CPU        (1)

struct _freq_map {
    int start_cpu;
    int end_cpu;
    int turbo_freq_khz;
    int perf_order;
};

int is_cpu_online(int);
int get_max_cpus(void);
int get_max_online_cpu(void);
int init_cpu_proxy(void);//defined in lpmd.h
void uninit_cpu_proxy();
cpu_set_t *get_cpu_mask(enum lp_state_idx idx);
char *get_cpus_hexstr(enum lp_state_idx);
int process_cpu_powerclamp_enter(int, int);
int process_cpu_powerclamp_exit(void);
bool is_state_disabled(enum lp_state_idx);
int apply_state_change(void);
enum lp_state_idx get_cur_state(void);
int cpu_applicable(int, enum lp_state_idx);
int state_has_ppw(enum lp_state_idx);
void set_state_reset(void);
int get_last_maxutil(void);
int set_last_maxutil(int);
int set_last_poll(int);
int get_last_poll(void);
int get_min_freq(int);
int get_turbo_freq(int);
int get_freq_map(int j, struct _freq_map *fmap);
int get_freq_map_count(void);
int get_poll_ms(enum lp_state_idx);
int get_state_poll(int, enum lp_state_idx);
int get_state_poll_order(enum lp_state_idx state);
int set_stay_count(enum lp_state_idx, int);
int get_stay_count(enum lp_state_idx);
int do_countdown(enum lp_state_idx);
void initialize_state_mask(void);
size_t alloc_cpu_set(cpu_set_t ** cpu_set);
int check_reset_status(void);
int get_state_epp(enum lp_state_idx);
int get_state_epb(enum lp_state_idx);
int state_support_freq_ctl(enum lp_state_idx);
void exit_state_change(void);
int check_cpu_powerclamp_support(void);

#endif                /* _CPU_GROUP_H_ */
