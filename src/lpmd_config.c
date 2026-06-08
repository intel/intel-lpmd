// SPDX-License-Identifier: GPL-2.0-or-later
/* Copyright (C) 2026 Intel Corporation */

#include "lpmd.h"
#include <libxml/parser.h>
#include <libxml/tree.h>

#define CONFIG_FILE_NAME "intel_lpmd_config.xml"

static int validate_slider_value(int value, const char *param_name, int state_id,
				 int min_val, int max_val)
{
	/* -1 is a special value meaning "ignore/disable" */
	if (value == -1)
		return LPMD_SUCCESS;  /* Valid */

	if (value < min_val || value > max_val) {
		if (state_id >= 0) {
			/* State-level parameter */
			lpmd_log_error("Invalid %s value: %d in state ID %d. "
					"Valid range: %d-%d or %d to disable\n",
					param_name, value, state_id,
					min_val, max_val, -1);
		} else {
			/* Global/default parameter */
			lpmd_log_error("Invalid %s (global default) value: %d. "
					"Valid range: %d-%d or %d to disable\n",
					param_name, value,
					min_val, max_val, -1);
		}
		return LPMD_ERROR;  /* Invalid */
	}

	return LPMD_SUCCESS;  /* Valid */
}

/*
 * Helper function to parse and validate slider parameters.
 * Handles both parsing (with error checking) and range validation in one call.
 * Returns 0 on success, LPMD_ERROR on failure (fail-fast approach).
 */
static int read_slider_and_validate(int *dest_ptr, const char *src_value,
				    const char *param_name, int state_id,
				    enum slider_param_type type)
{
	char *endptr;
	int value;

	errno = 0;
	value = strtol(src_value, &endptr, 10);

	/* Check for parsing errors */
	if (errno == ERANGE || endptr == src_value) {
		if (state_id >= 0)
			lpmd_log_error("Failed to parse %s value: '%s' is not a valid integer in state ID %d\n",
				param_name, src_value, state_id);
		else
			lpmd_log_error("Failed to parse %s value: '%s' is not a valid integer\n",
				param_name, src_value);
		return LPMD_ERROR;
	}

	/* Validate the parsed value based on type */
	switch (type) {
	case SLIDER_TYPE_OFFSET:
		if (validate_slider_value(value, param_name, state_id,
					  SLIDER_OFFSET_MIN, SLIDER_OFFSET_MAX) != LPMD_SUCCESS)
			return LPMD_ERROR;
		break;
	case SLIDER_TYPE_BALANCE:
		if (validate_slider_value(value, param_name, state_id,
					  SLIDER_BALANCE_MIN, SLIDER_BALANCE_MAX) != LPMD_SUCCESS)
			return LPMD_ERROR;
		break;
	default:
		lpmd_log_error("Unexpected slider type value: %d\n", type);
		return LPMD_ERROR;
	}

	/* Success - update the destination pointer */
	*dest_ptr = value;
	return LPMD_SUCCESS;
}

static void save_string_or_zero(char *tmp_value, char *dst_string, int dest_size)
{
	if (!strncmp(tmp_value, "-1", strlen("-1")))
		dst_string[0] = '\0';
	else
		copy_user_string(tmp_value, dst_string, dest_size);
}

