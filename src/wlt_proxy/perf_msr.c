/*
 * perf_msr.c: Intel cpu aperf/mperf msr counter interface
 *
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

#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <stdlib.h>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/stat.h>

#include "wlt_proxy_common.h"
#include "perf_msr.h"
#include "lpmd.h"

#define MSR_PLATFORM_INFO	0xce
int cpu_hfm_mhz;

int read_msr(int fd, uint32_t reg, uint64_t * data)
{
    if (pread(fd, data, sizeof(*data), reg) != sizeof(*data)) {
        lpmd_log_info("read_msr fail on fd:%d\n", fd);
        return -1;
    }
    return 0;
}

int write_msr(int fd, uint32_t reg, uint64_t * data)
{
    if (pwrite(fd, data, sizeof(*data), reg) != sizeof(*data)) {
        perror("wrmsr fail");
        return -1;
    }
    return 0;
}

int initialize_dev_msr(int c)
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

/* routine to evaluate & store a global msr value's diff */
#define VAR(a, b) (a##b)
#define generate_msr_diff(scope)                           \
uint64_t get_diff_##scope(uint64_t cur_value)                   \
{                                           \
    uint64_t diff;                                   \
    diff = (VAR(last_, scope) == 0) ? 0 : (cur_value - VAR(last_, scope)); \
    VAR(last_, scope) = cur_value;                           \
    return diff;                           \
}

uint64_t *last_aperf = NULL;
uint64_t *last_mperf = NULL;
uint64_t *last_pperf = NULL;
uint64_t *last_tsc = NULL;

int init_delta_vars(int n)
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

void uninit_delta_vars(){
    if (last_aperf) 
        free(last_aperf);
    if (last_mperf)
        free(last_mperf);
    if (last_pperf)
        free(last_pperf);
    if (last_tsc)
        free(last_tsc);
}

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

static long long prev_pkg0;
#define VAR(a, b) (a##b)
#define generate_rapl_ediff(scope)                                             \
int rapl_ediff_##scope(long long cur_ewma)                                     \
{                                                                              \
        int ediff;                                                       \
        ediff = (VAR(prev_, scope) == 0) ? 0 : (cur_ewma - VAR(prev_, scope)); \
        VAR(prev_, scope) = cur_ewma;                                          \
        if (ediff <= 0) {                                                      \
                return 0;                                                      \
        }                                                                      \
        return (int)ediff;                                                     \
}

cpu_generate_msr_diff(aperf);
cpu_generate_msr_diff(mperf);
cpu_generate_msr_diff(pperf);
cpu_generate_msr_diff(tsc);
generate_rapl_ediff(pkg0);
