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

#ifndef _SPIKE_MGMT_
#define _SPIKE_MGMT_

int add_spike_time(int);
int add_non_spike_time(int);
int get_spike_rate(void);
int get_burst_rate_per_min(void);
int fresh_burst_response(int initial_val);
int burst_rate_breach(void);
//int set_spike_type(int);
int strikeout_once(int);
//int update_burst_count(int);
int update_spike_rate_maxima();
int clear_spike_rate_maxima();

#endif /*_SPIKE_MGMT_*/
