// SPDX-License-Identifier: GPL-2.0-or-later
/* Copyright (C) 2026 Intel Corporation */

#define _GNU_SOURCE
#include "lpmd.h"

/* ITMT Management */
#define PATH_ITMT_CONTROL "/proc/sys/kernel/sched_itmt_enabled"
#define PATH_ITMT_CONTROL_DEBUGFS "/sys/kernel/debug/x86/sched_itmt_enabled"
#define PATH_INTEL_PSTATE_STATUS "/sys/devices/system/cpu/intel_pstate/status"

static int has_itmt;
static int saved_itmt = SETTING_IGNORE;
static int intel_pstate_saved;
static char saved_intel_pstate_mode[32];

static void trim_trailing_ws(char *s)
{
	int len;

	if (!s)
		return;

	len = strlen(s);
	while (len > 0 && (s[len - 1] == '\n' || s[len - 1] == '\r' ||
			   s[len - 1] == ' ' || s[len - 1] == '\t')) {
		s[len - 1] = '\0';
		len--;
	}
}

int get_itmt(void)
{
	int val, ret;

	if (!has_itmt)
		return -1;

	ret = lpmd_read_yn(PATH_ITMT_CONTROL_DEBUGFS, &val, -1);
	if (!ret)
		return val;

	lpmd_log_debug("Read ITMT debugfs failed, fallback to sysctl\n");
	ret = lpmd_read_int(PATH_ITMT_CONTROL, &val, -1);
	if (ret) {
		lpmd_log_error("Read ITMT sysctl failed\n");
			return -1;
	}

	return val;
}

void itmt_init(void)
{
	if (lpmd_read_yn(PATH_ITMT_CONTROL_DEBUGFS, &saved_itmt, -1)) {
		lpmd_log_debug("ITMT debugfs not detected\n");
	} else {
		has_itmt = 1;
		return;
	}

	if (lpmd_read_int(PATH_ITMT_CONTROL, &saved_itmt, -1))
		lpmd_log_debug("ITMT not detected\n");
	else
		has_itmt = 1;
}

int process_itmt(struct lpmd_config_state_t *state)
{
	int ret;

	if (!has_itmt)
		return 0;

	switch (state->itmt_state) {
	case SETTING_IGNORE:
		lpmd_log_debug("Ignore ITMT\n");
		return 0;
	case SETTING_RESTORE:
		ret = lpmd_write_yn(PATH_ITMT_CONTROL_DEBUGFS, saved_itmt, -1);
		if (ret)
			return lpmd_write_int(PATH_ITMT_CONTROL, saved_itmt, -1);
		return ret;
	default:
		lpmd_log_debug("%s ITMT\n", state->itmt_state ? "Enable" : "Disable");
		ret = lpmd_write_yn(PATH_ITMT_CONTROL_DEBUGFS, state->itmt_state, -1);
		if (ret)
			return lpmd_write_int(PATH_ITMT_CONTROL, state->itmt_state, -1);
		return ret;
	}
}

int process_intel_pstate_mode(struct lpmd_config_t *config)
{
	char current_mode[sizeof(saved_intel_pstate_mode)] = { 0 };
	char mode_active[] = "active";
	char mode_passive[] = "passive";
	char *mode;

	if (!config)
		return -1;

	if (!intel_pstate_saved) {
		if (lpmd_read_str((char *)PATH_INTEL_PSTATE_STATUS, current_mode,
				 sizeof(current_mode))) {
			lpmd_log_warn("Failed to read current intel_pstate mode\n");
		} else {
			trim_trailing_ws(current_mode);
			if (current_mode[0]) {
				snprintf(saved_intel_pstate_mode,
					 sizeof(saved_intel_pstate_mode), "%s",
					 current_mode);
				intel_pstate_saved = 1;
				lpmd_log_info("Saved original intel_pstate mode: %s\n",
					      saved_intel_pstate_mode);
			}
		}
	}

	mode = config->intel_pstate_mode == 1 ? mode_passive : mode_active;
	if (lpmd_write_str(PATH_INTEL_PSTATE_STATUS, mode, LPMD_LOG_INFO)) {
		lpmd_log_warn("Failed to set intel_pstate mode to %s\n", mode);
		return -1;
	}

	lpmd_log_info("Set intel_pstate mode to %s\n", mode);
	return 0;
}

