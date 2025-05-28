/*
 * state_util.c: Intel Linux Energy Optimizer WLT calculations
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
 * This file contains the proxy workload type detection data collection and calculations [perf, sma].
 */

#include <linux/perf_event.h> //perf_event_attr
#include <asm/unistd.h> //syscall __NR_perf_event_open
#include <stdio.h>
#include <stdint.h> //uint64_t
#include <math.h> //round

#include "lpmd.h"
#include "state_common.h"

/*
 * simple moving average (sma), event count based - not time.
 * updated for upto top 3 max util streams.
 * exact cpu # is not tracked; only the max since continuum of task
 * keeps switching cpus anyway.
 * array implementation with SMA_LENGTH number of values.
 */
#define SMA_LENGTH          (25)
#define SMA_CPU_COUNT       (3)
#define SCALE_DECIMAL       (100)

static int sample[3][SMA_LENGTH];

enum core_type {
    P_CORE = 1,
    E_CORE = 2,
    L_CORE = 3
};

typedef struct {

    int cpu;

    enum core_type cpu_type;

    int aperf_fd;
    int mperf_fd;
    int pperf_fd;

    uint64_t aperf_diff;
    uint64_t mperf_diff;
    uint64_t pperf_diff;
    uint64_t tsc_diff;

    uint64_t nperf;
    /*
     * As initial freq f0 changes to some other value
     * in the next cycle, it influences the initial
     * load l0 and associated stall-factor (1-s0)
     * track them for perf-per-watt evaluation.
     */
    float f0;
    float l0;
    float s0;

} perf_stats_t;

perf_stats_t *perf_stats;
struct group_util grp;

struct thread_data {
    unsigned long long tsc;
    unsigned long long aperf;
    unsigned long long mperf;
    unsigned long long pperf;
} *thread_even, *thread_odd;

static uint64_t *last_aperf = NULL;
static uint64_t *last_mperf = NULL;
static uint64_t *last_pperf = NULL;
static uint64_t *last_tsc = NULL;

/*
 * Intel Alderlake hardware errata #ADL026: pperf bits 31:64 could be incorrect.
 * https://edc.intel.com/content/www/us/en/design/ipla/software-development-plat
 * forms/client/platforms/alder-lake-desktop/682436/007/errata-details/#ADL026
 * u644diff() implements a workaround. Assuming real diffs less than MAX(uint32)
 */
#define u64diff(b, a) (((uint64_t)b < (uint64_t)a) ?                 \
            (uint64_t)((uint32_t)~0UL - (uint32_t)a + (uint32_t)b) :\
            ((uint64_t)b - (uint64_t)a))

/* routine to evaluate & store a per-cpu msr value's diff */
#define VARI(a, b, i) a##b[i]
#define cpu_generate_msr_diff(scope) \
uint64_t cpu_get_diff_##scope(uint64_t cur_value, int instance)\
{ \
    uint64_t diff; \
    diff = (VARI(last_, scope, instance) == 0) ? \
            0 : u64diff(cur_value, VARI(last_, scope, instance)); \
    VARI(last_, scope, instance) = cur_value; \
    return diff; \
}

/********************Perf calculation - begin *****************************************/

cpu_generate_msr_diff(aperf);
cpu_generate_msr_diff(mperf);
cpu_generate_msr_diff(pperf);
cpu_generate_msr_diff(tsc);

/* initialize perf_stat structure */
static int perf_stat_init(void) {

    int max_cpus = get_max_cpus();

    perf_stats = NULL;
    perf_stats = calloc(sizeof(perf_stats_t), max_cpus);
    if ( !perf_stats ) {
        lpmd_log_error("WLT_Proxy: memory failure\n");
        return 0;
    }

    for (int t = 0; t < max_cpus; t++) {
        if(!is_cpu_online(t)) {
            continue;
        }

        perf_stats[t].cpu = t;

        if (is_cpu_pcore(t)) {
            perf_stats[t].cpu_type = P_CORE;
        } else if (is_cpu_ecore(t)) {
            perf_stats[t].cpu_type = E_CORE;
        } else {
            perf_stats[t].cpu_type = L_CORE;
        }
    }

    return 1;
}

/* is cpu applicable for the given state*/
static int cpu_applicable(int cpu, enum state_idx state)
{
    switch (state) {
        case INIT_MODE:
            //for INIT mode need all cores [P,E,L]
            return 1;
        case NORM_MODE: // 2 L cores
        case DEEP_MODE: // 1 L core
        case RESP_MODE: // all L core
        case MDRT2E_MODE: // 2 E cores
        case MDRT3E_MODE: // 3 E cores
        case MDRT4E_MODE: // 4 E cores
        case PERF_MODE:
            if (perf_stats[cpu].cpu_type != L_CORE) {
                return 1;
            }
        default:
            break;
    }

    return 0;
}

