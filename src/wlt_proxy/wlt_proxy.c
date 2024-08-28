/*
 * wlt_proxy.c: Intel Linux Energy Optimier WLT proxy
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

#include "lpmd.h"
#include "state_common.h"
#include "wlt_proxy.h"

static bool proxy_initialized = false;
static lpmd_config_t *lpmd_config;
extern int next_proxy_poll;

/** Framework callback to update state */
void set_workload_hint(int type) {
    lpmd_log_debug("proxy WLT hint :%d\n", type);
    periodic_util_update(lpmd_config, type);
}

/** called at the configured interval to take action */
void wlt_proxy_action_loop(void) {
    if (proxy_initialized) {
        //lpmd_log_debug("\n\nwlt_proxy_action_loop, proxy initialzied\n");
        state_machine_auto();
        //lpmd_log_debug("wlt_proxy_action_loop, handled states\n");        
    } else {
        lpmd_log_debug("\n internal error \n");
    }
}

/** Return non zero if the proxy is not present for a platform */
int wlt_proxy_init(lpmd_config_t *_lpmd_config) {
    
    if (util_init_proxy()){
        return LPMD_ERROR; 
    }

    /*todo: check model check and fail */
    
    lpmd_config = _lpmd_config;//cb variable
    proxy_initialized = true;
    next_proxy_poll = 1000;//set to call wlt_proxy_action_loop asap [1sec]
    
    return LPMD_SUCCESS;
}

/** make sure all resource are properly released and closed */
void wlt_proxy_uninit(void) {
    util_uninit_proxy();
}
