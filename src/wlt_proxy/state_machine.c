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
#include <assert.h>

#include "wlt_proxy_common.h"
#include "state_machine.h"
#include "spike_mgmt.h"
#include "cpu_group.h"
#include "perf_msr.h"
#include "lpmd.h"

#define N_STRIKE	(10)

extern int burst_count;
extern struct group_util grp;
extern int state_demote;
int max_util; 

int state_machine_perf(int present_state)
{
	if (present_state != INIT_MODE)
		prep_state_change(present_state, INIT_MODE, 1);
	return 1;
}

#ifdef _REMOVE_
int state_machine_power(int present_state)
{
	switch (present_state) {
		/*
		 * TODO start with state_machine_auto and add customs
		 */
	}
	return 1;
}
#endif

int state_machine_auto1() {
 	return state_machine_auto(get_cur_state());
}

int state_machine_auto(int present_state)
{
	//this used to be part of function util_main
	float dummy;
	update_perf_diffs(&dummy, 0);
	max_util = (int)round(grp.c0_max); //end	
	
	int completed_poll = get_last_poll();
	float sum_c0 = grp.c0_max + grp.c0_2nd_max + grp.c0_3rd_max;
	//float sum_avg = grp.sma_avg1 + grp.sma_avg2 + grp.sma_avg3;
	int mdrt_count;		//new_mdrt_count;
	int perf_count, initial_burst_count;
	initial_burst_count = get_burst_rate_per_min();
	mdrt_count = get_stay_count(MDRT3E_MODE);
	int sr = get_spike_rate();

	assert(!is_state_disabled(present_state));

	if (A_LTE_B(grp.c0_max, UTIL_NEAR_FULL)) {
		add_non_spike_time(completed_poll);
	} else if (A_GT_B(grp.c0_max, UTIL_NEAR_FULL) || sr) {
		add_spike_time(completed_poll);
	}

	/* should we reset perf-count due to new burst? */
	if (fresh_burst_response(initial_burst_count)) {
		set_stay_count(PERF_MODE, staytime_to_staycount(PERF_MODE));
		set_stay_count(MDRT3E_MODE, 0);
	}
	perf_count = get_stay_count(PERF_MODE);
	if (!perf_count && !mdrt_count)
		set_stay_count(MDRT3E_MODE, staytime_to_staycount(MDRT3E_MODE));

	state_demote = 0;

	switch (present_state) {
		/*
		 * case BYPS_MODE:
		 *  TBD bypass mode exist to support scenarios (util conditions) that are
		 *  un-addressed by this solution. cpu topology is reverted to somewhat closer
		 *  to system default. That way, untreated scenarios do not regress power/perf.
		 */
	case INIT_MODE:
		/*
		 * init mode is super-set of all default/available cpu on the system.
		 */
		/* promote -- if not high multi-thread trend */
		if (!max_mt_detected(INIT_MODE)) {
            lpmd_log_info("INIT_MODE to NORM_MODE\n");			
			prep_state_change(INIT_MODE, NORM_MODE, 0);
			break;
		}
		// stay -- full MT
		break;

	case PERF_MODE:
		// Demote -- if highly MT
		if (max_mt_detected(PERF_MODE)) {
			prep_state_change(PERF_MODE, INIT_MODE, 0);
			lpmd_log_info("PERF_MODE to INIT_MODE\n");	
			break;
		}
		// Stay -- if there was recent perf/resp bursts
		//if (get_burst_rate_per_min() > BURST_COUNT_THRESHOLD)
		if (burst_count > 0 && !do_countdown(PERF_MODE)){
			//lpmd_log_info("burst_count is %d && !do_countdown\n", burst_count);
			break;
		}			
		// Promote but through responsive watch -- if top sampled util and their avg are receeding.
		if (A_LTE_B(sum_c0, (2 * UTIL_LOW)) &&
		    A_LTE_B(grp.sma_avg1, UTIL_ABOVE_HALF)) {
			prep_state_change(PERF_MODE, RESP_MODE, 0);
            lpmd_log_info("PERF_MODE to RESP_MODE\n");			
			break;
		}
		// Promote -- to moderate (3) MT state
		if (!burst_rate_breach() && A_LTE_B(grp.c0_max, UTIL_LOW))	// && A_LTE_B(sum_avg, UTIL_BELOW_HALF))
		{
			set_stay_count(MDRT3E_MODE, 0);
			prep_state_change(PERF_MODE, MDRT3E_MODE, 0);
            lpmd_log_info("PERF_MODE to MDRT3E_MODE\n");			
			break;
		}				
		// Stay -- all else
		//lpmd_log_info("stay in PERF_MODE\n");
		break;

	case RESP_MODE:
		// Demote -- if ST above halfway mark and avg trending higher
		if (A_GT_B(grp.c0_max, UTIL_ABOVE_HALF)
		    && A_GT_B(grp.sma_avg1, UTIL_BELOW_HALF)) {
			prep_state_change(RESP_MODE, PERF_MODE, 0);
            lpmd_log_info("RESP_MODE to PERF_MODE\n");			
			break;
		}
		// Stay -- if there were recent burst of spikes
		if (perf_count && burst_rate_breach())
			break;
		// Promote -- all else
        lpmd_log_info("RESP_MODE to MDRT3E_MODE\n");			
		prep_state_change(RESP_MODE, MDRT3E_MODE, 0);
		break;
	case MDRT4E_MODE:
		if (A_LTE_B(grp.worst_stall * 100, STALL_SCALE_LOWER_MARK)) {
			prep_state_change(MDRT4E_MODE, RESP_MODE, 0);
			lpmd_log_info("MDRT4E_MODE to RESP_MODE\n");
			break;
		}
		// Demote
		if (A_GT_B(grp.c0_max, UTIL_NEAR_FULL)) {
			if (!burst_rate_breach() && strikeout_once(N_STRIKE))
				break;
			prep_state_change(MDRT4E_MODE, PERF_MODE, 0);
			lpmd_log_info("MDRT4E_MODE to PERF_MODE\n");
			break;
		}
		// promote
		if (A_LTE_B(grp.sma_avg1, SUS_LOW_RANGE_END) &&
		    A_LTE_B(grp.sma_avg2, SUS_LOW_RANGE_END) &&
		    A_LTE_B(sum_c0, UTIL_HALF)) {
			if (!do_countdown(MDRT4E_MODE))
				break;
			prep_state_change(MDRT4E_MODE, NORM_MODE, 0);
			lpmd_log_info("MDRT4E_MODE to NORM_MODE\n");
			break;
		}
		// stay
		break;
	case MDRT3E_MODE:
		// Demote -- if mem bound work is stalling but didn't show higher utilization
		if (A_LTE_B(grp.worst_stall * 100, STALL_SCALE_LOWER_MARK)) {
            lpmd_log_info("MDRT3E_MODE to RESP_MODE %.2f < %d\n", grp.worst_stall, STALL_SCALE_LOWER_MARK);			
			prep_state_change(MDRT3E_MODE, RESP_MODE, 0);
			break;
		}
		// Demote to perf
		if (A_GT_B(grp.c0_max, UTIL_NEAR_FULL)) {
			if (!burst_rate_breach() && strikeout_once(N_STRIKE))
				break;
			prep_state_change(MDRT3E_MODE, PERF_MODE, 0);
            lpmd_log_info("MDRT3E_MODE to PERF_MODE %.2f > %d\n", grp.c0_max, UTIL_NEAR_FULL);					
			break;
		}
		// Demote to 4 thread sustained
		if (A_GTE_B(grp.sma_avg1, SUS_LOW_RANGE_END) &&
		    A_GTE_B(grp.sma_avg2, (SUS_LOW_RANGE_END - 5))) {
			prep_state_change(MDRT3E_MODE, MDRT4E_MODE, 0);
            lpmd_log_info("MDRT3E_MODE to MDRT4E_MODE %d > %d\n", grp.sma_avg1, SUS_LOW_RANGE_END);				
			break;
		}
		// promote
		if ((A_GT_B(grp.sma_avg1, SUS_LOW_RANGE_START) &&
		     A_LTE_B(grp.sma_avg1, SUS_LOW_RANGE_END)) &&
		    (A_GT_B(grp.sma_avg2, SUS_LOW_RANGE_START) &&
		     A_LTE_B(grp.sma_avg2, SUS_LOW_RANGE_END))) {
			if (!do_countdown(MDRT3E_MODE))
				break;
			prep_state_change(MDRT3E_MODE, MDRT2E_MODE, 0);
            lpmd_log_info("MDRT3E_MODE to MDRT2E_MODE %d < %d\n", grp.sma_avg1, MDRT2E_MODE);			
			break;
		}
		// Promote -- if top three avg util are trending lower.
		if (A_LTE_B(grp.sma_avg1, SUS_LOW_RANGE_START) &&
		    (A_LTE_B(grp.sma_avg2, SUS_LOWER) &&
		     A_LTE_B(grp.sma_avg3, SUS_LOWER))) {
			if (!do_countdown(MDRT3E_MODE))
				break;
			prep_state_change(MDRT3E_MODE, NORM_MODE, 0);
            lpmd_log_info("MDRT3E_MODE to NORM_MODE\n");			
			break;
		}
		// stay
        lpmd_log_debug("***********no change state:  MDRT3E_MODE\n");		
		break;

	case MDRT2E_MODE:
		// Demote -- if mem bound work is stalling but didn't show higher utilization
		if (A_LTE_B(grp.worst_stall * 100, STALL_SCALE_LOWER_MARK)) {
			prep_state_change(MDRT2E_MODE, RESP_MODE, 0);
			lpmd_log_info("MDRT2E_MODE to RESP_MODE\n");
			break;
		}
		// Demote -- if instant util nearing full or sustained moderate avg1 trend with avg2 trailing closeby
		if (A_GT_B(grp.c0_max, UTIL_NEAR_FULL) ||
		    (A_GTE_B(grp.sma_avg1, SUS_LOW_RANGE_END) &&
		     A_GTE_B(grp.sma_avg2, SUS_LOW_RANGE_END - 10))) {
			if (!burst_rate_breach() && strikeout_once(N_STRIKE))
				break;
			prep_state_change(MDRT2E_MODE, MDRT3E_MODE, 0);
			lpmd_log_info("MDRT2E_MODE to MDRT3E_MODE\n");
			break;
		}
		// Promote -- if top two avg util are trending lower.
		if ((A_GT_B(grp.sma_avg1, SUS_LOW_RANGE_START)
		     && A_LTE_B(grp.sma_avg1, SUS_LOW_RANGE_END))
		    && A_LTE_B(grp.sma_avg2, SUS_LOW_RANGE_END)) {
			if (!do_countdown(MDRT2E_MODE)) {
				break;
			}
			prep_state_change(MDRT2E_MODE, NORM_MODE, 0);
			lpmd_log_info("MDRT2E_MODE to NORM_MODE\n");
			break;
		}
		// stay
		break;
	case NORM_MODE:
		// Demote -- if mem bound work is stalling but didn't show higher utilization
		if (A_LTE_B(grp.worst_stall * 100, STALL_SCALE_LOWER_MARK)) {
			prep_state_change(NORM_MODE, RESP_MODE, 0);
			lpmd_log_info("NORM_MODE to RESP_MODE\n");
			break;
		}
		// Demote -- if instant util more than half or if signs of sustained ST activity.
		if (A_GT_B(grp.c0_max, UTIL_HALF)
		    || (A_GT_B(grp.sma_avg1, UTIL_BELOW_HALF))) {
			/* In this state its better to absorb few spike (noise) before reacting */
			if (!burst_rate_breach() && strikeout_once(N_STRIKE))
				break;
			prep_state_change(NORM_MODE, MDRT2E_MODE, 0);
			lpmd_log_info("NORM_MODE to MDRT2E_MODE\n");
			break;
		}
		// Promote -- if top few instant util or top avg is trending lower.
		if ((A_LTE_B(grp.c0_max, UTIL_LOW)
		     && A_LTE_B(grp.c0_2nd_max, UTIL_LOWEST))
		    || A_LTE_B(grp.sma_avg1, SUS_LOWER)) {
			/* its better to absorb few dips before reacting out of a steady-state */
			if (!do_countdown(NORM_MODE))
				break;
			prep_state_change(NORM_MODE, DEEP_MODE, 0);
			lpmd_log_info("NORM_MODE to DEEP_MODE\n");
			break;
		}
		break;
	case DEEP_MODE:
		// Demote -- if mem bound work is stalling but didn't show higher util.
		if (A_LTE_B(grp.worst_stall * 100, STALL_SCALE_LOWER_MARK)) {
			prep_state_change(DEEP_MODE, RESP_MODE, 0);
			lpmd_log_info("DEEP_MODE to RESP_MODE\n");
			break;
		}
		// Demote -- if there are early signs of instantaneous utilization build-up.
		if (A_GT_B(grp.c0_max, UTIL_FILL_START)) {
			prep_state_change(DEEP_MODE, NORM_MODE, 0);
			lpmd_log_info("DEEP_MODE to NORM_MODE\n");
			break;
		}

		break;
	}
	return 1;
}
