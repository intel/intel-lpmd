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
//#define _GNU_SOURCE

#include "lpmd.h" //logs
#include "wlt_proxy_common.h"

/*
 * If polling is too fast some of the stats (such as util)
 * could be momentarily high owing to state change disturbances.
 * avoid unexpected decision due to this as it may not be tied to workload per-se.
 * any setting below, say 15ms, needs careful assessment.
 */
#define MIN_POLL_PERIOD 15

#ifdef __REMOVE__
#define MAX_MDRT4E_LP_CPU    (4)
#define MAX_MDRT3E_LP_CPU    (3)
#define MAX_MDRT2E_LP_CPU    (2)
#define MAX_RESP_LP_CPU        (2)
#define MAX_NORM_LP_CPU        (2)
#define MAX_DEEP_LP_CPU        (1)
#endif

#define BASE_POLL_RESP          96
#define BASE_POLL_MT           100
#define BASE_POLL_PERF         280
#define BASE_POLL_MDRT4E       600    // e.g., 4E cores of a module
#define BASE_POLL_MDRT3E       800    // e.g., 3E cores of a module
#define BASE_POLL_MDRT2E      1000    // e.g., 2E cores of a module
#define BASE_POLL_NORM        1200
#define BASE_POLL_DEEP        1800

static size_t size_cpumask;

struct _lp_state {
    bool disabled;
    cpu_set_t *mask;
    cpu_set_t *inj_mask;
    char *inj_hexstr;
    char *name;
    char *str;
    char *str_reverse;
    char *hexstr;
    char *hexstr_reverse;
    int poll;
    enum elastic_poll poll_order;
    int stay_count;
    int stay_count_update_sec;
    int stay_count_update_sec_prev;
    int spike_type;
    float stay_scalar;
    int epp;
    int epb;
    int ppw_enabled;
    int last_max_util;
    int last_poll;
    int freq_ctl; //enable freq clamping
};

static struct _lp_state lp_state[MAX_MODE] = {
    [INIT_MODE] = {.name =   "Avail cpu: P/E/L",.poll = BASE_POLL_MT,.poll_order = ZEROTH},
    [PERF_MODE] = {.name =   "Perf:non-soc cpu",.poll = BASE_POLL_PERF,.poll_order = ZEROTH},
    [MDRT2E_MODE] = {.name = "Moderate 2E",.poll =    BASE_POLL_MDRT2E,.poll_order = LINEAR},
    [MDRT3E_MODE] = {.name = "Moderate 3E",.poll = BASE_POLL_MDRT3E,.poll_order = LINEAR},
    [MDRT4E_MODE] = {.name = "Moderate 4E",.poll =    BASE_POLL_MDRT4E, .poll_order = LINEAR},
    [RESP_MODE] = {.name =   "Responsive 2L",.poll = BASE_POLL_RESP, .poll_order = CUBIC},
    [NORM_MODE] = {.name =   "Normal LP 2L",.poll = BASE_POLL_NORM, .poll_order = QUADRATIC},
    [DEEP_MODE] = {.name =   "Deep LP 1L",.poll = BASE_POLL_DEEP, .poll_order = CUBIC},
};

/* start current state as NORM_MODE */
static enum lp_state_idx cur_state = NORM_MODE;
static int needs_state_reset = 1;

int get_state_reset(void)
{
    return needs_state_reset;
}

void set_state_reset(void)
{
    needs_state_reset = 1;
}

char *get_mode_name(enum lp_state_idx state)
{
    return lp_state[state].name;
}

int get_mode_max(void)
{
    return MAX_MODE;
}

bool is_state_disabled(enum lp_state_idx state)
{
    return lp_state[state].disabled;
}

enum lp_state_idx get_cur_state(void)
{
    return cur_state;
}

void set_cur_state(enum lp_state_idx state)
{
    cur_state = state;
}

int is_state_valid(enum lp_state_idx state)
{
    return ((state >= INIT_MODE) && (state < MAX_MODE)
        && !lp_state[state].disabled);
}

int get_state_epp(enum lp_state_idx state)
{
    return lp_state[state].epp;
}

int get_state_epb(enum lp_state_idx state)
{
    return lp_state[state].epb;
}