static void lpmd_parse_state(xmlDoc *doc, xmlNode *a_node, struct lpmd_config_t *config, int idx)
{
	struct lpmd_config_state_t *state = &config->config_states[idx];
	xmlNode *cur_node = NULL;
	char *tmp_value;
	int ret = 0;
	char *pos;

	if (!doc || !a_node || !state)
		return;

	lpmd_log_debug("%s: %d\n", __func__, idx);
	lpmd_init_config_state(state);

	for (cur_node = a_node; cur_node; cur_node = cur_node->next) {
		if (cur_node->type != XML_ELEMENT_NODE)
			continue;

		tmp_value = (char *)xmlNodeListGetString(doc, cur_node->xmlChildrenNode, 1);

		if (!tmp_value)
			continue;

		if (!strncmp((const char *)cur_node->name, "ID", strlen("ID")))
			state->id = strtol(tmp_value, &pos, 10);
		if (!strncmp((const char *)cur_node->name, "Name", strlen("Name"))) {
			snprintf(state->name, MAX_STATE_NAME - 1, "%s", tmp_value);
			state->name[MAX_STATE_NAME - 1] = '\0';
		}
		if (!strncmp((const char *)cur_node->name, "WLTTypeMask", strlen("WLTTypeMask")))
			state->wlt_type_mask = strtol(tmp_value, &pos, 10);
		else if (!strncmp((const char *)cur_node->name, "WLTType", strlen("WLTType")))
			state->wlt_type = strtol(tmp_value, &pos, 10);
		if (!strncmp((const char *)cur_node->name, "EntrySystemLoadThres", strlen("EntrySystemLoadThres")))
			state->entry_system_load_thres = strtol(tmp_value, &pos, 10);
		if (!strncmp((const char *)cur_node->name, "ExitSystemLoadThres", strlen("ExitSystemLoadThres")))
			state->exit_system_load_thres = strtol(tmp_value, &pos, 10);
		if (!strncmp((const char *)cur_node->name, "ExitSystemLoadhysteresis", strlen("ExitSystemLoadhysteresis")))
			state->exit_system_load_hyst = strtol(tmp_value, &pos, 10);
		if (!strncmp((const char *)cur_node->name, "EnterCPULoadThres", strlen("EnterCPULoadThres")))
			state->enter_cpu_load_thres = strtol(tmp_value, &pos, 10);
		if (!strncmp((const char *)cur_node->name, "ExitCPULoadThres", strlen("ExitCPULoadThres")))
			state->exit_cpu_load_thres = strtol(tmp_value, &pos, 10);
		if (!strncmp((const char *)cur_node->name, "EnterGFXLoadThres", strlen("EnterGFXLoadThres")))
			state->enter_gfx_load_thres = strtol(tmp_value, &pos, 10);
		if (!strncmp((const char *)cur_node->name, "ExitGFXLoadThres", strlen("ExitGFXLoadThres")))
			state->exit_gfx_load_thres = strtol(tmp_value, &pos, 10);
		if (!strncmp((const char *)cur_node->name, "MinPollInterval", strlen("MinPollInterval")))
			state->min_poll_interval = strtol(tmp_value, &pos, 10);
		if (!strncmp((const char *)cur_node->name, "MaxPollInterval", strlen("MaxPollInterval")))
			state->max_poll_interval = strtol(tmp_value, &pos, 10);
		if (!strncmp((const char *)cur_node->name, "PollIntervalIncrement", strlen("PollIntervalIncrement")))
			state->poll_interval_increment = strtol(tmp_value, &pos, 10);
		if (!strncmp((const char *)cur_node->name, "EPP", strlen("EPP")))
			state->epp = strtol(tmp_value, &pos, 10);
		if (!strncmp((const char *)cur_node->name, "EPB", strlen("EPB")))
			state->epb = strtol(tmp_value, &pos, 10);
		if (!strncmp((const char *)cur_node->name, "ITMTState", strlen("ITMTState")))
			state->itmt_state = strtol(tmp_value, &pos, 10);
		if (!strncmp((const char *)cur_node->name, "IRQMigrate", strlen("IRQMigrate")))
			state->irq_migrate = strtol(tmp_value, &pos, 10);
		if (!strncmp((const char *)cur_node->name, "ActivePcores", strlen("ActivePcores")))
			save_string_or_zero(tmp_value, state->active_p_cores, sizeof(state->active_p_cores));
		if (!strncmp((const char *)cur_node->name, "ActiveEcores", strlen("ActiveEcores")))
			save_string_or_zero(tmp_value, state->active_e_cores, sizeof(state->active_e_cores));
		if (!strncmp((const char *)cur_node->name, "ActiveLcores", strlen("ActiveLcores")))
			save_string_or_zero(tmp_value, state->active_l_cores, sizeof(state->active_l_cores));
		if (!strncmp((const char *)cur_node->name, "ActiveCPUs", strlen("ActiveCPUs")))
			save_string_or_zero(tmp_value, state->active_cpus, sizeof(state->active_cpus));
		if (!strncmp((const char *)cur_node->name, "BalanceSliderAC", strlen("BalanceSliderAC")))
			ret = read_slider_and_validate(&state->balance_slider_ac, tmp_value, "BalanceSliderAC", state->id, SLIDER_TYPE_BALANCE);
		if (!strncmp((const char *)cur_node->name, "SliderOffsetAC", strlen("SliderOffsetAC")))
			ret = read_slider_and_validate(&state->slider_offset_ac, tmp_value, "SliderOffsetAC", state->id, SLIDER_TYPE_OFFSET);
		if (!strncmp((const char *)cur_node->name, "BalanceSliderDC", strlen("BalanceSliderDC")))
			ret = read_slider_and_validate(&state->balance_slider_dc, tmp_value, "BalanceSliderDC", state->id, SLIDER_TYPE_BALANCE);
		if (!strncmp((const char *)cur_node->name, "SliderOffsetDC", strlen("SliderOffsetDC")))
			ret = read_slider_and_validate(&state->slider_offset_dc, tmp_value, "SliderOffsetDC", state->id, SLIDER_TYPE_OFFSET);

		xmlFree(tmp_value);
		if (ret)
			return;
	}
}

