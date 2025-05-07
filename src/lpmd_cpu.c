/*
 * lpmd_cpu.c: CPU related processing
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
 * This file contain functions to manage Linux cpuset for LP CPUs. Also using
 * power clamp in lieu of Linux cpuset. There are helper functions to format
 * cpuset strings based on the which cpuset method is used or power clamp low
 * power cpumask.
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
#include <sys/mount.h>
#include <sys/types.h>
#include <sys/time.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <errno.h>
#include <getopt.h>
#include <sched.h>
#include <dirent.h>
#include <ctype.h>
#include <signal.h>
#include <netlink/genl/genl.h>
#include <netlink/genl/family.h>
#include <netlink/genl/ctrl.h>
#include <systemd/sd-bus.h>

#include "lpmd.h"

static int topo_max_cpus;
static int max_online_cpu;
static size_t size_cpumask;

struct lpm_cpus {
	cpu_set_t *mask;
	char *name;
	char *str;
	char *str_reverse;
	char *hexstr;
	char *hexstr_reverse;
};

static struct lpm_cpus cpumasks[CPUMASK_MAX] = {
		[CPUMASK_LPM_DEFAULT] = { .name = "Low Power", },
		[CPUMASK_ONLINE] = { .name = "Online", },
		[CPUMASK_HFI] = { .name = "HFI Low Power", },
		[CPUMASK_HFI_BANNED] = { .name = "HFI BANNED", },
		[CPUMASK_HFI_LAST] = { .name = "HFI LAST", },
};

static enum cpumask_idx lpm_cpus_cur = CPUMASK_MAX;

int is_cpu_online(int cpu)
{
	if (cpu < 0 || cpu >= topo_max_cpus)
		return 0;

	if (!cpumasks[CPUMASK_ONLINE].mask)
		return 0;

	return CPU_ISSET_S(cpu, size_cpumask, cpumasks[CPUMASK_ONLINE].mask);
}

int is_cpu_for_lpm(int cpu)
{
	if (cpu < 0 || cpu >= topo_max_cpus)
		return 0;

	if (lpm_cpus_cur == CPUMASK_MAX)
		return 0;

	if (!cpumasks[lpm_cpus_cur].mask)
		return 0;

	return !!CPU_ISSET_S(cpu, size_cpumask, cpumasks[lpm_cpus_cur].mask);
}

int get_max_cpus(void)
{
	return topo_max_cpus;
}

int get_max_online_cpu(void)
{
	return max_online_cpu;
}

static size_t alloc_cpu_set(cpu_set_t **cpu_set)
{
	cpu_set_t *_cpu_set;
	size_t size;

	_cpu_set = CPU_ALLOC((topo_max_cpus + 1));
	if (_cpu_set == NULL)
		err (3, "CPU_ALLOC");
	size = CPU_ALLOC_SIZE((topo_max_cpus + 1));
	CPU_ZERO_S(size, _cpu_set);

	*cpu_set = _cpu_set;

	if (!size_cpumask)
		size_cpumask = size;

	if (size_cpumask && size_cpumask != size) {
		lpmd_log_error ("Conflict cpumask size %zu vs. %zu\n", size, size_cpumask);
		exit (-1);
	}
	return size;
}

static int cpu_migrate(int cpu)
{
	cpu_set_t *mask;
	int ret;

	alloc_cpu_set (&mask);
	CPU_SET_S(cpu, size_cpumask, mask);
	ret = sched_setaffinity(0, size_cpumask, mask);
	CPU_FREE(mask);

	if (ret == -1)
		return -1;
	else
		return 0;
}

int cpumask_to_str(cpu_set_t *mask, char *buf, int length)
{
	int i;
	int offset = 0;

	buf[0] = '\0';
	for (i = 0; i < topo_max_cpus; i++) {
		if (!CPU_ISSET_S(i, size_cpumask, mask))
			continue;
		if (length - 1 < offset) {
			lpmd_log_debug ("cpumask_to_str: Too many cpus\n");
			return 1;
		}
		offset += snprintf (buf + offset, length - 1 - offset, "%d,", i);
	}
	if (offset)
		buf[offset - 1] = '\0';
	return 0;
}

static char to_hexchar(int val)
{
	if (val <= 9)
		return val + '0';
	if (val >= 16)
		return -1;
	return val - 10 + 'a';
}

int cpumask_to_hexstr(cpu_set_t *mask, char *str, int size)
{
	int cpu;
	int i;
	int pos = 0;
	char c = 0;

	for (cpu = 0; cpu < topo_max_cpus; cpu++) {
		i = cpu % 4;

		if (!i)
			c = 0;

		if (CPU_ISSET_S(cpu, size_cpumask, mask))
			c += (1 << i);

		if (i == 3) {
			str[pos] = to_hexchar (c);
			pos++;
			if (pos >= size)
				return -1;
		}
	}
	str[pos] = '\0';

	pos--;
	for (i = 0; i <= pos / 2; i++) {
		c = str[i];
		str[i] = str[pos - i];
		str[pos - i] = c;
	}

	return 0;
}

char* get_cpus_str(enum cpumask_idx idx)
{
	if (!cpumasks[idx].mask)
		return NULL;

	if (!CPU_COUNT_S(size_cpumask, cpumasks[idx].mask))
		return NULL;

	if (cpumasks[idx].str)
		return cpumasks[idx].str;

	cpumasks[idx].str = calloc (MAX_STR_LENGTH, 1);
	if (!cpumasks[idx].str)
		err (3, "STR_ALLOC");

	cpumask_to_str (cpumasks[idx].mask, cpumasks[idx].str, MAX_STR_LENGTH);
	return cpumasks[idx].str;
}

static char* get_cpus_hexstr(enum cpumask_idx idx)
{
	if (!cpumasks[idx].mask)
		return NULL;

	if (!CPU_COUNT_S(size_cpumask, cpumasks[idx].mask))
		return NULL;

	if (cpumasks[idx].hexstr)
		return cpumasks[idx].hexstr;

	cpumasks[idx].hexstr = calloc (MAX_STR_LENGTH, 1);
	if (!cpumasks[idx].hexstr)
		err (3, "STR_ALLOC");

	cpumask_to_hexstr (cpumasks[idx].mask, cpumasks[idx].hexstr, MAX_STR_LENGTH);
	return cpumasks[idx].hexstr;
}

char* get_lpm_cpus_hexstr(void)
{
	return get_cpus_hexstr (lpm_cpus_cur);
}

static char* get_cpus_hexstr_reverse(enum cpumask_idx idx)
{
	cpu_set_t *mask;

	if (!cpumasks[idx].mask)
		return NULL;

	if (!CPU_COUNT_S(size_cpumask, cpumasks[idx].mask))
		return NULL;

	if (cpumasks[idx].hexstr_reverse)
		return cpumasks[idx].hexstr_reverse;

	cpumasks[idx].hexstr_reverse = calloc (MAX_STR_LENGTH, 1);
	if (!cpumasks[idx].hexstr_reverse)
		err (3, "STR_ALLOC");

	alloc_cpu_set (&mask);
	CPU_XOR_S(size_cpumask, mask, cpumasks[idx].mask, cpumasks[CPUMASK_ONLINE].mask);
	cpumask_to_hexstr (mask, cpumasks[idx].hexstr_reverse, MAX_STR_LENGTH);
	CPU_FREE(mask);

	return cpumasks[idx].hexstr_reverse;
}

int cpumask_to_str_reverse(cpu_set_t *mask, char *buf, int size)
{
	cpu_set_t *tmp;

	alloc_cpu_set (&tmp);
	CPU_XOR_S(size_cpumask, tmp, mask, cpumasks[CPUMASK_ONLINE].mask);
	cpumask_to_str (tmp, buf, size);
	CPU_FREE(tmp);

	return 0;
}

static char* get_cpus_str_reverse(enum cpumask_idx idx)
{
	cpu_set_t *mask;

	if (!cpumasks[idx].mask)
		return NULL;

	if (!CPU_COUNT_S(size_cpumask, cpumasks[idx].mask))
		return NULL;

	if (cpumasks[idx].str_reverse)
		return cpumasks[idx].str_reverse;

	cpumasks[idx].str_reverse = calloc (MAX_STR_LENGTH, 1);
	if (!cpumasks[idx].str_reverse)
		err (3, "STR_ALLOC");

	alloc_cpu_set (&mask);
	CPU_XOR_S(size_cpumask, mask, cpumasks[idx].mask, cpumasks[CPUMASK_ONLINE].mask);
	cpumask_to_str (mask, cpumasks[idx].str_reverse, MAX_STR_LENGTH);
	CPU_FREE(mask);

	return cpumasks[idx].str_reverse;
}

static int get_cpus_hexvals(enum cpumask_idx idx, uint8_t *vals, int size)
{
	int i, j, k;
	uint8_t v = 0;

	if (!cpumasks[idx].mask)
		return -1;

	for (i = 0; i < topo_max_cpus; i++) {
		j = i % 8;
		k = i / 8;

		if (k >= size) {
			lpmd_log_error ("size too big\n");
			return -1;
		}

		if (!CPU_ISSET_S(i, size_cpumask, cpumasks[idx].mask))
			goto set_val;

		v |= 1 << j;
set_val: if (j == 7) {
			vals[k] = v;
			v = 0;
		}
	}

	return 0;
}

int is_equal(enum cpumask_idx idx1, enum cpumask_idx idx2)
{
	if (!cpumasks[idx1].mask || !cpumasks[idx2].mask)
		return 0;

	if (CPU_EQUAL_S(size_cpumask, cpumasks[idx1].mask, cpumasks[idx2].mask))
		return 1;

	return 0;
}

int has_cpus(enum cpumask_idx idx)
{
	if (idx == CPUMASK_MAX)
		return 0;

	if (!cpumasks[idx].mask)
		return 0;

	return CPU_COUNT_S(size_cpumask, cpumasks[idx].mask);
}

int has_lpm_cpus(void)
{
	return has_cpus (lpm_cpus_cur);
}

cpu_set_t *get_cpumask(enum cpumask_idx idx)
{
	return cpumasks[idx].mask;
}

static int _add_cpu(int cpu, enum cpumask_idx idx)
{
	if (idx != CPUMASK_ONLINE && !is_cpu_online (cpu))
		return 0;

	if (!cpumasks[idx].mask)
		alloc_cpu_set (&cpumasks[idx].mask);

	CPU_SET_S(cpu, size_cpumask, cpumasks[idx].mask);

	return LPMD_SUCCESS;
}

int add_cpu(int cpu, enum cpumask_idx idx)
{
	if (cpu < 0 || cpu >= topo_max_cpus)
		return 0;

	_add_cpu (cpu, idx);

	if (idx & (CPUMASK_HFI | CPUMASK_HFI_BANNED))
		return 0;

	if (idx == CPUMASK_LPM_DEFAULT) {
		lpmd_log_info ("\tDetected %s CPU%d\n", cpumasks[idx].name, cpu);
	} else {
		if (idx < CPUMASK_MAX)
			lpmd_log_debug ("\tDetected %s CPU%d\n", cpumasks[idx].name, cpu);
		else
			lpmd_log_debug ("\tIncorrect CPU ID for CPU%d\n", cpu);
	}

	return 0;
}

void reset_cpus(enum cpumask_idx idx)
{
	if (cpumasks[idx].mask)
		CPU_ZERO_S(size_cpumask, cpumasks[idx].mask);
	free (cpumasks[idx].str);
	free (cpumasks[idx].str_reverse);
	free (cpumasks[idx].hexstr);
	free (cpumasks[idx].hexstr_reverse);
	cpumasks[idx].str = NULL;
	cpumasks[idx].str_reverse = NULL;
	cpumasks[idx].hexstr = NULL;
	cpumasks[idx].hexstr_reverse = NULL;
	lpm_cpus_cur = CPUMASK_LPM_DEFAULT;
}

void copy_cpu_mask(enum cpumask_idx source, enum cpumask_idx dest)
{
	int i;

	for (i = 0; i < topo_max_cpus; i++) {
		if (!CPU_ISSET_S(i, size_cpumask, cpumasks[source].mask))
			continue;

		_add_cpu(i, dest);
	}
}

void copy_cpu_mask_exclude(enum cpumask_idx source, enum cpumask_idx dest, enum cpumask_idx exlude)
{
	int i;

	for (i = 0; i < topo_max_cpus; i++) {
		if (!CPU_ISSET_S(i, size_cpumask, cpumasks[source].mask))
			continue;

		if (CPU_ISSET_S(i, size_cpumask, cpumasks[exlude].mask))
			continue;

		_add_cpu(i, dest);
	}
}

int set_lpm_cpus(enum cpumask_idx new)
{
	if (lpm_cpus_cur == new)
		return 0;

	lpm_cpus_cur = new;
	return 0;
}

#define BITMASK_SIZE 32
static int set_max_cpu_num(void)
{
	FILE *filep;
	unsigned long dummy;
	int i;

	topo_max_cpus = 0;
	for (i = 0; i < 256; ++i) {
		char path[MAX_STR_LENGTH];

		snprintf (path, sizeof(path), "/sys/devices/system/cpu/cpu%d/topology/thread_siblings", i);

		filep = fopen (path, "r");
		if (filep)
			break;
	}

	if (!filep) {
		lpmd_log_error ("Can't get max cpu number\n");
		return -1;
	}

	while (fscanf (filep, "%lx,", &dummy) == 1)
		topo_max_cpus += BITMASK_SIZE;
	fclose (filep);

	lpmd_log_debug ("\t%d CPUs supported in maximum\n", topo_max_cpus);
	return 0;
}

/* Handling EPP */

