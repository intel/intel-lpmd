// SPDX-License-Identifier: GPL-2.0-or-later
/* Copyright(c) 2022 Intel Corporation. All rights reserved. */

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
#include <limits.h>
#include <pthread.h>

#include "lpmd.h"

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

/*
 * Graphics utilization is derived from the GT idle residency counters (RC6 for
 * a render/compute GT, MC6 for a media GT) exposed by the xe and i915 drivers.
 *
 * DRM card numbering is not stable and a system can have more than one Intel
 * GPU, so probe every card instead of assuming card0, and keep every counter
 * found. Utilization is that of the busiest GT, so it does not matter which
 * card or GT the workload actually runs on.
 */
#define MAX_GFX_COUNTERS	8
#define MAX_DRM_CARDS		8
#define MAX_GT_TILES		2
#define MAX_GTS			4

/* Return values of get_gfx_util_sysfs() besides a valid 0-10000 utilization */
#define GFX_UTIL_NO_SAMPLE	-1	/* Not enough samples collected yet */
#define GFX_UTIL_ERROR		-2	/* No counter could be read at all */

struct gfx_counter_t {
	char path[160];
	/* Previous residency snapshot, ULLONG_MAX when there is none */
	unsigned long long prev;
};

static struct gfx_counter_t gfx_counters[MAX_GFX_COUNTERS];
static int gfx_counter_count;

static int read_residency_ms(const char *path, unsigned long long *val)
{
	FILE *fp;
	int ret;

	fp = fopen(path, "r");
	if (!fp)
		return 1;

	ret = fscanf(fp, "%llu", val);
	fclose(fp);

	return ret == 1 ? 0 : 1;
}

static int add_gfx_counter(const char *path, const char *kind)
{
	struct gfx_counter_t *counter;

	if (gfx_counter_count >= MAX_GFX_COUNTERS) {
		lpmd_log_debug("Ignore %s, too many gfx counters\n", path);
		return 1;
	}

	if (access(path, R_OK))
		return 1;

	counter = &gfx_counters[gfx_counter_count++];
	snprintf(counter->path, sizeof(counter->path), "%s", path);
	counter->prev = ULLONG_MAX;

	lpmd_log_debug("Use %s for gfx %s\n", path, kind);

	return 0;
}

/* xe reports the GT type in gtidle/name: "gtN-rc" render, "gtN-mc" media */
static const char *xe_gt_kind(int card, int tile, int gt)
{
	char path[160];
	char buf[16];
	FILE *fp;
	size_t ret;

	snprintf(path, sizeof(path),
		 "/sys/class/drm/card%d/device/tile%d/gt%d/gtidle/name", card, tile,
		 gt);

	fp = fopen(path, "r");
	if (!fp)
		return "rc6";

	ret = fread(buf, sizeof(char), sizeof(buf) - 1, fp);
	fclose(fp);

	buf[ret] = '\0';

	return strstr(buf, "-mc") ? "mc6" : "rc6";
}

static int probe_gfx_util_sysfs(void)
{
	char path[160];
	int card, tile, gt, found;

	gfx_counter_count = 0;

	for (card = 0; card < MAX_DRM_CARDS; card++) {
		/* xe: one gtidle counter per GT, per tile */
		for (tile = 0; tile < MAX_GT_TILES; tile++) {
			for (gt = 0; gt < MAX_GTS; gt++) {
				snprintf(path, sizeof(path),
					 "/sys/class/drm/card%d/device/tile%d/gt%d/gtidle/idle_residency_ms",
					 card, tile, gt);
				if (access(path, R_OK))
					continue;

				add_gfx_counter(path, xe_gt_kind(card, tile, gt));
			}
		}

		/* i915 with per GT sysfs */
		found = 0;
		for (gt = 0; gt < MAX_GTS; gt++) {
			snprintf(path, sizeof(path),
				 "/sys/class/drm/card%d/gt/gt%d/rc6_residency_ms",
				 card, gt);
			if (!add_gfx_counter(path, "rc6"))
				found = 1;
		}

		/* i915 single GT, same counter as gt/gt0 when that one exists */
		if (found)
			continue;

		snprintf(path, sizeof(path),
			 "/sys/class/drm/card%d/power/rc6_residency_ms", card);
		add_gfx_counter(path, "rc6");
	}

	if (!gfx_counter_count) {
		lpmd_log_debug("No gfx idle residency counter found\n");
		return 1;
	}

	return 0;
}

