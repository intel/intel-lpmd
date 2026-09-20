// SPDX-License-Identifier: GPL-2.0-or-later
/* Copyright (C) 2026 Intel Corporation */

#define _GNU_SOURCE
#include <systemd/sd-bus.h>
#include "lpmd.h"

/* Support for LPM_CPU_CGROUPV2 */
#define PATH_CGROUP			"/sys/fs/cgroup"
#define PATH_CG2_SUBTREE_CONTROL	PATH_CGROUP "/cgroup.subtree_control"

#define SYSTEMD_CGROUP_UNIT_COUNT 3

static const char *const systemd_cgroup_units[SYSTEMD_CGROUP_UNIT_COUNT] = {
	"system.slice",
	"user.slice",
	"machine.slice",
};

struct systemd_allowed_cpus_snapshot {
	uint8_t *values;
	size_t size;
};

static struct systemd_allowed_cpus_snapshot
	systemd_allowed_cpus[SYSTEMD_CGROUP_UNIT_COUNT];
static int systemd_allowed_cpus_saved;
/* Set only when this invocation enabled the cpuset subtree controller. */
static int cgroup_controller_added;
static int isolate_cgroup_created;

static void free_systemd_allowed_cpus_snapshot(void)
{
	for (int i = 0; i < SYSTEMD_CGROUP_UNIT_COUNT; i++) {
		free(systemd_allowed_cpus[i].values);
		systemd_allowed_cpus[i].values = NULL;
		systemd_allowed_cpus[i].size = 0;
	}
	systemd_allowed_cpus_saved = 0;
}

/* Read the exact byte-array value systemd currently exposes for one unit. */
static int read_allowed_cpus(const char *unit, uint8_t **values,
				     size_t *size)
{
	sd_bus_error error = SD_BUS_ERROR_NULL;
	sd_bus_message *unit_reply = NULL;
	sd_bus_message *property_reply = NULL;
	sd_bus *bus = NULL;
	const char *unit_path;
	const void *data;
	size_t n;
	int ret;

	if (!unit || !values || !size)
		return -1;
	*values = NULL;
	*size = 0;

	ret = sd_bus_open_system(&bus);
	if (ret < 0)
		goto finish;
	ret = sd_bus_call_method(bus, "org.freedesktop.systemd1",
				 "/org/freedesktop/systemd1",
				 "org.freedesktop.systemd1.Manager", "GetUnit",
				 &error, &unit_reply, "s", unit);
	if (ret < 0)
		goto finish;
	ret = sd_bus_message_read(unit_reply, "o", &unit_path);
	if (ret < 0)
		goto finish;

	ret = sd_bus_call_method(bus, "org.freedesktop.systemd1", unit_path,
				 "org.freedesktop.DBus.Properties", "Get", &error,
				 &property_reply, "ss",
				 "org.freedesktop.systemd1.Slice",
				 "AllowedCPUs");
	if (ret < 0)
		goto finish;
	ret = sd_bus_message_enter_container(property_reply,
						     SD_BUS_TYPE_VARIANT, "ay");
	if (ret < 0)
		goto finish;
	ret = sd_bus_message_read_array(property_reply, SD_BUS_TYPE_BYTE,
					      &data, &n);
	if (ret < 0)
		goto finish;
	if (n) {
		*values = malloc(n);
		if (!*values) {
			ret = -ENOMEM;
			goto finish;
		}
		memcpy(*values, data, n);
	}
	*size = n;
	ret = sd_bus_message_exit_container(property_reply);

finish:
	if (ret < 0)
		lpmd_log_info("Failed to read AllowedCPUs for %s: %s\n", unit,
			      error.message ? error.message : strerror(-ret));
	sd_bus_error_free(&error);
	sd_bus_message_unref(property_reply);
	sd_bus_message_unref(unit_reply);
	sd_bus_unref(bus);
	if (ret < 0) {
		free(*values);
		*values = NULL;
		*size = 0;
	}
	return ret < 0 ? -1 : 0;
}

static int snapshot_systemd_allowed_cpus(void)
{
	free_systemd_allowed_cpus_snapshot();
	for (int i = 0; i < SYSTEMD_CGROUP_UNIT_COUNT; i++) {
		if (read_allowed_cpus(systemd_cgroup_units[i],
				      &systemd_allowed_cpus[i].values,
				      &systemd_allowed_cpus[i].size) < 0) {
			free_systemd_allowed_cpus_snapshot();
			return -1;
		}
	}
	systemd_allowed_cpus_saved = 1;
	return 0;
}

