/*
 * lpmd_state_machine.c: intel_lpmd state machine
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
#include <pthread.h>

#include "lpmd.h"

/* LPMD state control: ON/OFF/AUTO/FREEZE/RESTORE/TERMINATE */
static int lpmd_state = LPMD_OFF;
static int saved_lpmd_state = LPMD_OFF;

int update_lpmd_state(int new)
{
	lpmd_lock();
	switch (new) {
		case LPMD_FREEZE:
			if (lpmd_state == LPMD_FREEZE)
				break;
			saved_lpmd_state = lpmd_state;
			lpmd_state == LPMD_FREEZE;
			break;
		case LPMD_RESTORE:
			if (lpmd_state != LPMD_FREEZE)
				break;
			lpmd_state == saved_lpmd_state;
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

int lpmd_init_config_state(lpmd_config_state_t *state)
{
	state->id = -1;
	state->valid = 0;
	state->name[0] = '\0';

	state->wlt_type = -1;

	state->entry_system_load_thres = 0;
	state->exit_system_load_thres = 0;
	state->exit_system_load_hyst = 0;
	state->enter_cpu_load_thres = 0;
	state->exit_cpu_load_thres = 0;
	state->enter_gfx_load_thres = 0;
	state->exit_gfx_load_thres = 0;

	state->min_poll_interval = 0;
	state->max_poll_interval = 0;
	state->poll_interval_increment = 0;

	state->epp = SETTING_IGNORE;
	state->epb = SETTING_IGNORE;
	state->active_cpus[0] = '\0';

	state->island_0_number_p_cores = 0;
	state->island_0_number_e_cores = 0;
	state->island_1_number_p_cores = 0;
	state->island_1_number_e_cores = 0;
	state->island_2_number_p_cores = 0;
	state->island_2_number_e_cores = 0;

	state->itmt_state = SETTING_IGNORE;
	state->irq_migrate = SETTING_IGNORE;

	state->entry_load_sys = 0;
	state->entry_load_cpu = 0;
	state->cpumask_idx = CPUMASK_NONE;
	
	return 0;
}

int lpmd_build_default_config_states(lpmd_config_t *config)
{
	lpmd_config_state_t *state;

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
	if (config->hfi_lpm_enable){
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
	
		config->config_state_count = 1;
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

static int current_idx = STATE_NONE;

static int config_state_match(lpmd_config_t *config, int idx)
{
	lpmd_config_state_t *state = &config->config_states[idx];
	int bcpu = config->data.util_cpu;
	int bsys = config->data.util_sys;
	int bgfx = config->data.util_gfx;
	int wlt_index = config->data.wlt_hint;

	if (!state->valid)
		return 0;

	if (state->wlt_type != -1 && state->wlt_type != wlt_index)
		return 0;

	if (state->enter_cpu_load_thres && state->enter_cpu_load_thres < bcpu)
		return 0;

	if (state->enter_gfx_load_thres && state->enter_gfx_load_thres < bgfx)
		return 0;

	if (state->entry_system_load_thres && state->entry_system_load_thres < bsys) {
		if (!state->exit_system_load_hyst)
			return 0;
		if ((state->entry_load_sys + state->exit_system_load_hyst) < bsys || (state->entry_system_load_thres + state->exit_system_load_hyst) < bsys)
			return 0;
	}

	return 1;
}

static int polling_enabled;

static int get_config_state_interval(lpmd_config_t *config, int idx)
{
	lpmd_config_state_t *state = &config->config_states[idx];

	/* Start polling only when needed */
	if (!polling_enabled) {
		config->data.polling_interval = -1;
		return 0;
	}

	/* wlt proxy updates polling separately */
	if (config->wlt_proxy_enable)
		return 0;

	/* Always start with minumum polling interval for a new state */
	if (idx != current_idx) {
		config->data.polling_interval = state->min_poll_interval;
		return 0;
	}

	/* CPU utilization based adaptive polling */
	if (state->poll_interval_increment == -1) {
		config->data.polling_interval = state->max_poll_interval * (10000 - config->data.util_cpu) / 10000;
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

static int choose_next_state(lpmd_config_t *config)
{
	int i;

	switch (lpmd_state) {
		case LPMD_ON:
			return DEFAULT_ON;
		case LPMD_OFF:
		case LPMD_TERMINATE:
			return DEFAULT_OFF;
	}

	/*
	 * DEFAULT_HFI is enabled only if HFI monitor is enabled
	 * and there is no user config states defined in the config file
	 */
	if (config->config_states[DEFAULT_HFI].valid) {
		if (config->data.has_hfi_update)
			return DEFAULT_HFI;
		else
			return STATE_NONE;
	}

	/* Choose a config state */
	for (i = CONFIG_STATE_BASE; i < CONFIG_STATE_BASE + config->config_state_count; ++i) {
		if (config_state_match(config, i))
			return i;
	}

	return STATE_NONE;
}

static int get_state_interval(lpmd_config_t *config, int idx)
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

static int enter_state(lpmd_config_t *config, int idx)
{
	lpmd_config_state_t *state = &config->config_states[idx];
	int ret;

	state->entry_load_sys = config->data.util_sys;
	state->entry_load_cpu = config->data.util_cpu;
	
	if (state->cpumask_idx != CPUMASK_NONE) {
		set_lpm_cpus(state->cpumask_idx);
	} else {
		set_lpm_cpus(CPUMASK_NONE); /* Ignore Task migration */
        }

	process_itmt(state);
	
	process_epp_epb(state);

	process_irq(state);

	process_cpus (1, get_cpu_mode ());

end:
	return ret;
}

int lpmd_enter_next_state(void)
{
	lpmd_config_t *config = get_lpmd_config();
	int idx = current_idx;

	lpmd_lock();

	if (lpmd_state == LPMD_FREEZE) {
		/* Wait till RESTORE */
		config->data.polling_interval = -1;
		goto end;
	}

	idx = choose_next_state(config);
	/* No action needed, keep previous idx and interval */
	if (idx == STATE_NONE)
		goto end;

	get_state_interval(config, idx);

	enter_state(config, idx);

	current_idx = idx;
	config->data.has_hfi_update = 0;

end:
	lpmd_unlock();

	return 0;
}

#define DEFAULT_POLL_RATE_MS	1000

int lpmd_parse_config_states(lpmd_config_t *lpmd_config)
{
	lpmd_config_state_t *state;
	int nr_state = 0;
	int i, ret;

	for (i = 0; i < lpmd_config->config_state_count; i++) {
		state = &lpmd_config->config_states[i];

		if (state->active_cpus[0] != '\0') {
			state->cpumask_idx = alloc_cpumask();
			if (state->cpumask_idx == CPUMASK_NONE) {
				lpmd_log_error("Cannot alloc CPUMASK\n");
				return -1;
			}
			ret = parse_cpu_str(state->active_cpus, state->cpumask_idx);
			if (ret <= 0) {
				state->valid = 0;
				continue;
			}
		}

		if (state->entry_system_load_thres || state->enter_cpu_load_thres || state->enter_gfx_load_thres)
			polling_enabled = 1;

		if (!state->min_poll_interval)
			state->min_poll_interval = state->max_poll_interval > DEFAULT_POLL_RATE_MS ? DEFAULT_POLL_RATE_MS : state->max_poll_interval;
		if (!state->max_poll_interval)
			state->max_poll_interval = state->min_poll_interval > DEFAULT_POLL_RATE_MS ? state->min_poll_interval : DEFAULT_POLL_RATE_MS;
		if (!state->poll_interval_increment)
			state->poll_interval_increment = -1;

		state->entry_system_load_thres *= 100;
		state->enter_cpu_load_thres *= 100;
		state->exit_cpu_load_thres *= 100;
		state->enter_gfx_load_thres *= 100;

		nr_state++;
	}

	if (nr_state < 1) {
		lpmd_log_info("%d valid config states found\n", nr_state);
		return 1;
	}

	return 0;
}
