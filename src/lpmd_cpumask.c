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

void set_max_cpus(int num)
{
	topo_max_cpus = num;
}

int get_max_online_cpu(void)
{
	return max_online_cpu;
}

void set_max_online_cpu(int num)
{
	max_online_cpu = num;
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

int cpu_migrate(int cpu)
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

int cpumask_alloc(void)
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

int cpumask_free(enum cpumask_idx idx)
{
	if (!cpumasks[idx].mask)
		return 0;
		
	cpumask_reset(idx);
	free(cpumasks[idx].mask);
	cpumasks[idx].mask = NULL;
	return 0;
}

int cpumask_reset(enum cpumask_idx idx)
{
	if (!cpumasks[idx].mask)
		alloc_cpu_set(&cpumasks[idx].mask);
	else
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
	return 0;
}

int cpumask_add_cpu(int cpu, enum cpumask_idx idx)
{
	if (idx != CPUMASK_ONLINE && !is_cpu_online (cpu))
		return 0;

	if (!cpumasks[idx].mask)
		alloc_cpu_set (&cpumasks[idx].mask);

	CPU_SET_S(cpu, size_cpumask, cpumasks[idx].mask);

	return LPMD_SUCCESS;
}

int cpumask_init_cpus(char *buf, enum cpumask_idx idx)
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

		cpumask_add_cpu (start, idx);
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
			cpumask_add_cpu (start, idx);
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

int cpumask_nr_cpus(enum cpumask_idx idx)
{
	if (idx == CPUMASK_NONE)
		return 0;

	if (!cpumasks[idx].mask)
		return 0;

	return CPU_COUNT_S(size_cpumask, cpumasks[idx].mask);
}

int cpumask_has_cpu(enum cpumask_idx idx)
{
	return cpumask_nr_cpus(idx);
}

int cpumask_equal(enum cpumask_idx idx1, enum cpumask_idx idx2)
{
	if (!cpumasks[idx1].mask || !cpumasks[idx2].mask)
		return 0;

	if (CPU_EQUAL_S(size_cpumask, cpumasks[idx1].mask, cpumasks[idx2].mask))
		return 1;

	return 0;
}

void cpumask_copy(enum cpumask_idx source, enum cpumask_idx dest)
{
	int i;

	cpumask_reset(dest);
	for (i = 0; i < topo_max_cpus; i++) {
		if (!CPU_ISSET_S(i, size_cpumask, cpumasks[source].mask))
			continue;

		cpumask_add_cpu(i, dest);
	}
}

void cpumask_exclude_copy(enum cpumask_idx source, enum cpumask_idx dest, enum cpumask_idx exlude)
{
	int i;

	cpumask_reset(dest);
	for (i = 0; i < topo_max_cpus; i++) {
		if (!CPU_ISSET_S(i, size_cpumask, cpumasks[source].mask))
			continue;

		if (CPU_ISSET_S(i, size_cpumask, cpumasks[exlude].mask))
			continue;

	}
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

static int cpumask_to_hexstr(cpu_set_t *mask, char *str, int size)
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