/* Return whether the root cgroup already enables the cpuset controller. */
static int cgroup_cpuset_enabled(void)
{
	char buf[MAX_STR_LENGTH];
	FILE *file;
	char *saveptr;
	char *token;

	file = fopen(PATH_CG2_SUBTREE_CONTROL, "r");
	if (!file) {
		lpmd_log_info("Failed to read %s: %s\n", PATH_CG2_SUBTREE_CONTROL,
			      strerror(errno));
		return -1;
	}

	while (fgets(buf, sizeof(buf), file)) {
		for (token = strtok_r(buf, " \t\n", &saveptr); token;
		     token = strtok_r(NULL, " \t\n", &saveptr)) {
			if (!strcmp(token, "cpuset")) {
				fclose(file);
				return 1;
			}
		}
	}

	if (ferror(file)) {
		lpmd_log_info("Failed to read %s: %s\n", PATH_CG2_SUBTREE_CONTROL,
			      strerror(errno));
		fclose(file);
		return -1;
	}
	fclose(file);
	return 0;
}

static int update_allowed_cpus(const char *unit, uint8_t *vals, int size)
{
	sd_bus_error error = SD_BUS_ERROR_NULL;
	sd_bus_message *m = NULL;
	char buf[MAX_STR_LENGTH];
	sd_bus *bus = NULL;
	int offset;
	int ret;
	int i;

	ret = sd_bus_open_system(&bus);
	if (ret < 0) {
		lpmd_log_info("Failed to connect to system bus: %s\n", strerror(-ret));
		goto finish;
	}

	ret = sd_bus_message_new_method_call(bus, &m, "org.freedesktop.systemd1", "/org/freedesktop/systemd1",
					     "org.freedesktop.systemd1.Manager", "SetUnitProperties");
	if (ret < 0) {
		lpmd_log_info("Failed to issue method call: %s\n", error.message);
		goto finish;
	}

	ret = sd_bus_message_append(m, "sb", unit, 1);
	if (ret < 0) {
		lpmd_log_info("Failed to append unit: %s\n", error.message);
		goto finish;
	}

	ret = sd_bus_message_open_container(m, SD_BUS_TYPE_ARRAY, "(sv)");
	if (ret < 0) {
		lpmd_log_info("Failed to append array: %s\n", error.message);
		goto finish;
	}

	ret = sd_bus_message_open_container(m, SD_BUS_TYPE_STRUCT, "sv");
	if (ret < 0) {
		lpmd_log_info("Failed to open container struct: %s\n", error.message);
		goto finish;
	}

	ret = sd_bus_message_append_basic(m, SD_BUS_TYPE_STRING, "AllowedCPUs");
	if (ret < 0) {
		lpmd_log_info("Failed to append string: %s\n", error.message);
		goto finish_1;
	}

	ret = sd_bus_message_open_container(m, 'v', "ay");
	if (ret < 0) {
		lpmd_log_info("Failed to open container: %s\n", error.message);
		goto finish_2;
	}

	/* sd-bus accepts a non-NULL pointer even for the empty exact value. */
	{
		static const uint8_t empty;
		ret = sd_bus_message_append_array(m, 'y', vals ? vals : &empty,
						  size);
	}
	if (ret < 0) {
		lpmd_log_info("Failed to append allowed_cpus: %s\n", error.message);
		goto finish_2;
	}

	offset = snprintf(buf, MAX_STR_LENGTH, "\tSending Dbus message to systemd: %s: ", unit);
	for (i = 0; i < size; i++) {
		if (offset < MAX_STR_LENGTH)
			offset += snprintf(buf + offset, MAX_STR_LENGTH - offset, "0x%02x ", vals[i]);
	}
	buf[MAX_STR_LENGTH - 1] = '\0';
	lpmd_log_info("%s\n", buf);

	sd_bus_message_close_container(m);

finish_2:
	sd_bus_message_close_container(m);

finish_1:
	  sd_bus_message_close_container(m);

finish:
	if (ret >= 0) {
		ret = sd_bus_call(bus, m, 0, &error, NULL);
		if (ret < 0)
			lpmd_log_info("Failed to call: %s\n", error.message);
	}

	sd_bus_error_free(&error);
	sd_bus_message_unref(m);
	sd_bus_unref(bus);

	return ret < 0 ? -1 : 0;
}

static int restore_systemd_cgroup_snapshot(void)
{
	int ret = 0;

	if (!systemd_allowed_cpus_saved)
		return -1;
	for (int i = 0; i < SYSTEMD_CGROUP_UNIT_COUNT; i++) {
		if (update_allowed_cpus(systemd_cgroup_units[i],
					 systemd_allowed_cpus[i].values,
					 (int)systemd_allowed_cpus[i].size))
			ret = -1;
	}

	return ret;
}