int is_wildcard(char *str)
{
	if (!str)
		return 1;
	if (!strncmp(str, "*", strlen("*")))
		return 1;
	if (!strncmp(str, " * ", strlen(" * ")))
		return 1;

	return 0;
}

static void lpmd_parse_states(xmlDoc *doc, xmlNode *a_node, struct lpmd_config_t *lpmd_config)
{
	int cpu_family = -1, cpu_model = -1, config_state_count = 0;
	char cpu_config[MAX_CONFIG_LEN];
	xmlNode *cur_node = NULL;
	char *tmp_value;
	char *pos;

	if (!doc || !a_node || !lpmd_config)
		return;

	/* A valid states table has been parsed */
	if (lpmd_config->config_state_count)
		return;

	cpu_config[0] = '\0';

	for (cur_node = a_node; cur_node; cur_node = cur_node->next) {
		if (cur_node->type != XML_ELEMENT_NODE)
			continue;

		if (!cur_node->name)
			continue;

		tmp_value = (char *)xmlNodeListGetString(doc, cur_node->xmlChildrenNode, 1);
		if (!strncmp((const char *)cur_node->name, "CPUFamily", strlen("CPUFamily"))) {
			if (is_wildcard(tmp_value))
				cpu_family = lpmd_config->cpu_family;
			else
				cpu_family = strtol(tmp_value, &pos, 10);
		}

		if (!strncmp((const char *)cur_node->name, "CPUModel", strlen("CPUModel"))) {
			if (is_wildcard(tmp_value))
				cpu_model = lpmd_config->cpu_model;
			else
				cpu_model = strtol(tmp_value, &pos, 10);
		}

		if (!strncmp((const char *)cur_node->name, "CPUConfig", strlen("CPUConfig"))) {
			if (is_wildcard(tmp_value))
				strncpy(cpu_config, lpmd_config->cpu_config, MAX_CONFIG_LEN);
			else
				snprintf(cpu_config, MAX_CONFIG_LEN - 1, "%s", tmp_value);
			cpu_config[MAX_CONFIG_LEN - 1] = '\0';
		}

		if (tmp_value)
			xmlFree(tmp_value);

		if (strncmp((const char *)cur_node->name, "State", strlen("State")))
			continue;

		/* Must check cpu family/model/config first to make sure the states applies */
		if (cpu_family != lpmd_config->cpu_family ||
		    cpu_model != lpmd_config->cpu_model ||
		    strncmp(cpu_config, lpmd_config->cpu_config,
			    MAX_CONFIG_LEN)) {
			lpmd_log_info("Ignore unsupported states for CPU family:%d,model%d,config:%s\n",
				      cpu_family, cpu_model, cpu_config);
			return;
		}

		if (lpmd_config->config_state_count >= MAX_CONFIG_STATES)
			break;
		lpmd_parse_state(doc, cur_node->children, lpmd_config,
				 CONFIG_STATE_BASE + config_state_count);
		config_state_count++;
	}
	lpmd_log_debug("Found %d config states\n", config_state_count);
	lpmd_config->config_state_count = config_state_count;
}

