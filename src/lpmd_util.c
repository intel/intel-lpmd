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

extern int next_proxy_poll; 

static lpmd_config_state_t *current_state;

void reset_config_state(void)
{
	current_state = NULL;
}

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
};

struct proc_stat_info {
	int cpu;
	int valid;
	unsigned long long stat[STAT_MAX];
};

struct proc_stat_info *proc_stat_prev;
struct proc_stat_info *proc_stat_cur;

static int busy_sys = -1;
static int busy_cpu = -1;

static int calculate_busypct(struct proc_stat_info *cur, struct proc_stat_info *prev)
{
	int idx;
	unsigned long long busy = 0, total = 0;

	for (idx = STAT_USER; idx < STAT_MAX; idx++) {
		total += (cur->stat[idx] - prev->stat[idx]);
//		 Align with the "top" utility logic
		if (idx != STAT_IDLE && idx != STAT_IOWAIT)
			busy += (cur->stat[idx] - prev->stat[idx]);
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
	int count = get_max_online_cpu() + 1;
	int sys_idx = count - 1;
	int size = sizeof(struct proc_stat_info) * count;

	filep = fopen (PATH_PROC_STAT, "r");
	if (!filep)
		return 1;

	if (!proc_stat_prev)
		proc_stat_prev = calloc(sizeof(struct proc_stat_info), count);
	if (!proc_stat_cur)
		proc_stat_cur = calloc(sizeof(struct proc_stat_info), count);

	memcpy (proc_stat_prev, proc_stat_cur, size);
	memset (proc_stat_cur, 0, size);

	while (!feof (filep)) {
		int idx;
		char *tmpline = NULL;
		struct proc_stat_info *info;
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
			/* Read system line */
			info = &proc_stat_cur[sys_idx];
		}
		else if (ret == 1) {
			info = &proc_stat_cur[cpu];
		}
		else {
			free (tmpline);
			free (line);
			continue;
		}

		info->valid = 1;
		idx = STAT_CPU;

		while (p != NULL) {
			if (idx >= STAT_MAX)
				break;

			if (idx == STAT_CPU) {
				idx++;
				p = strtok (NULL, " ");
				continue;
			}

			int result = sscanf (p, "%llu", &info->stat[idx]);
            if (result != EOF)
                p = strtok (NULL, " ");
			idx++;
		}

		free (tmpline);
		free (line);
	}

	fclose (filep);
	busy_sys = calculate_busypct (&proc_stat_cur[sys_idx], &proc_stat_prev[sys_idx]);

	busy_cpu = 0;
	for (i = 1; i <= get_max_online_cpu(); i++) {
		if (!proc_stat_cur[i].valid)
			continue;

		val = calculate_busypct (&proc_stat_cur[i], &proc_stat_prev[i]);
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

static int state_match(lpmd_config_state_t *state, int bsys, int bcpu, int wlt_index)
{
	if (!state->valid)
		return 0;

	if (state->wlt_type != -1) {
		if (state->wlt_type == wlt_index) {
			lpmd_log_debug("Match  %12s: WLT index:%d\n", state->name, wlt_index);
			return 1;
		}
		return 0;
	}

	if (state->enter_cpu_load_thres) {
		if (bcpu > state->enter_cpu_load_thres)
			goto unmatch;
	}

	if (state->entry_system_load_thres) {
		if (bsys > state->entry_system_load_thres) {
			if (!state->exit_system_load_hyst || state != current_state)
				goto unmatch;

			if (bsys > state->entry_load_sys + state->exit_system_load_hyst ||
			    bsys > state->entry_system_load_thres + state->exit_system_load_hyst)
				goto unmatch;
		}
	}

	lpmd_log_debug("Match  %12s: sys_thres %3d cpu_thres %3d hyst %3d\n", state->name, state->entry_system_load_thres, state->enter_cpu_load_thres, state->exit_system_load_hyst);
	return 1;
unmatch:
	lpmd_log_debug("Ignore %12s: sys_thres %3d cpu_thres %3d hyst %3d\n", state->name, state->entry_system_load_thres, state->enter_cpu_load_thres, state->exit_system_load_hyst);
	return 0;
}

#define DEFAULT_POLL_RATE_MS	1000

static int enter_state(lpmd_config_state_t *state, int bsys, int bcpu)
{
	static int interval = DEFAULT_POLL_RATE_MS;

	state->entry_load_sys = bsys;
	state->entry_load_cpu = bcpu;

	/* Adjust polling interval only */
	if (state == current_state) {
		if (state->poll_interval_increment > 0) {
			interval += state->poll_interval_increment;
		}
		/* Adaptive polling interval based on cpu utilization */
		if (state->poll_interval_increment == -1) {
			interval = state->max_poll_interval * (10000 - bcpu) / 10000;
			interval /= 100;
			interval *= 100;
		}
		if (state->min_poll_interval && interval < state->min_poll_interval)
			interval = state->min_poll_interval;
		if (state->max_poll_interval && interval > state->max_poll_interval)
			interval = state->max_poll_interval;
		return interval;
	}

	set_lpm_epp(state->epp);
	set_lpm_epb(state->epb);
	set_lpm_itmt(state->itmt_state);

	if (state->active_cpus[0] != '\0') {
		reset_cpus(CPUMASK_UTIL);
		parse_cpu_str(state->active_cpus, CPUMASK_UTIL);
		if (state->irq_migrate != SETTING_IGNORE)
			set_lpm_irq(get_cpumask(CPUMASK_UTIL), 1);
		else
			set_lpm_irq(NULL, SETTING_IGNORE);
		set_lpm_cpus(CPUMASK_UTIL);
	} else {
		set_lpm_irq(NULL, SETTING_IGNORE);
		set_lpm_cpus(CPUMASK_MAX); /* Ignore Task migration */
	}

	process_lpm(UTIL_ENTER);

	if (state->min_poll_interval)
		interval = state->min_poll_interval;
	else
		interval = DEFAULT_POLL_RATE_MS;

	current_state = state;

	return interval;
}

static int process_next_config_state(lpmd_config_t *config, int wlt_index)
{
	lpmd_config_state_t *state = NULL;
	int i = 0;
	int interval = -1;
	int epp, epb;
	char epp_str[32] = "";

	// Check for new state
	for (i = 0; i < config->config_state_count; ++i) {
		state = &config->config_states[i];
		if (state_match(state, busy_sys, busy_cpu, wlt_index)) {
			interval = enter_state(state, busy_sys, busy_cpu);
			break;
		}
	}

	if (!current_state)
		return interval;

	get_epp_epb(&epp, epp_str, 32, &epb);

	if (config->wlt_proxy_enable){
    	interval = 2000;

		//gets interval of different states 
		if (interval != next_proxy_poll && next_proxy_poll > 0)
			interval = next_proxy_poll;
	}	

	if (epp >= 0)
		lpmd_log_info(
				"[%d/%d] %12s: bsys: %3d.%02d, bcpu: %3d.%02d, epp %20d, epb %3d, itmt %2d, interval %4d\n",
				current_state->id, config->config_state_count,
				current_state->name, busy_sys / 100, busy_sys % 100,
				busy_cpu / 100, busy_cpu % 100, epp, epb, get_itmt(), interval);
	else
		lpmd_log_info(
				"[%d/%d] %12s: bsys: %3d.%02d, bcpu: %3d.%02d, epp %20s, epb %3d, itmt %2d, interval %4d\n",
				current_state->id, config->config_state_count,
				current_state->name, busy_sys / 100, busy_sys % 100,
				busy_cpu / 100, busy_cpu % 100, epp_str, epb, get_itmt(),
				interval);

	return interval;
}

static int use_config_state = 1;

int use_config_states(void)
{
	return use_config_state;
}

int periodic_util_update(lpmd_config_t *lpmd_config, int wlt_index)
{
	int interval;
	static int initialized;

	if (wlt_index >= 0) {
		process_next_config_state(lpmd_config, wlt_index);
		return -1;
	}

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

	if (!lpmd_config->config_state_count || !use_config_state) {
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
	} else
		interval = process_next_config_state(lpmd_config, wlt_index);

	return interval;
}

int util_init(lpmd_config_t *lpmd_config)
{
	lpmd_config_state_t *state;
	int nr_state = 0;
	int i, ret;

	for (i = 0; i < lpmd_config->config_state_count; i++) {
		state = &lpmd_config->config_states[i];

		if (state->active_cpus[0] != '\0') {
			ret = parse_cpu_str(state->active_cpus, CPUMASK_UTIL);
			if (ret <= 0) {
				state->valid = 0;
				continue;
			}
		}

		if (!state->min_poll_interval)
			state->min_poll_interval = state->max_poll_interval > DEFAULT_POLL_RATE_MS ? DEFAULT_POLL_RATE_MS : state->max_poll_interval;
		if (!state->max_poll_interval)
			state->max_poll_interval = state->min_poll_interval > DEFAULT_POLL_RATE_MS ? state->min_poll_interval : DEFAULT_POLL_RATE_MS;
		if (!state->poll_interval_increment)
			state->poll_interval_increment = -1;

		state->entry_system_load_thres *= 100;
		state->enter_cpu_load_thres *= 100;
		state->exit_cpu_load_thres *= 100;

		state->valid = 1;
		nr_state++;
	}

	if (nr_state < 2) {
		lpmd_log_info("%d valid config states found\n", nr_state);
		use_config_state = 0;
		return 1;
	}

	return 0;
}