static int update_systemd_cgroup(struct lpmd_config_state_t *state)
{
	int size = get_max_cpus() / 8;
	uint8_t *vals;
	int ret;

	vals = get_cgroup_systemd_vals(state->cpumask_idx);
	if (!vals)
		return -1;

	ret = update_allowed_cpus("system.slice", vals, size);
	if (ret)
		goto restore;

	ret = update_allowed_cpus("user.slice", vals, size);
	if (ret)
		goto restore;

	ret = update_allowed_cpus("machine.slice", vals, size);
	if (ret)
		goto restore;

	return 0;

restore:
	(void)restore_systemd_cgroup_snapshot();
	return ret;
}

static int process_cpu_cgroupv2(struct lpmd_config_state_t *state)
{
	int cpuset_enabled;

	if (cpumask_equal(state->cpumask_idx, CPUMASK_ONLINE)) {
		if (restore_systemd_cgroup_snapshot())
			return 1;
		if (cgroup_controller_added) {
			if (lpmd_write_str(PATH_CG2_SUBTREE_CONTROL, "-cpuset",
					    LPMD_LOG_DEBUG))
				return 1;
			cgroup_controller_added = 0;
		}
		return 0;
	}

	cpuset_enabled = cgroup_cpuset_enabled();
	if (cpuset_enabled < 0)
		return 1;
	if (!cpuset_enabled) {
		if (lpmd_write_str(PATH_CG2_SUBTREE_CONTROL, "+cpuset",
				    LPMD_LOG_DEBUG))
			return 1;
		cgroup_controller_added = 1;
	}
	return update_systemd_cgroup(state);
}

static enum cpumask_idx last_applied_cpumask = CPUMASK_NONE;

enum cgroup_backend {
	CGROUP_BACKEND_NONE,
	CGROUP_BACKEND_SYSTEMD,
	CGROUP_BACKEND_ISOLATE,
};

/* Only clean up a backend that this invocation explicitly initialized. */
static enum cgroup_backend active_backend = CGROUP_BACKEND_NONE;

/* Support for cgroup based cpu isolation */
static int process_cpu_isolate(struct lpmd_config_state_t *state)
{
	if (lpmd_write_str("/sys/fs/cgroup/lpm/cpuset.cpus.partition", "member", LPMD_LOG_DEBUG))
		return 1;

	if (!cpumask_equal(state->cpumask_idx, CPUMASK_ONLINE)) {
		if (lpmd_write_str("/sys/fs/cgroup/lpm/cpuset.cpus.exclusive", get_cpu_isolation_str(state->cpumask_idx), LPMD_LOG_DEBUG))
			return 1;
		if (lpmd_write_str("/sys/fs/cgroup/lpm/cpuset.cpus.partition", "isolated", LPMD_LOG_DEBUG))
			return 1;
		if (lpmd_write_str("/sys/fs/cgroup/lpm/cpuset.cpus", get_cpu_isolation_str(state->cpumask_idx), LPMD_LOG_DEBUG))
			return 1;
	} else {
		if (lpmd_write_str("/sys/fs/cgroup/lpm/cpuset.cpus", get_cpu_isolation_str(CPUMASK_ONLINE), LPMD_LOG_DEBUG))
			return 1;
	}

	return 0;
}

int cgroup_cleanup(struct lpmd_config_t *config)
{
	DIR *dir;
	int ret = 0;

	if (!config || !config->mode_configured) {
		last_applied_cpumask = CPUMASK_NONE;
		active_backend = CGROUP_BACKEND_NONE;
		cgroup_controller_added = 0;
		isolate_cgroup_created = 0;
		free_systemd_allowed_cpus_snapshot();
		return 0;
	}

	if (active_backend == CGROUP_BACKEND_NONE) {
		last_applied_cpumask = CPUMASK_NONE;
		cgroup_controller_added = 0;
		isolate_cgroup_created = 0;
		free_systemd_allowed_cpus_snapshot();
		return 0;
	}

	if (active_backend == CGROUP_BACKEND_ISOLATE) {
		if (isolate_cgroup_created) {
			dir = opendir("/sys/fs/cgroup/lpm");
			if (dir) {
				closedir(dir);
				if (rmdir("/sys/fs/cgroup/lpm"))
					ret = -1;
			}
		}
	} else if (active_backend == CGROUP_BACKEND_SYSTEMD) {
		if (restore_systemd_cgroup_snapshot())
			ret = -1;
	}

	if (cgroup_controller_added &&
	    lpmd_write_str(PATH_CG2_SUBTREE_CONTROL, "-cpuset",
			    LPMD_LOG_DEBUG))
		ret = -1;

	if (ret)
		return ret;
	last_applied_cpumask = CPUMASK_NONE;
	active_backend = CGROUP_BACKEND_NONE;
	cgroup_controller_added = 0;
	isolate_cgroup_created = 0;
	free_systemd_allowed_cpus_snapshot();
	return 0;
}

