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
	uint8_t *hexvals;
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

int alloc_cpumask(void)
{
	int idx;

	for (idx = CPUMASK_USER; idx < CPUMASK_MAX; idx++) {
		if (!cpumasks[idx].mask) {
			alloc_cpu_set(&cpumasks[idx].mask);
			break;
		}
	}
	return idx;
}

int free_cpumask(enum cpumask_idx idx)
{
	if (!cpumasks[idx].mask)
		return 0;
		
	reset_cpus(idx);
	free(cpumasks[idx].mask);
	cpumasks[idx].mask = NULL;
	return 0;
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

static int cpumask_to_str(cpu_set_t *mask, char *buf, int length)
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

static uint8_t *get_cpus_hexvals(enum cpumask_idx idx, int *size)
{
	int i, j, k;
	uint8_t v = 0;
	uint8_t *vals;

	if (!cpumasks[idx].mask)
		return NULL;

	if (!CPU_COUNT_S(size_cpumask, cpumasks[idx].mask))
		return NULL;

	*size = topo_max_cpus / 8;

	if (cpumasks[idx].hexvals)
		return cpumasks[idx].hexvals;

	vals = calloc (*size, 1);
	if (!vals)
		return NULL;

	for (i = 0; i < topo_max_cpus; i++) {
		j = i % 8;
		k = i / 8;

		if (k >= *size) {
			lpmd_log_error ("size too big\n");
			free(vals);
			return NULL;
		}

		if (!CPU_ISSET_S(i, size_cpumask, cpumasks[idx].mask))
			goto set_val;

		v |= 1 << j;
set_val: if (j == 7) {
			vals[k] = v;
			v = 0;
		}
	}

	cpumasks[idx].hexvals = vals;
	return cpumasks[idx].hexvals;
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
	free (cpumasks[idx].hexvals);
	cpumasks[idx].str = NULL;
	cpumasks[idx].str_reverse = NULL;
	cpumasks[idx].hexstr = NULL;
	cpumasks[idx].hexstr_reverse = NULL;
	cpumasks[idx].hexvals = NULL;
}

void copy_cpu_mask(enum cpumask_idx source, enum cpumask_idx dest)
{
	int i;

	reset_cpus(dest);
	for (i = 0; i < topo_max_cpus; i++) {
		if (!CPU_ISSET_S(i, size_cpumask, cpumasks[source].mask))
			continue;

		_add_cpu(i, dest);
	}
}

void copy_cpu_mask_exclude(enum cpumask_idx source, enum cpumask_idx dest, enum cpumask_idx exlude)
{
	int i;

	reset_cpus(dest);
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

char *get_proc_irq_str(enum cpumask_idx idx)
{
	return get_cpus_hexstr(idx);
}

char *get_irqbalance_str(enum cpumask_idx idx)
{
	return get_cpus_str_reverse(idx);
}

char *get_cpu_isolation_str(enum cpumask_idx idx)
{
	if (idx == CPUMASK_ONLINE)
		return get_cpus_str(idx);
	else
		return get_cpus_str_reverse(idx);
}

uint8_t *get_cgroup_systemd_vals(enum cpumask_idx idx, int *size)
{
	return get_cpus_hexvals(idx, size);
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

	if (!has_cpus (CPUMASK_LPM_DEFAULT))
		return 0;

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

err:
	reset_cpus (CPUMASK_LPM_DEFAULT);
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

end: if (has_cpus (CPUMASK_LPM_DEFAULT))
		lpmd_log_info ("\tUse CPU %s as Default Low Power CPUs (%s)\n",
						get_cpus_str (CPUMASK_LPM_DEFAULT), str);

	return 0;
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

int check_cpu_capability(lpmd_config_t *lpmd_config)
{
	FILE *filep;
	int i;
	char path[MAX_STR_LENGTH];
	int ret;
	int pcores, ecores, lcores;
	int tdp;

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

int cpu_init(char *cmd_cpus)
{
	return detect_lpm_cpus (cmd_cpus);
}
