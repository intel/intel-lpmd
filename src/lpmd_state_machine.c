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

/* for lpmd state control: ON/OFF/AUTO/FREEZE/RESTORE/TERMINATE */
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
	
	set_lpm_epp(state->epp);
	set_lpm_epb(state->epb);
	set_lpm_itmt(state->itmt_state);

	if (state->cpumask_idx != CPUMASK_NONE) {
		if (state->irq_migrate != SETTING_IGNORE)
			set_lpm_irq(state->cpumask_idx);
		else
			set_lpm_irq(SETTING_IGNORE);
		set_lpm_cpus(state->cpumask_idx);
	} else {
		set_lpm_irq(SETTING_IGNORE);
		set_lpm_cpus(CPUMASK_NONE); /* Ignore Task migration */
        }

	process_itmt();

	process_irqs (1, get_cpu_mode ());

	process_cpus (1, get_cpu_mode ());

end:
	return ret;
}

int enter_next_state(void)
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


