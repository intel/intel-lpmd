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
#include <stdio.h>
#include "wlt_proxy_common.h"

int get_last_poll(void)
{
	//return lp_state[cur_state].last_poll;
    return 1;
}

bool is_state_disabled(enum lp_state_idx state)
{
	//return lp_state[state].disabled;
    return true;
}

int get_stay_count(enum lp_state_idx state)
{
	//return (lp_state[state].stay_count);
    return 1;
}

int set_stay_count(enum lp_state_idx state, int count)
{
	//return (lp_state[state].stay_count = count);
    return 0;
}

int do_countdown(enum lp_state_idx state)
{
	/*lp_state[state].stay_count -= 1;

	if (lp_state[state].stay_count <= 0) {
		lp_state[state].stay_count = 0;
		return 1;
	}

	lp_state[state].stay_count_last = lp_state[state].stay_count;*/

	return 0;
}