#define MAX_EPP_STRING_LENGTH	32
struct cpu_info {
	char epp_str[MAX_EPP_STRING_LENGTH];
	int epp;
	int epb;
};
static struct cpu_info *saved_cpu_info;

static int lp_mode_epp = SETTING_IGNORE;

int get_lpm_epp(void)
{
	return lp_mode_epp;
}

void set_lpm_epp(int val)
{
	lp_mode_epp = val;
}

static int lp_mode_epb = SETTING_IGNORE;

int get_lpm_epb(void)
{
	return lp_mode_epb;
}

void set_lpm_epb(int val)
{
	lp_mode_epb = val;
}

int get_epp(char *path, int *val, char *str, int size)
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

int set_epp(char *path, int val, char *str)
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
	int c;
	char path[MAX_STR_LENGTH];

	for (c = 0; c < max_online_cpu; c++) {
		if (!is_cpu_online (c))
			continue;

		*epp = -1;
		epp_str[0] = '\0';
		snprintf (path, sizeof(path), "/sys/devices/system/cpu/cpu%d/cpufreq/energy_performance_preference", c);
		get_epp (path, epp, epp_str, size);

		snprintf(path, MAX_STR_LENGTH, "/sys/devices/system/cpu/cpu%d/power/energy_perf_bias", c);
		lpmd_read_int(path, epb, -1);
		return 0;
	}
	return 1;
}

