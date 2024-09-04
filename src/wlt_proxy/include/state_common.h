/*
 * state_common.h: Intel Linux Energy Optimizer proxy detection common header file
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
 */

#ifndef _WLT_PROXY_COMMON_H_
#define _WLT_PROXY_COMMON_H_

/* state indexs for  WLT proxy detection based cpu usage high to low */
enum state_idx {
    INIT_MODE,
    PERF_MODE,
    MDRT4E_MODE,
    MDRT3E_MODE,
    MDRT2E_MODE,
    RESP_MODE,
    NORM_MODE,
    DEEP_MODE
};
#define    MAX_MODE 8

/* spike_mgmt.c */
int add_spike_time(int);
int add_non_spike_time(int);
int get_spike_rate(void);
int get_burst_rate_per_min(void);
int fresh_burst_response(int initial_val);
int burst_rate_breach(void);
int strikeout_once(int);

#endif /* _WLT_PROXY_COMMON_H_ */