int restore_intel_pstate_mode(void)
{
	if (!intel_pstate_saved || !saved_intel_pstate_mode[0])
		return 0;

	if (lpmd_write_str(PATH_INTEL_PSTATE_STATUS, saved_intel_pstate_mode,
			  LPMD_LOG_INFO)) {
		lpmd_log_warn("Failed to restore intel_pstate mode to %s\n",
			      saved_intel_pstate_mode);
		return -1;
	}

	lpmd_log_info("Restored intel_pstate mode to %s\n",
		      saved_intel_pstate_mode);
	intel_pstate_saved = 0;
	return 0;
}

/* Slider Management */

#define PATH_PLATFORM_PROFILE	"/sys/class/platform-profile"
#define NAME_SOC_SLD		"SoC Power Slider"

static char soc_sld_path[MAX_STR_LENGTH];
static char soc_sld_profile[MAX_STR_LENGTH];
static int slider_available;
static int slider_unavailable;

#define PATH_SOC_BALANCE_SLIDER	"/sys/module/processor_thermal_soc_slider/parameters/slider_balance"
#define PATH_SOC_OFFSET		"/sys/module/processor_thermal_soc_slider/parameters/slider_offset"

static void init_slider_path(void)
{
	struct dirent *entry;
	DIR *dir;
	int ret;

	snprintf(soc_sld_path, MAX_STR_LENGTH, "%s", PATH_PLATFORM_PROFILE);

	dir = opendir(soc_sld_path);
	if (!dir) {
		lpmd_log_debug("Cannot find %s\n", soc_sld_path);
		goto slider_failed;
	}

	while ((entry = readdir(dir)) != NULL) {
		if (strlen(entry->d_name) > 100)
			continue;
		if (strncmp(entry->d_name, "platform-profile",
			    strlen("platform-profile")))
			continue;

		snprintf(soc_sld_path, MAX_STR_LENGTH, "%s/%s/name",
			 PATH_PLATFORM_PROFILE, entry->d_name);

		ret = lpmd_read_str(soc_sld_path, soc_sld_profile, MAX_STR_LENGTH);
		if (ret)
			continue;

		if (!strncmp(soc_sld_profile, NAME_SOC_SLD, strlen(NAME_SOC_SLD))) {
			snprintf(soc_sld_path, MAX_STR_LENGTH, "%s/%s/profile",
				 PATH_PLATFORM_PROFILE, entry->d_name);
			slider_available = 1;
			break;
		}
	}

	closedir(dir);

	if (!entry) {
		lpmd_log_debug("\tCannot find %s\n", NAME_SOC_SLD);
		goto slider_failed;
	}

	lpmd_log_info("\tAvailable at %s/%s, use profile [%s]\n",
		      PATH_PLATFORM_PROFILE, entry->d_name, soc_sld_profile);

	return;

slider_failed:
	lpmd_log_debug("\tIgnore soc_sld/sld_offset setting\n");
	slider_unavailable = 1;
}

static int update_balance_slider(int slider)
{
	int ret;
	static int current_slider = -1;

	lpmd_log_debug("%s\n", __func__);

	if (slider < 0)
		return 0;

	if (slider_unavailable)
		return -1;

	if (!slider_available) {
		init_slider_path();
		if (slider_unavailable)
			return -1;
	}

	if (current_slider >= 0 && current_slider == slider)
		return 0;

	ret = lpmd_write_int(PATH_SOC_BALANCE_SLIDER, slider, 1);
	if (ret)
		return ret;

	/* Read the current profile and rewrite to make the module params effective */
	ret = lpmd_read_str(soc_sld_path, soc_sld_profile, MAX_STR_LENGTH);
	if (ret)
		return ret;

	ret = lpmd_write_str(soc_sld_path, soc_sld_profile, 1);
	if (ret)
		return ret;

	current_slider = slider;

	return 0;
}

