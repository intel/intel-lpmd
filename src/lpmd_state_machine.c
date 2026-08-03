// SPDX-License-Identifier: GPL-2.0-or-later
/* Copyright (C) 2026 Intel Corporation */


#define _GNU_SOURCE
#include <stdio.h>
#include <err.h>
#include <stdlib.h>
#include <stdarg.h>
#include <ctype.h>
#include <string.h>
#include <strings.h>
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

/* LPMD state control: ON/OFF/AUTO/FREEZE/RESTORE/TERMINATE */
static int lpmd_state = LPMD_OFF;
static int saved_lpmd_state = LPMD_OFF;

static char *lpmd_state_name[] = {
	[LPMD_ON]		= "             ON",
	[LPMD_OFF]		= "            OFF",
	[LPMD_AUTO]		= "           AUTO",
	[LPMD_PROCESS_PRECONFIG]= "PROCESS-PRECONF",
	[LPMD_FREEZE]		= "         FREEZE",
	[LPMD_RESTORE]		= "        RESTORE",
	[LPMD_TERMINATE]	= "           TERM",
};

int update_lpmd_state(int new)
{
	lpmd_lock();
	switch (new) {
	case LPMD_FREEZE:
		if (lpmd_state == LPMD_FREEZE)
			break;
		lpmd_log_debug("Freeze lpmd\n");
		saved_lpmd_state = lpmd_state;
		lpmd_state = LPMD_FREEZE;
		break;
	case LPMD_RESTORE:
		if (lpmd_state != LPMD_FREEZE)
			break;
		lpmd_log_debug("Restore lpmd\n");
		lpmd_state = saved_lpmd_state;
		saved_lpmd_state = lpmd_state;
		break;
	default:
		if (lpmd_state == LPMD_FREEZE)
			saved_lpmd_state = new;
		else
			lpmd_state = new;
		break;
	}
	lpmd_unlock();
	return 0;
}

int get_lpmd_state(void)
{
	return lpmd_state;
}

/* LPMD config states control */

int lpmd_init_config_state(struct lpmd_config_state_t *state)
{
	state->id = -1;
	state->valid = 0;
	state->name[0] = '\0';

	state->wlt_type = -1;
	state->wlt_type_mask = -1;

	state->entry_system_load_thres = 0;
	state->exit_system_load_thres = 0;
	state->exit_system_load_hyst = 0;
	state->enter_cpu_load_thres = 0;
	state->exit_cpu_load_thres = 0;
	state->enter_gfx_load_thres = 0;
	state->exit_gfx_load_thres = 0;
	state->exit_gfx_load_hyst = 0;

	state->min_poll_interval = 0;
	state->max_poll_interval = 0;
	state->poll_interval_increment = 0;

	state->epp = SETTING_IGNORE;
	state->epb = SETTING_IGNORE;
	state->min_perf_pct_ac = SETTING_IGNORE;
	state->min_perf_pct_dc = SETTING_IGNORE;
	state->max_perf_pct_ac = SETTING_IGNORE;
	state->max_perf_pct_dc = SETTING_IGNORE;
	state->min_perf_pct_scope_ac = LPMD_PERF_SCOPE_GLOBAL;
	state->min_perf_pct_scope_dc = LPMD_PERF_SCOPE_GLOBAL;
	state->max_perf_pct_scope_ac = LPMD_PERF_SCOPE_GLOBAL;
	state->max_perf_pct_scope_dc = LPMD_PERF_SCOPE_GLOBAL;
	state->max_perf_pct_is_scoped_ac = 0;
	state->max_perf_pct_is_scoped_dc = 0;
	state->active_cpus[0] = '\0';
	state->cpumask_idx = CPUMASK_NONE;

	state->active_p_cores[0] = '\0';
	state->active_e_cores[0] = '\0';
	state->active_l_cores[0] = '\0';

	state->itmt_state = SETTING_IGNORE;
	state->irq_migrate = SETTING_IGNORE;

	state->entry_load_sys = 0;
	state->entry_load_cpu = 0;
	state->entry_load_gfx = 0;
	state->cpumask_idx = CPUMASK_NONE;

	state->balance_slider_ac = -1;
	state->balance_slider_dc = -1;
	state->slider_offset_ac = -1;
	state->slider_offset_dc = -1;

	return 0;
}

static int current_idx = DEFAULT_OFF;

static int config_state_match(struct lpmd_config_t *config, int idx)
{
	struct lpmd_config_state_t *state = &config->config_states[idx];
	int bcpu = config->data.util_cpu;
	int bsys = config->data.util_sys;
	int bgfx = config->data.util_gfx;
	int wlt_index = config->data.wlt_hint;

	if (!state->valid)
		return 0;

	if (state->wlt_type_mask != -1) {
		if (config->wlt_hint_mask != -1)
			wlt_index &= config->wlt_hint_mask;

		if (!(state->wlt_type_mask & (1 << wlt_index)))
			return 0;
	}
	if (state->wlt_type != -1) {
		if (config->wlt_hint_mask != -1)
			wlt_index &= config->wlt_hint_mask;

		if (state->wlt_type != wlt_index)
			return 0;
	}

	if (state->enter_cpu_load_thres && state->enter_cpu_load_thres < bcpu)
		return 0;

	if (state->enter_gfx_load_thres && state->enter_gfx_load_thres < bgfx) {
		if (!state->exit_gfx_load_hyst)
			return 0;
		if ((state->entry_load_gfx + state->exit_gfx_load_hyst) < bgfx ||
		    (state->enter_gfx_load_thres + state->exit_gfx_load_hyst) < bgfx)
			return 0;
	}

	if (state->entry_system_load_thres && state->entry_system_load_thres < bsys) {
		if (!state->exit_system_load_hyst)
			return 0;
		if ((state->entry_load_sys + state->exit_system_load_hyst) < bsys ||
		    (state->entry_system_load_thres + state->exit_system_load_hyst) < bsys)
			return 0;
	}

	return 1;
}