int init_epp_epb(void)
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

int process_epp_epb(void)
{
	int max_cpus = get_max_cpus ();
	int c;
	int ret;
	char path[MAX_STR_LENGTH];

	if (lp_mode_epp == SETTING_IGNORE)
		lpmd_log_info ("Ignore EPP\n");
	if (lp_mode_epb == SETTING_IGNORE)
		lpmd_log_info ("Ignore EPB\n");
	if (lp_mode_epp == SETTING_IGNORE && lp_mode_epb == SETTING_IGNORE)
		return 0;

	for (c = 0; c < max_cpus; c++) {
		int val;
		char *str = NULL;

		if (!is_cpu_online (c))
			continue;

		if (lp_mode_epp != SETTING_IGNORE) {
			if (lp_mode_epp == SETTING_RESTORE) {
				val = -1;
				str = get_ppd_default_epp();
				if (!str) {
					/* Fallback to cached EPP */
					val = saved_cpu_info[c].epp;
					str = saved_cpu_info[c].epp_str;
				}
			} else {
				val = lp_mode_epp;
			}

			snprintf (path, sizeof(path), "/sys/devices/system/cpu/cpu%d/cpufreq/energy_performance_preference", c);
			ret = set_epp (path, val, str);
			if (!ret) {
				if (val != -1)
					lpmd_log_debug ("Set CPU%d EPP to 0x%x\n", c, val);
				else
					lpmd_log_debug ("Set CPU%d EPP to %s\n", c, saved_cpu_info[c].epp_str);
			}
		}

		if (lp_mode_epb != SETTING_IGNORE) {
			if (lp_mode_epb == SETTING_RESTORE)
				val = saved_cpu_info[c].epb;
			else
				val = lp_mode_epb;

			snprintf (path, MAX_STR_LENGTH, "/sys/devices/system/cpu/cpu%d/power/energy_perf_bias", c);
			ret = lpmd_write_int(path, val, -1);
			if (!ret)
				lpmd_log_debug ("Set CPU%d EPB to 0x%x\n", c, val);
		}
	}
	return 0;
}

static int uevent_fd = -1;

int uevent_init(void)
{
	struct sockaddr_nl nls;

	memset (&nls, 0, sizeof(struct sockaddr_nl));

	nls.nl_family = AF_NETLINK;
	nls.nl_pid = getpid();
	nls.nl_groups = -1;

	uevent_fd = socket (PF_NETLINK, SOCK_DGRAM, NETLINK_KOBJECT_UEVENT);
	if (uevent_fd < 0)
		return uevent_fd;

	if (bind (uevent_fd, (struct sockaddr*) &nls, sizeof(struct sockaddr_nl))) {
		lpmd_log_warn ("kob_uevent bind failed\n");
		close (uevent_fd);
		return -1;
	}

	lpmd_log_debug ("Uevent binded\n");
	return uevent_fd;
}

static int has_cpu_uevent(void)
{
	ssize_t i = 0;
	ssize_t len;
	const char *dev_path = "DEVPATH=";
	unsigned int dev_path_len = strlen(dev_path);
	const char *cpu_path = "/devices/system/cpu/cpu";
	char buffer[MAX_STR_LENGTH];

	len = recv (uevent_fd, buffer, sizeof(buffer) - 1, MSG_DONTWAIT);
	if (len <= 0)
		return 0;
	buffer[len] = '\0';

	lpmd_log_debug ("Receive uevent: %s\n", buffer);

	while (i < len) {
		if (strlen (buffer + i) > dev_path_len
				&& !strncmp (buffer + i, dev_path, dev_path_len)) {
			if (!strncmp (buffer + i + dev_path_len, cpu_path,
					strlen (cpu_path))) {
				lpmd_log_debug ("\tMatches: %s\n", buffer + i + dev_path_len);
				return 1;
			}
		}
		i += strlen (buffer + i) + 1;
	}

	return 0;
}

#define PATH_PROC_STAT "/proc/stat"

