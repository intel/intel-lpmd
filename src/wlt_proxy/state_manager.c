/*
 * state_manager.c: Intel Linux Energy Optimier proxy detection state manager
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
 * This file contains the proxy workload type detection - state machine set/get functions.
 */
#define _GNU_SOURCE
#include <sched.h>

#include "lpmd.h" //logs
#include "state_common.h"

/*
 * If polling is too fast some of the stats (such as util)
 * could be momentarily high owing to state change disturbances.
 * avoid unexpected decision due to this as it may not be tied to workload per-se.
 * any setting below, say 15ms, needs careful assessment.
 */
#define MIN_POLL_PERIOD 15

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

static int common_min_freq;
static int freq_map_count;
int get_freq_map_count()
{
    return freq_map_count;
}

#define MAX_FREQ_MAPS (6)
static struct _freq_map freq_map[MAX_FREQ_MAPS];

int get_freq_map(int j, struct _freq_map *fmap)
{
    *fmap = freq_map[j];
    return 1;
}
static struct _lp_state lp_state[MAX_MODE] = {
    [INIT_MODE] = {.name =   "Avail cpu: P/E/L",.poll = BASE_POLL_MT,.poll_order = LINEAR},
    [PERF_MODE] = {.name =   "Perf:non-soc cpu",.poll = BASE_POLL_PERF,.poll_order = LINEAR},
    [MDRT2E_MODE] = {.name = "Moderate 2E",.poll =    BASE_POLL_MDRT2E,.poll_order = QUADRATIC},
    [MDRT3E_MODE] = {.name = "Moderate 3E",.poll = BASE_POLL_MDRT3E,.poll_order = QUADRATIC},
    [MDRT4E_MODE] = {.name = "Moderate 4E",.poll =    BASE_POLL_MDRT4E, .poll_order = QUADRATIC},
    [RESP_MODE] = {.name =   "Responsive 2L",.poll = BASE_POLL_RESP, .poll_order = CUBIC},
    [NORM_MODE] = {.name =   "Normal LP 2L",.poll = BASE_POLL_NORM, .poll_order = CUBIC},
    [DEEP_MODE] = {.name =   "Deep LP 1L",.poll = BASE_POLL_DEEP, .poll_order = CUBIC},
};

/*
    [INIT_MODE] = {.name =   "Avail cpu: P/E/L",.poll = BASE_POLL_MT,.poll_order = ZEROTH},
    [PERF_MODE] = {.name =   "Perf:non-soc cpu",.poll = BASE_POLL_PERF,.poll_order = ZEROTH},
    [MDRT2E_MODE] = {.name = "Moderate 2E",.poll =    BASE_POLL_MDRT2E,.poll_order = LINEAR},
    [MDRT3E_MODE] = {.name = "Moderate 3E",.poll = BASE_POLL_MDRT3E,.poll_order = LINEAR},
    [MDRT4E_MODE] = {.name = "Moderate 4E",.poll =    BASE_POLL_MDRT4E, .poll_order = LINEAR},
    [RESP_MODE] = {.name =   "Responsive 2L",.poll = BASE_POLL_RESP, .poll_order = CUBIC},
    [NORM_MODE] = {.name =   "Normal LP 2L",.poll = BASE_POLL_NORM, .poll_order = QUADRATIC},
    [DEEP_MODE] = {.name =   "Deep LP 1L",.poll = BASE_POLL_DEEP, .poll_order = CUBIC},
*/


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

static char to_hexchar(int val)
{
    if (val <= 9)
        return val + '0';
    if (val >= 16)
        return -1;
    return val - 10 + 'a';
}

void and_into_injmask(enum lp_state_idx idx1, enum lp_state_idx idx2,
              enum lp_state_idx inj_idx)
{
    if (!lp_state[inj_idx].inj_mask)
        return;

    CPU_AND_S(size_cpumask, lp_state[inj_idx].inj_mask,
          lp_state[idx1].mask, lp_state[idx2].mask);
}

char* get_cpus_hexstr(enum lp_state_idx idx)
{
    int ret;
    if (!lp_state[idx].mask)
        return NULL;

    if (!CPU_COUNT_S(size_cpumask, lp_state[idx].mask))
        return NULL;

    if (lp_state[idx].hexstr)
        return lp_state[idx].hexstr;

    lp_state[idx].hexstr = calloc(1, MAX_STR_LENGTH);
    if (!lp_state[idx].hexstr)
        lpmd_log_error("3, STR_ALLOC\n");

    ret = cpumask_to_hexstr(lp_state[idx].mask, lp_state[idx].hexstr,
              MAX_STR_LENGTH);
    if(ret == -1) {
        //todo: handle error
    }
    return lp_state[idx].hexstr;
}