static int get_config_state_interval(struct lpmd_config_t *config, int idx)
{
	struct lpmd_config_state_t *state = &config->config_states[idx];

	/* wlt proxy updates polling separately */
	if (config->wlt_proxy_enable)
		return 0;

	/*
	 * TODO: make HFI timeout value dynamic so it adjusts to how quickly
	 * states switch
	 */

	/* HFI timer has static polling interval */
	if (hfi_timeout == HFI_TIMEOUT_TIMER)
		return 0;

	/*
	 * Enable polling only if either UTIL is the main state change source or
	 * if WLT is running in polling mode.
	 */
	if (!config->util_monitor && !config->wlt_hint_poll_enable) {
		config->data.polling_interval = -1;
		return 0;
	}

	/* Always start with minimum polling interval for a new state */
	if (idx != current_idx) {
		/* WLT polling manages the polling interval */
		if (config->wlt_hint_poll_enable && !config->util_monitor)
			config->data.polling_interval = DEF_POLLING_INTERVAL;
		else /* UTIL manages the polling interval */
			config->data.polling_interval = state->min_poll_interval;
		return 0;
	}

	/* CPU utilization based adaptive polling */
	if (state->poll_interval_increment == -1) {
		config->data.polling_interval =
			state->max_poll_interval * (10000 - config->data.util_cpu) / 10000;
		config->data.polling_interval /= 100;
		config->data.polling_interval *= 100;
		goto end;
	}

	/* lazy polling if load is sustained */
	if (state->poll_interval_increment > 0)
		config->data.polling_interval += state->poll_interval_increment;

end:
	/* Adjust based on min/max limitation */
	if (config->data.polling_interval < state->min_poll_interval)
		config->data.polling_interval = state->min_poll_interval;
	if (config->data.polling_interval > state->max_poll_interval)
		config->data.polling_interval = state->max_poll_interval;
	return 0;
}

static void dump_state(struct lpmd_config_state_t *state, char *str, int debug)
{
#define DUMP_STATE_BUF_SIZE	512
	char buf[DUMP_STATE_BUF_SIZE];
	char *cpus;
	int offset = 0;

	if (debug && !in_debug_mode())
		return;

	offset += snprintf(buf + offset, DUMP_STATE_BUF_SIZE - offset,
			   "[%6s] [%s] [%s]: ", str,
			   lpmd_state_name[lpmd_state], state->name);

	if (state->wlt_type)
		offset += snprintf(buf + offset, DUMP_STATE_BUF_SIZE - offset,
				   "WLT [%2d] ", state->wlt_type);

	if (state->wlt_type_mask)
		offset += snprintf(buf + offset, DUMP_STATE_BUF_SIZE - offset, "WLTMASK [%2d] ", state->wlt_type_mask);

	if (state->entry_system_load_thres)
		offset += snprintf(buf + offset, DUMP_STATE_BUF_SIZE - offset,
				   "SYS [%6d] ",
				   state->entry_system_load_thres / 100);

	if (state->enter_cpu_load_thres)
		offset += snprintf(buf + offset, DUMP_STATE_BUF_SIZE - offset,
				   "CPU [%6d] ",
				   state->enter_cpu_load_thres / 100);

	if (state->enter_gfx_load_thres)
		offset += snprintf(buf + offset, DUMP_STATE_BUF_SIZE - offset,
				   "GFX [%6d] ",
				   state->enter_gfx_load_thres / 100);

	cpus = get_cpus_str(state->cpumask_idx, false);
	offset += snprintf(buf + offset, DUMP_STATE_BUF_SIZE - offset,
			   "CPUMASK [%d:%s] ", state->cpumask_idx,
			   cpus ? cpus : "?");
	offset += snprintf(buf + offset, DUMP_STATE_BUF_SIZE - offset,
			   "IRQ [%d] ", state->irq_migrate);
	offset += snprintf(buf + offset, DUMP_STATE_BUF_SIZE - offset,
			   "ITMT [%d] ", state->itmt_state);
	offset += snprintf(buf + offset, DUMP_STATE_BUF_SIZE - offset,
			   "EPB [%d] ", state->epb);
	offset += snprintf(buf + offset, DUMP_STATE_BUF_SIZE - offset,
			   "EPP [%d] ", state->epp);
	offset += snprintf(buf + offset, DUMP_STATE_BUF_SIZE - offset,
			   "MIN_PERF_PCT_AC [%d] ", state->min_perf_pct_ac);
	offset += snprintf(buf + offset, DUMP_STATE_BUF_SIZE - offset,
			   "MIN_PERF_PCT_DC [%d] ", state->min_perf_pct_dc);
	offset += snprintf(buf + offset, DUMP_STATE_BUF_SIZE - offset,
			   "MAX_PERF_PCT_AC [%d] ", state->max_perf_pct_ac);
	offset += snprintf(buf + offset, DUMP_STATE_BUF_SIZE - offset,
			   "MAX_PERF_PCT_DC [%d] ", state->max_perf_pct_dc);
	offset += snprintf(buf + offset, DUMP_STATE_BUF_SIZE - offset,
			   "SliderAC [%d] ", state->balance_slider_ac);
	offset += snprintf(buf + offset, DUMP_STATE_BUF_SIZE - offset,
			   "SliderDC [%d] ", state->balance_slider_dc);
	offset += snprintf(buf + offset, DUMP_STATE_BUF_SIZE - offset,
			   "OffsetAC [%d] ", state->slider_offset_ac);
	offset += snprintf(buf + offset, DUMP_STATE_BUF_SIZE - offset,
			   "OffsetDC [%d] ", state->slider_offset_dc);

	if (debug)
		lpmd_log_debug("%s\n", buf);
	else
		lpmd_log_info("%s\n", buf);
#undef DUMP_STATE_BUF_SIZE
}