int check_cpu_hotplug(void)
{
	FILE *filep;
	static cpu_set_t *curr;
	static cpu_set_t *prev;
	cpu_set_t *tmp;

	if (!has_cpu_uevent ())
		return 0;

	if (!curr) {
		alloc_cpu_set (&curr);
		alloc_cpu_set (&prev);
		CPU_OR_S (size_cpumask, curr, cpumasks[CPUMASK_ONLINE].mask, cpumasks[CPUMASK_ONLINE].mask);
	}

	tmp = prev;
	prev = curr;
	curr = tmp;
	CPU_ZERO_S (size_cpumask, curr);

	filep = fopen (PATH_PROC_STAT, "r");
	if (!filep)
		return 0;

	while (!feof (filep)) {
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

		ret = sscanf (p, "cpu%d", &cpu);
		if (ret != 1)
			goto free;

		CPU_SET_S (cpu, size_cpumask, curr);

free:
		free (tmpline);
		free (line);
	}

	fclose (filep);

	/* CPU Hotplug detected, should freeze lpmd */
	if (!CPU_EQUAL_S (size_cpumask, curr, cpumasks[CPUMASK_ONLINE].mask)) {
		lpmd_log_debug ("check_cpu_hotplug: CPU Hotplug detected, freeze lpmd\n");
		return freeze_lpm ();
	}

	/* CPU restored to original state, should restore lpmd */
	if (CPU_EQUAL_S (size_cpumask, curr, cpumasks[CPUMASK_ONLINE].mask) &&
	    !CPU_EQUAL_S (size_cpumask, curr, prev)) {
		lpmd_log_debug ("check_cpu_hotplug: CPU Hotplug restored, restore lpmd\n");
		return restore_lpm ();
	}

	/* No update since last change */
	return 0;
}

/* Bit 15 of CPUID.7 EDX stands for Hybrid support */
#define CPUFEATURE_HYBRID	(1 << 15)
#define PATH_PM_PROFILE "/sys/firmware/acpi/pm_profile"

struct cpu_model_entry {
	unsigned int family;
	unsigned int model;
};

static struct cpu_model_entry id_table[] = {
		{ 6, 0x97 }, // Alderlake
		{ 6, 0x9a }, // Alderlake
		{ 6, 0xb7 }, // Raptorlake
		{ 6, 0xba }, // Raptorlake
		{ 6, 0xbf }, // Raptorlake S
		{ 6, 0xaa }, // Meteorlake
		{ 6, 0xac }, // Meteorlake
		{ 6, 0xbd }, // Lunarlake
		{ 6, 0xcc }, // Pantherlake
		{ 0, 0 } // Last Invalid entry
};

static int detect_supported_cpu(lpmd_config_t *lpmd_config)
{
	unsigned int eax, ebx, ecx, edx;
	unsigned int max_level, family, model, stepping;
	int val;

	cpuid(0, eax, ebx, ecx, edx);

	/* Unsupported vendor */
        if (ebx != 0x756e6547 || edx != 0x49656e69 || ecx != 0x6c65746e) {
		lpmd_log_info("Unsupported vendor\n");
		return -1;
	}

	max_level = eax;
	cpuid(1, eax, ebx, ecx, edx);
	family = (eax >> 8) & 0xf;
	model = (eax >> 4) & 0xf;
	stepping = eax & 0xf;

	if (family == 6)
		model += ((eax >> 16) & 0xf) << 4;

	lpmd_log_info("%u CPUID levels; family:model:stepping 0x%x:%x:%x (%u:%u:%u)\n",
			max_level, family, model, stepping, family, model, stepping);

	if (!do_platform_check()) {
		lpmd_log_info("Ignore platform check\n");
		goto end;
	}

	/* Need CPUID.1A to detect CPU core type */
        if (max_level < 0x1a) {
		lpmd_log_info("CPUID leaf 0x1a not supported, unable to detect CPU type\n");
		return -1;
        }

	cpuid_count(7, 0, eax, ebx, ecx, edx);

	/* Run on Hybrid platforms only */
	if (!(edx & CPUFEATURE_HYBRID)) {
		lpmd_log_info("Non-Hybrid platform detected\n");
		return -1;
	}

	/* /sys/firmware/acpi/pm_profile is mandatory */
	if (lpmd_read_int(PATH_PM_PROFILE, &val, -1)) {
		lpmd_log_info("Failed to read PM profile %s\n", PATH_PM_PROFILE);
		return -1;
	}

	if (val != 2) {
		lpmd_log_info("Non-Mobile PM profile detected. %s returns %d\n", PATH_PM_PROFILE, val);
		return -1;
	}

	/* Platform meets all the criteria for lpmd to run, check the allow list */
	val = 0;
	while (id_table[val].family) {
		if (id_table[val].family == family && id_table[val].model == model)
			break;
		val++;
        }

	/* Unsupported model */
	if (!id_table[val].family) {
		lpmd_log_info("Platform not supported yet.\n");
		lpmd_log_debug("Supported platforms:\n");
		val = 0;
		while (id_table[val].family) {
			lpmd_log_debug("\tfamily %d model %d\n", id_table[val].family, id_table[val].model);
			val++;
		}
		return -1;
	}

end:
	lpmd_config->cpu_family = family;
	lpmd_config->cpu_model = model;

	return 0;
}

/* Run intel_lpmd on the LP-Mode CPUs only */
static void lpmd_set_cpu_affinity(void)
{
	if (!cpumasks[CPUMASK_LPM_DEFAULT].mask)
		return;
	if (!CPU_COUNT_S (size_cpumask, cpumasks[CPUMASK_LPM_DEFAULT].mask))
		return;
	if (!sched_setaffinity (0, size_cpumask, cpumasks[CPUMASK_LPM_DEFAULT].mask))
		lpmd_log_info ("\tSet intel_lpmd cpu affinity to CPU %s\n", get_cpus_str (CPUMASK_LPM_DEFAULT));
	else
		lpmd_log_warn ("\tFailed to set intel_lpmd cpu affinity\n");
}

/*
 * Detect LPM cpus
 * parse cpuset with following syntax
 * 1,2,4..6,8-10 and set bits in cpu_subset
 */
