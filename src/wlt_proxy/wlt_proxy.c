/*
 * wlt_proxy.c: Intel Linux Energy Optimizer WLT proxy
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
 * This file contains the Workload type detection proxy entry and callback functions.
 */

#include "lpmd.h" //wlt_type
#include "state_common.h"

/* wlt_proxy polling interval - updated at every state change */
int next_proxy_poll = 1000;

/* wlt_proxy hint - updated at every state change */
int wlt_type = WLT_IDLE;

/* called at the configured interval to take action; return next interval and workload type*/
int read_wlt_proxy(int *interval) {
    state_machine_auto();
    *interval = next_proxy_poll;

    return wlt_type;
}

/* Returns success if proxy supported on platform */
int wlt_proxy_init() {
    return util_init_proxy();
}

/* make sure all resource are properly released and closed */
void wlt_proxy_uninit(void) {
    util_uninit_proxy();
}