static int choose_next_state(struct lpmd_config_t *config)
{
	int i;

	switch (lpmd_state) {
	case LPMD_ON:
		return DEFAULT_ON;
	case LPMD_OFF:
	case LPMD_PROCESS_PRECONFIG:
	case LPMD_TERMINATE:
		return DEFAULT_OFF;
	}

	/*
	 * DEFAULT_HFI is enabled only if HFI monitor is enabled
	 * and there is no user config states defined in the config file
	 */
	if (config->config_states[DEFAULT_HFI].valid)
		return DEFAULT_HFI;

	/* Choose a config state */
	for (i = CONFIG_STATE_BASE; i < CONFIG_STATE_BASE + config->config_state_count; ++i) {
		if (config_state_match(config, i)) {
			dump_state(&config->config_states[i], "Choose", 1);
			return i;
		}
		dump_state(&config->config_states[i], "Ignore", 1);
	}

	return STATE_NONE;
}

static int get_state_interval(struct lpmd_config_t *config, int idx)
{
	switch (idx) {
	case DEFAULT_ON:
	case DEFAULT_OFF:
	case DEFAULT_HFI:
		config->data.polling_interval = -1;
		return 0;
	default:
		get_config_state_interval(config, idx);
		return 0;
	}
}

static int need_enter(struct lpmd_config_t *config, int idx)
{
	if (idx != current_idx) {
		update_reason(UPDATE_STATE);
		return 1;
	}
	if (config->data.need_update & (1 << UPDATE_HFI))
		return 1;

	return 0;
}

/*
 * Evaluate the best override class based on current attached processes.
 * Returns the cpumask_idx and class name of the best override, or CPUMASK_NONE
 * if no override class has attached processes.
 */
static enum cpumask_idx evaluate_best_override_state(
	const struct lpmd_config_t *config,
	const char **best_class_out,
	int *best_cpu_count_out)
{
	int best_cpu_count = 0;
	enum cpumask_idx best_override_idx = CPUMASK_NONE;
	const char *best_override_class = NULL;
	int i;

	for (i = 0; i < config->override_classes_count; i++) {
		if (config->override_classes[i].cpumask_idx == CPUMASK_NONE)
			continue;
		if (!lpmd_process_cpuset_class_has_attached(
		    config->override_classes[i].class_name))
			continue;

		if (config->override_classes[i].cpu_count > best_cpu_count) {
			best_cpu_count = config->override_classes[i].cpu_count;
			best_override_idx = config->override_classes[i].cpumask_idx;
			best_override_class = config->override_classes[i].class_name;
		}
	}

	if (best_class_out)
		*best_class_out = best_override_class;
	if (best_cpu_count_out)
		*best_cpu_count_out = best_cpu_count;

	return best_override_idx;
}

/*
 * Check if the current override is still valid, and update to a new one if needed.
 * This is called periodically while in a state to detect when processes attach/detach.
 * Returns 1 if override changed, 0 if no change.
 */
static int update_override_state_if_needed(struct lpmd_config_t *config, int current_state_idx)
{
	enum cpumask_idx new_override_idx = CPUMASK_NONE;
	const char *new_override_class = NULL;
	int new_cpu_count = 0;
	struct lpmd_config_state_t *state;
	char cpumask_str[MAX_STR_LENGTH] = {0};

	if (current_state_idx < 0 || current_state_idx >= MAX_STATES)
		return 0;

	state = &config->config_states[current_state_idx];
	if (!state->valid)
		return 0;

	/* Evaluate best override based on current attached processes */
	new_override_idx = evaluate_best_override_state(config, &new_override_class,
							&new_cpu_count);

	/* Check if override changed */
	if (new_override_idx != config->current_override_idx) {
		config->current_override_idx = new_override_idx;
		config->current_override_class = new_override_class;
		config->current_override_cpu_count = new_cpu_count;

		if (new_override_idx != CPUMASK_NONE) {
			/* Override changed to a different class */
			state->cpumask_idx = new_override_idx;
			snprintf(cpumask_str, sizeof(cpumask_str), "%s",
				 get_cpus_hexstr(state->cpumask_idx, false));
			lpmd_log_info(
				"state %s: override switched to class=%s (cpus=%d) CPUMASK [%s]",
				state->name, new_override_class, new_cpu_count, cpumask_str);
			/* Re-apply cpumask to system with new override */
			process_cgroup(config, state);
		} else {
			/* Override deactivated - restore to original state cpumask */
			state->cpumask_idx = config->saved_state_cpumask_idx;
			snprintf(cpumask_str, sizeof(cpumask_str), "%s",
				 get_cpus_hexstr(state->cpumask_idx, false));
			lpmd_log_info(
				"state %s: override deactivated, cpumask restored to [%s] (no classes with attached processes)",
				state->name, cpumask_str);
			/* Re-apply restored cpumask to system */
			process_cgroup(config, state);
		}
		return 1;
	}

	return 0;
}

static int enter_state(struct lpmd_config_t *config, int idx)
{
	struct lpmd_config_state_t *state = &config->config_states[idx];
	enum cpumask_idx save_cpumask_idx = CPUMASK_NONE;
	int override_active = 0;
	enum cpumask_idx best_override_idx = CPUMASK_NONE;
	const char *best_override_class = NULL;
	int best_cpu_count = 0;

	state->entry_load_sys = config->data.util_sys;
	state->entry_load_cpu = config->data.util_cpu;
	state->entry_load_gfx = config->data.util_gfx;

	/*
	 * Some changes shouldn't be applied if non-state changing updates are
	 * queued:
	 * 	- HFI manages only cgroups so there is no need to reapply all
	 * 	  other settings.
	 */
	if (config->data.need_update & (1 << UPDATE_STATE)) {
		process_slider(config, state);
		process_itmt(state);
		process_epp_epb(state);
		process_irq(state);
	}

	/* Save original cpumask for potential override deactivation later */
	config->saved_state_cpumask_idx = state->cpumask_idx;

	/* Check all override classes: pick one with attached processes + most CPUs. */
	best_override_idx = evaluate_best_override_state(config, &best_override_class,
							 &best_cpu_count);

	if (best_override_idx != CPUMASK_NONE) {
		save_cpumask_idx = state->cpumask_idx;
		state->cpumask_idx = best_override_idx;
		override_active = 1;

		/* Update runtime tracking */
		config->current_override_idx = best_override_idx;
		config->current_override_class = best_override_class;
		config->current_override_cpu_count = best_cpu_count;

		lpmd_log_info(
			"state %s: override active from class=%s (cpus=%d)",
			state->name, best_override_class, best_cpu_count);
	} else {
		/* No override active now */
		if (config->current_override_idx != CPUMASK_NONE) {
			lpmd_log_info(
				"state %s: override deactivated (no classes with attached processes)",
				state->name);
		}
		config->current_override_idx = CPUMASK_NONE;
		config->current_override_class = NULL;
		config->current_override_cpu_count = 0;
	}

	process_epp_epb(state);
	process_min_perf_pct(state);
	process_max_perf_pct(state);

	process_slider(config, state);

	process_irq(state);

	process_cgroup(config, state);

	/* Restore original cpumask after state processing. */
	if (override_active)
		state->cpumask_idx = save_cpumask_idx;

	return 0;
}