int parse_cpu_str(char *buf, enum cpumask_idx idx)
{
	unsigned int start, end;
	char *next;
	int nr_cpus = 0;

	if (buf[0] == '\0')
		return 0;

	next = buf;

	while (next && *next) {
		if (*next == '\n')
			*next = '\0';
		next++;
	}
	next = buf;

	while (next && *next) {
		if (*next == '\n')
			*next = '\0';

		if (*next == '-') /* no negative cpu numbers */
			goto error;

		start = strtoul (next, &next, 10);

		_add_cpu (start, idx);
		nr_cpus++;

		if (*next == '\0')
			break;

		if (*next == ',') {
			next += 1;
			continue;
		}

		if (*next == '-') {
			next += 1; /* start range */
		}
		else if (*next == '.') {
			next += 1;
			if (*next == '.')
				next += 1; /* start range */
			else
				goto error;
		}

		end = strtoul (next, &next, 10);
		if (end <= start)
			goto error;

		while (++start <= end) {
			_add_cpu (start, idx);
			nr_cpus++;
		}

		if (*next == ',')
			next += 1;
		else if (*next != '\0')
			goto error;
	}

	return nr_cpus;
error:
	lpmd_log_error ("CPU string malformed: %s\n", buf);
	return -1;
}

static int detect_lpm_cpus_cmd(char *cmd)
{
	int ret;

	ret = parse_cpu_str (cmd, CPUMASK_LPM_DEFAULT);
	if (ret <= 0)
		reset_cpus (CPUMASK_LPM_DEFAULT);

	return ret;
}

/*
 * Use one Ecore Module as LPM CPUs.
 * Applies on Hybrid platforms like AlderLake/RaptorLake.
 */
static int is_cpu_atom(int cpu)
{
	unsigned int eax, ebx, ecx, edx;
	unsigned int type;

	if (cpu_migrate(cpu) < 0) {
		lpmd_log_error("Failed to migrated to cpu%d\n", cpu);
		return -1;
	}

	cpuid(0x1a, eax, ebx, ecx, edx);

	type = (eax >> 24) & 0xFF;

	return type == 0x20;
}

static int is_cpu_in_l3(int cpu)
{
	unsigned int eax, ebx, ecx, edx, subleaf;

	if (cpu_migrate(cpu) < 0) {
		lpmd_log_error("Failed to migrated to cpu%d\n", cpu);
		err (1, "cpu migrate");
	}

	for(subleaf = 0;; subleaf++) {
		unsigned int type, level;

		cpuid_count(4, subleaf, eax, ebx, ecx, edx);

		type = eax & 0x1f;
		level = (eax >> 5) & 0x7;

		/* No more caches */
		if (!type)
			break;
		/* Unified Cache */
		if (type !=3 )
			continue;
		/* L3 */
		if (level != 3)
			continue;

		return 1;
	}
	return 0;
}

int is_cpu_pcore(int cpu)
{
	return !is_cpu_atom(cpu);
}

int is_cpu_ecore(int cpu)
{
	if (!is_cpu_atom(cpu))
		return 0;
	return is_cpu_in_l3(cpu);
}

int is_cpu_lcore(int cpu)
{
	if (!is_cpu_atom(cpu))
		return 0;
	return !is_cpu_in_l3(cpu);
}

static int detect_lpm_cpus_cluster(void)
{
	FILE *filep;
	char path[MAX_STR_LENGTH];
	char str[MAX_STR_LENGTH];
	int i, ret;

	for (i = topo_max_cpus; i >= 0; i--) {
		if (!is_cpu_online (i))
			continue;

		snprintf (path, sizeof(path), "/sys/devices/system/cpu/cpu%d/topology/cluster_cpus_list",
					i);
		path[MAX_STR_LENGTH - 1] = '\0';

		filep = fopen (path, "r");
		if (!filep)
			continue;

		ret = fread (str, 1, MAX_STR_LENGTH - 1, filep);
		fclose (filep);

		if (ret <= 0)
			continue;

		str[ret] = '\0';

		if (parse_cpu_str (str, CPUMASK_LPM_DEFAULT) <= 0)
			continue;

		/* An Ecore module contains 4 Atom cores */
		if (CPU_COUNT_S(size_cpumask, cpumasks[CPUMASK_LPM_DEFAULT].mask) == 4 && is_cpu_atom(i))
			break;

		reset_cpus (CPUMASK_LPM_DEFAULT);
	}

	if (!has_cpus (CPUMASK_LPM_DEFAULT)) {
		reset_cpus (CPUMASK_LPM_DEFAULT);
		return 0;
	}

	return CPU_COUNT_S(size_cpumask, cpumasks[CPUMASK_LPM_DEFAULT].mask);
}

static int detect_cpu_lcore(int cpu)
{
	if (is_cpu_lcore(cpu))
		_add_cpu (cpu, CPUMASK_LPM_DEFAULT);
	return 0;
}

/*
 * Use Lcore CPUs as LPM CPUs.
 * Applies on platforms like MeteorLake.
 */
static int detect_lpm_cpus_lcore(void)
{
	int i;

	for (i = 0; i < topo_max_cpus; i++) {
		if (!is_cpu_online (i))
			continue;
		if (detect_cpu_lcore(i) < 0)
			return -1;
	}

	/* All cpus has L3 */
	if (!has_cpus (CPUMASK_LPM_DEFAULT))
		return 0;

	/* All online cpus don't have L3 */
	if (CPU_EQUAL_S(size_cpumask, cpumasks[CPUMASK_LPM_DEFAULT].mask,
					cpumasks[CPUMASK_ONLINE].mask))
		goto err;

	return CPU_COUNT_S(size_cpumask, cpumasks[CPUMASK_LPM_DEFAULT].mask);

err: reset_cpus (CPUMASK_LPM_DEFAULT);
	return 0;
}

static int detect_lpm_cpus(char *cmd_cpus)
{
	int ret;
	char *str;

	if (cmd_cpus && cmd_cpus[0] != '\0') {
		ret = detect_lpm_cpus_cmd (cmd_cpus);
		if (ret <= 0) {
			lpmd_log_error ("\tInvalid -c parameter: %s\n", cmd_cpus);
			exit (-1);
		}
		str = "CommandLine";
		goto end;
	}

	ret = detect_lpm_cpus_lcore ();
	if (ret < 0)
		return ret;

	if (ret > 0) {
		str = "Lcores";
		goto end;
	}

	if (detect_lpm_cpus_cluster ()) {
		str = "Ecores";
		goto end;
	}

	if (has_hfi_lpm_monitor ()) {
		lpmd_log_info (
				"\tNo valid Low Power CPUs detected, use dynamic Low Power CPUs from HFI hints\n");
		return 0;
	}
	else {
		lpmd_log_error ("\tNo valid Low Power CPUs detected, exit\n");
		exit (1);
	}

end: if (has_cpus (CPUMASK_LPM_DEFAULT))
		lpmd_log_info ("\tUse CPU %s as Default Low Power CPUs (%s)\n",
						get_cpus_str (CPUMASK_LPM_DEFAULT), str);

	lpmd_set_cpu_affinity ();
	return 0;
}

