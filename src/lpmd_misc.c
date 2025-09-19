/*
 * lpmd_misc.c: processing misc PowerManagement features
 *
 * Copyright (C) 2025 Intel Corporation. All rights reserved.
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
 */
#define _GNU_SOURCE
#include "lpmd.h"

/* ITMT Management */
#define PATH_ITMT_CONTROL "/proc/sys/kernel/sched_itmt_enabled"

static int has_itmt;
static int saved_itmt = SETTING_IGNORE;

int get_itmt(void)
{
	int val;

	if (!has_itmt)
		return -1;

	lpmd_read_int(PATH_ITMT_CONTROL, &val, -1);
	return val;
}

int itmt_init(void)
{
	if (lpmd_read_int(PATH_ITMT_CONTROL, &saved_itmt, -1))
		lpmd_log_debug("ITMT not detected\n");
	else
		has_itmt = 1;

	return 0;
}

int process_itmt(lpmd_config_state_t *state)
{
	if (!has_itmt)
		return 0;

	switch (state->itmt_state) {
		case SETTING_IGNORE:
			lpmd_log_debug("Ignore ITMT\n");
			return 0;
		case SETTING_RESTORE:
			return lpmd_write_int(PATH_ITMT_CONTROL, saved_itmt, -1);
		default:
			lpmd_log_debug ("%s ITMT\n", state->itmt_state ? "Enable" : "Disable");
			return lpmd_write_int(PATH_ITMT_CONTROL, state->itmt_state, -1);
	}
}

/* EPP/EPB Management */
#define MAX_EPP_STRING_LENGTH	32
struct cpu_info {
	char epp_str[MAX_EPP_STRING_LENGTH];
	int epp;
	int epb;
};
static struct cpu_info *saved_cpu_info;

static int get_epp(char *path, int *val, char *str, int size)
{
	FILE *filep;
	int epp;
	int ret;

	filep = fopen (path, "r");
	if (!filep)
		return 1;

	ret = fscanf (filep, "%d", &epp);
	if (ret == 1) {
		*val = epp;
		ret = 0;
		goto end;
	}

	ret = fread (str, 1, size, filep);
	if (ret <= 0)
		ret = 1;
	else {
		if (ret >= size)
			ret = size - 1;
		str[ret - 1] = '\0';
		ret = 0;
	}
end:
	fclose (filep);
	return ret;
}

static int set_epp(char *path, int val, char *str)
{
	FILE *filep;
	int ret;

	filep = fopen (path, "r+");
	if (!filep)
		return 1;

	if (val >= 0)
		ret = fprintf (filep, "%d", val);
	else if (str && str[0] != '\0')
		ret = fprintf (filep, "%s", str);
	else {
		fclose (filep);
		return 1;
	}

	fclose (filep);

	if (ret <= 0) {
		if (val >= 0)
			lpmd_log_error ("Write \"%d\" to %s failed, ret %d\n", val, path, ret);
		else
			lpmd_log_error ("Write \"%s\" to %s failed, ret %d\n", str, path, ret);
	}
	return !(ret > 0);
}

static char *get_ppd_default_epp(void)
{
	int ppd_mode = get_ppd_mode();

	if (ppd_mode == PPD_INVALID)
		return NULL;

	if (ppd_mode == PPD_PERFORMANCE)
		return "performance";

	if (ppd_mode == PPD_POWERSAVER)
		return "power";

	if (is_on_battery())
		return "balance_power";

	return "balance_performance";
}

int get_epp_epb(int *epp, char *epp_str, int size, int *epb)
{
	char path[MAX_STR_LENGTH];

	*epp = -1;
	epp_str[0] = '\0';
	/* CPU0 is always online */
	snprintf (path, sizeof(path), "/sys/devices/system/cpu/cpu%d/cpufreq/energy_performance_preference", 0);
	get_epp (path, epp, epp_str, size);

	snprintf(path, MAX_STR_LENGTH, "/sys/devices/system/cpu/cpu%d/power/energy_perf_bias", 0);
	lpmd_read_int(path, epb, -1);
	return 0;
}

int process_epp_epb(lpmd_config_state_t *state)
{
	int max_cpus = get_max_cpus ();
	int c;
	int ret;
	char path[MAX_STR_LENGTH];

	if (state->epp == SETTING_IGNORE)
		lpmd_log_info ("Ignore EPP\n");
	if (state->epb == SETTING_IGNORE)
		lpmd_log_info ("Ignore EPB\n");
	if (state->epp == SETTING_IGNORE && state->epb == SETTING_IGNORE)
		return 0;

	for (c = 0; c < max_cpus; c++) {
		int val;
		char *str = NULL;

		if (!is_cpu_online (c))
			continue;

		if (state->epp != SETTING_IGNORE) {
			if (state->epp == SETTING_RESTORE) {
				val = -1;
				str = get_ppd_default_epp();
				if (!str) {
					/* Fallback to cached EPP */
					val = saved_cpu_info[c].epp;
					str = saved_cpu_info[c].epp_str;
				}
			} else {
				val = state->epp;
			}

			snprintf(path, sizeof(path), "/sys/devices/system/cpu/cpu%d/cpufreq/energy_performance_preference", c);
			ret = set_epp (path, val, str);
			if (!ret) {
				if (val != -1)
					lpmd_log_debug ("Set CPU%d EPP to 0x%x\n", c, val);
				else
					lpmd_log_debug ("Set CPU%d EPP to %s\n", c, saved_cpu_info[c].epp_str);
			}
		}

		if (state->epb != SETTING_IGNORE) {
			if (state->epb == SETTING_RESTORE)
				val = saved_cpu_info[c].epb;
			else
				val = state->epb;

			snprintf (path, MAX_STR_LENGTH, "/sys/devices/system/cpu/cpu%d/power/energy_perf_bias", c);
			ret = lpmd_write_int(path, val, -1);
			if (!ret)
				lpmd_log_debug ("Set CPU%d EPB to 0x%x\n", c, val);
		}
	}
	return 0;
}

int epp_epb_init(void)
{
	int max_cpus = get_max_cpus ();
	int c;
	int ret;
	char path[MAX_STR_LENGTH];

	saved_cpu_info = calloc (max_cpus, sizeof(struct cpu_info));

	for (c = 0; c < max_cpus; c++) {
		saved_cpu_info[c].epp_str[0] = '\0';
		saved_cpu_info[c].epp = -1;

		if (!is_cpu_online (c))
			continue;

		snprintf (path, sizeof(path), "/sys/devices/system/cpu/cpu%d/cpufreq/energy_performance_preference", c);
		ret = get_epp (path, &saved_cpu_info[c].epp, saved_cpu_info[c].epp_str, MAX_EPP_STRING_LENGTH);
		if (!ret) {
			if (saved_cpu_info[c].epp != -1)
				lpmd_log_debug ("CPU%d EPP: 0x%x\n", c, saved_cpu_info[c].epp);
			else
				lpmd_log_debug ("CPU%d EPP: %s\n", c, saved_cpu_info[c].epp_str);
		}

		snprintf(path, MAX_STR_LENGTH, "/sys/devices/system/cpu/cpu%d/power/energy_perf_bias", c);
		ret = lpmd_read_int(path, &saved_cpu_info[c].epb, -1);
		if (ret) {
			saved_cpu_info[c].epb = -1;
			continue;
		}
		lpmd_log_debug ("CPU%d EPB: 0x%x\n", c, saved_cpu_info[c].epb);
	}
	return 0;
}