static void lpmd_init_config(struct lpmd_config_t *config)
{
	config->performance_def = LPM_FORCE_OFF;
	config->balanced_def = LPM_FORCE_OFF;
	config->powersaver_def = LPM_FORCE_OFF;
	config->lp_mode_epp = -1;
	config->data.util_sys = -1;
	config->data.util_cpu = -1;
	config->data.util_gfx = -1;
	config->data.wlt_hint = -1;
	config->balance_slider_def_ac = -1;
	config->balance_slider_def_dc = -1;
	config->slider_offset_def_ac = -1;
	config->slider_offset_def_dc = -1;
	config->wlt_hint_mask = -1;
	config->wlt_notification_delay = -1;
}

static int lpmd_fill_config(xmlDoc *doc, xmlNode *a_node, struct lpmd_config_t *lpmd_config)
{
	xmlNode *cur_node = NULL;
	char *tmp_value;
	char *pos;
	int config_error = 0;  /* Track if we encountered a parsing error */

	if (!doc || !a_node || !lpmd_config)
		return LPMD_ERROR;

	lpmd_init_config(lpmd_config);

	for (cur_node = a_node; cur_node; cur_node = cur_node->next) {
		if (cur_node->type != XML_ELEMENT_NODE)
			continue;

		tmp_value = (char *)xmlNodeListGetString(doc, cur_node->xmlChildrenNode, 1);
		if (!tmp_value)
			continue;

		if (!strncmp((const char *)cur_node->name, "Mode", strlen("Mode"))) {
			errno = 0;
			lpmd_config->mode = strtol(tmp_value, &pos, 10);
			if (errno || *pos != '\0' ||
			    lpmd_config->mode > LPM_CPU_MODE_MAX ||
			    lpmd_config->mode < 0)
				goto err;
		} else if (!strncmp((const char *)cur_node->name,
				    "HfiLpmEnable", strlen("HfiLpmEnable"))) {
			errno = 0;
			lpmd_config->hfi_lpm_enable = strtol(tmp_value, &pos, 10);
			if (errno || *pos != '\0' ||
			    (lpmd_config->hfi_lpm_enable != 1 &&
			     lpmd_config->hfi_lpm_enable != 0))
				goto err;
		} else if (!strncmp((const char *)cur_node->name,
				    "WLTHintEnable", strlen("WLtHintEnable"))) {
			errno = 0;
			lpmd_config->wlt_hint_enable = strtol(tmp_value, &pos, 10);
			if (errno || *pos != '\0' ||
			    (lpmd_config->wlt_hint_enable != 1 &&
			     lpmd_config->wlt_hint_enable != 0))
				goto err;
		} else if (!strncmp((const char *)cur_node->name, "WLTHintMask",
				    strlen("WLTHintMask"))) {
			errno = 0;
			lpmd_config->wlt_hint_mask = strtol(tmp_value, &pos, 10);
		} else if (!strncmp((const char *)cur_node->name,
				    "WLTHintPollEnable",
				    strlen("WLtHintPollEnable"))) {
			errno = 0;
			lpmd_config->wlt_hint_poll_enable = strtol(tmp_value, &pos, 10);
			if (errno || *pos != '\0' ||
			    (lpmd_config->wlt_hint_poll_enable != 1 &&
			     lpmd_config->wlt_hint_poll_enable != 0))
				goto err;
		} else if (!strncmp((const char *)cur_node->name,
				    "WLTProxyEnable",
				    strlen("WLTProxyEnable"))) {
			errno = 0;
			lpmd_config->wlt_proxy_enable = strtol(tmp_value, &pos, 10);
			if (errno || *pos != '\0' ||
			    (lpmd_config->wlt_proxy_enable != 1 &&
			     lpmd_config->wlt_proxy_enable != 0))
				goto err;
		} else if (!strncmp((const char *)cur_node->name,
				    "EntryDelayMS", strlen("EntryDelayMS"))) {
			errno = 0;
			lpmd_config->util_entry_delay = strtol(tmp_value, &pos, 10);
			if (errno || *pos != '\0' ||
			    lpmd_config->util_entry_delay < 0 ||
			    lpmd_config->util_entry_delay > UTIL_DELAY_MAX)
				goto err;
		} else if (!strncmp((const char *)cur_node->name, "ExitDelayMS",
				    strlen("ExitDelayMS"))) {
			errno = 0;
			lpmd_config->util_exit_delay = strtol(tmp_value, &pos, 10);
			if (errno || *pos != '\0' ||
			    lpmd_config->util_exit_delay < 0 ||
			    lpmd_config->util_exit_delay > UTIL_DELAY_MAX)
				goto err;
		} else if (!strncmp((const char *)cur_node->name,
				    "util_entry_threshold",
				    strlen("util_entry_threshold"))) {
			errno = 0;
			lpmd_config->util_entry_threshold = strtol(tmp_value, &pos, 10);
			if (errno || *pos != '\0' ||
			    lpmd_config->util_entry_threshold < 0 ||
			    lpmd_config->util_entry_threshold > 100)
				goto err;
		} else if (!strncmp((const char *)cur_node->name,
				    "util_exit_threshold",
				    strlen("util_exit_threshold"))) {
			errno = 0;
			lpmd_config->util_exit_threshold = strtol(tmp_value, &pos, 10);
			if (errno || *pos != '\0' ||
			    lpmd_config->util_exit_threshold < 0 ||
			    lpmd_config->util_exit_threshold > 100)
				goto err;
		} else if (!strncmp((const char *)cur_node->name, "EntryHystMS",
				    strlen("EntryHystMS"))) {
			errno = 0;
			lpmd_config->util_entry_hyst = strtol(tmp_value, &pos, 10);
			if (errno || *pos != '\0' ||
			    lpmd_config->util_entry_hyst < 0 ||
			    lpmd_config->util_entry_hyst > UTIL_HYST_MAX)
				goto err;
		} else if (!strncmp((const char *)cur_node->name, "ExitHystMS",
				    strlen("ExitHystMS"))) {
			errno = 0;
			lpmd_config->util_exit_hyst = strtol(tmp_value, &pos, 10);
			if (errno || *pos != '\0' ||
			    lpmd_config->util_exit_hyst < 0 ||
			    lpmd_config->util_exit_hyst > UTIL_HYST_MAX)
				goto err;
		} else if (!strncmp((const char *)cur_node->name, "lp_mode_epp",
				    strlen("lp_mode_epp"))) {
			errno = 0;
			lpmd_config->lp_mode_epp = strtol(tmp_value, &pos, 10);
			if (errno || *pos != '\0' ||
			    lpmd_config->lp_mode_epp > 255 ||
			    lpmd_config->lp_mode_epp < -1)
				goto err;
			if (lpmd_config->lp_mode_epp < 0)
				lpmd_config->lp_mode_epp = -1;
		} else if (!strncmp((const char *)cur_node->name, "IgnoreITMT",
				    strlen("IgnoreITMT"))) {
			errno = 0;
			lpmd_config->ignore_itmt = strtol(tmp_value, &pos, 10);
			if (errno || *pos != '\0' ||
			    lpmd_config->ignore_itmt < 0 ||
			    lpmd_config->ignore_itmt > 1)
				goto err;
		} else if (!strncmp((const char *)cur_node->name,
				    "lp_mode_cpus", strlen("lp_mode_cpus"))) {
			if (!strncmp(tmp_value, "-1", strlen("-1")))
				lpmd_config->lp_mode_cpus[0] = '\0';
			else
				copy_user_string(tmp_value, lpmd_config->lp_mode_cpus,
						 sizeof(lpmd_config->lp_mode_cpus));
		} else if (!strncmp((const char *)cur_node->name,
				    "PerformanceDef",
				    strlen("PerformanceDef"))) {
			errno = 0;
			lpmd_config->performance_def = strtol(tmp_value, &pos, 10);
			if (errno || *pos != '\0')
				goto err;
			if (lpmd_config->performance_def == -1)
				lpmd_config->performance_def = LPM_FORCE_OFF;
			else if (lpmd_config->performance_def == 1)
				lpmd_config->performance_def = LPM_FORCE_ON;
			else if (!lpmd_config->performance_def)
				lpmd_config->performance_def = LPM_AUTO;
			else
				goto err;
		} else if (!strncmp((const char *)cur_node->name, "BalancedDef",
				    strlen("BalancedDef"))) {
			errno = 0;
			lpmd_config->balanced_def = strtol(tmp_value, &pos, 10);
			if (errno || *pos != '\0')
				goto err;
			if (lpmd_config->balanced_def == -1)
				lpmd_config->balanced_def = LPM_FORCE_OFF;
			else if (lpmd_config->balanced_def == 1)
				lpmd_config->balanced_def = LPM_FORCE_ON;
			else if (!lpmd_config->balanced_def)
				lpmd_config->balanced_def = LPM_AUTO;
			else
				goto err;
		} else if (!strncmp((const char *)cur_node->name,
				    "PowersaverDef", strlen("PowersaverDef"))) {
			errno = 0;
			lpmd_config->powersaver_def = strtol(tmp_value, &pos, 10);
			if (errno || *pos != '\0')
				goto err;
			if (lpmd_config->powersaver_def == -1)
				lpmd_config->powersaver_def = LPM_FORCE_OFF;
			else if (lpmd_config->powersaver_def == 1)
				lpmd_config->powersaver_def = LPM_FORCE_ON;
			else if (!lpmd_config->powersaver_def)
				lpmd_config->powersaver_def = LPM_AUTO;
			else
				goto err;
		} else if (!strncmp((const char *)cur_node->name, "States",
				    strlen("States"))) {
			errno = 0;
			lpmd_parse_states(doc, cur_node->children, lpmd_config);
		} else if (!strncmp((const char *)cur_node->name, "BalancedSliderAC", strlen("BalancedSliderAC"))) {
			if (read_slider_and_validate(&lpmd_config->balance_slider_def_ac,
						     tmp_value, "BalancedSliderAC", -1, SLIDER_TYPE_BALANCE) != 0)
				goto err;
		} else if (!strncmp((const char *)cur_node->name, "BalancedSliderDC", strlen("BalancedSliderDC"))) {
			if (read_slider_and_validate(&lpmd_config->balance_slider_def_dc,
						     tmp_value, "BalancedSliderDC", -1, SLIDER_TYPE_BALANCE) != 0)
				goto err;
		} else if (!strncmp((const char *)cur_node->name, "SliderOffsetAC", strlen("SliderOffsetAC"))) {
			if (read_slider_and_validate(&lpmd_config->slider_offset_def_ac,
						     tmp_value, "SliderOffsetAC", -1, SLIDER_TYPE_OFFSET) != 0)
				goto err;
		} else if (!strncmp((const char *)cur_node->name, "SliderOffsetDC", strlen("SliderOffsetDC"))) {
			if (read_slider_and_validate(&lpmd_config->slider_offset_def_dc,
						     tmp_value, "SliderOffsetDC", -1, SLIDER_TYPE_OFFSET) != 0)
				goto err;
		} else {
			if (!strncmp((const char *)cur_node->name, "HfiSuvEnable", strlen("HfiSuvEnable"))) {
				lpmd_log_debug("Ignore deprecated HfiSuvEnable setting\n");
			} else {
				lpmd_log_info("Invalid configuration data\n");
				goto err;
			}
		}
		xmlFree(tmp_value);
		continue;
err:
		lpmd_log_error("node type: Element, name: %s value: %s\n", cur_node->name,
			       tmp_value);
		xmlFree(tmp_value);
		config_error = 1;  /* Mark that a parsing error occurred */
		break;  /* Exit loop and validate before returning error */
	}

	/* If a parsing error occurred, return error */
	if (config_error)
		return LPMD_ERROR;

	return LPMD_SUCCESS;
}