static void dump_data(struct lpmd_config_t *config, int idx)
{
	struct lpmd_config_state_t *state = &config->config_states[idx];
	char buf[MAX_STR_LENGTH];
	int epp, epb, ret;
	char epp_str[32];
	int offset = 0;

	if (!in_debug_mode())
		return;

	offset += snprintf(buf + offset, MAX_STR_LENGTH - offset,
			   "[  Data] [%s] [%s]: ", lpmd_state_name[lpmd_state],
			   state->name);

	if (config->wlt_hint_enable)
		offset += snprintf(buf + offset, MAX_STR_LENGTH - offset,
				   "WLT [%2d] ", config->data.wlt_hint);

	if (config->util_sys_enable) {
		if (config->data.util_sys == -1)
			offset += snprintf(buf + offset,
					   MAX_STR_LENGTH - offset,
					   "SYS [   N/A] ");
		else
			offset += snprintf(buf + offset,
					   MAX_STR_LENGTH - offset,
					   "SYS [%3d.%02d] ",
					   config->data.util_sys / 100,
					   config->data.util_sys % 100);
	}

	if (config->util_cpu_enable) {
		if (config->data.util_cpu == -1)
			offset += snprintf(buf + offset,
					   MAX_STR_LENGTH - offset,
					   "CPU [   N/A] ");
		else
			offset += snprintf(buf + offset,
					   MAX_STR_LENGTH - offset,
					   "CPU [%3d.%02d] ",
					   config->data.util_cpu / 100,
					   config->data.util_cpu % 100);
	}

	if (config->util_gfx_enable) {
		if (config->data.util_gfx == -1)
			offset += snprintf(buf + offset,
					   MAX_STR_LENGTH - offset,
					   "GFX [   N/A] ");
		else
			offset += snprintf(buf + offset,
					   MAX_STR_LENGTH - offset,
					   "GFX [%3d.%02d] ",
					   config->data.util_gfx / 100,
					   config->data.util_gfx % 100);
	}

	if (state->cpumask_idx != CPUMASK_NONE)
		offset += snprintf(buf + offset, MAX_STR_LENGTH - offset,
				   "CPUMASK [%s] ",
				   get_cpus_hexstr(state->cpumask_idx, false));
	else
		offset += snprintf(buf + offset, MAX_STR_LENGTH - offset,
				   "CPUMASK [%s] ",
				   get_cpus_hexstr(CPUMASK_ONLINE, false));

	offset += snprintf(buf + offset, MAX_STR_LENGTH - offset,
				   "ITMT [%d] ", get_itmt());

	ret = get_epp_epb(&epp, epp_str, 32, &epb);
	if (ret || epp == -1)
		offset += snprintf(buf + offset, MAX_STR_LENGTH - offset,
				   "EPB [%d] EPP[%s] ", epb, epp_str);
	else
		offset += snprintf(buf + offset, MAX_STR_LENGTH - offset,
				   "EPB [%d] EPP[%d] ", epb, epp);

	if (state->min_perf_pct_ac != SETTING_IGNORE)
		offset += snprintf(buf + offset, MAX_STR_LENGTH - offset,
				   "MIN_PERF_PCT_AC [%d] ", state->min_perf_pct_ac);

	if (state->min_perf_pct_dc != SETTING_IGNORE)
		offset += snprintf(buf + offset, MAX_STR_LENGTH - offset,
				   "MIN_PERF_PCT_DC [%d] ", state->min_perf_pct_dc);

	if (state->max_perf_pct_ac != SETTING_IGNORE)
		offset += snprintf(buf + offset, MAX_STR_LENGTH - offset,
				   "MAX_PERF_PCT_AC [%d] ", state->max_perf_pct_ac);

	if (state->max_perf_pct_dc != SETTING_IGNORE)
		offset += snprintf(buf + offset, MAX_STR_LENGTH - offset,
				   "MAX_PERF_PCT_DC [%d] ", state->max_perf_pct_dc);

	if (config->hfi_lpm_enable)
		offset += snprintf(buf + offset, MAX_STR_LENGTH - offset,
				   "UPDATE [%d] ", config->data.need_update);

	offset += snprintf(buf + offset, MAX_STR_LENGTH - offset,
			   "Interval [%d]", config->data.polling_interval);

	lpmd_log_debug("%s\n", buf);
}

