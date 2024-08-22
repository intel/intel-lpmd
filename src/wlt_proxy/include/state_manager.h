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

#include "wlt_proxy_common.h"

int init_cpu_proxy(void);//defined in lpmd.h
void uninit_cpu_proxy();

bool is_state_disabled(enum lp_state_idx);
int apply_state_change(void);

enum lp_state_idx get_cur_state(void);

void set_cur_state(enum lp_state_idx);
int is_state_valid(enum lp_state_idx);

int cpu_applicable(int, enum lp_state_idx);

void set_state_reset(void);
int set_last_maxutil(int);

int get_last_poll(void);
int get_poll_ms(enum lp_state_idx);
int get_state_poll(int, enum lp_state_idx);

int set_stay_count(enum lp_state_idx, int);
int get_stay_count(enum lp_state_idx);

int do_countdown(enum lp_state_idx);
void initialize_state_mask(void);

void exit_state_change(void);

#ifdef __REMOVE__
cpu_set_t *get_cpu_mask(enum lp_state_idx idx);
char *get_cpus_hexstr(enum lp_state_idx);
#endif

#ifdef __REMOVE__
int process_cpu_powerclamp_enter(int, int);
int process_cpu_powerclamp_exit(void);
#endif


#ifdef __REMOVE__
int state_has_ppw(enum lp_state_idx);
#endif

#ifdef __REMOVE__
int get_last_maxutil(void);
int set_last_poll(int);
#endif

#ifdef __REMOVE__
int get_min_freq(int);
int get_turbo_freq(int);

int get_freq_map(int j, struct _freq_map *fmap);
int get_freq_map_count(void);
#endif

#ifdef __REMOVE__
int get_state_poll_order(enum lp_state_idx state);
#endif

#ifdef __REMOVE__
size_t alloc_cpu_set(cpu_set_t ** cpu_set);
int check_reset_status(void);

int get_state_epp(enum lp_state_idx);
int get_state_epb(enum lp_state_idx);

int state_support_freq_ctl(enum lp_state_idx);
#endif 

#ifdef __REMOVE__
int check_cpu_powerclamp_support(void);
#endif

#endif /* _CPU_GROUP_H_ */