static int check_cpu_offline_support(void)
{
	return lpmd_open ("/sys/devices/system/cpu/cpu0/online", 1);
}

static int online_cpu(int cpu, int val)
{
	char path[MAX_STR_LENGTH];

	snprintf (path, sizeof(path), "/sys/devices/system/cpu/cpu%d/online", cpu);

	return lpmd_write_int (path, val, LPMD_LOG_INFO);
}

static int process_cpu_offline(int enter)
{
	int cpu;

	lpmd_log_info ("\t%s CPUs\n", enter ? "Offline" : "Online");
	for (cpu = 0; cpu < topo_max_cpus; cpu++) {
		if (!is_cpu_online (cpu))
			continue;
		if (!is_cpu_for_lpm (cpu)) {
			if (enter)
				online_cpu (cpu, 0);
			else
				online_cpu (cpu, 1);
		}
		else {
			online_cpu (cpu, 1);
		}
	}

	return 0;
}

/* Support for LPM_CPU_CGROUPV2 */
#define PATH_CGROUP                    "/sys/fs/cgroup"
#define PATH_CG2_SUBTREE_CONTROL	PATH_CGROUP "/cgroup.subtree_control"

static int update_allowed_cpus(const char *unit, uint8_t *vals, int size)
{
	sd_bus_error error = SD_BUS_ERROR_NULL;
	sd_bus_message *m = NULL;
	sd_bus *bus = NULL;
	char buf[MAX_STR_LENGTH];
	int offset;
	int ret;
	int i;

//	 creates a new, independent bus connection to the system bus
	ret = sd_bus_open_system (&bus);
	if (ret < 0) {
		fprintf (stderr, "Failed to connect to system bus: %s\n", strerror (-ret));
		goto finish;
	}

	/*
	 * creates a new bus message object that encapsulates a D-Bus method call,
	 * and returns it in the m output parameter.
	 * The call will be made on the destination, path, on the interface, member.
	 */
	/* Issue the method call and store the response message in m */
	ret = sd_bus_message_new_method_call (bus, &m, "org.freedesktop.systemd1",
											"/org/freedesktop/systemd1",
											"org.freedesktop.systemd1.Manager",
											"SetUnitProperties");
	if (ret < 0) {
		fprintf (stderr, "Failed to issue method call: %s\n", error.message);
		goto finish;
	}


//	Attach fields to a D-Bus message based on a type string
	ret = sd_bus_message_append (m, "sb", unit, 1);
	if (ret < 0) {
		fprintf (stderr, "Failed to append unit: %s\n", error.message);
		goto finish;
	}

	/*
	 * appends a new container to the message m.
	 * After opening a new container, it can be filled with content using
	 * sd_bus_message_append(3) and similar functions.
	 * Containers behave like a stack. To nest containers inside each other,
	 * call sd_bus_message_open_container() multiple times without calling
	 * sd_bus_message_close_container() in between. Each container will be
	 * nested inside the previous container.
	 * Instead of literals, the corresponding constants SD_BUS_TYPE_STRUCT,
	 * SD_BUS_TYPE_ARRAY, SD_BUS_TYPE_VARIANT or SD_BUS_TYPE_DICT_ENTRY can also be used.
	 */
	ret = sd_bus_message_open_container (m, SD_BUS_TYPE_ARRAY, "(sv)");
	if (ret < 0) {
		fprintf (stderr, "Failed to append array: %s\n", error.message);
		goto finish;
	}

	ret = sd_bus_message_open_container (m, SD_BUS_TYPE_STRUCT, "sv");
	if (ret < 0) {
		fprintf (stderr, "Failed to open container struct: %s\n", error.message);
		goto finish;
	}

	/*
	 * appends a single field to the message m.
	 * The parameter type determines how the pointer p is interpreted.
	 */
	ret = sd_bus_message_append_basic (m, SD_BUS_TYPE_STRING, "AllowedCPUs");
	if (ret < 0) {
		fprintf (stderr, "Failed to append string: %s\n", error.message);
		goto finish_1;
	}

	ret = sd_bus_message_open_container (m, 'v', "ay");
	if (ret < 0) {
		fprintf (stderr, "Failed to open container: %s\n", error.message);
		goto finish_2;
	}

	/*
	 * appends an array to a D-Bus message m. A container will be opened,
	 * the array contents appended, and the container closed.
	 */
	ret = sd_bus_message_append_array (m, 'y', vals, size);
	if (ret < 0) {
		fprintf (stderr, "Failed to append allowed_cpus: %s\n", error.message);
		goto finish_2;
	}

	offset = snprintf (buf, MAX_STR_LENGTH, "\tSending Dbus message to systemd: %s: ", unit);
	for (i = 0; i < size; i++) {
		if (offset < MAX_STR_LENGTH)
			offset += snprintf (buf + offset, MAX_STR_LENGTH - offset, "0x%02x ", vals[i]);
	}
	buf[MAX_STR_LENGTH - 1] = '\0';
	lpmd_log_info ("%s\n", buf);

	sd_bus_message_close_container (m);

finish_2: sd_bus_message_close_container (m);

finish_1: sd_bus_message_close_container (m);

finish: if (ret >= 0) {
		ret = sd_bus_call (bus, m, 0, &error, NULL);
		if (ret < 0) {
			fprintf (stderr, "Failed to call: %s\n", error.message);
		}
	}

	sd_bus_error_free (&error);
	sd_bus_message_unref (m);
	sd_bus_unref (bus);

	return ret < 0 ? -1 : 0;
}

static int restore_systemd_cgroup()
{
	int size = topo_max_cpus / 8;
	uint8_t *vals;

	vals = calloc (size, 1);
	if (!vals)
		return -1;
	get_cpus_hexvals (CPUMASK_ONLINE, vals, size);

	update_allowed_cpus ("system.slice", vals, size);
	update_allowed_cpus ("user.slice", vals, size);
	update_allowed_cpus ("machine.slice", vals, size);
	free (vals);
	return 0;
}

static int update_systemd_cgroup()
{
	int size = topo_max_cpus / 8;
	uint8_t *vals;
	int ret;

	vals = calloc (size, 1);
	if (!vals)
		return -1;
	get_cpus_hexvals (lpm_cpus_cur, vals, size);

	ret = update_allowed_cpus ("system.slice", vals, size);
	if (ret)
		goto restore;

	ret = update_allowed_cpus ("user.slice", vals, size);
	if (ret)
		goto restore;

	ret = update_allowed_cpus ("machine.slice", vals, size);
	if (ret)
		goto restore;

	free (vals);
	return 0;

restore: free (vals);
	restore_systemd_cgroup ();
	return ret;
}

