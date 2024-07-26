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

#ifndef _WLT_PROXY_H_
#define _WLT_PROXY_H_

#include <stdio.h>

/* WLT hints parsing */
typedef enum {
	WLT_IDLE,
	WLT_BATTERY_LIFE,
	WLT_SUSTAINED,
	WLT_BURSTY,
    WLT_SUSTAINED_BAT,
    WLT_BATTERY_LIFE_BAT,    
	WLT_INVALID,
} wlt_type_t;

void set_workload_hint(int type); 

void wlt_proxy_action_loop(void);

void wlt_proxy_uninit(void);

#endif				/* _WLT_PROXY_H_ */