int match_config_file(int family, int model, int tdp, char *save_file_name)
{
	char file_name[MAX_FILE_NAME_PATH];
	struct stat s;
	int ret;

	snprintf(file_name, MAX_FILE_NAME_PATH,
		 "%s/intel_lpmd_config_F%d_M%d_T%d.xml", TDCONFDIR, family,
		 model, tdp);

	lpmd_log_msg("Looking for config file %s\n", file_name);
	ret = stat(file_name, &s);
	if (!ret) {
		strncpy(save_file_name, file_name, MAX_FILE_NAME_PATH);
		return ret;
	}

	snprintf(file_name, MAX_FILE_NAME_PATH,
		 "%s/intel_lpmd_config_F%d_M%d.xml", TDCONFDIR, family, model);

	lpmd_log_msg("Looking for config file %s\n", file_name);

	ret = stat(file_name, &s);
	if (!ret) {
		strncpy(save_file_name, file_name, MAX_FILE_NAME_PATH);
		return ret;
	}

	return ret;
}

int lpmd_get_config(struct lpmd_config_t *lpmd_config)
{
	char file_name_str[MAX_FILE_NAME_PATH];
	xmlNode *root_element;
	xmlNode *cur_node;
	char *file_name;
	struct stat s;
	xmlDoc *doc;

	if (!lpmd_config)
		return LPMD_ERROR;

	/*
	 * If a config file was already found skip looking for it the second
	 * time.
	 */
	if (strcmp(lpmd_config->file_name, "")) {
		file_name = lpmd_config->file_name;
		goto process_xml;
	}

	/*
	 * If a model specific config file was not found use the generic
	 * config file.
	 */
	file_name = file_name_str;
	snprintf(file_name, MAX_FILE_NAME_PATH, "%s/%s", TDCONFDIR, CONFIG_FILE_NAME);

	lpmd_log_msg("Reading configuration file %s\n", file_name);

	if (stat(file_name, &s)) {
		lpmd_log_msg("error: could not find file %s\n", file_name);
		return LPMD_ERROR;
	}

	snprintf(lpmd_config->file_name, MAX_FILE_NAME_PATH, "%s", file_name);

process_xml:
	doc = xmlReadFile(file_name, NULL, 0);
	if (!doc) {
		lpmd_log_msg("error: could not parse file %s\n", file_name);
		return LPMD_ERROR;
	}

	root_element = xmlDocGetRootElement(doc);
	if (!root_element) {
		lpmd_log_warn("error: could not get root element\n");
		return LPMD_ERROR;
	}

	cur_node = NULL;

	for (cur_node = root_element; cur_node; cur_node = cur_node->next) {
		if (cur_node->type != XML_ELEMENT_NODE)
			continue;

		if (strncmp((const char *)cur_node->name, "Configuration", strlen("Configuration")))
			continue;

		if (lpmd_fill_config(doc, cur_node->children, lpmd_config) != LPMD_SUCCESS) {
			xmlFreeDoc(doc);
			return LPMD_ERROR;
		}
	}

	xmlFreeDoc(doc);

	return LPMD_SUCCESS;
}
