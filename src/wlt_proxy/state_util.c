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
#define _GNU_SOURCE

#include <linux/perf_event.h> //pref_event_attr
#include <asm/unistd.h> //syscall __NR_pref_event_open
#include <assert.h>

#include "wlt_proxy_common.h"
#include "wlt_proxy.h"
#include "state_manager.h"
#include "knobs_common.h"

#define PERF_API 1

#ifdef __GNUC__
#define likely(x)       __builtin_expect(!!(x), 1)
#define unlikely(x)     __builtin_expect(!!(x), 0)
#else
#define likely(x)       (x)
#define unlikely(x)     (x)
#endif

#ifndef PERF_API
#define MSR_IA32_MPERF        0xe7
#define MSR_IA32_APERF        0xe8
#define MSR_IA32_PPERF        0x64e
#define MSR_IA32_TSC          0x10
#define MSR_PERF_STATUS       0x198
#define MSR_PERF_CTL          0x1fc
#endif 

#define MSR_HWP            0x774
#define MSR_EPB            0x1b0

//todo: hardcoded platform info? should we get it from config file?
#define MSR_PLATFORM_INFO	0xce

#define SCALE_DECIMAL (100)

#define MAX_INJECT    (90)
#define LIMIT_INJECT(i)    (i > MAX_INJECT ? MAX_INJECT:(i < 0 ? 1 : i))

int idle_inject_feature = IDLE_INJECT_FEATURE;
int inject_update = UNDEFINED;
int irq_rebalance = 0;
static int record = 0;
static int prev_type = -1;

int state_demote = 0;
int next_proxy_poll = 2000;

bool AC_CONNECTED = true;

/* 
 * simple moving average (sma), event count based - not time. 
 * updated for upto top 3 max util streams.
 * exact cpu # is not tracked; only the max since continuum of task
 * keeps switching cpus anyway.
 * array implementation with SMA_LENGTH number of values.
 * XXX: reimplement dynamic for advance tunable SMA_LENGTH
 */
#define SMA_LENGTH    (25)
#define SMA_CPU_COUNT    (3)
static int sample[3][SMA_LENGTH];

extern int max_util;