int lpmd_enter_next_state(void)
{
	struct lpmd_config_t *config = get_lpmd_config();
	int idx = current_idx;

	lpmd_lock();

	if (lpmd_state == LPMD_FREEZE) {
		/* Wait till RESTORE */
		config->data.polling_interval = -1;
		goto end;
	}

	idx = choose_next_state(config);

	/*
	 * After switching power profiles polling gets disabled and needs to be
	 * updated.
	 */
	if (config->data.polling_interval == -1 &&
	    (config->util_monitor || config->wlt_hint_poll_enable) &&
	    idx != DEFAULT_OFF)
		get_config_state_interval(config, idx);

	/* No action needed, keep previous idx and interval */
	if (idx == STATE_NONE) {
		/* Even if no state transition, check if override needs updating
		 * (e.g., processes attached/detached while in same state) */
		if (current_idx >= 0 && current_idx < MAX_STATES)
			update_override_state_if_needed(config, current_idx);
		goto end;
	}

	get_state_interval(config, idx);

	if (need_enter(config, idx)) {
		enter_state(config, idx);
		current_idx = idx;
		dump_state(&config->config_states[idx], "Enter", 0);
	} else {
		/* Staying in same state, but check if override needs updating */
		update_override_state_if_needed(config, current_idx);
	}

end:
	dump_data(config, current_idx);
	lpmd_unlock();

	return 0;
}

static void dump_states(struct lpmd_config_t *lpmd_config)
{
	int i;
	struct lpmd_config_state_t *state;

	if (!lpmd_config)
		return;

	lpmd_log_info("Mode:%d\n", lpmd_config->mode);
	lpmd_log_info("HFI LPM Enable:%d\n", lpmd_config->hfi_lpm_enable);
	lpmd_log_info("WLT Hint Enable:%d\n", lpmd_config->wlt_hint_enable);
	lpmd_log_info("WLT Hint Notification Delay:%d\n", lpmd_config->wlt_notification_delay);
	lpmd_log_info("WLT Proxy Enable:%d\n", lpmd_config->wlt_proxy_enable);
	lpmd_log_info("WLT Polling Enable:%d\n", lpmd_config->wlt_hint_poll_enable);
	lpmd_log_info("WLT Hint mask:%d\n", lpmd_config->wlt_hint_mask);
	lpmd_log_info("Util Enable:%d\n", lpmd_config->util_monitor);
	lpmd_log_info("Util entry threshold:%d\n", lpmd_config->util_entry_threshold);
	lpmd_log_info("Util exit threshold:%d\n", lpmd_config->util_exit_threshold);
	lpmd_log_info("Util LP Mode CPUs:%s\n", lpmd_config->lp_mode_cpus);
	lpmd_log_info("EPP in LP Mode:%d\n", lpmd_config->lp_mode_epp);
	lpmd_log_info("CPU Family:%d\n", lpmd_config->cpu_family);
	lpmd_log_info("CPU Model:%d\n", lpmd_config->cpu_model);
	lpmd_log_info("CPU Config:%s\n", lpmd_config->cpu_config);

	lpmd_log_info("balance_slider_def_ac:%d\n", lpmd_config->balance_slider_def_ac);
	lpmd_log_info("balance_slider_def_dc:%d\n", lpmd_config->balance_slider_def_dc);
	lpmd_log_info("slider_offset_def_ac:%d\n", lpmd_config->slider_offset_def_ac);
	lpmd_log_info("slider_offset_def_dc:%d\n", lpmd_config->slider_offset_def_dc);

	for (i = 0; i < MAX_STATES; ++i) {
		state = &lpmd_config->config_states[i];

		if (!state->valid)
			continue;

		lpmd_log_info("Index:%d\n", i);
		lpmd_log_info("\tID:%d\n", state->id);
		lpmd_log_info("\tName:%s\n", state->name);
		lpmd_log_info("\tentry_system_load_thres:%d\n", state->entry_system_load_thres);
		lpmd_log_info("\texit_system_load_thres:%d\n", state->exit_system_load_thres);
		lpmd_log_info("\texit_system_load_hyst:%d\n", state->exit_system_load_hyst);
		lpmd_log_info("\tentry_cpu_load_thres:%d\n", state->enter_cpu_load_thres);
		lpmd_log_info("\texit_cpu_load_thres:%d\n", state->exit_cpu_load_thres);
		lpmd_log_info("\tentry_gfx_load_thres:%d\n", state->enter_gfx_load_thres);
		lpmd_log_info("\texit_gfx_load_thres:%d\n", state->exit_gfx_load_thres);
		lpmd_log_info("\texit_gfx_load_hyst:%d\n", state->exit_gfx_load_hyst);
		lpmd_log_info("\tWLT Type:%d\n", state->wlt_type);
		lpmd_log_info("\tWLT Type Mask:%d\n", state->wlt_type_mask);
		lpmd_log_info("\tmin_poll_interval:%d\n", state->min_poll_interval);
		lpmd_log_info("\tmax_poll_interval:%d\n", state->max_poll_interval);
		lpmd_log_info("\tpoll_interval_increment:%d\n", state->poll_interval_increment);
		lpmd_log_info("\tEPP:%d\n", state->epp);
		lpmd_log_info("\tEPB:%d\n", state->epb);
		lpmd_log_info("\tMinPerfPctAC:%d\n", state->min_perf_pct_ac);
		lpmd_log_info("\tMinPerfPctDC:%d\n", state->min_perf_pct_dc);
		lpmd_log_info("\tMaxPerfPctAC:%d\n", state->max_perf_pct_ac);
		lpmd_log_info("\tMaxPerfPctDC:%d\n", state->max_perf_pct_dc);
		lpmd_log_info("\tITMTState:%d\n", state->itmt_state);
		lpmd_log_info("\tIRQMigrate:%d\n", state->irq_migrate);
		if (state->active_cpus[0] != '\0')
			lpmd_log_info("\tactive_cpus:%s\n", state->active_cpus);
		if (state->active_p_cores[0] != '\0')
			lpmd_log_info("\tactive_p_cores:%s\n", state->active_p_cores);
		if (state->active_e_cores[0] != '\0')
			lpmd_log_info("\tactive_e_cores:%s\n", state->active_e_cores);
		if (state->active_l_cores[0] != '\0')
			lpmd_log_info("\tactive_l_cores:%s\n", state->active_l_cores);
		lpmd_log_info("\tCPUMASK idx:%d\n", state->cpumask_idx);
		lpmd_log_info("\tBalancedSliderAC:%d\n", state->balance_slider_ac);
		lpmd_log_info("\tBalancedSliderDC:%d\n", state->balance_slider_dc);
		lpmd_log_info("\tSliderOffsetAC:%d\n", state->slider_offset_ac);
		lpmd_log_info("\tSliderOffsetDC:%d\n", state->slider_offset_dc);
	}
}

