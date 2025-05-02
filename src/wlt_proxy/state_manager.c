/*
 * state_manager.c: Intel Linux Energy Optimizer WLT proxy detection state manager
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
 *
 * This file contains the proxy workload type detection - state definition, initialization and set/get functions.
 */
#define _GNU_SOURCE
#include <sched.h>

#include "lpmd.h" //logs
#include "state_common.h"
#include "wlt_proxy.h" //set_workload_hint

#ifdef __GNUC__
#define likely(x)       __builtin_expect(!!(x), 1)
#define unlikely(x)     __builtin_expect(!!(x), 0)
#else
#define likely(x)       (x)
#define unlikely(x)     (x)
#endif

/*
 * If polling is too fast some of the stats (such as util)
 * could be momentarily high owing to state change disturbances.
 * avoid unexpected decision due to this as it may not be tied to workload per-se.
 * any setting below, say 100ms, needs careful assessment.
 */
#define MIN_POLL_PERIOD        100

#define BASE_POLL_RESP          96
#define BASE_POLL_MT           100
#define BASE_POLL_PERF         280
#define BASE_POLL_MDRT4E       600    // e.g., 4E cores of a module
#define BASE_POLL_MDRT3E       800    // e.g., 3E cores of a module
#define BASE_POLL_MDRT2E      1000    // e.g., 2E cores of a module
#define BASE_POLL_NORM        1200
#define BASE_POLL_DEEP        1800

/* hold period (ms) before moving to deeper state */
#define MDRT_MODE_STAY        (4000)
#define PERF_MODE_STAY        (10000)

/* poll interval type */
enum elastic_poll {
    ZEROTH,
    LINEAR,
    QUADRATIC,
    CUBIC,
};

/* state properties */
struct _stState {
    bool disabled;
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
    int ppw_enabled;
    int last_max_util;
    int last_poll;
};

static struct _stState state_info[MAX_MODE] = {
    [INIT_MODE] = {.name =   "Avail cpu: P/E/L",.poll = BASE_POLL_MT,.poll_order = ZEROTH},
    [PERF_MODE] = {.name =   "Perf:non-soc cpu",.poll = BASE_POLL_PERF,.poll_order = ZEROTH},
    [MDRT2E_MODE] = {.name = "Moderate 2E",.poll =    BASE_POLL_MDRT2E,.poll_order = LINEAR},
    [MDRT3E_MODE] = {.name = "Moderate 3E",.poll = BASE_POLL_MDRT3E,.poll_order = LINEAR},
    [MDRT4E_MODE] = {.name = "Moderate 4E",.poll =    BASE_POLL_MDRT4E, .poll_order = LINEAR},
    [RESP_MODE] = {.name =   "Responsive 2L",.poll = BASE_POLL_RESP, .poll_order = CUBIC},
    [NORM_MODE] = {.name =   "Normal LP 2L",.poll = BASE_POLL_NORM, .poll_order = QUADRATIC},
    [DEEP_MODE] = {.name =   "Deep LP 1L",.poll = BASE_POLL_DEEP, .poll_order = CUBIC},
};

static enum state_idx cur_state = NORM_MODE;
static int needs_state_reset = 1;
extern int wlt_type;

int state_demote = 0;
extern int next_proxy_poll;
extern int max_util;

static void set_state_reset(void)
{
    needs_state_reset = 1;
}

enum state_idx get_cur_state(void)
{
    return cur_state;
}

static void set_cur_state(enum state_idx state)
{
    cur_state = state;
}

static int is_state_valid(enum state_idx state)
{
    return ((state >= INIT_MODE) && (state < MAX_MODE)
        && !state_info[state].disabled);
}

int get_poll_ms(enum state_idx state)
{
    return state_info[state].poll;
}

int get_stay_count(enum state_idx state)
{
    return (state_info[state].stay_count);
}

int set_stay_count(enum state_idx state, int count)
{
    return (state_info[state].stay_count = count);
}

/* return 1 if stay count reaches 0 */
int do_countdown(enum state_idx state)
{
    state_info[state].stay_count -= 1;

    if (state_info[state].stay_count <= 0) {
        state_info[state].stay_count = 0;
        return 1;
    }

    return 0;
}

/* get poll value in microsec */
int get_state_poll(int util, enum state_idx state)
{
    int poll, scale = (100 - util);
    float scale2;
    int order = (int)state_info[state].poll_order;

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

    poll = (int)(state_info[cur_state].poll * scale2);

    /* limiting min poll to MIN_POLL_PERIOD ms */
    if (poll < MIN_POLL_PERIOD)
        return MIN_POLL_PERIOD;

    return poll;
}

int get_last_maxutil(void)
{
    return state_info[cur_state].last_max_util;
}

static int set_last_maxutil(int v)
{
    state_info[cur_state].last_max_util = v;
    return 1;
}

int set_last_poll(int v)
{
    state_info[cur_state].last_poll = v;
    return 1;
}

int get_last_poll(void)
{
    return state_info[cur_state].last_poll;
}

/* initiate state change */
static int apply_state_change(void)
{
    float test;

    if (!needs_state_reset) {
        return 0;
    }

    update_perf_diffs(&test, 1);
    needs_state_reset = 0;

    return 1;
}

/* Internal state to WLT mapping*/
static int get_state_mapping(enum state_idx state){

    switch(state) {
        case PERF_MODE:
            return WLT_BURSTY;

        case RESP_MODE:
        case NORM_MODE:
            return WLT_BATTERY_LIFE;

        case DEEP_MODE:
            return WLT_IDLE;

        case INIT_MODE:
        case MDRT4E_MODE:
        case MDRT3E_MODE:
        case MDRT2E_MODE:
            return WLT_SUSTAINED;

        default:
            return WLT_IDLE;
    }
}

/* prepare for state change */
int prep_state_change(enum state_idx from_state, enum state_idx to_state,
              int reset)
{
    set_cur_state(to_state);
    set_state_reset();
    set_last_maxutil(DEACTIVATED);

    if (to_state < from_state)
        state_demote = 1;

    //proxy: apply state change and get poll interval
    apply_state_change();

    if (likely(is_state_valid(to_state))) {
        next_proxy_poll = get_state_poll(max_util, to_state);
    }

    wlt_type = get_state_mapping(to_state);

    return 1;
}

/* return staycount for the state */
int staytime_to_staycount(enum state_idx state)
{
    int stay_count = 0;

    switch (state) {
        case MDRT2E_MODE:
        case MDRT3E_MODE:
        case MDRT4E_MODE:
            stay_count = (int)MDRT_MODE_STAY/get_poll_ms(MDRT3E_MODE);
            break;
        case PERF_MODE:
            stay_count = (int)PERF_MODE_STAY/get_poll_ms(PERF_MODE);
            break;
	default:
	    break;
    }
    return stay_count;
}

/* cleanup */
void uninit_state_manager() {

    for (int idx = INIT_MODE; idx < MAX_MODE; idx++) {
        if(state_info[idx].str != NULL)
            free(state_info[idx].str);

        if(state_info[idx].str_reverse != NULL)
            free(state_info[idx].str_reverse);

        if(state_info[idx].hexstr != NULL)
            free(state_info[idx].hexstr);

        if(state_info[idx].hexstr_reverse != NULL)
            free(state_info[idx].hexstr_reverse);
    }
}