static int check_cpu_cgroupv2_support(void)
{
	if (lpmd_write_str (PATH_CG2_SUBTREE_CONTROL, "+cpuset", LPMD_LOG_DEBUG))
		return 1;

	return 0;
}

static int process_cpu_cgroupv2_enter(void)
{
	if (lpmd_write_str (PATH_CG2_SUBTREE_CONTROL, "+cpuset", LPMD_LOG_DEBUG))
		return 1;

	return update_systemd_cgroup ();
}

static int process_cpu_cgroupv2_exit(void)
{
	restore_systemd_cgroup ();

	return lpmd_write_str (PATH_CG2_SUBTREE_CONTROL, "-cpuset", LPMD_LOG_DEBUG);
}

static int process_cpu_cgroupv2(int enter)
{
	if (enter)
		return process_cpu_cgroupv2_enter ();
	else
		return process_cpu_cgroupv2_exit ();
}

/*
 * Support for LPM_CPU_POWERCLAMP:
 * /sys/module/intel_powerclamp/parameters/cpumask
 * /sys/module/intel_powerclamp/parameters/max_idle
 */
#define PATH_CPUMASK "/sys/module/intel_powerclamp/parameters/cpumask"
#define PATH_MAXIDLE "/sys/module/intel_powerclamp/parameters/max_idle"
#define PATH_DURATION "/sys/module/intel_powerclamp/parameters/duration"
#define PATH_THERMAL "/sys/class/thermal"

static char path_powerclamp[MAX_STR_LENGTH * 2];

static int check_cpu_powerclamp_support(void)
{
	FILE *filep;
	DIR *dir;
	struct dirent *entry;
	char *name = "intel_powerclamp";
	char str[20];
	int ret;

	if (lpmd_open (PATH_CPUMASK, 0))
		return 1;

	if ((dir = opendir (PATH_THERMAL)) == NULL) {
		perror ("opendir() error");
		return 1;
	}

	while ((entry = readdir (dir)) != NULL) {
		if (strlen (entry->d_name) > 100)
			continue;
		snprintf (path_powerclamp, MAX_STR_LENGTH * 2, "%s/%s/type", PATH_THERMAL, entry->d_name);
		filep = fopen (path_powerclamp, "r");
		if (!filep)
			continue;

		ret = fread (str, strlen (name), 1, filep);
		fclose (filep);

		if (ret != 1)
			continue;

		if (!strncmp (str, name, strlen (name))) {
			snprintf (path_powerclamp, MAX_STR_LENGTH * 2, "%s/%s/cur_state", PATH_THERMAL,
						entry->d_name);
			lpmd_log_info ("\tFound %s device at %s/%s\n", name, PATH_THERMAL, entry->d_name);
			break;
		}
	}
	closedir (dir);

	if (path_powerclamp[0] == '\0')
		return 1;

	return 0;
}

static int default_dur = -1;

static int _process_cpu_powerclamp_enter(char *cpumask_str, int pct, int dur)
{
	if (lpmd_write_str (PATH_CPUMASK, cpumask_str, LPMD_LOG_DEBUG))
		return 1;

	if (dur > 0) {
		if (lpmd_read_int (PATH_DURATION, &default_dur, LPMD_LOG_DEBUG))
			return 1;

		if (lpmd_write_int (PATH_DURATION, dur, LPMD_LOG_DEBUG))
			return 1;
	}

	if (lpmd_write_int (PATH_MAXIDLE, pct, LPMD_LOG_DEBUG))
		return 1;

	if (lpmd_write_int (path_powerclamp, pct, LPMD_LOG_DEBUG))
		return 1;

	return 0;
}

static int process_cpu_powerclamp_enter(void)
{
	int pct = get_idle_percentage ();
	int dur = get_idle_duration ();

	return _process_cpu_powerclamp_enter (get_cpus_hexstr_reverse (lpm_cpus_cur), pct, dur);
}

static int process_cpu_powerclamp_exit()
{
	if (lpmd_write_int (PATH_DURATION, default_dur, LPMD_LOG_DEBUG))
		return 1;

	return lpmd_write_int (path_powerclamp, 0, LPMD_LOG_DEBUG);
}

static int process_cpu_powerclamp(int enter)
{
	if (enter)
		return process_cpu_powerclamp_enter ();
	else
		return process_cpu_powerclamp_exit ();
}

static int __process_cpu_isolate_exit(char *name)
{
	char path[MAX_STR_LENGTH];
	DIR *dir;

	snprintf(path, MAX_STR_LENGTH, "/sys/fs/cgroup/%s", name);
	dir = opendir(path);
	if (!dir)
		return 1;

	closedir(dir);

	snprintf(path, MAX_STR_LENGTH, "/sys/fs/cgroup/%s/cpuset.cpus.partition", name);
	if (lpmd_write_str (path, "member", LPMD_LOG_DEBUG))
		return 1;

	if (!get_cpus_str (CPUMASK_ONLINE))
		return 0;

	snprintf(path, MAX_STR_LENGTH, "/sys/fs/cgroup/%s/cpuset.cpus", name);
	if (lpmd_write_str (path, get_cpus_str (CPUMASK_ONLINE),
						LPMD_LOG_DEBUG))
		return 1;

	return 0;
}

static int check_cpu_isolate_support(void)
{
	return check_cpu_cgroupv2_support ();
}

static int process_cpu_isolate_enter(void)
{
	DIR *dir;
	int ret;

	dir = opendir ("/sys/fs/cgroup/lpm");
	if (!dir) {
		ret = mkdir ("/sys/fs/cgroup/lpm", 0744);
		if (ret) {
			printf ("Can't create dir:%s errno:%d\n", "/sys/fs/cgroup/lpm", errno);
			return ret;
		}
		lpmd_log_info ("\tCreate %s\n", "/sys/fs/cgroup/lpm");
	} else {
		closedir (dir);
	}

	if (lpmd_write_str ("/sys/fs/cgroup/lpm/cpuset.cpus.partition", "member", LPMD_LOG_DEBUG))
		return 1;

	if (!CPU_EQUAL_S(size_cpumask, cpumasks[lpm_cpus_cur].mask, cpumasks[CPUMASK_ONLINE].mask)) {
		if (lpmd_write_str ("/sys/fs/cgroup/lpm/cpuset.cpus", get_cpus_str_reverse (lpm_cpus_cur),
						LPMD_LOG_DEBUG))
			return 1;

		if (lpmd_write_str ("/sys/fs/cgroup/lpm/cpuset.cpus.partition", "isolated", LPMD_LOG_DEBUG))
			return 1;
	} else {
		if (lpmd_write_str ("/sys/fs/cgroup/lpm/cpuset.cpus", get_cpus_str (CPUMASK_ONLINE),
						LPMD_LOG_DEBUG))
			return 1;
	}

	return 0;
}

