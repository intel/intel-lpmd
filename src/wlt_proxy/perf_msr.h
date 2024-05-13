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

#ifndef _PERF_MSR_
#define _PERF_MSR_
#include <unistd.h>
#include <stdint.h>

#define MSR_IA32_MPERF		0xe7
#define MSR_IA32_APERF		0xe8
#define MSR_IA32_PPERF		0x64e
#define MSR_IA32_TSC		0x10
#define MSR_PLATFORM_INFO	0xce
#define MSR_PERF_STATUS		0x198
#define MSR_HWP			0x774
#define MSR_EPB			0x1b0
#define MSR_PERF_CTL		0x1fc

typedef struct {
	int cpu;
	int dev_msr_fd;
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

extern int cpu_hfm_mhz;
extern int read_msr(int fd, uint32_t reg, uint64_t * data);
extern int write_msr(int fd, uint32_t reg, uint64_t * data);
extern int initialize_dev_msr(int c);
extern int initialize_cpu_hfm_mhz(int fd);
extern int init_delta_vars(int n);
extern uint64_t cpu_get_diff_aperf(uint64_t a, int i);
extern uint64_t cpu_get_diff_mperf(uint64_t m, int i);
extern uint64_t cpu_get_diff_pperf(uint64_t p, int i);
extern uint64_t cpu_get_diff_tsc(uint64_t t, int i);
extern int rapl_ediff_pkg0(long long x);

#endif
