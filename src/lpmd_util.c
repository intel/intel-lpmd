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

#define PATH_PROC_STAT "/proc/stat"

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
static int busy_gfx = -1;

char *path_gfx_rc6;
char *path_sam_mc6;

static int probe_gfx_util_sysfs(void)
{
	FILE *fp;
	char buf[8];
	bool gt0_is_gt;

	if (access("/sys/class/drm/card0/device/tile0/gt0/gtidle/idle_residency_ms", R_OK))
		return 1;

	fp = fopen("/sys/class/drm/card0/device/tile0/gt0/gtidle/name", "r");
	if (!fp)
		return 1;

	if (!fread(buf, sizeof(char), 7, fp)) {
		fclose(fp);
		return 1;
	}

	fclose(fp);

	if (!strncmp(buf, "gt0-rc", strlen("gt0-rc"))) {
		if (!access("/sys/class/drm/card0/device/tile0/gt0/gtidle/idle_residency_ms", R_OK))
			path_gfx_rc6 = "/sys/class/drm/card0/device/tile0/gt0/gtidle/idle_residency_ms";
		if (!access("/sys/class/drm/card0/device/tile0/gt1/gtidle/idle_residency_ms", R_OK))
			path_sam_mc6 = "/sys/class/drm/card0/device/tile0/gt1/gtidle/idle_residency_ms";
	} else if (!strncmp(buf, "gt0-mc", strlen("gt0-mc"))) {
		if (!access("/sys/class/drm/card0/device/tile0/gt1/gtidle/idle_residency_ms", R_OK))
			path_gfx_rc6 = "/sys/class/drm/card0/device/tile0/gt1/gtidle/idle_residency_ms";
		if (!access("/sys/class/drm/card0/device/tile0/gt0/gtidle/idle_residency_ms", R_OK))
			path_sam_mc6 = "/sys/class/drm/card0/device/tile0/gt0/gtidle/idle_residency_ms";
	}
	lpmd_log_debug("Use %s for gfx rc6\n", path_gfx_rc6);
	lpmd_log_debug("Use %s for sam mc6\n", path_sam_mc6);
	return 0;
}

static int get_gfx_util_sysfs(unsigned long long time_ms)
{
	static unsigned long long gfx_rc6_prev = ULLONG_MAX, sam_mc6_prev = ULLONG_MAX;
	unsigned long long gfx_rc6, sam_mc6;
	unsigned long long val;
	FILE *fp;
	int gfx_util, sam_util;
	int ret;
	int i;

	gfx_util = sam_util = -1;

	fp = fopen(path_gfx_rc6, "r");
	if (fp) {
		ret = fscanf(fp, "%lld", &gfx_rc6);
		if (ret != 1)
			gfx_rc6 = ULLONG_MAX;
		fclose(fp);
	}

	fp = fopen(path_sam_mc6, "r");
	if (fp) {
		ret = fscanf(fp, "%lld", &sam_mc6);
		if (ret != 1)
			sam_mc6 = ULLONG_MAX;
		fclose(fp);
	}

	if (gfx_rc6 == ULLONG_MAX && sam_mc6 == ULLONG_MAX)
		return -1;

	if (gfx_rc6 != ULLONG_MAX) {
		if (gfx_rc6_prev != ULLONG_MAX)
			gfx_util = 10000 - (gfx_rc6 - gfx_rc6_prev) * 10000 / time_ms;
		gfx_rc6_prev = gfx_rc6;
		lpmd_log_debug("GFX Utilization: %d.%d\n", gfx_util / 100, gfx_util % 100);
	}

	if (sam_mc6 != ULLONG_MAX) {
		if (sam_mc6_prev != ULLONG_MAX)
			sam_util = 10000 - (sam_mc6 - sam_mc6_prev) * 10000 / time_ms;
		sam_mc6_prev = sam_mc6;
		lpmd_log_debug("SAM Utilization: %d.%d\n", sam_util / 100, sam_util % 100);
	}

	return gfx_util > sam_util ? gfx_util : sam_util;
}

/* Get GFX_RC6 and SAM_MC6 from sysfs and calculate gfx util based on this */
static int parse_gfx_util_sysfs(void)
{
	static int gfx_sysfs_available = 1;
	static struct timespec ts_prev;
	struct timespec ts_cur;
	unsigned long time_ms;
	int ret;

	busy_gfx = -1;

	if (!gfx_sysfs_available)
		return 1;

	clock_gettime (CLOCK_MONOTONIC, &ts_cur);

	if (!ts_prev.tv_sec && !ts_prev.tv_nsec) {
		ret = probe_gfx_util_sysfs();
		if (ret) {
			gfx_sysfs_available = 0;
			return 1;
		}
		ts_prev = ts_cur;
		return 0;
	}

	time_ms = (ts_cur.tv_sec - ts_prev.tv_sec) * 1000 + (ts_cur.tv_nsec - ts_prev.tv_nsec) / 1000000;

	ts_prev = ts_cur;
	busy_gfx = get_gfx_util_sysfs(time_ms);

	return 0;
}

