/*
 * state_machine.c: Intel Low Power Daemon WLT proxy state change handling.
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
 * This file contains condition checks for state switch.
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <assert.h>

#include "state_common.h"
#include "lpmd.h" //logs

extern int next_proxy_poll;

/*
 * stall scalability refer to non-stallable percentage of utilization.
 * e.g due to memory or other dependency. If work is reasonably scaling well,
 * values in 80 to 90+% is expected
 */
//#define STALL_SCALE_LOWER_MARK    70
#define STALL_SCALE_LOWER_MARK    40

#define N_STRIKE    (10)

/* threshold (%) for sustained (avg) utilizations */
//#define SUS_LOWEST               1
#define SUS_LOWER                2
#define SUS_LOW_RANGE_START      4
#define SUS_LOW_RANGE_END       25

extern int burst_count;
extern struct group_util grp;
extern int state_demote;

int max_util; 
int only_once = 0;

/* function checks conditions for state switch */
int state_machine_auto() {

    float dummy;
    int present_state = get_cur_state();
    update_perf_diffs(&dummy, 0);
    max_util = (int)round(grp.c0_max); //end

    /*
     * we do not want to track avg util for following cases:
     * a) bypass mode where the solution temporary bypassed
     * b) Responsive transit mode (fast poll can flood avg leading to incorrect decisions)
     */
    if (/*(present_state != BYPS_MODE) || */(present_state != RESP_MODE))
        state_max_avg();
    
    int completed_poll = get_last_poll();
    float sum_c0 = grp.c0_max + grp.c0_2nd_max + grp.c0_3rd_max;
    //float sum_avg = grp.sma_avg1 + grp.sma_avg2 + grp.sma_avg3;
    int mdrt_count;        //new_mdrt_count;
    int perf_count, initial_burst_count;
    initial_burst_count = get_burst_rate_per_min();
    mdrt_count = get_stay_count(MDRT3E_MODE);
    int sr = get_spike_rate();

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
    int isMT = !max_mt_detected(INIT_MODE);
    
    if(only_once == 0) {
        lpmd_log_debug("present_state, isMT, C0_max, C0_2ndMax, sum_c0, sma avg1, sma avg2, sma avg3, worst_stall, next_proxy_poll\n");
        only_once = 1;
    }
    lpmd_log_debug("%d, %d,     %.2f,       %.2f,   %.2f,       %d,      %d,        %d,        %.2f, %d\n", \
        present_state, \
        isMT, \
        grp.c0_max, \
        grp.c0_2nd_max, \
        sum_c0, \
        grp.sma_avg1, \
        grp.sma_avg2, \
        grp.sma_avg3, \
        grp.worst_stall, \
        next_proxy_poll);

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
            lpmd_log_debug("INIT_MODE to PERF_MODE\n");            
            prep_state_change(INIT_MODE, PERF_MODE, 0);
            break;
        }
        // stay -- full MT
        break;

    case PERF_MODE:
        //lpmd_log_debug("PERF_MODE sum_c0 = %.2f < 20, grp.sma_avg1 %d < 70\n", sum_c0, grp.sma_avg1);
        // Demote -- if highly MT
        if (max_mt_detected(PERF_MODE)) {
            lpmd_log_debug("PERF_MODE to INIT_MODE = mt detected.\n");    
            prep_state_change(PERF_MODE, INIT_MODE, 0);
            break;
        }
        // Stay -- if there was recent perf/resp bursts
        //if (get_burst_rate_per_min() > BURST_COUNT_THRESHOLD)
        if (burst_count > 0 && !do_countdown(PERF_MODE)){
            lpmd_log_debug("PERF_MODE: burst_count is %d > 0 && !do_countdown\n", burst_count);
            break;
        }            
        // Promote but through responsive watch -- if top sampled util and their avg are receeding.
        if (A_LTE_B(sum_c0, (2 * UTIL_LOW)) &&
            A_LTE_B(grp.sma_avg1, UTIL_ABOVE_HALF)) {
            lpmd_log_debug("PERF_MODE to RESP_MODE\n");
            prep_state_change(PERF_MODE, RESP_MODE, 0);
            break;
        }
        // Promote -- to moderate (3) MT state
        if (!burst_rate_breach() && A_LTE_B(grp.c0_max, UTIL_LOW))    // && A_LTE_B(sum_avg, UTIL_BELOW_HALF))
        {
            set_stay_count(MDRT3E_MODE, 0);
            lpmd_log_debug("PERF_MODE to MDRT3E_MODE\n");
            prep_state_change(PERF_MODE, MDRT3E_MODE, 0);
            break;
        }                
        // Stay -- all else
        //lpmd_log_info("stay in PERF_MODE\n");
        break;

    case RESP_MODE:
        // Demote -- if ST above halfway mark and avg trending higher
        if (A_GT_B(grp.c0_max, UTIL_ABOVE_HALF)
            && A_GT_B(grp.sma_avg1, UTIL_BELOW_HALF)) {
            lpmd_log_debug("RESP_MODE to PERF_MODE\n");
            prep_state_change(RESP_MODE, PERF_MODE, 0);
            break;
        }
        // Stay -- if there were recent burst of spikes
        if (perf_count && burst_rate_breach())
            break;        
        
        // Promote -- all else
        if (A_LTE_B(grp.worst_stall * 100, STALL_SCALE_LOWER_MARK)) {
           lpmd_log_debug("worst stall is less than STALL_SCALE_LOWER_MARK -- stay here.\n");            
        } else {
            lpmd_log_debug("RESP_MODE to MDRT3E_MODE\n");            
            prep_state_change(RESP_MODE, MDRT3E_MODE, 0);
        }
        break;
    case MDRT4E_MODE:
        if (A_LTE_B(grp.worst_stall * 100, STALL_SCALE_LOWER_MARK)) {
            lpmd_log_debug("MDRT4E_MODE to RESP_MODE\n");
            prep_state_change(MDRT4E_MODE, RESP_MODE, 0);
            break;
        }
        // Demote
        if (A_GT_B(grp.c0_max, UTIL_NEAR_FULL)) {
            if (!burst_rate_breach() && strikeout_once(N_STRIKE))
                break;
            lpmd_log_debug("MDRT4E_MODE to PERF_MODE\n");
            prep_state_change(MDRT4E_MODE, PERF_MODE, 0);
            break;
        }
        // promote
        if (A_LTE_B(grp.sma_avg1, SUS_LOW_RANGE_END) &&
            A_LTE_B(grp.sma_avg2, SUS_LOW_RANGE_END) &&
            A_LTE_B(sum_c0, UTIL_HALF)) {
            if (!do_countdown(MDRT4E_MODE))
                break;
            lpmd_log_debug("MDRT4E_MODE to NORM_MODE\n");
            prep_state_change(MDRT4E_MODE, NORM_MODE, 0);
            break;
        }
        // stay
        break;
    case MDRT3E_MODE:
        // Demote -- if mem bound work is stalling but didn't show higher utilization
        if (A_LTE_B(grp.worst_stall * 100, STALL_SCALE_LOWER_MARK)) {
            lpmd_log_debug("MDRT3E_MODE to RESP_MODE %.2f < %d\n", grp.worst_stall, STALL_SCALE_LOWER_MARK);
            prep_state_change(MDRT3E_MODE, RESP_MODE, 0);
            break;
        }
        // Demote to perf
        if (A_GT_B(grp.c0_max, UTIL_NEAR_FULL)) {
            if (!burst_rate_breach() && strikeout_once(N_STRIKE)) {
                lpmd_log_debug("MDRT3E_MODE: burst_rate_breach AND strikeout_once - not met\n");
                break;
            }
            lpmd_log_debug("MDRT3E_MODE to PERF_MODE\n");
            prep_state_change(MDRT3E_MODE, PERF_MODE, 0);
            break;
        }
        
        // Demote to 4 thread sustained
        if (A_GTE_B(grp.sma_avg1, SUS_LOW_RANGE_END) &&
            A_GTE_B(grp.sma_avg2, (SUS_LOW_RANGE_END - 5))) {
            lpmd_log_debug("MDRT3E_MODE to MDRT4E_MODE %d > %d\n", grp.sma_avg1, SUS_LOW_RANGE_END);
            prep_state_change(MDRT3E_MODE, MDRT4E_MODE, 0);
            break;
        }
        // promote
        if ((A_GT_B(grp.sma_avg1, SUS_LOW_RANGE_START) &&
             A_LTE_B(grp.sma_avg1, SUS_LOW_RANGE_END)) &&
            (A_GT_B(grp.sma_avg2, SUS_LOW_RANGE_START) &&
             A_LTE_B(grp.sma_avg2, SUS_LOW_RANGE_END))) {
            if (!do_countdown(MDRT3E_MODE)) {
                lpmd_log_debug("MDRT3E_MODE: to MDRT2E_MODE - do countdown not met\n");
                break;
            }
            lpmd_log_debug("MDRT3E_MODE to MDRT2E_MODE %d < %d\n", grp.sma_avg1, MDRT2E_MODE);            
            prep_state_change(MDRT3E_MODE, MDRT2E_MODE, 0);            
            break;
        }
        // Promote -- if top three avg util are trending lower.
        if (A_LTE_B(grp.sma_avg1, SUS_LOW_RANGE_END) &&
            (A_LTE_B(grp.sma_avg2, SUS_LOWER) &&
             A_LTE_B(grp.sma_avg3, SUS_LOWER))) {
            if (!do_countdown(MDRT3E_MODE)) {
                lpmd_log_debug("MDRT3E_MODE: to NORM_MODE - do countdown not met\n");
                break;
            }
            lpmd_log_debug("MDRT3E_MODE to NORM_MODE\n");
            prep_state_change(MDRT3E_MODE, NORM_MODE, 0);
            break;
        }

        lpmd_log_debug("MDRT3E_MODE: stay\n");        
        break;

    case MDRT2E_MODE:
        // Demote -- if mem bound work is stalling but didn't show higher utilization
        if (A_LTE_B(grp.worst_stall * 100, STALL_SCALE_LOWER_MARK)) {
            lpmd_log_debug("MDRT2E_MODE to RESP_MODE\n");
            prep_state_change(MDRT2E_MODE, RESP_MODE, 0);
            break;
        }
        // Demote -- if instant util nearing full or sustained moderate avg1 trend with avg2 trailing closeby
        if (A_GT_B(grp.c0_max, UTIL_NEAR_FULL) ||
            (A_GTE_B(grp.sma_avg1, SUS_LOW_RANGE_END) &&
             A_GTE_B(grp.sma_avg2, SUS_LOW_RANGE_END - 10))) {
            if (!burst_rate_breach() && strikeout_once(N_STRIKE))
                break;
            lpmd_log_debug("MDRT2E_MODE to MDRT3E_MODE\n");
            prep_state_change(MDRT2E_MODE, MDRT3E_MODE, 0);
            break;
        }
        // Promote -- if top two avg util are trending lower.
        if ((A_GT_B(grp.sma_avg1, SUS_LOW_RANGE_START)
             && A_LTE_B(grp.sma_avg1, SUS_LOW_RANGE_END))
            && A_LTE_B(grp.sma_avg2, SUS_LOW_RANGE_END)) {
            if (!do_countdown(MDRT2E_MODE)) {
                break;
            }
            lpmd_log_debug("MDRT2E_MODE to NORM_MODE\n");
            prep_state_change(MDRT2E_MODE, NORM_MODE, 0);
            break;
        }
        // stay
        break;
    case NORM_MODE:
        // Demote -- if mem bound work is stalling but didn't show higher utilization
        if (A_LTE_B(grp.worst_stall * 100, STALL_SCALE_LOWER_MARK)) {
            lpmd_log_debug("NORM_MODE to RESP_MODE\n");
            prep_state_change(NORM_MODE, RESP_MODE, 0);
            break;
        }
        // Demote -- if instant util more than half or if signs of sustained ST activity.
        if (A_GT_B(grp.c0_max, UTIL_HALF)
            || (A_GT_B(grp.sma_avg1, UTIL_BELOW_HALF))) {
            /* In this state its better to absorb few spike (noise) before reacting */
            if (!burst_rate_breach() && strikeout_once(N_STRIKE))
                break;
            lpmd_log_debug("NORM_MODE to MDRT2E_MODE\n");
            prep_state_change(NORM_MODE, MDRT2E_MODE, 0);
            break;
        }
        // Promote -- if top few instant util or top avg is trending lower.
        if ((A_LTE_B(grp.c0_max, UTIL_LOW)
             && A_LTE_B(grp.c0_2nd_max, UTIL_LOWEST))
            || A_LTE_B(grp.sma_avg1, SUS_LOWER)) {
            /* its better to absorb few dips before reacting out of a steady-state */
            if (!do_countdown(NORM_MODE))
                break;
            lpmd_log_debug("NORM_MODE to DEEP_MODE\n");
            prep_state_change(NORM_MODE, DEEP_MODE, 0);
            break;
        }
        break;
    case DEEP_MODE:
        // Demote -- if mem bound work is stalling but didn't show higher util.
        if (A_LTE_B(grp.worst_stall * 100, STALL_SCALE_LOWER_MARK)) {
            lpmd_log_debug("DEEP_MODE to RESP_MODE\n");
            prep_state_change(DEEP_MODE, RESP_MODE, 0);
            break;
        }
        // Demote -- if there are early signs of instantaneous utilization build-up.
        if (A_GT_B(grp.c0_max, UTIL_FILL_START)) {
            lpmd_log_debug("DEEP_MODE to NORM_MODE\n");
            prep_state_change(DEEP_MODE, NORM_MODE, 0);
            break;
        }

        break;
    }

    return 1;
}