static int build_default_states(struct lpmd_config_t *config)
{
	struct lpmd_config_state_t *state;

	state = &config->config_states[DEFAULT_OFF];
	lpmd_init_config_state(state);
	state->id = -1;
	snprintf(state->name, MAX_STATE_NAME, "DEFAULT_OFF");
	state->itmt_state = SETTING_RESTORE;
	state->irq_migrate = SETTING_RESTORE;
	state->epp = SETTING_RESTORE;
	state->epb = SETTING_RESTORE;
	state->cpumask_idx = CPUMASK_ONLINE;
	state->valid = 1;

	state = &config->config_states[DEFAULT_ON];
	lpmd_init_config_state(state);
	state->id = -1;
	snprintf(state->name, MAX_STATE_NAME, "DEFAULT_ON");
	state->itmt_state = config->ignore_itmt ? SETTING_IGNORE : 0;
	state->irq_migrate = 1;
	state->epp = config->lp_mode_epp;
	state->epb = SETTING_IGNORE;
	state->cpumask_idx = CPUMASK_LPM_DEFAULT;
	state->valid = 1;

	if (config->config_state_count)
		return 0;

	/*
	 * When HFI monitor is enabled and config states are not used,
	 * Switch system with different CPU affinity based on HFI hints
	 */
	if (config->hfi_lpm_enable) {
		state = &config->config_states[DEFAULT_HFI];
		lpmd_init_config_state(state);
		state->id = -1;
		snprintf(state->name, MAX_STATE_NAME, "DEFAULT_HFI");
		state->itmt_state = SETTING_IGNORE;
		state->irq_migrate = SETTING_IGNORE;
		state->epp = SETTING_IGNORE;
		state->epb = SETTING_IGNORE;
		state->cpumask_idx = CPUMASK_HFI;
		state->valid = 1;

		return 0;
	}

	/*
	 * When HFI monitor is not enabled and config states are not used,
	 * Switch system following global setting based on utilization.
	 */
	state = &config->config_states[CONFIG_STATE_BASE];
	lpmd_init_config_state(state);
	state->id = 1;
	snprintf(state->name, MAX_STATE_NAME, "UTIL_POWER");
	state->entry_system_load_thres = config->util_entry_threshold;
	state->enter_cpu_load_thres = config->util_exit_threshold;
	state->itmt_state = config->ignore_itmt ? SETTING_IGNORE : 0;
	state->irq_migrate = 1;
	state->min_poll_interval = 100;
	state->max_poll_interval = 1000;
	state->poll_interval_increment = -1;
	state->epp = config->lp_mode_epp;
	state->epb = SETTING_IGNORE;
	state->cpumask_idx = CPUMASK_LPM_DEFAULT;
	state->valid = 1;

	state = &config->config_states[CONFIG_STATE_BASE + 1];
	lpmd_init_config_state(state);
	state->id = 2;
	snprintf(state->name, MAX_STATE_NAME, "UTIL_PERF");
	state->entry_system_load_thres = 100;
	state->enter_cpu_load_thres = 100;
	state->itmt_state = config->ignore_itmt ? SETTING_IGNORE : SETTING_RESTORE;
	state->irq_migrate = 1;
	state->min_poll_interval = 1000;
	state->max_poll_interval = 1000;
	state->epp = config->lp_mode_epp == SETTING_IGNORE ? SETTING_IGNORE : SETTING_RESTORE;
	state->epb = SETTING_IGNORE;
	state->cpumask_idx = CPUMASK_ONLINE;
	state->valid = 1;

	config->config_state_count = 2;
	return 0;
}

static int config_states_update_config(struct lpmd_config_t *config)
{
	struct lpmd_config_state_t *state;
	int i;

	for (i = CONFIG_STATE_BASE; i < CONFIG_STATE_BASE + config->config_state_count; i++) {
		state = &config->config_states[i];

		if (!state->valid)
			continue;

		if (state->cpumask_idx == CPUMASK_HFI)
			config->hfi_lpm_enable = 1;

		if (state->wlt_type != -1 || state->wlt_type_mask != -1)
			config->wlt_hint_enable = 1;

		if (state->entry_system_load_thres)
			config->util_sys_enable = 1;

		if (state->enter_cpu_load_thres)
			config->util_cpu_enable = 1;

		if (state->enter_gfx_load_thres)
			config->util_gfx_enable = 1;
	}
	return 0;
}

/*
 * Return 0 on success - either when nothing requires setting up Return -1 on error
 * and -2 if there are no active cpus specified.
 */
static int build_state_cpumask_activecpus(struct lpmd_config_t *lpmd_config,
					  struct lpmd_config_state_t *state)
{
	if (state->cpumask_idx != CPUMASK_NONE)
		return 0;

	if (state->active_cpus[0] == '\0')
		return -2;

	if (!strcmp(state->active_cpus, "all") ||
	    !strcmp(state->active_cpus, "ALL") ||
	    is_wildcard(state->active_cpus)) {
		state->cpumask_idx = CPUMASK_ONLINE;
		return 0;
	}

	if (!strcmp(state->active_cpus, "lp") ||
	    !strcmp(state->active_cpus, "LP")) {
		state->cpumask_idx = CPUMASK_LPM_DEFAULT;
		return 0;
	}

	if (!strcmp(state->active_cpus, "hfi") ||
	    !strcmp(state->active_cpus, "HFI")) {
		state->cpumask_idx = CPUMASK_HFI;
		lpmd_config->config_states_hfi = true;
		return 0;
	}

	state->cpumask_idx = cpumask_alloc();
	if (state->cpumask_idx == CPUMASK_NONE) {
		lpmd_log_error("Cannot alloc CPUMASK\n");
		return -1;
	}

