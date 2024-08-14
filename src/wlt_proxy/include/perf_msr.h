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

int initialize_dev_msr(int c);
int initialize_cpu_hfm_mhz(int fd);
int init_delta_vars(int n);

int read_msr(int fd, uint32_t reg, uint64_t * data);
int write_msr(int fd, uint32_t reg, uint64_t * data);

void uninit_delta_vars();

#endif