//todo: check lpmd_has it?
static int fs_read_int(const char *name, int *val)
{
    FILE *filep;
    int t, ret;

    filep = fopen(name, "r");
    if (!filep) {
        lpmd_log_error("fs_read_int: Open %s failed\n", name);
        return 1;
    }

    ret = fscanf(filep, "%d", &t);
    if (ret != 1) {
        lpmd_log_error("fs_read_int: Read %s failed, ret %d\n", name, ret);
        fclose(filep);
        return 1;
    }

    fclose(filep);

    *val = t;

    return 0;

}

//todo: check lpmd_has it?
static int fs_open_check(const char *name)
{
    FILE *filep;

    filep = fopen(name, "r");
    if (!filep) {
        lpmd_log_debug("Open %s failed\n", name);
        return 1;
    }

    fclose(filep);
    return 0;
}

static int detect_lp_state_actual(void)
{
    char path[MAX_STR_LENGTH];
    int i;
    int tmp, tmp_min = INT_MAX, tmp_max = 0;
    enum lp_state_idx idx;
    int j = 0, actual_freq_buckets;
    int prev_turbo = 0;
    
    lpmd_log_debug("detect_lp_state_actual/n");

    for (idx = INIT_MODE; idx < MAX_MODE; idx++) {
        if (!alloc_cpu_set(&lp_state[idx].mask)) {
            lpmd_log_error("aloc fail");
        }
    }

    /* 
     * based on cpu advertized max turbo frequncies
     * bucket the cpu into groups (MAX_FREQ_MAPS)
     * there after map them to state's mask etc.
     * XXX: need to handle corner cases in this logic.
     */

    for (i = 0; i < get_max_online_cpu(); i++) {

        if (!is_cpu_online(i)) {
            continue;
        }
        
        snprintf(path, sizeof(path),
             "/sys/devices/system/cpu/cpu%d/cpufreq/cpuinfo_max_freq",
             i);
        fs_read_int(path, &tmp);

        if (!prev_turbo) {
            /* max turbo freq */
            freq_map[j].start_cpu = i;
            freq_map[j].turbo_freq_khz = tmp;
        } else if (prev_turbo != tmp) {
            freq_map[j].end_cpu = i - 1;    // fix me for i-1 not online
            j++;
            freq_map[j].start_cpu = i;
            freq_map[j].turbo_freq_khz = tmp;
        }
        prev_turbo = tmp;

        if (tmp < tmp_min)
            tmp_min = tmp;
        else if (tmp > tmp_max)
            tmp_max = tmp;

        /* min freq */
        snprintf(path, sizeof(path),
             "/sys/devices/system/cpu/cpu%d/cpufreq/cpuinfo_min_freq",
             i);
        fs_read_int(path, &tmp);

        if ((common_min_freq == 0) || (common_min_freq == tmp))
            common_min_freq = tmp;
        else if (common_min_freq == -1)
            continue;
        else
            common_min_freq = -1;

    }

    freq_map[j].end_cpu = i - 1;
    actual_freq_buckets = j + 1;

    lpmd_log_debug("Freq buckets [%d]\n\tbucket turbo cpu-range", actual_freq_buckets);
    for (j = 0; j <= actual_freq_buckets - 1; j++) {
        freq_map_count++;
        lpmd_log_info("\n\t [%d]  %dMHz  %d-%d", j,
               freq_map[j].turbo_freq_khz / 1000, freq_map[j].start_cpu,
               freq_map[j].end_cpu);
    }

    for (i = get_max_cpus() - 1; i >= 0; i--) {
        if (!is_cpu_online(i))
            continue;
        snprintf(path, sizeof(path),
             "/sys/devices/system/cpu/cpu%d/cpufreq/cpuinfo_max_freq",
             i);
        fs_read_int(path, &tmp);
        /* TBD: address the favoured core system with 1 or 2 top bin cpu */
        if (tmp == tmp_max) {
            /* for PERF mode need all cpu other than LP */
            if (actual_freq_buckets > 1)
                CPU_SET_S(i, size_cpumask,
                      lp_state[PERF_MODE].mask);
            //CPU_SET_S(i, size_cpumask, lp_state[BYPS_MODE].mask);
        }
        /* fix me for case of favourd cores */
        if ((tmp > tmp_min) || ((freq_map_count == 1) && (tmp == tmp_min))) {
            /* for moderate2 mode 4 cpu are sufficient */
            if (CPU_COUNT_S(size_cpumask, lp_state[MDRT4E_MODE].mask)
                >= MAX_MDRT4E_LP_CPU)
                continue;
            CPU_SET_S(i, size_cpumask, lp_state[MDRT4E_MODE].mask);

            /* for moderate mode 3 cpu are sufficient */
            if (CPU_COUNT_S(size_cpumask, lp_state[MDRT3E_MODE].mask)
                >= MAX_MDRT3E_LP_CPU)
                continue;
            CPU_SET_S(i, size_cpumask, lp_state[MDRT3E_MODE].mask);

            /* for moderate0 mode  cpu are sufficient */
            if (CPU_COUNT_S(size_cpumask, lp_state[MDRT2E_MODE].mask)
                >= MAX_MDRT2E_LP_CPU)
                continue;
            CPU_SET_S(i, size_cpumask, lp_state[MDRT2E_MODE].mask);
        }
        if (tmp == tmp_min) {
            if (CPU_COUNT_S(size_cpumask, lp_state[RESP_MODE].mask)
                >= MAX_RESP_LP_CPU)
                continue;
            CPU_SET_S(i, size_cpumask, lp_state[RESP_MODE].mask);

            /* for LP mode 2 cpu are sufficient */
            /* fixme: club the "continue" statement correctly */

            if (CPU_COUNT_S(size_cpumask, lp_state[NORM_MODE].mask)
                >= MAX_NORM_LP_CPU)
                continue;
            CPU_SET_S(i, size_cpumask, lp_state[NORM_MODE].mask);

            if (CPU_COUNT_S(size_cpumask, lp_state[DEEP_MODE].mask)
                >= MAX_DEEP_LP_CPU)
                continue;
            CPU_SET_S(i, size_cpumask, lp_state[DEEP_MODE].mask);
        }
    }

    for (i = 0; i < get_max_cpus(); i++) {
        if (!is_cpu_online(i))
            continue;
        snprintf(path, sizeof(path),
             "/sys/devices/system/cpu/cpu%d/cache/index3/level", i);
        if (!fs_open_check(path)) {
            CPU_SET_S(i, size_cpumask, lp_state[PERF_MODE].mask);
            //CPU_SET_S(i, size_cpumask, lp_state[BYPS_MODE].mask);
        }
    }

    /* bypass mode TBD. any strange workloads can be let to run bypass */
    //lp_state[BYPS_MODE].disabled = true;
    /* MDRT with 2 cores is not know to be beneficial comapred. simplyfy */
    lp_state[MDRT2E_MODE].disabled = true;
    if (get_max_online_cpu() <= 4) {
        lpmd_log_info("too few CPU: %d", get_max_online_cpu());
        exit(1);
    } else if (get_max_online_cpu() <= 8) {
        lp_state[MDRT2E_MODE].disabled = true;
        lp_state[MDRT4E_MODE].disabled = true;
    }

    int cpu_count;
    for (idx = INIT_MODE; idx < MAX_MODE; idx++) {
        cpu_count = CPU_COUNT_S(size_cpumask, lp_state[idx].mask);
        if (!lp_state[idx].disabled && cpu_count) {
            if (state_has_ppw(idx)) {
                if (!lp_state[idx].inj_mask)
                    alloc_cpu_set(&lp_state[idx].inj_mask);
                and_into_injmask(INIT_MODE, idx, idx);
            }
            lpmd_log_info("\t[%d] %s [0x%s] cpu count: %2d\n", idx, \
                   lp_state[idx].name, get_cpus_hexstr(idx), \
                   cpu_count);
        }
    }
    for (idx = INIT_MODE; idx < MAX_MODE; idx++) {
        cpu_count = CPU_COUNT_S(size_cpumask, lp_state[idx].mask);
        if (lp_state[idx].disabled || !cpu_count) {
            lpmd_log_info("\t[%d] %s [0x%s] cpu count: %2d\n", idx,
                   lp_state[idx].name, get_cpus_hexstr(idx),
                   cpu_count);
        }
    }

    return 0;
}

/*initialize*/
int init_state_manager(void)
{
    int ret;

    ret = set_max_cpu_num();
    if (ret)
        return ret;
    
    //detecting cpus
    //ret = parse_cpu_topology();

    /*ret = check_cpu_isolate_support();
    if (ret)
        return ret;*/
    

    perf_stat_init();

    ret = detect_lp_state_actual();
    if (ret)
        return ret;
    
    return 0;
}

/*clean*/
void uninit_state_manager() {
    
    exit_state_change();
    for (int idx = INIT_MODE; idx < MAX_MODE; idx++) {
        reset_cpus_proxy(idx);
    }
}