#define MSR_TSC			0x10
#define MSR_PKG_ANY_GFXE_C0_RES	0x65A
static int parse_gfx_util_msr(void)
{
	static uint64_t val_prev;
	uint64_t val;
	static uint64_t tsc_prev;
	uint64_t tsc;
	int cpu;

	cpu = sched_getcpu();
	tsc = read_msr(cpu, MSR_TSC);
	if (tsc == UINT64_MAX)
		goto err;

	val = read_msr(cpu, MSR_PKG_ANY_GFXE_C0_RES);
	if (val == UINT64_MAX)
		goto err;

	if (!tsc_prev || !val_prev) {
		tsc_prev = tsc;
		val_prev = val;
		busy_gfx = -1;
		return 0;
	}

	busy_gfx = (val - val_prev) * 10000 / (tsc - tsc_prev);
	tsc_prev = tsc;
	val_prev = val;
	return 0;
err:
	lpmd_log_debug("parse_gfx_util_msr failed\n");
	busy_gfx = -1;
	return 1;
}

static int parse_gfx_util(void)
{
	int ret;

	/* Prefer to get graphics utilization from GFX/SAM RC6 sysfs */
	ret = parse_gfx_util_sysfs();
	if (!ret)
		return 0;

	/* Fallback to MSR */
	return parse_gfx_util_msr();
}

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

	if (!proc_stat_prev) {
		fclose (filep);
		return 1;
	}

	if (!proc_stat_cur)
		proc_stat_cur = calloc(sizeof(struct proc_stat_info), count);

	if (!proc_stat_cur) {
		free(proc_stat_prev);
		fclose (filep);
		proc_stat_prev = NULL;
		return 1;
	}

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

			if (sscanf (p, "%llu", &info->stat[idx]) <= 0)
				lpmd_log_debug("Failed to parse /proc/stat, defer update in next snapshot.");

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

