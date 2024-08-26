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

#include "lpmd.h"

/* WLT hints - mapped to configuraiton file WLTtype */
typedef enum {
    WLT_IDLE = 0,
    WLT_BATTERY_LIFE = 1,
    WLT_SUSTAINED = 2,
    WLT_BURSTY = 3,
    WLT_SUSTAINED_BAT = 4,
    WLT_BATTERY_LIFE_BAT = 5,
    WLT_INVALID = 6,
} wlt_type_t;

void set_workload_hint(int type);

void wlt_proxy_action_loop(void);

int wlt_proxy_init(lpmd_config_t *_lpmd_config);

void wlt_proxy_uninit(void);

#endif/* _WLT_PROXY_H_ */