static int process_cpu_isolate_exit(void)
{
	return __process_cpu_isolate_exit("lpm");
}

static int process_cpu_isolate(int enter)
{
	if (enter)
		return process_cpu_isolate_enter ();
	else
		return process_cpu_isolate_exit ();
}

static int check_cpu_mode_support(enum lpm_cpu_process_mode mode)
{
	int ret;

	switch (mode) {
		case LPM_CPU_OFFLINE:
			ret = check_cpu_offline_support ();
			break;
		case LPM_CPU_CGROUPV2:
			ret = check_cpu_cgroupv2_support ();
			break;
		case LPM_CPU_POWERCLAMP:
			ret = check_cpu_powerclamp_support ();
			break;
		case LPM_CPU_ISOLATE:
			ret = check_cpu_isolate_support ();
			break;
		default:
			lpmd_log_error ("Invalid CPU process mode %d\n", mode);
			exit (-1);
	}
	if (ret) {
		lpmd_log_error ("Mode %d not supported\n", mode);
		return ret;
	}

	return ret;
}

#define PATH_RAPL	"/sys/class/powercap"
static int get_tdp(void)
{
	FILE *filep;
	DIR *dir;
	struct dirent *entry;
	int ret;
	char path[MAX_STR_LENGTH * 2];
	char str[MAX_STR_LENGTH];
	char *pos;
	int tdp = 0;


	if ((dir = opendir (PATH_RAPL)) == NULL) {
		perror ("opendir() error");
		return 1;
	}

	while ((entry = readdir (dir)) != NULL) {
		if (strlen (entry->d_name) > 100)
			continue;

		if (strncmp(entry->d_name, "intel-rapl", strlen("intel-rapl")))
			continue;

		snprintf (path, MAX_STR_LENGTH * 2, "%s/%s/name", PATH_RAPL, entry->d_name);
		filep = fopen (path, "r");
		if (!filep)
			continue;

		ret = fread (str, 1, MAX_STR_LENGTH, filep);
		fclose (filep);

		if (ret <= 0)
			continue;

		if (strncmp(str, "package", strlen("package")))
			continue;

		snprintf (path, MAX_STR_LENGTH * 2, "%s/%s/constraint_0_max_power_uw", PATH_RAPL, entry->d_name);
		filep = fopen (path, "r");
		if (!filep)
			continue;

		ret = fread (str, 1, MAX_STR_LENGTH, filep);
		fclose (filep);

		if (ret <= 0)
			continue;

		if (ret >= MAX_STR_LENGTH)
			ret = MAX_STR_LENGTH - 1;

		str[ret] = '\0';
		tdp = strtol(str, &pos, 10);
		break;
	}
	closedir (dir);

	return tdp / 1000000;
}

static void cpu_cleanup(void)
{
	/* leanup previous cgroup setting in case service quits unexpectedly last time */
	__process_cpu_isolate_exit("lpm");
}

int check_cpu_capability(lpmd_config_t *lpmd_config)
{
	FILE *filep;
	int i;
	char path[MAX_STR_LENGTH];
	int ret;
	int pcores, ecores, lcores;
	int tdp;

	/* Must be called before migrating any tasks */
	cpu_cleanup();

	ret = detect_supported_cpu(lpmd_config);
	if (ret) {
		lpmd_log_info("Unsupported CPU type\n");
		return ret;
	}

	ret = set_max_cpu_num ();
	if (ret)
		return ret;

	reset_cpus (CPUMASK_ONLINE);
	pcores = ecores = lcores = 0;

	for (i = 0; i < topo_max_cpus; i++) {
		unsigned int online = 0;

		snprintf (path, sizeof(path), "/sys/devices/system/cpu/cpu%d/online", i);
		filep = fopen (path, "r");
		if (filep) {
			if (fscanf (filep, "%u", &online) != 1)
				lpmd_log_warn ("fread failed for %s\n", path);
			fclose (filep);
		}
		else if (!i)
			online = 1;
		else
			break;

		if (!online)
			continue;

		add_cpu (i, CPUMASK_ONLINE);
		if (is_cpu_pcore(i))
			pcores++;
		else if (is_cpu_ecore(i))
			ecores++;
		else if (is_cpu_lcore(i))
			lcores++;
	}
	max_online_cpu = i;

	tdp = get_tdp();
	lpmd_log_info("Detected %d Pcores, %d Ecores, %d Lcores, TDP %dW\n", pcores, ecores, lcores, tdp);
	ret = snprintf(lpmd_config->cpu_config, MAX_CONFIG_LEN - 1, " %dP%dE%dL-%dW ", pcores, ecores, lcores, tdp);

	lpmd_config->tdp = tdp;

	return 0;
}

int init_cpu(char *cmd_cpus, enum lpm_cpu_process_mode mode, int epp)
{
	int ret;

	ret = detect_lpm_cpus (cmd_cpus);
	if (ret)
		return ret;

	ret = check_cpu_mode_support (mode);
	if (ret)
		return ret;

	init_epp_epb ();
	return 0;
}

int process_cpus(int enter, enum lpm_cpu_process_mode mode)
{
	int ret;

	if (enter != 1 && enter != 0)
		return LPMD_ERROR;

	process_epp_epb ();

	if (lpm_cpus_cur == CPUMASK_MAX) {
		lpmd_log_info ("Ignore Task migration\n");
		return 0;
	}

	lpmd_log_info ("Process CPUs ...\n");
	switch (mode) {
		case LPM_CPU_OFFLINE:
			ret = process_cpu_offline (enter);
			break;
		case LPM_CPU_CGROUPV2:
			ret = process_cpu_cgroupv2 (enter);
			break;
		case LPM_CPU_POWERCLAMP:
			ret = process_cpu_powerclamp (enter);
			break;
		case LPM_CPU_ISOLATE:
			ret = process_cpu_isolate (enter);
			break;
		default:
			exit (-1);
	}
	return ret;
}
