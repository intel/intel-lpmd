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

static char *lpmd_state_name[] = {
	[LPMD_ON]		= "     ON",
	[LPMD_OFF]		= "    OFF",
	[LPMD_AUTO]		= "   AUTO",
	[LPMD_FREEZE]		= " FREEZE",
	[LPMD_RESTORE]		= "RESTORE",
	[LPMD_TERMINATE]	= "   TERM",
};

int update_lpmd_state(int new)
{
	lpmd_lock();
	switch (new) {
		case LPMD_FREEZE:
			if (lpmd_state == LPMD_FREEZE)
				break;
			lpmd_log_debug ("Freeze lpmd\n");
			saved_lpmd_state = lpmd_state;
			lpmd_state = LPMD_FREEZE;
			break;
		case LPMD_RESTORE:
			if (lpmd_state != LPMD_FREEZE)
				break;
			lpmd_log_debug ("Restore lpmd\n");
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
	state->cpumask_idx = CPUMASK_NONE;

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

static int current_idx = DEFAULT_OFF;

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

static void dump_state(lpmd_config_state_t *state, char *str, int debug)
{
	char buf[MAX_STR_LENGTH];
	int offset = 0;

	if (debug && !in_debug_mode())
		return;

	offset += snprintf(buf + offset , MAX_STR_LENGTH - offset, "[%6s] [%s] [%s]: ", str, lpmd_state_name[lpmd_state], state->name);

	if (state->wlt_type)
		offset += snprintf(buf + offset , MAX_STR_LENGTH - offset, "WLT [%2d] ", state->wlt_type);

	if (state->entry_system_load_thres)
		offset += snprintf(buf + offset , MAX_STR_LENGTH - offset, "SYS [%6d] ", state->entry_system_load_thres / 100);

	if (state->enter_cpu_load_thres)
		offset += snprintf(buf + offset , MAX_STR_LENGTH - offset, "CPU [%6d] ", state->enter_cpu_load_thres / 100);

	if (state->enter_gfx_load_thres)
		offset += snprintf(buf + offset , MAX_STR_LENGTH - offset, "GFX [%6d] ", state->enter_gfx_load_thres / 100);

	offset += snprintf(buf + offset , MAX_STR_LENGTH - offset, "CPUMASK [%d] ", state->cpumask_idx);
	offset += snprintf(buf + offset , MAX_STR_LENGTH - offset, "IRQ [%d] ", state->irq_migrate);
	offset += snprintf(buf + offset , MAX_STR_LENGTH - offset, "ITMT [%d] ", state->itmt_state);
	offset += snprintf(buf + offset , MAX_STR_LENGTH - offset, "EPB [%d] ", state->epb);
	offset += snprintf(buf + offset , MAX_STR_LENGTH - offset, "EPP [%d] ", state->epp);

	if (debug)
		lpmd_log_debug("%s\n", buf);
	else
		lpmd_log_info("%s\n", buf);
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

static int need_enter(lpmd_config_t *config, int idx)
{
	if (idx != current_idx)
		return 1;
	if (!config->config_states[idx].steady)
		return 1;

	return 0;
}

static int enter_state(lpmd_config_t *config, int idx)
{
	lpmd_config_state_t *state = &config->config_states[idx];

	state->entry_load_sys = config->data.util_sys;
	state->entry_load_cpu = config->data.util_cpu;
	
	process_itmt(state);
	
	process_epp_epb(state);

	process_irq(state);

	process_cgroup(state, config->mode);

	return 0;
}

static void dump_data(lpmd_config_t *config, int idx)
{
	char buf[MAX_STR_LENGTH];
	char epp_str[32];
	int epp, epb;
	int offset = 0;
	lpmd_config_state_t *state = &config->config_states[idx];

	if (!in_debug_mode())
		return;

	offset += snprintf(buf + offset , MAX_STR_LENGTH - offset, "[  Data] [%s] [%s]: ", lpmd_state_name[lpmd_state], state->name);

	if (config->wlt_hint_enable)
		offset += snprintf(buf + offset , MAX_STR_LENGTH - offset, "WLT [%2d] ", config->data.wlt_hint);

	if (config->util_sys_enable)
		if (config->data.util_sys == -1)
			offset += snprintf(buf + offset , MAX_STR_LENGTH - offset, "SYS [   N/A] ");
		else
			offset += snprintf(buf + offset , MAX_STR_LENGTH - offset, "SYS [%3d.%02d] ", config->data.util_sys / 100, config->data.util_sys % 100);

	if (config->util_cpu_enable)
		if (config->data.util_cpu == -1)
			offset += snprintf(buf + offset , MAX_STR_LENGTH - offset, "CPU [   N/A] ");
		else
			offset += snprintf(buf + offset , MAX_STR_LENGTH - offset, "CPU [%3d.%02d] ", config->data.util_cpu / 100, config->data.util_cpu % 100);

	if (config->util_gfx_enable)
		if (config->data.util_gfx == -1)
			offset += snprintf(buf + offset , MAX_STR_LENGTH - offset, "GFX [   N/A] ");
		else
			offset += snprintf(buf + offset , MAX_STR_LENGTH - offset, "GFX [%3d.%02d] ", config->data.util_gfx / 100, config->data.util_gfx % 100);

	if (state->cpumask_idx != CPUMASK_NONE)
		offset += snprintf(buf + offset , MAX_STR_LENGTH - offset, "CPUMASK [%s] ", get_cpus_hexstr(state->cpumask_idx));
	else
		offset += snprintf(buf + offset , MAX_STR_LENGTH - offset, "CPUMASK [%s] ", get_cpus_hexstr(CPUMASK_ONLINE));

	offset += snprintf(buf + offset , MAX_STR_LENGTH - offset, "ITMT [%d] ", get_itmt());

	get_epp_epb(&epp, epp_str, 32, &epb);
	if (epp == -1)
		offset += snprintf(buf + offset , MAX_STR_LENGTH - offset, "EPB [%d] EPP[%s] ", epb, epp_str);
	else
		offset += snprintf(buf + offset , MAX_STR_LENGTH - offset, "EPB [%d] EPP[%d] ", epb, epp);

	if (config->hfi_lpm_enable)
		offset += snprintf(buf + offset , MAX_STR_LENGTH - offset, "UPDATE [%d] ", config->data.need_update);

	offset += snprintf(buf + offset , MAX_STR_LENGTH - offset, "Interval [%d]", config->data.polling_interval);

	lpmd_log_debug("%s\n", buf);
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

	if (need_enter(config, idx)) {
		enter_state(config, idx);
		current_idx = idx;
		dump_state(&config->config_states[idx], "Enter", 0);
	}

end:
	dump_data(config, current_idx);
	lpmd_unlock();

	return 0;
}

static void dump_states(lpmd_config_t *lpmd_config)
{
	int i;
	lpmd_config_state_t *state;

	if (!lpmd_config)
		return;

	lpmd_log_info ("Mode:%d\n", lpmd_config->mode);
	lpmd_log_info ("HFI LPM Enable:%d\n", lpmd_config->hfi_lpm_enable);
	lpmd_log_info ("WLT Hint Enable:%d\n", lpmd_config->wlt_hint_enable);
	lpmd_log_info ("WLT Proxy Enable:%d\n", lpmd_config->wlt_proxy_enable);
	lpmd_log_info ("WLT Proxy Enable:%d\n", lpmd_config->wlt_hint_poll_enable);
	lpmd_log_info ("Util Enable:%d\n", lpmd_config->util_enable);
	lpmd_log_info ("Util entry threshold:%d\n", lpmd_config->util_entry_threshold);
	lpmd_log_info ("Util exit threshold:%d\n", lpmd_config->util_exit_threshold);
	lpmd_log_info ("Util LP Mode CPUs:%s\n", lpmd_config->lp_mode_cpus);
	lpmd_log_info ("EPP in LP Mode:%d\n", lpmd_config->lp_mode_epp);
	lpmd_log_info ("CPU Family:%d\n", lpmd_config->cpu_family);
	lpmd_log_info ("CPU Model:%d\n", lpmd_config->cpu_model);
	lpmd_log_info ("CPU Config:%s\n", lpmd_config->cpu_config);

	for (i = 0; i < MAX_STATES; ++i) {
		state = &lpmd_config->config_states[i];

		if (!state->valid)
			continue;
		lpmd_log_info ("Index:%d\n", i);
		lpmd_log_info ("\tID:%d\n", state->id);
		lpmd_log_info ("\tName:%s\n", state->name);
		lpmd_log_info ("\tentry_system_load_thres:%d\n", state->entry_system_load_thres);
		lpmd_log_info ("\texit_system_load_thres:%d\n", state->exit_system_load_thres);
		lpmd_log_info ("\texit_system_load_hyst:%d\n", state->exit_system_load_hyst);
		lpmd_log_info ("\tentry_cpu_load_thres:%d\n", state->enter_cpu_load_thres);
		lpmd_log_info ("\texit_cpu_load_thres:%d\n", state->exit_cpu_load_thres);
		lpmd_log_info ("\tentry_gfx_load_thres:%d\n", state->enter_gfx_load_thres);
		lpmd_log_info ("\texit_gfx_load_thres:%d\n", state->exit_gfx_load_thres);
		lpmd_log_info ("\tWLT Type:%d\n", state->wlt_type);
		lpmd_log_info ("\tmin_poll_interval:%d\n", state->min_poll_interval);
		lpmd_log_info ("\tmax_poll_interval:%d\n", state->max_poll_interval);
		lpmd_log_info ("\tpoll_interval_increment:%d\n", state->poll_interval_increment);
		lpmd_log_info ("\tEPP:%d\n", state->epp);
		lpmd_log_info ("\tEPB:%d\n", state->epb);
		lpmd_log_info ("\tITMTState:%d\n", state->itmt_state);
		lpmd_log_info ("\tIRQMigrate:%d\n", state->irq_migrate);
		if (state->active_cpus[0] != '\0')
			lpmd_log_info ("\tactive_cpus:%s\n", state->active_cpus);
		lpmd_log_info ("\tCPUMASK idx:%d\n", state->cpumask_idx);
		lpmd_log_info ("\tisland_0_number_p_cores:%d\n", state->island_0_number_p_cores);
		lpmd_log_info ("\tisland_0_number_e_cores:%d\n", state->island_0_number_e_cores);
		lpmd_log_info ("\tisland_1_number_p_cores:%d\n", state->island_1_number_p_cores);
		lpmd_log_info ("\tisland_1_number_e_cores:%d\n", state->island_1_number_e_cores);
		lpmd_log_info ("\tisland_2_number_p_cores:%d\n", state->island_2_number_p_cores);
		lpmd_log_info ("\tisland_2_number_e_cores:%d\n", state->island_2_number_e_cores);
	}
}

static int build_default_states(lpmd_config_t *config)
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
	state->steady = 1;
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
	state->steady = 1;
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
		state->steady = 0;
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
	state->steady = 1;
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
	state->steady = 1;
	state->valid = 1;

	config->config_state_count = 2;
	return 0;
}

static int config_states_update_config(lpmd_config_t *config)
{
	lpmd_config_state_t *state;
	int i;

	for (i = CONFIG_STATE_BASE; i < CONFIG_STATE_BASE + config->config_state_count; i++) {
		state = &config->config_states[i];

		if (!state->valid)
			continue;

		if (state->cpumask_idx == CPUMASK_HFI)
			config->hfi_lpm_enable = 1;

		if (state->wlt_type != -1)
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


static int build_state_cpumask(lpmd_config_state_t *state)
{
	state->steady = 1;

	if (state->cpumask_idx != CPUMASK_NONE)
		return 0;

	if (state->active_cpus[0] == '\0')
		return 0;

	if (!strncmp(state->active_cpus, "all", sizeof("all")) ||
	    !strncmp(state->active_cpus, "ALL", sizeof("ALL"))) {
		state->cpumask_idx = CPUMASK_ONLINE;
		return 0;
	}

	if (!strncmp(state->active_cpus, "lp", sizeof("lp")) ||
	    !strncmp(state->active_cpus, "LP", sizeof("LP"))) {
		state->cpumask_idx = CPUMASK_LPM_DEFAULT;
		return 0;
	}

	if (!strncmp(state->active_cpus, "hfi", sizeof("hfi")) ||
	    !strncmp(state->active_cpus, "HFI", sizeof("HFI"))) {
		state->cpumask_idx = CPUMASK_HFI;
		state->steady = 0;
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

#define DEFAULT_POLL_RATE_MS	1000

int lpmd_build_config_states(lpmd_config_t *lpmd_config)
{
	lpmd_config_state_t *state;
	int i, ret;

	build_default_states(lpmd_config);

	for (i = CONFIG_STATE_BASE; i < CONFIG_STATE_BASE + lpmd_config->config_state_count; i++) {
		state = &lpmd_config->config_states[i];

		if (build_state_cpumask(state))
			continue;

		if (state->entry_system_load_thres || state->enter_cpu_load_thres || state->enter_gfx_load_thres)
			polling_enabled = 1;

		if (state->min_poll_interval <= 0)
			state->min_poll_interval = state->max_poll_interval > DEFAULT_POLL_RATE_MS ? DEFAULT_POLL_RATE_MS : state->max_poll_interval;
		if (state->max_poll_interval <= 0)
			state->max_poll_interval = state->min_poll_interval > DEFAULT_POLL_RATE_MS ? state->min_poll_interval : DEFAULT_POLL_RATE_MS;
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

	config_states_update_config(lpmd_config);
	dump_states(lpmd_config);

	return 0;
}