static int update_slider_offset(int offset)
{
	int ret;
	static int current_slider_offset = -1;

	if (slider_unavailable)
		return -1;

	if (offset < 0)
		return 0;

	if (!slider_available) {
		init_slider_path();
		if (slider_unavailable)
			return -1;
	}

	if (current_slider_offset >= 0 && current_slider_offset == offset)
		return 0;

	ret = lpmd_write_int(PATH_SOC_OFFSET, offset, 1);
	if (ret)
		return ret;

	/* Read the current profile and rewrite to make the module params effective */
	ret = lpmd_read_str(soc_sld_path, soc_sld_profile, MAX_STR_LENGTH);
	if (ret)
		return ret;

	ret = lpmd_write_str(soc_sld_path, soc_sld_profile, 1);
	if (ret)
		return ret;

	current_slider_offset = offset;

	return 0;
}

void process_balance_slider_default_update(struct lpmd_config_t *config)
{
	lpmd_log_debug("%s\n", __func__);

	if (is_on_battery()) {
		if (config->balance_slider_def_dc >= 0)
			update_balance_slider(config->balance_slider_def_dc);
	} else {
		if (config->balance_slider_def_ac >= 0)
			update_balance_slider(config->balance_slider_def_ac);
	}
}

void process_slider_offset_default_update(struct lpmd_config_t *config)
{
	lpmd_log_debug("%s\n", __func__);

	if (is_on_battery()) {
		if (config->slider_offset_def_dc >= 0)
			update_slider_offset(config->slider_offset_def_dc);
	} else {
		if (config->slider_offset_def_ac >= 0)
			update_slider_offset(config->slider_offset_def_ac);
	}
}

static int process_balance_slider(struct lpmd_config_state_t *state)
{
	lpmd_log_debug("%s\n", __func__);

	if (is_on_battery())
		return update_balance_slider(state->balance_slider_dc);
	else
		return update_balance_slider(state->balance_slider_ac);
}

static int process_slider_offset(struct lpmd_config_state_t *state)
{
	lpmd_log_debug("%s\n", __func__);

	if (is_on_battery())
		return update_slider_offset(state->slider_offset_dc);
	else
		return update_slider_offset(state->slider_offset_ac);
}

int process_balance_slider_only(struct lpmd_config_t *config,
				       struct lpmd_config_state_t *state)
{
	int ret;

	if (!config || !state)
		return LPMD_ERROR;

	ret = process_balance_slider(state);
	if (ret)
		process_balance_slider_default_update(config);

	return ret;
}

int process_slider_offset_only(struct lpmd_config_t *config,
				      struct lpmd_config_state_t *state)
{
	int ret;

	if (!config || !state)
		return LPMD_ERROR;

	ret = process_slider_offset(state);
	if (ret)
		process_slider_offset_default_update(config);

	return ret;
}