static int state_match(lpmd_config_state_t *state, lpmd_data_t *data)
{
	int bcpu = data->util_cpu;
	int bsys = data->util_sys;
	int bgfx = data->util_gfx;
	int wlt_index = data->wlt_hint;

	if (!state->valid)
		return 0;

	if (state->wlt_type != -1) {
		/* wlt hint must match */
		if (state->wlt_type != wlt_index)
			return 0;

		/* return match directly if no util threshold specified */
		if (!state->enter_gfx_load_thres)
			return 1;
		/* leverage below logic to handle util threshold */
	}

	/* No need to dump utilization info if no threshold specified */
	if (!state->enter_cpu_load_thres && !state->entry_system_load_thres && !state->enter_gfx_load_thres)
		return 1;

	if (state->enter_cpu_load_thres) {
		if (bcpu > state->enter_cpu_load_thres)
			goto unmatch;
	}

	if (state->enter_gfx_load_thres) {
		if (bgfx == -1)
			lpmd_log_debug("Graphics utilization not available, ignore graphics threshold\n");
		else if (bgfx > state->enter_gfx_load_thres)
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

	lpmd_log_debug("Match  %12s: sys_thres %3d cpu_thres %3d gfx_thres %3d hyst %3d\n", state->name, state->entry_system_load_thres, state->enter_cpu_load_thres, state->enter_gfx_load_thres, state->exit_system_load_hyst);
	return 1;
unmatch:
	lpmd_log_debug("Ignore %12s: sys_thres %3d cpu_thres %3d gfx_thres %3d hyst %3d\n", state->name, state->entry_system_load_thres, state->enter_cpu_load_thres, state->enter_gfx_load_thres, state->exit_system_load_hyst);
	return 0;
}

#define DEFAULT_POLL_RATE_MS	1000

static int enter_state(lpmd_config_state_t *state, lpmd_data_t *data)
{
	static int interval = DEFAULT_POLL_RATE_MS;

	state->entry_load_sys = data->util_sys;
	state->entry_load_cpu = data->util_cpu;

	/* Adjust polling interval only */
	if (state == current_state) {
		if (state->poll_interval_increment > 0) {
			interval += state->poll_interval_increment;
		}
		/* Adaptive polling interval based on cpu utilization */
		if (state->poll_interval_increment == -1) {
			interval = state->max_poll_interval * (10000 - data->util_cpu) / 10000;
			interval /= 100;
			interval *= 100;
		}
		if (state->min_poll_interval && interval < state->min_poll_interval)
			interval = state->min_poll_interval;
		if (state->max_poll_interval && interval > state->max_poll_interval)
			interval = state->max_poll_interval;
		return interval;
	}

	enter_next_state(state);

	if (state->min_poll_interval)
		interval = state->min_poll_interval;
	else
		interval = DEFAULT_POLL_RATE_MS;

	current_state = state;

	return interval;
}

static void dump_system_status(lpmd_config_t *config, int interval)
{
	int epp, epb;
	char epp_str[32] = "";
	char buf[MAX_STR_LENGTH * 2];
	int offset;
	int size;

	offset = 0;
	size = MAX_STR_LENGTH * 2;

	offset += snprintf(buf, size, "[%d/%d] %12s: ",
		current_state->id, config->config_state_count, current_state->name);
	size = MAX_STR_LENGTH * 2 - offset;

	if (busy_sys == -1)
		offset += snprintf(buf + offset, size, "bsys     na, ");
	else
		offset += snprintf(buf + offset, size, "bsys %3d.%02d, ", busy_sys / 100, busy_sys % 100);
	size = MAX_STR_LENGTH * 2 - offset;

	if (busy_cpu == -1)
		offset += snprintf(buf + offset, size, "bcpu     na, ");
	else
		offset += snprintf(buf + offset, size, "bcpu %3d.%02d, ", busy_cpu / 100, busy_cpu % 100);
	size = MAX_STR_LENGTH * 2 - offset;

	if (busy_gfx == -1)
		offset += snprintf(buf + offset, size, "bgfx     na, ");
	else
		offset += snprintf(buf + offset, size, "bgfx %3d.%02d, ", busy_gfx / 100, busy_gfx % 100);
	size = MAX_STR_LENGTH * 2 - offset;

	get_epp_epb(&epp, epp_str, 32, &epb);

	if (epp >= 0)
		offset += snprintf(buf + offset, size, "epp %3d, ", epp);
	else
		offset += snprintf(buf + offset, size, "epp %s, ", epp_str);
	size = MAX_STR_LENGTH * 2 - offset;

	offset += snprintf(buf + offset, size, "epb %3d, ", epb);
	size = MAX_STR_LENGTH * 2 - offset;

	if (current_state->itmt_state != SETTING_IGNORE)
		offset += snprintf(buf + offset, size, "itmt %2d, ", get_itmt());

	size = MAX_STR_LENGTH * 2 - offset;

	snprintf(buf + offset, size, "interval %4d", interval);

	lpmd_log_info("%s\n", buf);
}

static int process_next_config_state(lpmd_config_t *config)
{
	lpmd_config_state_t *state = NULL;
	int i = 0;
	int interval = -1;

	// Check for new state
	for (i = CONFIG_STATE_BASE; i < CONFIG_STATE_BASE + config->config_state_count; ++i) {
		state = &config->config_states[i];
		if (state_match(state, &config->data)) {
			interval = enter_state(state, &config->data);
			break;
		}
	}

	if (!current_state)
		return interval;

	dump_system_status(config, interval);

	return interval;
}

int periodic_util_update(lpmd_config_t *lpmd_config)
{
	int interval;
	static int initialized;
	int wlt_index = lpmd_config->data.wlt_hint;

	if (wlt_index >= 0) {
		if (lpmd_config->wlt_hint_poll_enable) {
			parse_gfx_util();
			interval = process_next_config_state(lpmd_config);
		} else {
			process_next_config_state(lpmd_config);
			interval = -1;
		}
		return interval;
	}

//	 poll() timeout should be -1 when util monitor not enabled
	if (!has_util_monitor ())
		return -1;

	parse_proc_stat ();
	parse_gfx_util();
	lpmd_config->data.util_sys = busy_sys;
	lpmd_config->data.util_cpu = busy_cpu;
	lpmd_config->data.util_gfx = busy_gfx;

	return process_next_config_state(lpmd_config);
}

int util_init(lpmd_config_t *lpmd_config)
{
	lpmd_config_state_t *state;
	int nr_state = 0;
	int i, ret;

	for (i = 0; i < lpmd_config->config_state_count; i++) {
		state = &lpmd_config->config_states[i];

		if (state->active_cpus[0] != '\0') {
			state->cpumask_idx = alloc_cpumask();
			if (state->cpumask_idx == CPUMASK_NONE) {
				lpmd_log_error("Cannot alloc CPUMASK\n");
				return -1;
			}
			ret = parse_cpu_str(state->active_cpus, state->cpumask_idx);
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
		state->enter_gfx_load_thres *= 100;

		nr_state++;
	}

	if (nr_state < 2) {
		lpmd_log_info("%d valid config states found\n", nr_state);
		return 1;
	}

	return 0;
}
