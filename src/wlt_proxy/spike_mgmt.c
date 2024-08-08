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
#include <err.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
#include <fcntl.h>
#include <sys/file.h>
#include <sys/types.h>
#include <sys/time.h>
#include <sys/stat.h>
#include <errno.h>
#include <stdbool.h>

#include "wlt_proxy_common.h"

/*
 * spike burst refers to coninous spikes in a series of back to back samples.
 * burts count and strength (as %) are good indicators to segregate random noise
 * (that doesn't deserve performance) from bursty workload needing performance.
 *
 * Example of spike burst(|) and non-spike (.) sampling:
 *  	...||..||||...|...|||.....
 * - here, first burst has two spikes.
 * - second and third burst have 4 and 3 spikes respectively
 * - the single spike in between is not considered as burst
 *
 * Using this a few indicators are derived:
 *   spike rate = total_spike_time * 100/ MAX_TRACKED_SPIKE_TIME
 * spike_rate is defined as spike-time % of some MAX_TRACKED_SPIKE_TIME
 *
 *   spike rate avg = spike_rate_total / spike_rate_samples
 * spike rate avg is used to control how long "1 min" in bc_reset_min appears to the algo.
 *
 *  bc_rest_min is used to control how long to wait before reset of past spike burst count
 *   SPIKE_TIME_BIAS macro will bias this to be longer or shorter based on
 *   recent history (i.e more prominent the spiking, the longer it will be remembered)
 */

#define MAX_TRACKED_SPIKE_TIME 1000
#define MAX_BURST_COUNT 1000
static int total_spike_time;
int burst_count = 0;
static int spike_sec_prev = 0;
static int spike_rate_total;
static int spike_rate_samples;
static int burst_rate_per_min;
static bool spike_burst_flag = false;
static float bc_reset_min = 90.0;
extern int state_demote;
static int once_flag;
static int strike_count;

int update_spike_rate_avg(int sr)
{
	spike_rate_total += sr;
	spike_rate_samples++;
	return 1;
}

int clear_spike_rate_avg()
{
	spike_rate_samples = spike_rate_total = 0;
	return 1;
}

/* 
 * shorten time by 50% if spike rate was as low as 0. No change if spike rate was 100
 */
#define SPIKE_TIME_BIAS(avg, min) ((100 - avg) * min/(2 * 100))

/*
 * burst count determines number of bursts occured in recent past (1 min)
 * arg real_spike specifies if invoked to update actual spike (1) or
 * just a refresh to burst_count (0)
 * burst count is decremented if no spikes in last 1 min
 * TODO: The routine does not have support to decrement stale spikes in
 * specific case with continous bursts with gap of less than 1 min.
 */
int update_burst_count(int real_spike_burst)
{
	float minutes = 1.0;
	clockid_t clk = CLOCK_MONOTONIC;
	struct timespec ts;
	if (clock_gettime(clk, &ts)) {
		perror("clock_gettime1");
		return -1;
	}

	if (spike_sec_prev) {
		minutes = (float)(ts.tv_sec - spike_sec_prev) / bc_reset_min;
	} else {
		spike_sec_prev = ts.tv_sec;
		return 0;
	}

	if (real_spike_burst /*&& (get_cur_state() <= MDRT4E_MODE)*/) { //todo: error function not found
		burst_count++;
		spike_sec_prev = ts.tv_sec;
	} else if ((minutes > 1.0) || (burst_count > MAX_BURST_COUNT)) {
		burst_count = 0;
		spike_sec_prev = ts.tv_sec;
	}

	if (minutes < 1.0) {
		burst_rate_per_min = burst_count;
	} else if (minutes && (minutes > 1.0)) {
		burst_rate_per_min = (int)((float)burst_count / minutes);
	}

	return burst_rate_per_min;
}

int fresh_burst_response(int initial_burst_rate)
{
	if (!initial_burst_rate)
		return 0;
	if ((initial_burst_rate >= BURST_COUNT_THRESHOLD) ||
	    (get_burst_rate_per_min() > initial_burst_rate))
		return 1;
	return 0;
}

int burst_rate_breach(void)
{
	return (get_burst_rate_per_min() >= BURST_COUNT_THRESHOLD) ? 1 : 0;
}

int get_burst_rate_per_min(void)
{
	return burst_rate_per_min;
}

int get_spike_rate()
{
	int spike_pct = total_spike_time * 100 / MAX_TRACKED_SPIKE_TIME;
	return (spike_pct > 100) ? 100 : spike_pct;
}

int add_spike_time(int duration)
{
	int spike_rate;
	if (total_spike_time < MAX_TRACKED_SPIKE_TIME)
		total_spike_time += duration;

	/*
	 * spike burst has more than 1 spike
	 */
	if (!spike_burst_flag) {
		/* rising edge of spike burst */
		spike_burst_flag = true;
	} else if (state_demote && !once_flag) {
		update_burst_count(1);
		once_flag = 1;
	}

	spike_rate = get_spike_rate();
	update_spike_rate_avg(spike_rate);
	return 1;
}

int add_non_spike_time(int duration)
{
	float avg;
	int sr;
	if (total_spike_time > 0)
		total_spike_time -= duration;
	total_spike_time = (total_spike_time < 0) ? 0 : total_spike_time;

	sr = get_spike_rate();
	if (!sr && spike_burst_flag) {
		/* falling edge of burst */
		spike_burst_flag = false;
		avg = spike_rate_total / spike_rate_samples;
		if (!once_flag)
			update_burst_count(1);
		//printf(" burst_flag:0  avg %d/ %d = %.2f\n", spike_rate_total, spike_rate_samples, avg);
		bc_reset_min = 60.0 - (int)SPIKE_TIME_BIAS(avg, bc_reset_min);
		//printf("bias %d 1min = %.2f\n" , (int)SPIKE_TIME_BIAS(avg, bc_reset_min), bc_reset_min);
		clear_spike_rate_avg();
		once_flag = 0;
	} else {
		update_burst_count(0);
		once_flag = 0;
	}
	return 1;
}

int strikeout_once(int n)
{
	if (!strike_count)
		strike_count = n;
	else
		strike_count -= 1;

	if (strike_count < 0)
		strike_count = 0;

	return strike_count;
}