void process_slider(struct lpmd_config_t *config, struct lpmd_config_state_t *state)
{
	int ret;
	const char *owner;

	if (lpmd_process_cpuset_balance_slider_locked()) {
		owner = lpmd_process_cpuset_balance_slider_owner();
		lpmd_log_info("Skip state balance slider due to process_cpuset class lock (owner=%s)\n",
			      owner && owner[0] ? owner : "unknown");
	} else {
		ret = process_balance_slider(state);
		if (ret)
			process_balance_slider_default_update(config);
	}

	if (lpmd_process_cpuset_slider_offset_locked()) {
		owner = lpmd_process_cpuset_slider_offset_owner();
		lpmd_log_info("Skip state slider offset due to process_cpuset class lock (owner=%s)\n",
			      owner && owner[0] ? owner : "unknown");
	} else {
		ret = process_slider_offset(state);
		if (ret)
			process_slider_offset_default_update(config);
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

	filep = fopen(path, "r");
	if (!filep)
		return 1;

	ret = fscanf(filep, "%d", &epp);
	if (ret == 1) {
		*val = epp;
		ret = 0;
		goto end;
	}

	ret = fread(str, 1, size, filep);
	if (ret <= 0) {
		ret = 1;
	} else {
		if (ret >= size)
			ret = size - 1;
		str[ret - 1] = '\0';
		ret = 0;
	}
end:
	fclose(filep);
	return ret;
}

static int set_epp(char *path, int val, char *str)
{
	FILE *filep;
	int ret;

	filep = fopen(path, "r+");
	if (!filep)
		return 1;

	if (val >= 0) {
		ret = fprintf(filep, "%d", val);
	} else if (str && str[0] != '\0') {
		ret = fprintf(filep, "%s", str);
	} else {
		fclose(filep);
		return 1;
	}

	fclose(filep);

	if (ret <= 0) {
		if (val >= 0)
			lpmd_log_error("Write \"%d\" to %s failed, ret %d\n", val, path, ret);
		else
			lpmd_log_error("Write \"%s\" to %s failed, ret %d\n", str, path, ret);
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
	int ret;

	*epp = -1;
	epp_str[0] = '\0';
	/* CPU0 is always online */
	snprintf(path, sizeof(path),
		 "/sys/devices/system/cpu/cpu%d/cpufreq/energy_performance_preference", 0);
	get_epp(path, epp, epp_str, size);
	epp_str[size - 1] = '\0';

	*epb = -1;
	snprintf(path, MAX_STR_LENGTH, "/sys/devices/system/cpu/cpu%d/power/energy_perf_bias", 0);
	ret = lpmd_read_int(path, epb, -1);
	return ret;
}

int process_epp_epb(struct lpmd_config_state_t *state)
{
	int max_cpus = get_max_cpus();
	char path[MAX_STR_LENGTH];
	int ret;
	int c;

	if (state->epp == SETTING_IGNORE)
		lpmd_log_info("Ignore EPP\n");
	if (state->epb == SETTING_IGNORE)
		lpmd_log_info("Ignore EPB\n");
	if (state->epp == SETTING_IGNORE && state->epb == SETTING_IGNORE)
		return 0;

	for (c = 0; c < max_cpus; c++) {
		int val;
		char *str = NULL;

		if (!is_cpu_online(c))
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

			snprintf(path, sizeof(path),
				 "/sys/devices/system/cpu/cpu%d/cpufreq/energy_performance_preference", c);
			ret = set_epp(path, val, str);
			if (!ret) {
				if (val != -1)
					lpmd_log_debug("Set CPU%d EPP to 0x%x\n",
						       c, val);
				else
					lpmd_log_debug("Set CPU%d EPP to %s\n",
						       c, saved_cpu_info[c].epp_str);
			}
		}

		if (state->epb != SETTING_IGNORE) {
			if (state->epb == SETTING_RESTORE)
				val = saved_cpu_info[c].epb;
			else
				val = state->epb;

			snprintf(path, MAX_STR_LENGTH,
				 "/sys/devices/system/cpu/cpu%d/power/energy_perf_bias", c);
			ret = lpmd_write_int(path, val, -1);
			if (!ret)
				lpmd_log_debug("Set CPU%d EPB to 0x%x\n", c, val);
		}
	}
	return 0;
}

int epp_epb_init(void)
{
	int max_cpus = get_max_cpus();
	char path[MAX_STR_LENGTH];
	int ret;
	int c;

	saved_cpu_info = calloc(max_cpus, sizeof(struct cpu_info));

	for (c = 0; c < max_cpus; c++) {
		saved_cpu_info[c].epp_str[0] = '\0';
		saved_cpu_info[c].epp = -1;

		if (!is_cpu_online(c))
			continue;

		snprintf(path, sizeof(path),
			 "/sys/devices/system/cpu/cpu%d/cpufreq/energy_performance_preference", c);
		ret = get_epp(path, &saved_cpu_info[c].epp,
			      saved_cpu_info[c].epp_str, MAX_EPP_STRING_LENGTH);
		if (!ret) {
			if (saved_cpu_info[c].epp != -1)
				lpmd_log_debug("CPU%d EPP: 0x%x\n", c, saved_cpu_info[c].epp);
			else
				lpmd_log_debug("CPU%d EPP: %s\n", c, saved_cpu_info[c].epp_str);
		}

		snprintf(path, MAX_STR_LENGTH,
			 "/sys/devices/system/cpu/cpu%d/power/energy_perf_bias", c);
		ret = lpmd_read_int(path, &saved_cpu_info[c].epb, -1);
		if (ret) {
			saved_cpu_info[c].epb = -1;
			continue;
		}
		lpmd_log_debug("CPU%d EPB: 0x%x\n", c, saved_cpu_info[c].epb);
	}
	return 0;
}

/* intel_pstate min perf management */
#define PATH_INTEL_PSTATE_MIN_PERF_PCT "/sys/devices/system/cpu/intel_pstate/min_perf_pct"
#define PATH_INTEL_PSTATE_MAX_PERF_PCT "/sys/devices/system/cpu/intel_pstate/max_perf_pct"
#define PATH_CPUFREQ_SCALING_MIN_FREQ_FMT "/sys/devices/system/cpu/cpu%d/cpufreq/scaling_min_freq"
#define PATH_CPUFREQ_SCALING_MAX_FREQ_FMT "/sys/devices/system/cpu/cpu%d/cpufreq/scaling_max_freq"
#define PATH_CPUFREQ_CPUINFO_MAX_FREQ_FMT "/sys/devices/system/cpu/cpu%d/cpufreq/cpuinfo_max_freq"

static int saved_min_perf_pct = SETTING_IGNORE;
static int saved_max_perf_pct = SETTING_IGNORE;

struct perf_cpufreq_info {
	int valid;
	int saved_scaling_min;
	int saved_scaling_max;
	int cpuinfo_max;
};

static struct perf_cpufreq_info *saved_perf_cpufreq;
static int perf_cpufreq_ready;

static int process_min_perf_pct_impl(struct lpmd_config_state_t *state,
				     int honor_class_lock);
static int process_max_perf_pct_impl(struct lpmd_config_state_t *state,
				     unsigned int core_scope_mask);

static int perf_scope_cpu_match(const struct lpmd_config_t *config,
				int cpu, unsigned int scope_mask)
{
	if (!(scope_mask & LPMD_PERF_SCOPE_ALL))
		return 0;

	if (!config || !config->core_type_masks[P_CORE] ||
	    !config->core_type_masks[E_CORE] ||
	    !config->core_type_masks[L_CORE])
		return 1;

	if ((scope_mask & LPMD_PERF_SCOPE_P) &&
	    (config->core_type_masks[P_CORE][cpu / 8] & (1U << (cpu % 8))))
		return 1;
	if ((scope_mask & LPMD_PERF_SCOPE_E) &&
	    (config->core_type_masks[E_CORE][cpu / 8] & (1U << (cpu % 8))))
		return 1;
	if ((scope_mask & LPMD_PERF_SCOPE_L) &&
	    (config->core_type_masks[L_CORE][cpu / 8] & (1U << (cpu % 8))))
		return 1;

	return 0;
}

static void init_perf_cpufreq_cache(void)
{
	int max_cpus;
	char path[MAX_STR_LENGTH];
	int c;

	if (saved_perf_cpufreq)
		return;

	max_cpus = get_max_cpus();
	if (max_cpus <= 0)
		return;

	saved_perf_cpufreq = calloc(max_cpus, sizeof(*saved_perf_cpufreq));
	if (!saved_perf_cpufreq)
		return;

	for (c = 0; c < max_cpus; c++) {
		int minf, maxf, maxinfo;

		if (!is_cpu_online(c))
			continue;

		snprintf(path, sizeof(path), PATH_CPUFREQ_SCALING_MIN_FREQ_FMT, c);
		if (lpmd_read_int(path, &minf, -1))
			continue;

		snprintf(path, sizeof(path), PATH_CPUFREQ_SCALING_MAX_FREQ_FMT, c);
		if (lpmd_read_int(path, &maxf, -1))
			continue;

		snprintf(path, sizeof(path), PATH_CPUFREQ_CPUINFO_MAX_FREQ_FMT, c);
		if (lpmd_read_int(path, &maxinfo, -1))
			continue;

		saved_perf_cpufreq[c].saved_scaling_min = minf;
		saved_perf_cpufreq[c].saved_scaling_max = maxf;
		saved_perf_cpufreq[c].cpuinfo_max = maxinfo;
		saved_perf_cpufreq[c].valid = 1;
		perf_cpufreq_ready = 1;
	}
}

static int apply_perf_pct_scoped_cpufreq(int is_min, int val, int restore,
					 unsigned int scope_mask)
{
	struct lpmd_config_t *config = get_lpmd_config();
	char path[MAX_STR_LENGTH];
	int max_cpus = get_max_cpus();
	int attempted = 0;
	int applied = 0;
	int c;

	if (!(scope_mask & LPMD_PERF_SCOPE_ALL))
		return -1;

	init_perf_cpufreq_cache();
	if (!perf_cpufreq_ready || !saved_perf_cpufreq)
		return -1;

	for (c = 0; c < max_cpus; c++) {
		long long target;
		int write_val;

		if (!is_cpu_online(c) || !saved_perf_cpufreq[c].valid)
			continue;
		if (!perf_scope_cpu_match(config, c, scope_mask))
			continue;

		if (restore)
			write_val = is_min ? saved_perf_cpufreq[c].saved_scaling_min
					   : saved_perf_cpufreq[c].saved_scaling_max;
		else {
			target = ((long long)saved_perf_cpufreq[c].cpuinfo_max * val) / 100;
			if (target <= 0)
				target = 1;
			write_val = (int)target;
		}

		if (write_val <= 0)
			continue;

		attempted++;
		snprintf(path, sizeof(path),
			 is_min ? PATH_CPUFREQ_SCALING_MIN_FREQ_FMT :
				  PATH_CPUFREQ_SCALING_MAX_FREQ_FMT,
			 c);
		if (!lpmd_write_int(path, write_val, LPMD_LOG_DEBUG))
			applied++;
	}

	if (!attempted)
		return 0;

	return applied > 0 ? 0 : -1;
}

int min_perf_pct_init(void)
{
	init_perf_cpufreq_cache();

	if (lpmd_read_int(PATH_INTEL_PSTATE_MIN_PERF_PCT, &saved_min_perf_pct, -1)) {
		saved_min_perf_pct = SETTING_IGNORE;
		lpmd_log_debug("intel_pstate min_perf_pct not available\n");
		return 0;
	}

	lpmd_log_debug("Saved min_perf_pct: %d\n", saved_min_perf_pct);
	return 0;
}

int process_min_perf_pct(struct lpmd_config_state_t *state)
{
	return process_min_perf_pct_impl(state, 1);
}

int process_min_perf_pct_scoped(struct lpmd_config_state_t *state,
				 unsigned int core_scope_mask)
{
	if (!state)
		return 0;

	if (is_on_battery())
		state->min_perf_pct_scope_dc = core_scope_mask;
	else
		state->min_perf_pct_scope_ac = core_scope_mask;

	return process_min_perf_pct_impl(state, 1);
}

int process_min_perf_pct_override(struct lpmd_config_state_t *state)
{
	return process_min_perf_pct_impl(state, 0);
}

static int process_min_perf_pct_impl(struct lpmd_config_state_t *state,
				     int honor_class_lock)
{
	int val;
	int configured_val;
	unsigned int configured_scope;
	int restore;
	const char *owner;

	if (!state)
		return 0;

	if (honor_class_lock && lpmd_process_cpuset_min_perf_pct_locked()) {
		owner = lpmd_process_cpuset_min_perf_pct_owner();
		lpmd_log_info("Skip state min_perf_pct due to process_cpuset class lock (owner=%s)\n",
			      owner && owner[0] ? owner : "unknown");
		return 0;
	}

	if (is_on_battery())
		configured_val = state->min_perf_pct_dc,
		configured_scope = state->min_perf_pct_scope_dc;
	else
		configured_val = state->min_perf_pct_ac,
		configured_scope = state->min_perf_pct_scope_ac;

	if (configured_val == SETTING_IGNORE)
		return 0;

	if (configured_val == SETTING_RESTORE) {
		restore = 1;
		if (saved_min_perf_pct == SETTING_IGNORE)
			val = SETTING_IGNORE;
		else
			val = saved_min_perf_pct;
	} else {
		restore = 0;
		val = configured_val;
	}

	if (configured_scope != LPMD_PERF_SCOPE_GLOBAL) {
		if (apply_perf_pct_scoped_cpufreq(1, val, restore,
						 configured_scope) == 0)
			return 0;

		if (restore && saved_min_perf_pct == SETTING_IGNORE)
			return 0;
	}

	if (restore && saved_min_perf_pct == SETTING_IGNORE)
		return 0;

	if (val < 0 || val > 100) {
		lpmd_log_error("Invalid min_perf_pct value %d\n", val);
		return LPMD_ERROR;
	}

	return lpmd_write_int(PATH_INTEL_PSTATE_MIN_PERF_PCT, val, LPMD_LOG_DEBUG);
}

int max_perf_pct_init(void)
{
	init_perf_cpufreq_cache();

	if (lpmd_read_int(PATH_INTEL_PSTATE_MAX_PERF_PCT, &saved_max_perf_pct, -1)) {
		saved_max_perf_pct = SETTING_IGNORE;
		lpmd_log_debug("intel_pstate max_perf_pct not available\n");
		return 0;
	}

	lpmd_log_debug("Saved max_perf_pct: %d\n", saved_max_perf_pct);
	return 0;
}

int process_max_perf_pct(struct lpmd_config_state_t *state)
{
	return process_max_perf_pct_impl(state, LPMD_PERF_SCOPE_GLOBAL);
}

int process_max_perf_pct_scoped(struct lpmd_config_state_t *state,
				 unsigned int core_scope_mask)
{
	return process_max_perf_pct_impl(state, core_scope_mask);
}

static int process_max_perf_pct_impl(struct lpmd_config_state_t *state,
				     unsigned int core_scope_mask)
{
	int val;
	int configured_val;
	unsigned int configured_scope;
	int configured_is_scoped;
	int restore;

	if (!state)
		return 0;

	if (is_on_battery()) {
		configured_val = state->max_perf_pct_dc;
		configured_scope = state->max_perf_pct_scope_dc;
		configured_is_scoped = state->max_perf_pct_is_scoped_dc;
	} else {
		configured_val = state->max_perf_pct_ac;
		configured_scope = state->max_perf_pct_scope_ac;
		configured_is_scoped = state->max_perf_pct_is_scoped_ac;
	}

	if (configured_val == SETTING_IGNORE)
		return 0;

	if (core_scope_mask != LPMD_PERF_SCOPE_GLOBAL)
		configured_scope = core_scope_mask,
		configured_is_scoped = 1;

	if (configured_val == SETTING_RESTORE) {
		restore = 1;
		if (saved_max_perf_pct == SETTING_IGNORE)
			val = SETTING_IGNORE;
		else
			val = saved_max_perf_pct;
	} else {
		restore = 0;
		val = configured_val;
	}

	if (configured_is_scoped && configured_scope != LPMD_PERF_SCOPE_GLOBAL) {
		if (apply_perf_pct_scoped_cpufreq(0, val, restore,
						 configured_scope) == 0)
			return 0;

		if (restore && saved_max_perf_pct == SETTING_IGNORE)
			return 0;
	}

	if (restore && saved_max_perf_pct == SETTING_IGNORE)
		return 0;

	if (val < 0 || val > 100) {
		lpmd_log_error("Invalid max_perf_pct value %d\n", val);
		return LPMD_ERROR;
	}

	return lpmd_write_int(PATH_INTEL_PSTATE_MAX_PERF_PCT, val, LPMD_LOG_DEBUG);
}