/*
 * Utilization of the busiest GT over the last time_ms milliseconds.
 * A zero time_ms only refreshes the snapshots.
 */
static int get_gfx_util_sysfs(unsigned long long time_ms)
{
	int i, busy = GFX_UTIL_ERROR;

	for (i = 0; i < gfx_counter_count; i++) {
		struct gfx_counter_t *counter = &gfx_counters[i];
		unsigned long long cur, delta;
		int util;

		if (read_residency_ms(counter->path, &cur)) {
			/* Start over on the next sample */
			counter->prev = ULLONG_MAX;
			continue;
		}

		if (busy == GFX_UTIL_ERROR)
			busy = GFX_UTIL_NO_SAMPLE;

		/* First sample, or the counter was reset behind our back */
		if (!time_ms || counter->prev == ULLONG_MAX || cur < counter->prev) {
			counter->prev = cur;
			continue;
		}

		delta = cur - counter->prev;
		counter->prev = cur;

		/*
		 * Idle residency can slightly exceed the sampling window because
		 * the counter and the timestamp are not read atomically. Clamp
		 * instead of underflowing.
		 */
		if (delta >= time_ms)
			util = 0;
		else
			util = 10000 - delta * 10000 / time_ms;

		if (util > busy)
			busy = util;
	}

	return busy;
}

/* Get GT idle residency from sysfs and calculate gfx util based on this */
static int parse_gfx_util_sysfs(void)
{
	static int gfx_sysfs_available = 1;
	static struct timespec ts_prev;
	struct timespec ts_cur;
	long long time_ms;
	int busy;

	busy_gfx = -1;

	if (!gfx_sysfs_available)
		return 1;

	if (!gfx_counter_count && probe_gfx_util_sysfs()) {
		gfx_sysfs_available = 0;
		return 1;
	}

	clock_gettime(CLOCK_MONOTONIC, &ts_cur);

	time_ms = 0;
	if (ts_prev.tv_sec || ts_prev.tv_nsec)
		time_ms = ((long long)ts_cur.tv_sec - ts_prev.tv_sec) * 1000 +
			  ((long long)ts_cur.tv_nsec - ts_prev.tv_nsec) / 1000000;
	if (time_ms < 0)
		time_ms = 0;

	ts_prev = ts_cur;

	busy = get_gfx_util_sysfs(time_ms);

	if (busy == GFX_UTIL_ERROR) {
		/* The counters went away, re-probe and fall back to MSR if needed */
		lpmd_log_debug("Failed to read gfx idle residency, re-probing\n");
		if (probe_gfx_util_sysfs()) {
			gfx_sysfs_available = 0;
			return 1;
		}
		return 0;
	}

	if (busy >= 0)
		busy_gfx = busy;

	return 0;
}