static int init_perf_calculations(int n)
{
    if (!perf_stat_init()) {
        lpmd_log_error("\nerror initiating cpu proxy\n");
        return -1;
    }

    last_aperf = calloc(sizeof(uint64_t), n);
    last_mperf = calloc(sizeof(uint64_t), n);
    last_pperf = calloc(sizeof(uint64_t), n);
    last_tsc = calloc(sizeof(uint64_t), n);
    if (!last_aperf || !last_mperf || !last_mperf || !last_tsc) {
        lpmd_log_error("calloc failure perf vars\n");
        return -2;
    }

    return LPMD_SUCCESS;
}

/*helper - pperf reading */
static int read_perf_counter_info(const char *const path, const char *const parse_format, void *value_ptr) {
    int fdmt;
    int bytes_read;
    char buf[64];
    int ret = -1;

    fdmt = open(path, O_RDONLY, 0);
    if (fdmt == -1) {
        lpmd_log_error("Failed to parse perf counter info %s\n", path);
        ret = -1;
        goto cleanup_and_exit;
    }

    bytes_read = read(fdmt, buf, sizeof(buf) - 1);
    if (bytes_read <= 0 || bytes_read >= (int)sizeof(buf)) {
        lpmd_log_error("Failed to parse perf counter info %s\n", path);
        ret = -1;
        goto cleanup_and_exit;
    }

    buf[bytes_read] = '\0';

    if (sscanf(buf, parse_format, value_ptr) != 1) {
        lpmd_log_error("Failed to parse perf counter info %s\n", path);
        ret = -1;
        goto cleanup_and_exit;
    }

    ret = 0;

cleanup_and_exit:
    if (fdmt >= 0)
        close(fdmt);

    return ret;
}

/*helper - pperf reading */
static unsigned int read_perf_counter_info_n(const char *const path, const char *const parse_format) {
    unsigned int v;
    int status;

    status = read_perf_counter_info(path, parse_format, &v);
    if (status)
        v = -1;

    return v;
}

/*helper - pperf reading */
static int read_pperf_config(void) {
    const char *const path = "/sys/bus/event_source/devices/msr/events/pperf";
    const char *const format = "event=%x";

    return read_perf_counter_info_n(path, format);
}

/*helper - pperf reading */
static unsigned int read_aperf_config(void) {
    const char *const path = "/sys/bus/event_source/devices/msr/events/aperf";
    const char *const format = "event=%x";

    return read_perf_counter_info_n(path, format);
}

/*helper - pperf reading */
static unsigned int read_mperf_config(void) {
    const char *const path = "/sys/bus/event_source/devices/msr/events/mperf";
    const char *const format = "event=%x";

    return read_perf_counter_info_n(path, format);
}

/*helper - pperf reading */
static unsigned int read_msr_type(void) {
    const char *const path = "/sys/bus/event_source/devices/msr/type";
    const char *const format = "%u";

    return read_perf_counter_info_n(path, format);
}

/*helper - pperf reading */
static long perf_event_open(struct perf_event_attr *hw_event, pid_t pid, int cpu, int group_fd, unsigned long flags) {
    return syscall(__NR_perf_event_open, hw_event, pid, cpu, group_fd, flags);
}

