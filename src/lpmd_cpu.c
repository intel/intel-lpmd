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
#include <cpuid.h>
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
		[CPUMASK_HFI_SUV] = { .name = "HFI SUV", },
};

static enum cpumask_idx lpm_cpus_cur = CPUMASK_LPM_DEFAULT;

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

static int cpumask_to_str(cpu_set_t *mask, char *buf, int length)
{
	int i;
	int offset = 0;

	for (i = 0; i < topo_max_cpus; i++) {
		if (!CPU_ISSET_S(i, size_cpumask, mask))
			continue;
		if (length - 1 < offset) {
			lpmd_log_debug ("cpumask_to_str: Too many cpus\n");
			return 1;
		}
		offset += snprintf (buf + offset, length - 1 - offset, "%d,", i);
	}
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

static char* get_cpus_str(enum cpumask_idx idx)
{
	if (!cpumasks[idx].mask)
		return NULL;

	if (!CPU_COUNT_S(size_cpumask, cpumasks[idx].mask))
		return NULL;

	if (cpumasks[idx].str)
		return cpumasks[idx].str;

	cpumasks[idx].str = calloc (1, MAX_STR_LENGTH);
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

	cpumasks[idx].hexstr = calloc (1, MAX_STR_LENGTH);
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

	cpumasks[idx].hexstr_reverse = calloc (1, MAX_STR_LENGTH);
	if (!cpumasks[idx].hexstr_reverse)
		err (3, "STR_ALLOC");

	alloc_cpu_set (&mask);
	CPU_XOR_S(size_cpumask, mask, cpumasks[idx].mask, cpumasks[CPUMASK_ONLINE].mask);
	cpumask_to_hexstr (mask, cpumasks[idx].hexstr_reverse, MAX_STR_LENGTH);
	CPU_FREE(mask);

	return cpumasks[idx].hexstr_reverse;
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

	cpumasks[idx].str_reverse = calloc (1, MAX_STR_LENGTH);
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

int has_cpus(enum cpumask_idx idx)
{
	if (!cpumasks[idx].mask)
		return 0;
	return CPU_COUNT_S(size_cpumask, cpumasks[idx].mask);
}

int has_lpm_cpus(void)
{
	return has_cpus (lpm_cpus_cur);
}

static int _add_cpu(int cpu, enum cpumask_idx idx)
{
	if (idx != CPUMASK_ONLINE && !is_cpu_online (cpu))
		return 0;

	if (!cpumasks[idx].mask)
		alloc_cpu_set (&cpumasks[idx].mask);

	if (idx != CPUMASK_ONLINE && CPU_COUNT_S(size_cpumask, cpumasks[idx].mask) >= MAX_LPM_CPUS)
		return LPMD_FATAL_ERROR;

	CPU_SET_S(cpu, size_cpumask, cpumasks[idx].mask);

	return LPMD_SUCCESS;
}

int add_cpu(int cpu, enum cpumask_idx idx)
{
	if (cpu < 0 || cpu >= topo_max_cpus)
		return 0;

	_add_cpu (cpu, idx);

	if (idx == CPUMASK_LPM_DEFAULT)
		lpmd_log_info ("\tDetected %s CPU%d\n", cpumasks[idx].name, cpu);
	else
		lpmd_log_debug ("\tDetected %s CPU%d\n", cpumasks[idx].name, cpu);

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

int set_lpm_cpus(enum cpumask_idx new)
{
	if (lpm_cpus_cur == new)
		return 0;

	if (new == CPUMASK_HFI_SUV)
		CPU_XOR_S(size_cpumask, cpumasks[new].mask, cpumasks[CPUMASK_ONLINE].mask,
					cpumasks[new].mask);

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

/* Bit 15 of CPUID.7 EDX stands for Hybrid support */
#define CPUFEATURE_HYBRID	(1 << 15)
#define PATH_PM_PROFILE "/sys/firmware/acpi/pm_profile"

static int detect_supported_cpu(void)
{
	unsigned int eax, ebx, ecx, edx;
	int val;

	__cpuid(7, eax, ebx, ecx, edx);

	/* Run on Hybrid platforms only */
	if (!(edx & CPUFEATURE_HYBRID)) {
		lpmd_log_debug("Non-Hybrid platform detected\n");
		return -1;
	}

	/* /sys/firmware/acpi/pm_profile is mandatory */
	if (lpmd_read_int(PATH_PM_PROFILE, &val, -1)) {
		lpmd_log_debug("Failed to read %s\n", PATH_PM_PROFILE);
		return -1;
	}

	if (val != 2) {
		lpmd_log_debug("Non Mobile platform detected\n");
		return -1;
	}

	return 0;
}

static int parse_cpu_topology(void)
{
	FILE *filep;
	int i;
	char path[MAX_STR_LENGTH];
	int ret;

	ret = detect_supported_cpu();
	if (ret) {
		lpmd_log_info("Unsupported CPU type\n");
		return ret;
	}

	ret = set_max_cpu_num ();
	if (ret)
		return ret;

	reset_cpus (CPUMASK_ONLINE);
	for (i = 0; i < topo_max_cpus; i++) {
		unsigned int online;

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
	}
	max_online_cpu = i;

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
static int parse_cpu_str(char *buf, enum cpumask_idx idx)
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

		if (CPU_COUNT_S(size_cpumask, cpumasks[CPUMASK_LPM_DEFAULT].mask) == 4)
			break;
		reset_cpus (CPUMASK_LPM_DEFAULT);
	}

	if (!has_cpus (CPUMASK_LPM_DEFAULT)) {
		reset_cpus (CPUMASK_LPM_DEFAULT);
		return 0;
	}

	return CPU_COUNT_S(size_cpumask, cpumasks[CPUMASK_LPM_DEFAULT].mask);
}

/*
 * Use CPUs that don't have L3 as LPM CPUs.
 * Applies on platforms like MeteorLake.
 */
static int detect_lpm_cpus_l3(void)
{
	char path[MAX_STR_LENGTH];
	int i;

	for (i = 0; i < topo_max_cpus; i++) {
		if (!is_cpu_online (i))
			continue;

		snprintf (path, sizeof(path), "/sys/devices/system/cpu/cpu%d/cache/", i);
		if (access(path, F_OK)) {
			lpmd_log_error("Mandatory sysfs %s does not exist.\n", path);
			return -1;
		}

		snprintf (path, sizeof(path), "/sys/devices/system/cpu/cpu%d/cache/index3/level", i);
		if (lpmd_open (path, -1))
			_add_cpu (i, CPUMASK_LPM_DEFAULT);
	}

	if (!has_cpus (CPUMASK_LPM_DEFAULT))
		return 0;

	if (CPU_EQUAL_S(size_cpumask, cpumasks[CPUMASK_LPM_DEFAULT].mask,
					cpumasks[CPUMASK_ONLINE].mask))
		goto err;

	if (!CPU_COUNT_S(size_cpumask, cpumasks[CPUMASK_LPM_DEFAULT].mask))
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

	ret = detect_lpm_cpus_l3 ();
	if (ret < 0)
		return ret;

	if (ret > 0) {
		str = "Ecores";
		goto end;
	}

	if (detect_lpm_cpus_cluster ()) {
		str = "Ecores";
		goto end;
	}

	if (has_hfi_lpm_monitor () || has_hfi_suv_monitor ()) {
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
	if (lpmd_write_str (PATH_CG2_SUBTREE_CONTROL, "+cpuset", LPMD_LOG_INFO))
		return 1;

	return update_systemd_cgroup ();
}

static int process_cpu_cgroupv2_exit(void)
{
	restore_systemd_cgroup ();

	return lpmd_write_str (PATH_CG2_SUBTREE_CONTROL, "-cpuset", LPMD_LOG_INFO);
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
	if (lpmd_write_str (PATH_CPUMASK, cpumask_str, LPMD_LOG_INFO))
		return 1;

	if (dur > 0) {
		if (lpmd_read_int (PATH_DURATION, &default_dur, LPMD_LOG_INFO))
			return 1;

		if (lpmd_write_int (PATH_DURATION, dur, LPMD_LOG_INFO))
			return 1;
	}

	if (lpmd_write_int (PATH_MAXIDLE, pct, LPMD_LOG_INFO))
		return 1;

	if (lpmd_write_int (path_powerclamp, pct, LPMD_LOG_INFO))
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
	if (lpmd_write_int (PATH_DURATION, default_dur, LPMD_LOG_INFO))
		return 1;

	return lpmd_write_int (path_powerclamp, 0, LPMD_LOG_INFO);
}

static int process_cpu_powerclamp(int enter)
{
	if (enter)
		return process_cpu_powerclamp_enter ();
	else
		return process_cpu_powerclamp_exit ();
}

// Support for SUV mode, which uses powerclamp
#define SUV_IDLE_PCT	50
static int in_suv;

static int enter_suv_mode(enum lpm_command cmd)
{
	int ret;
	char *cpumask_str;
	char *name;

//	 in_suv can either be HFI_SUV or DBUS_SUV, can not be both
	if (in_suv)
		return 0;

	if (cmd == HFI_SUV_ENTER) {
		cpumask_str = get_cpus_hexstr (CPUMASK_HFI_SUV);
		name = "HFI";
	}
	else {
		cpumask_str = get_cpus_hexstr (CPUMASK_ONLINE);
		name = "DBUS";
	}

	/*
	 * When system is in LPM and it uses idle injection for LPM,
	 * we need to exit LPM first because we need to reset the cpumask
	 * of the intel_powerclamp sysfs I/F.
	 *
	 * In order to make the logic simpler, always exit LPM when Idle
	 * injection is used for LPM.
	 * The downside is that we need to do an extra LPM exit but this
	 * should be rare because it is abnormal to get an SUV request when
	 * system is already in LPM.
	 */
	if (get_cpu_mode () == LPM_CPU_POWERCLAMP)
		exit_lpm (cmd);

	lpmd_log_info ("------ Enter %s Survivability Mode ---\n", name);
	ret = _process_cpu_powerclamp_enter (cpumask_str, SUV_IDLE_PCT, -1);
	if (!ret)
		in_suv = cmd;
	return ret;
}

static int exit_suv_mode(enum lpm_command cmd)
{
	int cmd_enter;
	char *name;

//	 If SUV mode is disabled or exited
	if (in_suv == -1 || in_suv == 0)
		return 0;

	if (cmd == HFI_SUV_EXIT) {
		cmd_enter = HFI_SUV_ENTER;
		name = "HFI";
	}
	else {
		cmd_enter = DBUS_SUV_ENTER;
		name = "DBUS";
	}

	if (in_suv != cmd_enter)
		return 0;

	lpmd_log_info ("------ Exit %s Survivability Mode ---\n", name);

	process_cpu_powerclamp_exit ();

//	 Try to re-enter in case it was FORCED ON
	if (get_cpu_mode () == LPM_CPU_POWERCLAMP)
		enter_lpm (cmd);
	in_suv = 0;

	return LPMD_SUCCESS;
}

int process_suv_mode(enum lpm_command cmd)
{
	int ret;

	lpmd_lock ();
	if (cmd == HFI_SUV_ENTER || cmd == DBUS_SUV_ENTER)
		ret = enter_suv_mode (cmd);
	else if (cmd == HFI_SUV_EXIT || cmd == DBUS_SUV_EXIT)
		ret = exit_suv_mode (cmd);
	else
		ret = -1;
	lpmd_unlock ();
	return ret;
}

int has_suv_support(void)
{
	return !(in_suv == -1);
}

int in_hfi_suv_mode(void)
{
	int ret;

	lpmd_lock ();
	ret = in_suv;
	lpmd_unlock ();

	return ret == HFI_SUV_ENTER;
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

	if (lpmd_write_str ("/sys/fs/cgroup/lpm/cpuset.cpus.partition", "member", LPMD_LOG_INFO))
		return 1;

	if (lpmd_write_str ("/sys/fs/cgroup/lpm/cpuset.cpus", get_cpus_str_reverse (lpm_cpus_cur),
						LPMD_LOG_INFO))
		return 1;

	if (lpmd_write_str ("/sys/fs/cgroup/lpm/cpuset.cpus.partition", "isolated", LPMD_LOG_INFO))
		return 1;

	return 0;
}

static int process_cpu_isolate_exit(void)
{
	if (lpmd_write_str ("/sys/fs/cgroup/lpm/cpuset.cpus.partition", "member", LPMD_LOG_INFO))
		return 1;

	if (lpmd_write_str ("/sys/fs/cgroup/lpm/cpuset.cpus", get_cpus_str (CPUMASK_ONLINE),
						LPMD_LOG_INFO))
		return 1;

	return 0;
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

//	 Extra checks for SUV mode support
	if (mode != LPM_CPU_POWERCLAMP) {
		if (check_cpu_powerclamp_support ()) {
			in_suv = -1;
			lpmd_log_info ("Idle injection interface not detected, disable SUV mode support\n");
		}
	}

	return ret;
}

int init_cpu(char *cmd_cpus, enum lpm_cpu_process_mode mode)
{
	int ret;

	lpmd_log_info ("Detecting CPUs ...\n");
	ret = parse_cpu_topology ();
	if (ret)
		return ret;

	ret = detect_lpm_cpus (cmd_cpus);
	if (ret)
		return ret;

	ret = check_cpu_mode_support (mode);
	if (ret)
		return ret;

	return 0;
}

int process_cpus(int enter, enum lpm_cpu_process_mode mode)
{
	int ret;

	if (enter != 1 && enter != 0)
		return LPMD_ERROR;

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
