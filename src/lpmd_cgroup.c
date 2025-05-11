/*
 * lpmd_cgroup.c: task isolation via cgroup setting
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
#include <systemd/sd-bus.h>
#include "lpmd.h"

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

	ret = sd_bus_open_system (&bus);
	if (ret < 0) {
		fprintf (stderr, "Failed to connect to system bus: %s\n", strerror (-ret));
		goto finish;
	}

	ret = sd_bus_message_new_method_call (bus, &m, "org.freedesktop.systemd1", "/org/freedesktop/systemd1",
				"org.freedesktop.systemd1.Manager", "SetUnitProperties");
	if (ret < 0) {
		fprintf (stderr, "Failed to issue method call: %s\n", error.message);
		goto finish;
	}

	ret = sd_bus_message_append (m, "sb", unit, 1);
	if (ret < 0) {
		fprintf (stderr, "Failed to append unit: %s\n", error.message);
		goto finish;
	}

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

static int restore_systemd_cgroup(void)
{
	int size;
	uint8_t *vals;

	vals = get_cgroup_systemd_vals(CPUMASK_ONLINE, &size);
	if (!vals)
		return -1;

	update_allowed_cpus ("system.slice", vals, size);
	update_allowed_cpus ("user.slice", vals, size);
	update_allowed_cpus ("machine.slice", vals, size);

	return 0;
}

static int update_systemd_cgroup(lpmd_config_state_t *state)
{
	int size;
	uint8_t *vals;
	int ret;

	vals = get_cgroup_systemd_vals(state->cpumask_idx, &size);
	if (!vals)
		return -1;

	ret = update_allowed_cpus ("system.slice", vals, size);
	if (ret)
		goto restore;

	ret = update_allowed_cpus ("user.slice", vals, size);
	if (ret)
		goto restore;

	ret = update_allowed_cpus ("machine.slice", vals, size);
	if (ret)
		goto restore;

	return 0;

restore:
	restore_systemd_cgroup ();
	return ret;
}

static int process_cpu_cgroupv2(lpmd_config_state_t *state)
{
	if (is_equal(state->cpumask_idx, CPUMASK_ONLINE)) {
		restore_systemd_cgroup ();
		return lpmd_write_str (PATH_CG2_SUBTREE_CONTROL, "-cpuset", LPMD_LOG_DEBUG);	
	} else {
		if (lpmd_write_str (PATH_CG2_SUBTREE_CONTROL, "+cpuset", LPMD_LOG_DEBUG))
			return 1;
		return update_systemd_cgroup(state);
	}
}

/* Support for cgroup based cpu isolation */
static int process_cpu_isolate(lpmd_config_state_t *state)
{
	if (lpmd_write_str ("/sys/fs/cgroup/lpm/cpuset.cpus.partition", "member", LPMD_LOG_DEBUG))
		return 1;

	if (!is_equal(state->cpumask_idx, CPUMASK_ONLINE)) {
		if (lpmd_write_str ("/sys/fs/cgroup/lpm/cpuset.cpus", get_cpu_isolation_str(state->cpumask_idx), LPMD_LOG_DEBUG))
			return 1;
		if (lpmd_write_str ("/sys/fs/cgroup/lpm/cpuset.cpus.partition", "isolated", LPMD_LOG_DEBUG))
			return 1;
	} else {
		if (lpmd_write_str ("/sys/fs/cgroup/lpm/cpuset.cpus", get_cpu_isolation_str(CPUMASK_ONLINE), LPMD_LOG_DEBUG))
			return 1;
	}

	return 0;
}

int cgroup_exit(void)
{
	DIR *dir = opendir("/sys/fs/cgroup/lpm");
	if (dir) {
		closedir(dir);
		rmdir("/sys/fs/cgroup/lpm");
	}
	return 0;
}

int cgroup_init(lpmd_config_t *config)
{
	if (lpmd_write_str (PATH_CG2_SUBTREE_CONTROL, "+cpuset", LPMD_LOG_DEBUG))
		return 1;
	if (config->mode == LPM_CPU_ISOLATE)
		return mkdir ("/sys/fs/cgroup/lpm", 0744);
	return 0;
}

int process_cgroup(lpmd_config_state_t *state, enum lpm_cpu_process_mode mode)
{
	int ret;

	if (state->cpumask_idx == CPUMASK_NONE) {
		lpmd_log_debug ("Ignore cgroup processing\n");
		return 0;
	}

	lpmd_log_info ("Process Cgroup ...\n");
	if (mode == LPM_CPU_CGROUPV2)
		return process_cpu_cgroupv2(state);
	if (mode == LPM_CPU_ISOLATE)
		return process_cpu_isolate(state);
	return 0;
}
