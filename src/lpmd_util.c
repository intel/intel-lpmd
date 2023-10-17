/*
 * util.c: intel_lpmd utilization monitor
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
 * This file contains logic similar to "top" program to get utilization from
 * /proc/sys kernel interface.
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <err.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/file.h>
#include <sys/types.h>
#include <sys/time.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <errno.h>
#include <pthread.h>

#include "lpmd.h"

/* System should quit Low Power Mode when it is overloaded */
#define PATH_PROC_STAT "/proc/stat"

enum type_stat {
	STAT_CPU,
	STAT_USER,
	STAT_NICE,
	STAT_SYSTEM,
	STAT_IDLE,
	STAT_IOWAIT,
	STAT_IRQ,
	STAT_SOFTIRQ,
	STAT_STEAL,
	STAT_GUEST,
	STAT_GUEST_NICE,
	STAT_MAX,
	STAT_VALID = STAT_MAX,
	STAT_EXT_MAX,
};

// One for system utilization, MAX_LPM_CPUS for LPM cpu utilization
unsigned long long proc_stat_prev[MAX_LPM_CPUS + 1][STAT_EXT_MAX];
unsigned long long proc_stat_cur[MAX_LPM_CPUS + 1][STAT_EXT_MAX];

static int busy_sys = -1;
static int busy_cpu = -1;

static int calculate_busypct(unsigned long long *cur, unsigned long long *prev)
{
	int idx;
	unsigned long long busy = 0, total = 0;

	for (idx = STAT_USER; idx < STAT_MAX; idx++) {
		total += (cur[idx] - prev[idx]);
//		 Align with the "top" utility logic
		if (idx != STAT_IDLE && idx != STAT_IOWAIT)
			busy += (cur[idx] - prev[idx]);
	}

	if (total)
		return busy * 10000 / total;
	else
		return 0;
}

static int parse_proc_stat(void)
{
	FILE *filep;
	int i;
	int val;
	int pos = 0;
	int size = sizeof(unsigned long long) * (MAX_LPM_CPUS + 1) * STAT_MAX;

	filep = fopen (PATH_PROC_STAT, "r");
	if (!filep)
		return 1;

	memcpy (proc_stat_prev, proc_stat_cur, size);
	memset (proc_stat_cur, 0, size);

	while (!feof (filep)) {
		int idx;
		char *tmpline = NULL;
		size_t size = 0;
		char *line;
		int cpu;
		char *p;
		int ret;

		tmpline = NULL;
		size = 0;

		if (getline (&tmpline, &size, filep) <= 0) {
			free (tmpline);
			break;
		}

		line = strdup (tmpline);

		p = strtok (line, " ");

		if (strncmp (p, "cpu", 3)) {
			free (tmpline);
			free (line);
			continue;
		}

		ret = sscanf (p, "cpu%d", &cpu);
		if (ret == -1 && !(strncmp (p, "cpu", 3))) {
			proc_stat_cur[pos][STAT_VALID] = 1;
			proc_stat_cur[pos++][STAT_CPU] = -1;
		}
		else if (ret == 1) {
			if (!is_cpu_for_lpm (cpu)) {
				free (tmpline);
				free (line);
				continue;
			}

//			 array 0 is always for system utilization
			if (!pos)
				pos = 1;
			if (pos > MAX_LPM_CPUS) {
				free (tmpline);
				free (line);
				break;;
			}
			proc_stat_cur[pos][STAT_VALID] = 1;
			proc_stat_cur[pos++][STAT_CPU] = cpu;
		}
		else {
			free (tmpline);
			free (line);
			continue;
		}

		idx = STAT_CPU;
		line[MAX_STR_LENGTH - 1] = '\0';

		while (p != NULL) {
			if (idx >= STAT_MAX)
				break;

			if (idx == STAT_CPU) {
				idx++;
				p = strtok (NULL, " ");
				continue;
			}

			sscanf (p, "%llu", &proc_stat_cur[pos - 1][idx]);
			p = strtok (NULL, " ");
			idx++;
		}

		free (tmpline);
		free (line);
	}

	fclose (filep);
	busy_sys = calculate_busypct (proc_stat_cur[0], proc_stat_prev[0]);

	busy_cpu = 0;
	for (i = 1; i < MAX_LPM_CPUS + 1; i++) {
		if (!proc_stat_cur[i][STAT_VALID])
			continue;

		val = calculate_busypct (proc_stat_cur[i], proc_stat_prev[i]);
		if (busy_cpu < val)
			busy_cpu = val;
	}

	return 0;
}

enum system_status {
	SYS_IDLE, SYS_NORMAL, SYS_OVERLOAD, SYS_UNKNOWN,
};

static enum system_status sys_stat = SYS_NORMAL;

static int first_run = 1;

static enum system_status get_sys_stat(void)
{
	if (first_run)
		return SYS_NORMAL;