int cgroup_init(struct lpmd_config_t *config)
{
	int cpuset_enabled;
	int mkdir_ret;

	active_backend = CGROUP_BACKEND_NONE;
	last_applied_cpumask = CPUMASK_NONE;
	cgroup_controller_added = 0;
	isolate_cgroup_created = 0;
	free_systemd_allowed_cpus_snapshot();

	if (!config || !config->mode_configured)
		return 0;

	if (config->mode != LPM_CPU_CGROUPV2 &&
	    config->mode != LPM_CPU_ISOLATE)
		return 0;

	if (config->mode == LPM_CPU_CGROUPV2 &&
	    snapshot_systemd_allowed_cpus() < 0)
		return 1;

	cpuset_enabled = cgroup_cpuset_enabled();
	if (cpuset_enabled < 0) {
		free_systemd_allowed_cpus_snapshot();
		return 1;
	}
	if (!cpuset_enabled) {
		if (lpmd_write_str(PATH_CG2_SUBTREE_CONTROL, "+cpuset",
				    LPMD_LOG_DEBUG)) {
			free_systemd_allowed_cpus_snapshot();
			return 1;
		}
		cgroup_controller_added = 1;
	}
	if (config->mode == LPM_CPU_ISOLATE) {
		mkdir_ret = mkdir("/sys/fs/cgroup/lpm", 0744);
		if (mkdir_ret && errno != EEXIST) {
			if (cgroup_controller_added)
				(void)lpmd_write_str(PATH_CG2_SUBTREE_CONTROL, "-cpuset",
						     LPMD_LOG_DEBUG);
			cgroup_controller_added = 0;
			return 1;
		}
		if (mkdir_ret == 0)
			isolate_cgroup_created = 1;
		else {
			DIR *dir = opendir("/sys/fs/cgroup/lpm");
			if (!dir) {
				if (cgroup_controller_added)
					(void)lpmd_write_str(PATH_CG2_SUBTREE_CONTROL,
							     "-cpuset", LPMD_LOG_DEBUG);
				cgroup_controller_added = 0;
				return 1;
			}
			closedir(dir);
		}
		active_backend = CGROUP_BACKEND_ISOLATE;
	} else {
		active_backend = CGROUP_BACKEND_SYSTEMD;
	}
	return 0;
}

int process_cgroup(struct lpmd_config_t *config, struct lpmd_config_state_t *state)
{
	if (!config || !state || !config->mode_configured ||
	    active_backend == CGROUP_BACKEND_NONE)
		return 0;

	enum lpm_cpu_process_mode mode = config->mode;
	int ret;

	if (state->cpumask_idx == CPUMASK_NONE) {
		lpmd_log_debug("Ignore cgroup processing - CPUMASK empty\n");
		return 0;
	}

	/*
	 * HFI cpumask can't currently be used with last_applied_cpumask because
	 * CPUMASK_HFI takes on different values at the same index.
	 *
	 * TODO: Rewrite HFI with separate cpumasks instead of using the same
	 * one.
	 */
	if (last_applied_cpumask != CPUMASK_NONE &&
	    cpumask_equal(state->cpumask_idx, last_applied_cpumask)) {
		if (state->cpumask_idx == CPUMASK_HFI) {
			/* Don't update cgroups if HFI is enabled and it wasn't a reason for the update */
			if (!(config->data.need_update & (1 << UPDATE_HFI))) {
				lpmd_log_debug("Ignore cgroup processing - HFI enabled\n");
				return 0;
			}
		} else {
			lpmd_log_debug("Skip cgroup: cpumask unchanged\n");
			return 0;
		}
	}

	lpmd_log_info ("Process Cgroup ...\n");
	if (mode == LPM_CPU_CGROUPV2)
		ret = process_cpu_cgroupv2(state);
	else if (mode == LPM_CPU_ISOLATE)
		ret = process_cpu_isolate(state);
	else
		ret = 0;

	if (!ret)
		last_applied_cpumask = state->cpumask_idx;
	return ret;
}