typedef struct {
    int cpu;
    int dev_msr_fd;
    int aperf_fd;
    int mperf_fd;
    int pperf_fd; 
    uint64_t orig_epp;
    uint64_t orig_epb;
    uint64_t orig_perf_ctl;
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

static struct timespec ts_start, ts_prev;
static struct timespec ts_current = { 0, 0 };

struct thread_data {
    unsigned long long tsc;
    unsigned long long aperf;
    unsigned long long mperf;
    unsigned long long pperf; 
} *thread_even, *thread_odd;

int cpu_hfm_mhz;

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
#define cpu_generate_msr_diff(scope)                           \
uint64_t cpu_get_diff_##scope(uint64_t cur_value, int instance)               \
{                                           \
    uint64_t diff;                                   \
    diff = (VARI(last_, scope, instance) == 0) ?                   \
            0 : u64diff(cur_value, VARI(last_, scope, instance));  \
    VARI(last_, scope, instance) = cur_value;                   \
    return diff;                                          \
}

cpu_generate_msr_diff(aperf);
cpu_generate_msr_diff(mperf);
cpu_generate_msr_diff(pperf);
cpu_generate_msr_diff(tsc);

static int read_msr(int fd, uint32_t reg, uint64_t * data)
{
    if (pread(fd, data, sizeof(*data), reg) != sizeof(*data)) {
        lpmd_log_info("read_msr fail on fd:%d\n", fd);
        return -1;
    }
    return 0;
}

static int write_msr(int fd, uint32_t reg, uint64_t * data)
{
    if (pwrite(fd, data, sizeof(*data), reg) != sizeof(*data)) {
        perror("wrmsr fail");
        return -1;
    }
    return 0;
}

static int init_delta_vars(int n)
{
    last_aperf = calloc(sizeof(uint64_t), n);
    last_mperf = calloc(sizeof(uint64_t), n);
    last_pperf = calloc(sizeof(uint64_t), n);
    last_tsc = calloc(sizeof(uint64_t), n);
    if (!last_aperf || !last_mperf || !last_mperf || !last_tsc) {
        lpmd_log_info("malloc failure perf vars\n");
        return 0;
    }    

    return 1;
}

static void uninit_delta_vars(){
    if (last_aperf) 
        free(last_aperf);
    if (last_mperf)
        free(last_mperf);
    if (last_pperf)
        free(last_pperf);
    if (last_tsc)
        free(last_tsc);
}

static int initialize_dev_msr(int c)
{
    int fd;
    char msr_file[128];

    sprintf(msr_file, "/dev/cpu/%d/msr", c);
    fd = open(msr_file, O_RDWR);
    if (fd < 0) {
        perror("rdmsr: open");
        return -1;
    }
    return fd;
}

int initialize_cpu_hfm_mhz(int fd)
{
    uint64_t msr_val;
    int ret;

    ret = read_msr(fd, (uint32_t) MSR_PLATFORM_INFO, &msr_val);
    if (ret != -1) {
        /* most x86 platform have BaseCLK as 100MHz */
        cpu_hfm_mhz = ((msr_val >> 8) & 0xffUll) * 100;
    } else {
        lpmd_log_info("***can't read MSR_PLATFORM_INFO***\n");
        return -1;
    }
    
    return 0;
}

static int read_perf_counter_info(const char *const path, const char *const parse_format, void *value_ptr)
{
    int fdmt;
    int bytes_read;
    char buf[64];
    int ret = -1;

    fdmt = open(path, O_RDONLY, 0);
    if (fdmt == -1) {
        //if (debug)
            fprintf(stderr, "Failed to parse perf counter info %s\n", path);
        ret = -1;
        goto cleanup_and_exit;
    }

    bytes_read = read(fdmt, buf, sizeof(buf) - 1);
    if (bytes_read <= 0 || bytes_read >= (int)sizeof(buf)) {
        //if (debug)
            fprintf(stderr, "Failed to parse perf counter info %s\n", path);
        ret = -1;
        goto cleanup_and_exit;
    }

    buf[bytes_read] = '\0';

    if (sscanf(buf, parse_format, value_ptr) != 1) {
        //if (debug)
            fprintf(stderr, "Failed to parse perf counter info %s\n", path);
        ret = -1;
        goto cleanup_and_exit;
    }

    ret = 0;

cleanup_and_exit:
    if (fdmt >= 0)
        close(fdmt);
    return ret;
}

static unsigned int read_perf_counter_info_n(const char *const path, const char *const parse_format)
{
    unsigned int v;
    int status;

    status = read_perf_counter_info(path, parse_format, &v);
    if (status)
        v = -1;

    return v;
}

int read_pperf_config(void)
{
    const char *const path = "/sys/bus/event_source/devices/msr/events/pperf";
    const char *const format = "event=%x";

    return read_perf_counter_info_n(path, format);
}

unsigned int read_aperf_config(void)
{
    const char *const path = "/sys/bus/event_source/devices/msr/events/aperf";
    const char *const format = "event=%x";

    return read_perf_counter_info_n(path, format);    
}

unsigned int read_mperf_config(void)
{
    const char *const path = "/sys/bus/event_source/devices/msr/events/mperf";
    const char *const format = "event=%x";

    return read_perf_counter_info_n(path, format);
}
//not used?
unsigned int read_tsc_config(void)
{
    const char *const path = "/sys/bus/event_source/devices/msr/events/tsc";
    const char *const format = "event=%x";

    return read_perf_counter_info_n(path, format);
}

static unsigned int read_msr_type(void)
{
    const char *const path = "/sys/bus/event_source/devices/msr/type";
    const char *const format = "%u";

    return read_perf_counter_info_n(path, format);
}

static long perf_event_open(struct perf_event_attr *hw_event, pid_t pid, int cpu, int group_fd, unsigned long flags)
{
    return syscall(__NR_perf_event_open, hw_event, pid, cpu, group_fd, flags);
}

static long open_perf_counter(int cpu, unsigned int type, unsigned int config, int group_fd, __u64 read_format)
{
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

void open_amperf_fd(int cpu)
{
    const unsigned int msr_type = read_msr_type();
    const unsigned int aperf_config = read_aperf_config();
    const unsigned int mperf_config = read_mperf_config();
    const unsigned int pperf_config = read_pperf_config();    

    perf_stats[cpu].aperf_fd = open_perf_counter(cpu, msr_type, aperf_config, -1, PERF_FORMAT_GROUP);
    perf_stats[cpu].mperf_fd = open_perf_counter(cpu, msr_type, mperf_config, perf_stats[cpu].aperf_fd, PERF_FORMAT_GROUP);    
    perf_stats[cpu].pperf_fd = open_perf_counter(cpu, msr_type, pperf_config, perf_stats[cpu].aperf_fd, PERF_FORMAT_GROUP);    
}

int get_amperf_fd(int cpu)
{
    if (perf_stats[cpu].aperf_fd)
        return perf_stats[cpu].aperf_fd;

    open_amperf_fd(cpu);

    return perf_stats[cpu].aperf_fd;
}

unsigned long long rdtsc(void)
{
    unsigned int low, high;

    asm volatile ("rdtsc":"=a" (low), "=d"(high));

    return low | ((unsigned long long)high) << 32;
}

/* Read APERF, MPERF and TSC using the perf API. */
int read_aperf_mperf_tsc_perf(struct thread_data *t, int cpu)
{
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

    return 0;
}

int update_perf_diffs(float *sum_norm_perf, int stat_init_only)
{
    int fd, min_cpu, maxed_cpu = -1;
    float min_load = 100.0, min_s0 = 1.0, next_s0 = 1.0;
    float _sum_nperf = 0, nperf = 0;
    float max_load = 0, max_2nd_load = 0, max_3rd_load = 0, next_load = 0;
    uint64_t aperf_raw, mperf_raw, pperf_raw, tsc_raw, poll_cpu_us = 0;

    int t, min_s0_cpu = 0, first_pass = 1;

    for (t = 0; t < get_max_online_cpu(); t++) {

        /*reading through perf api*/
        struct thread_data tdata;
        read_aperf_mperf_tsc_perf(&tdata , t);
        //lpmd_log_debug("from api pperf_raw %lld\n", tdata.pperf);
        /*perf_stats[t].pperf_diff*/ uint64_t pperf = cpu_get_diff_pperf(tdata.pperf, t);
        //lpmd_log_debug("from api pperf_diff %ld\n", pperf);
        perf_stats[t].pperf_diff = pperf;

        //lpmd_log_debug("from api aperf_raw %lld\n", tdata.aperf);
        /*perf_stats[t].aperf_diff*/  uint64_t aperf = cpu_get_diff_aperf(tdata.aperf, t);
        //lpmd_log_debug("from api aperf_diff %ld\n", aperf);
        perf_stats[t].aperf_diff = aperf;

        //lpmd_log_debug("from api mperf_raw %lld\n", tdata.mperf);
        /*perf_stats[t].mperf_diff*/  uint64_t mperf = cpu_get_diff_mperf(tdata.mperf, t);
        //lpmd_log_debug("from api mperf_diff %ld\n", mperf);
        perf_stats[t].mperf_diff = mperf;

        //lpmd_log_debug("from api tsc_raw %lld\n", tdata.tsc);
        /*perf_stats[t].tsc_diff*/ uint64_t tsc = cpu_get_diff_tsc(tdata.tsc, t);
        //lpmd_log_debug("from api tsc_diff %ld\n\n", tsc);
        perf_stats[t].tsc_diff = tsc;

        if (stat_init_only)
            continue;

        poll_cpu_us = perf_stats[t].tsc_diff / cpu_hfm_mhz;
        /*
         * Normalized perf metric defined as pperf per load per time.
         * The rationale is detailed here:
         * github.com/intel/psst >whitepapers >Generic_perf_per_watt.pdf
         * Given that delta_load = delta_mperf/delta_tsc, we can rewrite
         * as given below.
         */
        if (perf_stats[t].mperf_diff) {
            if (poll_cpu_us != 0)
                nperf = (float)perf_stats[t].pperf_diff / poll_cpu_us;
            nperf = (float)nperf *perf_stats[t].tsc_diff;
            nperf = (float)nperf / (perf_stats[t].mperf_diff);
            perf_stats[t].nperf = (uint64_t) nperf;
            _sum_nperf += (float)nperf;
        }
        if (perf_stats[t].tsc_diff) {
            next_load =
                (float)100 *perf_stats[t].mperf_diff /
                perf_stats[t].tsc_diff;
            //lpmd_log_debug("next_load = perf_stats[t].l0 %0.2f\n", next_load); 
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
            min_cpu = perf_stats[t].cpu;
            /*
             * min cpu unused for now. 
             */
            (void)(min_cpu);
        }
        first_pass = 0;
        
        
    }
    if (stat_init_only)
        return 0;
    if (perf_stats[maxed_cpu].mperf_diff)
        perf_stats[maxed_cpu].f0 =
            (float)perf_stats[maxed_cpu].aperf_diff * 0.01 /
            perf_stats[maxed_cpu].mperf_diff * cpu_hfm_mhz;
    grp.worst_stall = min_s0;
    grp.worst_stall_cpu = min_s0_cpu;

    grp.c0_max = max_load;
    grp.c0_2nd_max = max_2nd_load;
    grp.c0_3rd_max = max_3rd_load;
    grp.c0_min = min_load;
    *sum_norm_perf = _sum_nperf;

    return maxed_cpu;
}

static void sma_init()
{
    for (int i = 0; i < SMA_CPU_COUNT; i++) {
        grp.sma_sum[i] = -1;
        for (int j = 0; j < SMA_LENGTH; j++)
            sample[i][j] = 0;
    }
    grp.sma_pos = -1;
}

static int do_sum(int *sam, int len)
{
    int sum = 0;
    for (int i = 0; i < len; i++)
        sum += sam[i];
    return sum;
}

int state_max_avg()
{
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

static enum lp_state_idx nearest_supported(enum lp_state_idx from_state, enum lp_state_idx to_state)
{
    enum lp_state_idx state;
    int operator =    (from_state < to_state) ? (+1) : (-1);
    for (state = to_state; (state >= 0) && (state < MAX_MODE); state = state + operator) {
        /* RESP mode is special and not good idea to step into as backup state */
        if (state == RESP_MODE)
            continue;
        if (is_state_valid(state))
            return state;
    }
    assert(0);

}

static int get_state_mapping(enum lp_state_idx state){

    switch(state) {
        case PERF_MODE:
            return WLT_BURSTY;
            
        case RESP_MODE:
        case NORM_MODE:
            return WLT_BATTERY_LIFE;
            
        case DEEP_MODE:
            return WLT_IDLE;
        
        //there is no corresponding wlt for INIT_MODE, it goes away quickly.
        //use WLT_SUSTAINED as default type    
        case INIT_MODE:
        case MDRT4E_MODE:
        case MDRT3E_MODE:
        case MDRT2E_MODE:
            return WLT_SUSTAINED;

        default:
            lpmd_log_error("unknown work load type\n");
            return WLT_IDLE;
    }
}

int prep_state_change(enum lp_state_idx from_state, enum lp_state_idx to_state,
              int reset)
{
    //switch(to_state)
    //do to_state to WLT mapping
    int type = get_state_mapping(to_state); 
    lpmd_log_debug("proxy WLT state value :%d, %d\n", type, prev_type);
    if (prev_type == -1) {//first time
        set_workload_hint(type);
        prev_type = type;
    } else if (prev_type != type) {
        set_workload_hint(type);
        prev_type = type;
    } /*else {
        //dont call type change
    }*/
    
    set_cur_state(to_state);
    set_state_reset();
    set_last_maxutil(DEACTIVATED);
    

    if (to_state < from_state)
        state_demote = 1;
    
    //proxy: apply state change and get poll of different states
    apply_state_change(); 
    if (likely(is_state_valid(to_state))) {
        next_proxy_poll = get_state_poll(max_util, to_state);
    }//proxy: change end

    return 1;
}

int staytime_to_staycount(enum lp_state_idx state)
{
    int stay_count = 0;
    if (unlikely((state < 0) || (state >= MAX_MODE)))
        assert(0);
    switch (state)
    {
        case MDRT2E_MODE:
        case MDRT3E_MODE:
        case MDRT4E_MODE:
            stay_count = (int)MDRT_MODE_STAY/get_poll_ms(MDRT3E_MODE);
            break;
        case PERF_MODE:
            stay_count = (int)PERF_MODE_STAY/get_poll_ms(PERF_MODE);
            break;
        case RESP_MODE:
        case INIT_MODE:
        case NORM_MODE:
        case DEEP_MODE:
        //case BYPS_MODE:
        //case MAX_MODE:
            /* undefined */
            assert(0);
            break;
    }
    return stay_count;
}


int max_mt_detected(enum lp_state_idx state)
{
    //lpmd_log_debug("no of cpus online: %d\n", get_max_online_cpu());
    
    for (int t = 0; t < get_max_online_cpu(); t++) {
        
        if A_LTE_B
            (perf_stats[t].l0, (UTIL_LOW))
                return 0;
    }
    return 1;
}


/* initialize perf_stat structure */
int perf_stat_init(void)
{
    int max_cpus = get_max_cpus();
    perf_stats = NULL; 
    perf_stats = calloc(sizeof(perf_stats_t), max_cpus);
    if ( !perf_stats ) {
        return 0;
    }

    for (int t = 0; t < max_cpus; t++) {
        if (!is_cpu_online(t))
            continue;
        
        perf_stats[t].cpu = t;
        perf_stats[t].dev_msr_fd = initialize_dev_msr(t);
    }
    return 1;
}

/* cleanup perf_stat structure */
static void perf_stat_uninit(){
    int max_cpus = get_max_cpus();
    if (perf_stats) {
        for (size_t i = 0; i < max_cpus; ++i) {
            memset( &perf_stats[i], 0, sizeof(perf_stats_t));                    
        }
        free(perf_stats); 
    }

}

/*defined in lpmd_util*/
/* initialize */
int util_init_proxy(void)
{
    float dummy;
    
    if (init_cpu_proxy()) {
        lpmd_log_error("\nerror initing cpu proxy\n");
        return -1; 
    }
    
    init_delta_vars(get_max_online_cpu());

    update_perf_diffs(&dummy, 1);

    initialize_cpu_hfm_mhz(perf_stats[0].dev_msr_fd);

    initialize_state_mask();
    sma_init();

    //close_all_fd();
    return 0;
}

/* cleanup */
void util_uninit_proxy(void) {
    
    exit_state_change();    
    uninit_cpu_proxy();
    uninit_delta_vars();
    perf_stat_uninit();
}