int state_support_freq_ctl(enum lp_state_idx state)
{
    return lp_state[state].freq_ctl;
}

int state_has_ppw(enum lp_state_idx state)
{
    return lp_state[state].ppw_enabled;
}

int get_poll_ms(enum lp_state_idx state)
{
    return lp_state[state].poll;
}

int set_state_poll(int poll, enum lp_state_idx state)
{
    return (lp_state[state].poll = poll);
}

int set_state_poll_order(int poll_order, enum lp_state_idx state)
{
    return (lp_state[state].poll_order = poll_order);
}

int get_state_poll_order(enum lp_state_idx state)
{
    return (lp_state[state].poll_order);
}

int get_stay_count(enum lp_state_idx state)
{
    return (lp_state[state].stay_count);
}

int set_stay_count(enum lp_state_idx state, int count)
{
    return (lp_state[state].stay_count = count);
}

/* returns 1 if stay count reaches 0 for the state.*/
int do_countdown(enum lp_state_idx state)
{
    lp_state[state].stay_count -= 1;

    if (lp_state[state].stay_count <= 0) {
        lp_state[state].stay_count = 0;
        return 1;
    }

    return 0;
}

/* get poll value in microsec */
int get_state_poll(int util, enum lp_state_idx state)
{
    int poll, scale = (100 - util);
    float scale2;

    int order = (int)lp_state[state].poll_order;
    /* avoiding fpow() overhead */
    switch (order) {
    case ZEROTH:
        scale2 = (float)1;
        break;
    case LINEAR:
        scale2 = (float)scale / 100;
        break;
    case QUADRATIC:
        scale2 = (float)scale *scale / 10000;
        break;
    case CUBIC:
        scale2 = (float)scale *scale * scale / 1000000;
        break;
    default:
        scale2 = (float)scale / 100;
        break;
    }

    poll = (int)(lp_state[cur_state].poll * scale2);

    /* idle inject driver min duration is 6ms.
     * limiting min poll to MIN_POLL_PERIOD ms */
    if (poll < MIN_POLL_PERIOD)
        return MIN_POLL_PERIOD;
    
    return poll;
}

int get_last_maxutil(void)
{
    return lp_state[cur_state].last_max_util;
}

int set_last_maxutil(int v)
{
    lp_state[cur_state].last_max_util = v;
    return 1;
}

int set_last_poll(int v)
{
    lp_state[cur_state].last_poll = v;
    return 1;
}

int get_last_poll(void)
{
    return lp_state[cur_state].last_poll;
}

int check_reset_status(void)
{
    return needs_state_reset;
}

static void exit_state_change(void)
{
    set_cur_state(NORM_MODE);
    needs_state_reset = 0;

#ifdef __USE_LPMD_IRQ__
    native_restore_irqs();
#endif
}

int apply_state_change(void)
{
    float test;
    
    if (!needs_state_reset) {
        return 0;
    }

    update_perf_diffs(&test, 1);
    needs_state_reset = 0;
    
    return 1;
}

static void reset_cpus_proxy(enum lp_state_idx idx)
{

    if(lp_state[idx].str != NULL) { 
        free(lp_state[idx].str);
        lp_state[idx].str = NULL;
    }
    
    if(lp_state[idx].str_reverse != NULL) {
        free(lp_state[idx].str_reverse);
        lp_state[idx].str_reverse = NULL;
    }
    
    if(lp_state[idx].hexstr != NULL) {
        free(lp_state[idx].hexstr);
        lp_state[idx].hexstr = NULL;
    }
    
    if(lp_state[idx].hexstr_reverse != NULL) {
        free(lp_state[idx].hexstr_reverse);
        lp_state[idx].hexstr_reverse = NULL;
    }    
}

/*initialize*/
int init_state_manager(void)
{
    int ret;

    ret = set_max_cpu_num();
    if (ret)
        return ret;

    ret = check_cpu_isolate_support();
    if (ret)
        return ret;

    perf_stat_init();

    return 0;
}

/*clean*/
void uninit_state_manager() {
    
    exit_state_change();
    for (int idx = INIT_MODE + 1; idx < MAX_MODE; idx++) {
        reset_cpus_proxy(idx);
    }
}