	if (cpumask_init_cpus(state->active_cpus, state->cpumask_idx) <= 0) {
		cpumask_free(state->cpumask_idx);
		lpmd_log_error("Cannot parse cpumask string: %s\n", state->active_cpus);
		return -1;
	}

	return 0;
}

static int build_state_cpumask_cputypes(struct lpmd_config_state_t *state, unsigned char **cmasks)
{
	int ret;

	if (state->cpumask_idx != CPUMASK_NONE)
		return 0;

	state->cpumask_idx = cpumask_alloc();
	if (state->cpumask_idx == CPUMASK_NONE) {
		lpmd_log_error("Cannot alloc CPUMASK\n");
		return -1;
	}

	/* Setup the specified P-cores */
	ret = cpumask_init_cpus_type(state->active_p_cores, state->cpumask_idx, cmasks, P_CORE);
	if (ret < 0) {
		cpumask_free(state->cpumask_idx);
		return -1;
	}

	/* Setup the specified E-cores */
	ret = cpumask_init_cpus_type(state->active_e_cores, state->cpumask_idx, cmasks, E_CORE);
	if (ret < 0) {
		cpumask_free(state->cpumask_idx);
		return -1;
	}

	/* Setup the specified L-cores */
	ret = cpumask_init_cpus_type(state->active_l_cores, state->cpumask_idx, cmasks, L_CORE);
	if (ret < 0) {
		cpumask_free(state->cpumask_idx);
		return -1;
	}

	return 0;
}

static const char *get_global_cpu_override_class_cores(const struct lpmd_config_t *config,
						 const char **class_name_out)
{
	if (!config)
		return NULL;

	const struct {
		const char *cfg_name;
		const char *cores;
		int enabled;
	} flags[] = {
		{ "Realtime", config->pc_class_default_realtime,
		  config->pc_class_override_global_cpu_realtime },
		{ "UserInteractive", config->pc_class_default_user_interactive,
		  config->pc_class_override_global_cpu_user_interactive },
		{ "UserInitiated", config->pc_class_default_user_initiated,
		  config->pc_class_override_global_cpu_user_initiated },
		{ "Unclassified", config->pc_class_default_unclassified,
		  config->pc_class_override_global_cpu_unclassified },
		{ "Utility", config->pc_class_default_utility,
		  config->pc_class_override_global_cpu_utility },
		{ "Background", config->pc_class_default_background,
		  config->pc_class_override_global_cpu_background },
		{ "GameProfileCPU", config->pc_class_default_gp_cpu,
		  config->pc_class_override_global_cpu_gp_cpu },
		{ "GameProfileGPU", config->pc_class_default_gp_gpu,
		  config->pc_class_override_global_cpu_gp_gpu },
		{ "GameProfileMixed", config->pc_class_default_gp_hybrid,
		  config->pc_class_override_global_cpu_gp_hybrid },
		{ "CustomProfile0", config->pc_class_default_custom_profile_0,
		  config->pc_class_override_global_cpu_custom_profile_0 },
		{ "CustomProfile1", config->pc_class_default_custom_profile_1,
		  config->pc_class_override_global_cpu_custom_profile_1 },
		{ "CustomProfile2", config->pc_class_default_custom_profile_2,
		  config->pc_class_override_global_cpu_custom_profile_2 },
	};
	const char *selected_cores = NULL;
	const char *selected_name = NULL;
	size_t i;

	for (i = 0; i < sizeof(flags) / sizeof(flags[0]); i++) {
		if (!flags[i].enabled)
			continue;

		if (!selected_cores) {
			selected_cores = flags[i].cores;
			selected_name = flags[i].cfg_name;
			continue;
		}

		lpmd_log_warn(
			"global cpu override: multiple OverrideGlobalCPUSettings tags enabled (%s, %s); using %s\n",
			selected_name, flags[i].cfg_name, selected_name);
	}

	if (class_name_out)
		*class_name_out = selected_name;

	return selected_cores;
}

static int build_global_override_cpumask_idx(struct lpmd_config_t *config,
					     const char *cores_spec,
					     enum cpumask_idx *idx_out)
{
	char literal_cpus[MAX_STR_LENGTH];
	char token[MAX_STR_LENGTH];
	const char *p;
	int use_p = 0, use_e = 0, use_l = 0;
	enum cpumask_idx idx;
	int ret;

	if (!config || !cores_spec || !cores_spec[0] || !idx_out)
		return -1;

	idx = cpumask_alloc();
	if (idx == CPUMASK_NONE)
		return -1;

	literal_cpus[0] = '\0';
	p = cores_spec;
	while (*p) {
		size_t tlen;

		while (*p && (isspace((unsigned char)*p) || *p == ',' || *p == '|'))
			p++;
		if (!*p)
			break;

		tlen = 0;
		while (p[tlen] && !isspace((unsigned char)p[tlen]) && p[tlen] != ',' &&
		       p[tlen] != '|') {
			if (tlen + 1 < sizeof(token))
				token[tlen] = p[tlen];
			tlen++;
		}

		if (tlen >= sizeof(token))
			tlen = sizeof(token) - 1;
		token[tlen] = '\0';

		if (!strcasecmp(token, "ActivePcores")) {
			use_p = 1;
		} else if (!strcasecmp(token, "ActiveEcores")) {
			use_e = 1;
		} else if (!strcasecmp(token, "ActiveLcores")) {
			use_l = 1;
		} else {
			size_t cur = strlen(literal_cpus);
			if (cur && cur + 1 < sizeof(literal_cpus))
				literal_cpus[cur++] = ',';
			if (cur + tlen < sizeof(literal_cpus)) {
				memcpy(literal_cpus + cur, token, tlen);
				literal_cpus[cur + tlen] = '\0';
			}
		}

		p += tlen;
	}

	if (use_p) {
		ret = cpumask_init_cpus_type("all", idx,
					    config->core_type_masks, P_CORE);
		if (ret < 0)
			goto err;
	}

	if (use_e) {
		ret = cpumask_init_cpus_type("all", idx,
					    config->core_type_masks, E_CORE);
		if (ret < 0)
			goto err;
	}

