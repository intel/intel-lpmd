/*
 * wlt_proxy_def.c: Intel Low Power Daemon WLT proxy
 *
 * Copyright (C) 2023 Intel Corporation. All rights reserved.
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
 * This file contains the Workload type detection proxy. This file logic
 * is specific to a CPU model and can be customized.
 */

#include "lpmd.h"
#include "wlt_proxy_common.h"
#include "state_machine.h"
#include "wlt_proxy.h"
#include "perf_msr.h"

static bool proxy_initialized = false;
static lpmd_config_t *lpmd_config;
extern int next_proxy_poll;

/** Framework callback to update state */
void set_workload_hint(int type) {
	lpmd_log_debug("proxy WLT hint :%d\n", type);
	periodic_util_update(lpmd_config, type);
}

/** Called at the configured interval to take action */
void wlt_proxy_action_loop(void) {

	if (proxy_initialized) {
		lpmd_log_debug("\n\nwlt_proxy_action_loop, proxy initialzied\n");
		//state_machine_auto(get_cur_state());
        state_machine_auto1();
		lpmd_log_debug("wlt_proxy_action_loop, handled states\n");		
	} else {
        lpmd_log_debug("\n internal error \n");
    }
}

/** Return non zero if the proxy is not present for a platform */
int wlt_proxy_init(lpmd_config_t *_lpmd_config) {
	
    if (util_init_proxy()){
        return LPMD_ERROR; 
    }

	/* Check model check and fail */
	/* TODO */
	lpmd_config = _lpmd_config;//todo: remove

	proxy_initialized = true;
    next_proxy_poll = 2000; 
	return LPMD_SUCCESS;
}

/** make sure all resource are properly released and closed */
void wlt_proxy_uninit(void) {
    
    util_uninit_proxy();
#ifdef __REMOVE__
    close_all_fd();
#endif    
    uninit_delta_vars();
    perf_stat_uninit();
}

