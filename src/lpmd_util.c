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

static int current_idx = STATE_NONE;

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

int util_update(lpmd_config_t *lpmd_config)
{
	parse_proc_stat ();
	parse_gfx_util();
	lpmd_config->data.util_sys = busy_sys;
	lpmd_config->data.util_cpu = busy_cpu;
	lpmd_config->data.util_gfx = busy_gfx;

	return 0;
}