	if (use_l) {
		ret = cpumask_init_cpus_type("all", idx,
					    config->core_type_masks, L_CORE);
		if (ret < 0)
			goto err;
	}

	if (literal_cpus[0]) {
		ret = cpumask_init_cpus(literal_cpus, idx);
		if (ret < 0)
			goto err;
	}

	if (!cpumask_has_cpu(idx))
		goto err;

	*idx_out = idx;
	return 0;

err:
	cpumask_free(idx);
	return -1;
}



#define DEFAULT_POLL_RATE_MS	1000

int lpmd_build_config_states(struct lpmd_config_t *lpmd_config)
{
	struct lpmd_config_state_t *state;
	int ret, i;

	build_default_states(lpmd_config);

	for (i = CONFIG_STATE_BASE; i < CONFIG_STATE_BASE + lpmd_config->config_state_count; i++) {
		state = &lpmd_config->config_states[i];

		ret = build_state_cpumask_activecpus(lpmd_config, state);
		if (ret == -2)
			build_state_cpumask_cputypes(state, lpmd_config->core_type_masks);
		else if (ret)
			continue;

		if (state->entry_system_load_thres ||
		    state->enter_cpu_load_thres || state->enter_gfx_load_thres)
			lpmd_config->util_monitor = 1;

		if (state->min_poll_interval <= 0)
			state->min_poll_interval = state->max_poll_interval > DEFAULT_POLL_RATE_MS ?
						   DEFAULT_POLL_RATE_MS : state->max_poll_interval;
		if (state->max_poll_interval <= 0)
			state->max_poll_interval = state->min_poll_interval > DEFAULT_POLL_RATE_MS ?
						   state->min_poll_interval : DEFAULT_POLL_RATE_MS;
		if (state->poll_interval_increment <= 0)
			state->poll_interval_increment = -1;

		if (state->entry_system_load_thres < 0 || state->entry_system_load_thres > 100)
			continue;
		else
			state->entry_system_load_thres *= 100;

		if (state->enter_cpu_load_thres < 0 || state->enter_cpu_load_thres > 100)
			continue;
		else
			state->enter_cpu_load_thres *= 100;

		if (state->exit_cpu_load_thres < 0 || state->exit_cpu_load_thres > 100)
			continue;
		else
			state->exit_cpu_load_thres *= 100;

		if (state->enter_gfx_load_thres < 0 || state->enter_gfx_load_thres > 100)
			continue;
		else
			state->enter_gfx_load_thres *= 100;

		state->valid = 1;
	}

/* Scan all classes for OverrideGlobalCPUSettings and store info for runtime. */
	{
		const struct {
			const char *cfg_name;
			const char *class_name;
			int enabled;
			const char *cores;
		} classes[] = {
			{ "Realtime", "realtime", lpmd_config->pc_class_override_global_cpu_realtime, lpmd_config->pc_class_default_realtime },
			{ "UserInteractive", "user_interactive", lpmd_config->pc_class_override_global_cpu_user_interactive, lpmd_config->pc_class_default_user_interactive },
			{ "UserInitiated", "user_initiated", lpmd_config->pc_class_override_global_cpu_user_initiated, lpmd_config->pc_class_default_user_initiated },
			{ "Unclassified", "unclassified", lpmd_config->pc_class_override_global_cpu_unclassified, lpmd_config->pc_class_default_unclassified },
			{ "Utility", "utility", lpmd_config->pc_class_override_global_cpu_utility, lpmd_config->pc_class_default_utility },
			{ "Background", "background", lpmd_config->pc_class_override_global_cpu_background, lpmd_config->pc_class_default_background },
			{ "GameProfileCPU", "game_profile_cpu", lpmd_config->pc_class_override_global_cpu_gp_cpu, lpmd_config->pc_class_default_gp_cpu },
			{ "GameProfileGPU", "game_profile_gpu", lpmd_config->pc_class_override_global_cpu_gp_gpu, lpmd_config->pc_class_default_gp_gpu },
			{ "GameProfileMixed", "game_profile_mixed", lpmd_config->pc_class_override_global_cpu_gp_hybrid, lpmd_config->pc_class_default_gp_hybrid },
			{ "CustomProfile0", "custom_profile_0", lpmd_config->pc_class_override_global_cpu_custom_profile_0, lpmd_config->pc_class_default_custom_profile_0 },
			{ "CustomProfile1", "custom_profile_1", lpmd_config->pc_class_override_global_cpu_custom_profile_1, lpmd_config->pc_class_default_custom_profile_1 },
			{ "CustomProfile2", "custom_profile_2", lpmd_config->pc_class_override_global_cpu_custom_profile_2, lpmd_config->pc_class_default_custom_profile_2 },
		};
		int i;
		lpmd_config->override_classes_count = 0;

		for (i = 0; i < sizeof(classes) / sizeof(classes[0]); i++) {
			if (!classes[i].enabled || !classes[i].cores || !classes[i].cores[0])
				continue;

			enum cpumask_idx idx = cpumask_alloc();
			if (idx == CPUMASK_NONE)
				continue;

			if (build_global_override_cpumask_idx(lpmd_config, classes[i].cores, &idx) < 0) {
				cpumask_free(idx);
				continue;
			}

			int cpu_count = cpumask_nr_cpus(idx);
			if (cpu_count <= 0) {
				cpumask_free(idx);
				continue;
			}

			lpmd_config->override_classes[lpmd_config->override_classes_count].class_name = classes[i].class_name;
			lpmd_config->override_classes[lpmd_config->override_classes_count].cpumask_idx = idx;
			lpmd_config->override_classes[lpmd_config->override_classes_count].cpu_count = cpu_count;
			lpmd_config->override_classes_count++;

			lpmd_log_info(
				"global cpu override: stored class=%s cpus=%d, will apply if processes attached",
				classes[i].class_name, cpu_count);
		}
	}

	config_states_update_config(lpmd_config);
	dump_states(lpmd_config);

	return 0;
}