	if (!in_lpm () && busy_sys <= (get_util_entry_threshold () * 100))
		return SYS_IDLE;
	else if (in_lpm () && busy_cpu > (get_util_exit_threshold () * 100))
		return SYS_OVERLOAD;

	return SYS_NORMAL;
}

/*
 * Support for hyst statistics
 * Ignore the current request if:
 * a. stay in current state too short
 * b. average time of the target state is too low
 * Note: This is not well tuned yet, set either util_in_hyst or util_out_hyst to 0
 * to avoid the hyst algorithm.
 */
#define DECAY_PERIOD	5

static struct timespec tp_last_in, tp_last_out;

static unsigned long util_out_hyst, util_in_hyst;

static unsigned long util_in_min, util_out_min;

static unsigned long avg_in, avg_out;

static int util_should_proceed(enum system_status status)
{
	struct timespec tp_now;
	unsigned long cur_in, cur_out;

	if (!util_out_hyst && !util_in_hyst)
		return 1;

	clock_gettime (CLOCK_MONOTONIC, &tp_now);

	if (status == SYS_IDLE) {
		cur_out = (tp_now.tv_sec - tp_last_out.tv_sec) * 1000000000 + tp_now.tv_nsec
				- tp_last_out.tv_nsec;
//		 in msec
		cur_out /= 1000000;

		avg_out = avg_out * (DECAY_PERIOD - 1) / DECAY_PERIOD + cur_out / DECAY_PERIOD;

		if (avg_in >= util_in_hyst && cur_out >= util_out_min)
			return 1;

		lpmd_log_info ("\t\t\tIgnore SYS_IDLE: avg_in %lu, avg_out %lu, cur_out %lu\n", avg_in,
						avg_out, cur_out);
		avg_in = avg_in * (DECAY_PERIOD + 1) / DECAY_PERIOD;

		return 0;
	}
	else if (status == SYS_OVERLOAD) {
		cur_in = (tp_now.tv_sec - tp_last_in.tv_sec) * 1000000000 + tp_now.tv_nsec
				- tp_last_in.tv_nsec;
		cur_in /= 1000000;

		avg_in = avg_in * (DECAY_PERIOD - 1) / DECAY_PERIOD + cur_in / DECAY_PERIOD;

		if (avg_out >= util_out_hyst && cur_in >= util_in_min)
			return 1;

		lpmd_log_info ("\t\t\tIgnore SYS_OVERLOAD: avg_in %lu, avg_out %lu, cur_in %lu\n", avg_in,
						avg_out, cur_in);
		avg_out = avg_out * (DECAY_PERIOD + 1) / DECAY_PERIOD;

		return 0;
	}
	return 0;
}

static int get_util_interval(void)
{
	int interval;

	if (in_lpm ()) {
		interval = get_util_exit_interval ();
		if (interval || busy_cpu < 0)
			return interval;
		if (first_run)
			return 1000;
		interval = 1000 * (10000 - busy_cpu) / 10000;
	}
	else {
		interval = get_util_entry_interval ();
		if (interval)
			return interval;
		interval = 1000;
	}

	interval = (interval / 100) * 100;
	if (!interval)
		interval = 100;
	return interval;
}

int periodic_util_update(void)
{
	int interval;
	static int initialized;

//	 poll() timeout should be -1 when util monitor not enabled
	if (!has_util_monitor ())
		return -1;

	if (!initialized) {
		clock_gettime (CLOCK_MONOTONIC, &tp_last_in);
		clock_gettime (CLOCK_MONOTONIC, &tp_last_out);
		avg_in = util_in_hyst = get_util_entry_hyst ();
		avg_out = util_out_hyst = get_util_exit_hyst ();
		util_in_min = util_in_hyst / 2;
		util_out_min = util_out_hyst / 2;
		initialized = 1;
	}

	parse_proc_stat ();
	sys_stat = get_sys_stat ();
	interval = get_util_interval ();

	lpmd_log_info (
			"\t\tSYS util %3d.%02d (Entry threshold : %3d ),"
			" CPU util %3d.%02d ( Exit threshold : %3d ), resample after"
			" %4d ms\n", busy_sys / 100, busy_sys % 100, get_util_entry_threshold (),
			busy_cpu / 100, busy_cpu % 100, get_util_exit_threshold (), interval);

	first_run = 0;

	if (!util_should_proceed (sys_stat))
		return interval;

	switch (sys_stat) {
		case SYS_IDLE:
			process_lpm (UTIL_ENTER);
			first_run = 1;
			clock_gettime (CLOCK_MONOTONIC, &tp_last_in);
			interval = 1000;
			break;
		case SYS_OVERLOAD:
			process_lpm (UTIL_EXIT);
			first_run = 1;
			clock_gettime (CLOCK_MONOTONIC, &tp_last_out);
			break;
		default:
			break;
	}
	return interval;
}