#define MSR_TSC			0x10
#define MSR_PKG_ANY_GFXE_C0_RES	0x65A
static int parse_gfx_util_msr(void)
{
	static uint64_t val_prev, tsc_prev;
	static int primed;
	uint64_t _busy_gfx, val, tsc;
	int cpu;

	busy_gfx = -1;

	cpu = sched_getcpu();
	tsc = read_msr(cpu, MSR_TSC);
	if (tsc == UINT64_MAX)
		goto err;

	val = read_msr(cpu, MSR_PKG_ANY_GFXE_C0_RES);
	if (val == UINT64_MAX)
		goto err;

	/* A never busy GFX reads 0, so track the first sample explicitly */
	if (!primed) {
		primed = 1;
		tsc_prev = tsc;
		val_prev = val;
		return 0;
	}

	/*
	 * Do not use abs() on the deltas: it takes an int, and the TSC delta
	 * alone exceeds INT_MAX after ~0.7 second on a 3GHz part, which turns
	 * the divisor into an unrelated value.
	 */
	if (val >= val_prev && tsc > tsc_prev) {
		_busy_gfx = (val - val_prev) * 10000ULL / (tsc - tsc_prev);
		if (_busy_gfx > 10000)
			_busy_gfx = 10000;
		busy_gfx = (int)_busy_gfx;
	}

	tsc_prev = tsc;
	val_prev = val;
	return 0;
err:
	lpmd_log_debug("%s failed\n", __func__);
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
	size_t size = sizeof(struct proc_stat_info) * count;

	filep = fopen(PATH_PROC_STAT, "r");
	if (!filep)
		return 1;

	if (!proc_stat_prev)
		proc_stat_prev = calloc(count, sizeof(struct proc_stat_info));

	if (!proc_stat_prev) {
		fclose(filep);
		return 1;
	}

	if (!proc_stat_cur)
		proc_stat_cur = calloc(count, sizeof(struct proc_stat_info));

	if (!proc_stat_cur) {
		free(proc_stat_prev);
		fclose(filep);
		proc_stat_prev = NULL;
		return 1;
	}

	memcpy(proc_stat_prev, proc_stat_cur, size);
	memset(proc_stat_cur, 0, size);

	while (!feof(filep)) {
		int idx;
		char *tmpline = NULL;
		struct proc_stat_info *info;
		char *line;
		int cpu;
		char *p;
		int ret;

		tmpline = NULL;
		size = 0;

		if (getline(&tmpline, &size, filep) <= 0) {
			free(tmpline);
			break;
		}

		line = strdup(tmpline);

		p = strtok(line, " ");

		/* Match 'cpu' prefix */
		if (strncmp(p, "cpu", 3)) {
			free(tmpline);
			free(line);
			continue;
		}

		ret = sscanf(p, "cpu%d", &cpu);

		/* Match 'cpu' prefix */
		if (ret == -1 && !(strncmp(p, "cpu", 3))) {
			/* Read system line */
			info = &proc_stat_cur[sys_idx];
		} else if (ret == 1) {
			info = &proc_stat_cur[cpu];
		} else {
			free(tmpline);
			free(line);
			continue;
		}

		info->valid = 1;
		idx = STAT_CPU;

		while (p) {
			if (idx >= STAT_MAX)
				break;

			if (idx == STAT_CPU) {
				idx++;
				p = strtok(NULL, " ");
				continue;
			}

			if (sscanf(p, "%llu", &info->stat[idx]) <= 0)
				lpmd_log_debug("Failed to parse /proc/stat, defer update in next snapshot.");

			p = strtok(NULL, " ");
			idx++;
		}

		free(tmpline);
		free(line);
	}

	fclose(filep);
	busy_sys = calculate_busypct(&proc_stat_cur[sys_idx], &proc_stat_prev[sys_idx]);

	busy_cpu = 0;
	for (i = 1; i <= get_max_online_cpu(); i++) {
		if (!proc_stat_cur[i].valid)
			continue;

		val = calculate_busypct(&proc_stat_cur[i], &proc_stat_prev[i]);
		if (busy_cpu < val)
			busy_cpu = val;
	}

	return 0;
}

int util_update(struct lpmd_config_t *lpmd_config)
{
	if (lpmd_config->util_sys_enable || lpmd_config->util_cpu_enable) {
		parse_proc_stat();
		lpmd_config->data.util_sys = busy_sys;
		lpmd_config->data.util_cpu = busy_cpu;
	}

	if (lpmd_config->util_gfx_enable) {
		parse_gfx_util();
		lpmd_config->data.util_gfx = busy_gfx;
	}

	return 0;
}