/*helper - pperf reading */
static long open_perf_counter(int cpu, unsigned int type, unsigned int config, int group_fd, __u64 read_format) {
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

/*helper - pperf reading */
static void open_amperf_fd(int cpu) {
    const unsigned int msr_type = read_msr_type();
    const unsigned int aperf_config = read_aperf_config();
    const unsigned int mperf_config = read_mperf_config();
    const unsigned int pperf_config = read_pperf_config();

    perf_stats[cpu].aperf_fd = open_perf_counter(cpu, msr_type, aperf_config, -1, PERF_FORMAT_GROUP);
    perf_stats[cpu].mperf_fd = open_perf_counter(cpu, msr_type, mperf_config, perf_stats[cpu].aperf_fd, PERF_FORMAT_GROUP);
    perf_stats[cpu].pperf_fd = open_perf_counter(cpu, msr_type, pperf_config, perf_stats[cpu].aperf_fd, PERF_FORMAT_GROUP);

}

/*helper - pperf reading */
static int get_amperf_fd(int cpu) {
    if (perf_stats[cpu].aperf_fd)
        return perf_stats[cpu].aperf_fd;

    open_amperf_fd(cpu);

    return perf_stats[cpu].aperf_fd;
}

/*helper - pperf reading */
static unsigned long long rdtsc(void) {
    unsigned int low, high;

    asm volatile ("rdtsc":"=a" (low), "=d"(high));

    return low | ((unsigned long long)high) << 32;
}

/* Helper for - Reading APERF, MPERF and TSC using the perf API.
    Calc perf [cpu utilization per core] difference from MSR registers  */
static int read_aperf_mperf_tsc_perf(struct thread_data *t, int cpu) {
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
    if(fd_amperf == -1) {
        return LPMD_ERROR;
    }

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

    return LPMD_SUCCESS;
}

/*Calc perf [cpu utilization per core] difference from MSR registers */
int update_perf_diffs(float *sum_norm_perf, int stat_init_only) {

    int maxed_cpu = -1;
    float min_load = 100.0, min_s0 = 1.0, next_s0 = 1.0;
    float max_load = 0, max_2nd_load = 0, max_3rd_load = 0, next_load = 0;
    int t, min_s0_cpu = 0, first_pass = 1;

    for (t = 0; t < get_max_online_cpu(); t++) {
        if (!cpu_applicable(t, get_cur_state())) {
            continue;
        }

        /*reading through perf api*/
        struct thread_data tdata;
        if(read_aperf_mperf_tsc_perf(&tdata , t) != LPMD_SUCCESS) {
            lpmd_log_error("read_aperf_mperf_tsc_perf failed for cpu = %d\n", t);
            continue;
        }
        perf_stats[t].pperf_diff = cpu_get_diff_pperf(tdata.pperf, t);
        perf_stats[t].aperf_diff = cpu_get_diff_aperf(tdata.aperf, t);
        perf_stats[t].mperf_diff = cpu_get_diff_mperf(tdata.mperf, t);
        perf_stats[t].tsc_diff   = cpu_get_diff_tsc(tdata.tsc, t);

        if (stat_init_only)
            continue;

        /*
         * Normalized perf metric defined as pperf per load per time.
         * The rationale is detailed here:
         * github.com/intel/psst >whitepapers >Generic_perf_per_watt.pdf
         * Given that delta_load = delta_mperf/delta_tsc, we can rewrite
         * as given below.
         */
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
        }
        first_pass = 0;
    }

    if (stat_init_only)
        return 0;

    grp.worst_stall = min_s0;
    grp.worst_stall_cpu = min_s0_cpu;

    grp.c0_max = max_load;
    grp.c0_2nd_max = max_2nd_load;
    grp.c0_3rd_max = max_3rd_load;
    grp.c0_min = min_load;

    return maxed_cpu;
}

/* close perf fd's */
static void close_amperf_fd(int cpu) {
    if(perf_stats[cpu].aperf_fd) close(perf_stats[cpu].aperf_fd);
    if(perf_stats[cpu].mperf_fd) close(perf_stats[cpu].mperf_fd);
    if(perf_stats[cpu].pperf_fd) close(perf_stats[cpu].pperf_fd);
}

/* cleanup perf_stat structure */
static void perf_stat_uninit() {
    int max_cpus = get_max_cpus();

    if (perf_stats) {
        for (size_t i = 0; i < max_cpus; ++i) {
            close_amperf_fd(i);
            memset( &perf_stats[i], 0, sizeof(perf_stats_t));
        }
        free(perf_stats);
    }
}

static void uninit_perf_calculations() {

    perf_stat_uninit();

    if (last_aperf)
        free(last_aperf);
    if (last_mperf)
        free(last_mperf);
    if (last_pperf)
        free(last_pperf);
    if (last_tsc)
        free(last_tsc);
}
/********************perf calculation - end *****************************************/


/********************SMA calculation - begin *****************************************/

/* initialize avg calculation variables */
static void init_sma_calculations() {
    for (int i = 0; i < SMA_CPU_COUNT; i++) {
        grp.sma_sum[i] = -1;
        for (int j = 0; j < SMA_LENGTH; j++)
            sample[i][j] = 0;
    }
    grp.sma_pos = -1;
}

/* Helper avg calculation */
static int do_sum(int *sam, int len) {
    int sum = 0;

    for (int i = 0; i < len; i++)
        sum += sam[i];
    return sum;
}

/* average cpu usage */
int state_max_avg() {
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

/********************SMA calculation - end *****************************************/

/* return multi threaded false if at least one cpu is under utilizied */
int max_mt_detected(enum state_idx state) {
    for (int t = 0; t < get_max_online_cpu(); t++) {

        if (!cpu_applicable(t, state))
            continue;

        if A_LTE_B
            (perf_stats[t].l0, (UTIL_LOW))
                return 0;
    }
    return 1;
}

/* initialize */
int util_init_proxy(void) {
    float dummy;

    if(init_perf_calculations(get_max_online_cpu()) < 0) {
        lpmd_log_error("WLT_Proxy: error initializing perf calculations");
      return LPMD_ERROR;
    }

    update_perf_diffs(&dummy, 1);

    init_sma_calculations();

    return LPMD_SUCCESS;
}

/* cleanup */
void util_uninit_proxy(void) {
    uninit_perf_calculations();
    uninit_state_manager();
}
