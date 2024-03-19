/*
 * lpmd_config.c: xml config file parser
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
 */

#include "lpmd.h"
#include <libxml/parser.h>
#include <libxml/tree.h>

#define CONFIG_FILE_NAME "intel_lpmd_config.xml"
#define MAX_FILE_NAME_PATH	64

static void lpmd_dump_config(lpmd_config_t *lpmd_config)
{
	int i;
	lpmd_config_state_t *state;

	if (!lpmd_config)
		return;

	lpmd_log_info ("Mode:%d\n", lpmd_config->mode);
	lpmd_log_info ("HFI LPM Enable:%d\n", lpmd_config->hfi_lpm_enable);
	lpmd_log_info ("HFI SUV Enable:%d\n", lpmd_config->hfi_suv_enable);
	lpmd_log_info ("Util entry threshold:%d\n", lpmd_config->util_entry_threshold);
	lpmd_log_info ("Util exit threshold:%d\n", lpmd_config->util_exit_threshold);
	lpmd_log_info ("Util LP Mode CPUs:%s\n", lpmd_config->lp_mode_cpus);
	lpmd_log_info ("EPP in LP Mode:%d\n", lpmd_config->lp_mode_epp);

	if (!lpmd_config->config_state_count)
		return;

	lpmd_log_info ("CPU Family:%d\n", lpmd_config->cpu_family);
	lpmd_log_info ("CPU Model:%d\n", lpmd_config->cpu_model);
	lpmd_log_info ("CPU Config:%s\n", lpmd_config->cpu_config);

	for (i = 0; i < lpmd_config->config_state_count; ++i) {
		state = &lpmd_config->config_states[i];

		lpmd_log_info ("ID:%d\n", state->id);
		lpmd_log_info ("\tName:%s\n", state->name);
		lpmd_log_info ("\tentry_system_load_thres:%d\n", state->entry_system_load_thres);
		lpmd_log_info ("\texit_system_load_thres:%d\n", state->exit_system_load_thres);
		lpmd_log_info ("\texit_system_load_hyst:%d\n", state->exit_system_load_hyst);
		lpmd_log_info ("\tentry_cpu_load_thres:%d\n", state->enter_cpu_load_thres);
		lpmd_log_info ("\texit_cpu_load_thres:%d\n", state->exit_cpu_load_thres);
		lpmd_log_info ("\tmin_poll_interval:%d\n", state->min_poll_interval);
		lpmd_log_info ("\tmax_poll_interval:%d\n", state->max_poll_interval);
		lpmd_log_info ("\tpoll_interval_increment:%d\n", state->poll_interval_increment);
		lpmd_log_info ("\tEPP:%d\n", state->epp);
		lpmd_log_info ("\tEPB:%d\n", state->epb);
		lpmd_log_info ("\tITMTState:%d\n", state->itmt_state);
		lpmd_log_info ("\tIRQMigrate:%d\n", state->irq_migrate);
		if (state->active_cpus[0] != '\0')
			lpmd_log_info ("\tactive_cpus:%s\n", state->active_cpus);
		lpmd_log_info ("\tisland_0_number_p_cores:%d\n", state->island_0_number_p_cores);
		lpmd_log_info ("\tisland_0_number_e_cores:%d\n", state->island_0_number_e_cores);
		lpmd_log_info ("\tisland_1_number_p_cores:%d\n", state->island_1_number_p_cores);
		lpmd_log_info ("\tisland_1_number_e_cores:%d\n", state->island_1_number_e_cores);
		lpmd_log_info ("\tisland_2_number_p_cores:%d\n", state->island_2_number_p_cores);
		lpmd_log_info ("\tisland_2_number_e_cores:%d\n", state->island_2_number_e_cores);
	}
}

static void lpmd_parse_state(xmlDoc *doc, xmlNode *a_node, lpmd_config_state_t *state)
{
	xmlNode *cur_node = NULL;
	char *tmp_value;
	char *pos;

	if (!doc || !a_node || !state)
		return;

	for (cur_node = a_node; cur_node; cur_node = cur_node->next) {
		if (cur_node->type == XML_ELEMENT_NODE) {
			tmp_value = (char*) xmlNodeListGetString (doc, cur_node->xmlChildrenNode, 1);
			if (tmp_value) {
				if (!strncmp((const char*)cur_node->name, "ID", strlen("ID")))
					state->id = strtol (tmp_value, &pos, 10);
				if (!strncmp((const char*)cur_node->name, "Name", strlen("Name"))) {
					snprintf(state->name, MAX_STATE_NAME - 1, "%s", tmp_value);
					state->name[MAX_STATE_NAME - 1] = '\0';
				}
				if (!strncmp((const char*)cur_node->name, "EntrySystemLoadThres", strlen("EntrySystemLoadThres")))
					state->entry_system_load_thres = strtol (tmp_value, &pos, 10);
				if (!strncmp((const char*)cur_node->name, "ExitSystemLoadThres", strlen("ExitSystemLoadThres")))
					state->exit_system_load_thres = strtol (tmp_value, &pos, 10);
				if (!strncmp((const char*)cur_node->name, "ExitSystemLoadhysteresis", strlen("ExitSystemLoadhysteresis")))
					state->exit_system_load_hyst = strtol (tmp_value, &pos, 10);
				if (!strncmp((const char*)cur_node->name, "EnterCPULoadThres", strlen("EnterCPULoadThres")))
					state->enter_cpu_load_thres = strtol (tmp_value, &pos, 10);
				if (!strncmp((const char*)cur_node->name, "ExitCPULoadThres", strlen("ExitCPULoadThres")))
					state->exit_cpu_load_thres = strtol (tmp_value, &pos, 10);
				if (!strncmp((const char*)cur_node->name, "MinPollInterval", strlen("MinPollInterval")))
					state->min_poll_interval = strtol (tmp_value, &pos, 10);
				if (!strncmp((const char*)cur_node->name, "MaxPollInterval", strlen("MaxPollInterval")))
					state->max_poll_interval = strtol (tmp_value, &pos, 10);
				if (!strncmp((const char*)cur_node->name, "PollIntervalIncrement", strlen("PollIntervalIncrement")))
					state->poll_interval_increment = strtol (tmp_value, &pos, 10);
				if (!strncmp((const char*)cur_node->name, "EPP", strlen("EPP")))
					state->epp = strtol (tmp_value, &pos, 10);
				if (!strncmp((const char*)cur_node->name, "EPB", strlen("EPB")))
					state->epb = strtol (tmp_value, &pos, 10);
				if (!strncmp((const char*)cur_node->name, "ITMTState", strlen("ITMTState")))
					state->itmt_state = strtol (tmp_value, &pos, 10);
				if (!strncmp((const char*)cur_node->name, "IRQMigrate", strlen("IRQMigrate")))
					state->irq_migrate = strtol (tmp_value, &pos, 10);
				if (!strncmp((const char*)cur_node->name, "Island0Pcores", strlen("Island0Pcores")))
					state->island_0_number_p_cores = strtol (tmp_value, &pos, 10);
				if (!strncmp((const char*)cur_node->name, "Island0Ecores", strlen("Island0Ecores")))
					state->island_0_number_e_cores = strtol (tmp_value, &pos, 10);
				if (!strncmp((const char*)cur_node->name, "Island1Pcores", strlen("Island1Pcores")))
					state->island_1_number_p_cores = strtol (tmp_value, &pos, 10);
				if (!strncmp((const char*)cur_node->name, "Island1Ecores", strlen("Island1Ecores")))
					state->island_1_number_e_cores = strtol (tmp_value, &pos, 10);
				if (!strncmp((const char*)cur_node->name, "Island2Pcores", strlen("Island2Pcores")))
					state->island_2_number_p_cores = strtol (tmp_value, &pos, 10);
				if (!strncmp((const char*)cur_node->name, "Island2Ecores", strlen("Island2Ecores")))
					state->island_2_number_e_cores = strtol (tmp_value, &pos, 10);
				if (!strncmp((const char*)cur_node->name, "ActiveCPUs", strlen("ActiveCPUs"))) {
					if (!strncmp (tmp_value, "-1", strlen ("-1")))
						state->active_cpus[0] = '\0';
					else
						snprintf (state->active_cpus, sizeof(state->active_cpus), "%s", tmp_value);
				}
			}
		}
	}
}

static void lpmd_parse_states(xmlDoc *doc, xmlNode *a_node, lpmd_config_t *lpmd_config)
{
	xmlNode *cur_node = NULL;
	char *tmp_value;
	char *pos;
	int config_state_count = 0;

	if (!doc || !a_node || !lpmd_config)
		return;

	/* A valid states table has been parsed */
	if (lpmd_config->config_state_count)
		return;

	for (cur_node = a_node; cur_node; cur_node = cur_node->next) {
		if (cur_node->type == XML_ELEMENT_NODE) {
			if (cur_node->name) {
				tmp_value = (char*) xmlNodeListGetString (doc, cur_node->xmlChildrenNode, 1);

				if (!strncmp ((const char*) cur_node->name, "CPUFamily", strlen ("CPUFamily")))
					lpmd_config->cpu_family = strtol (tmp_value, &pos, 10);

				if (!strncmp ((const char*) cur_node->name, "CPUModel", strlen ("CPUModel")))
					lpmd_config->cpu_model = strtol (tmp_value, &pos, 10);

				if (!strncmp ((const char*) cur_node->name, "CPUConfig", strlen ("CPUConfig"))) {
					snprintf (lpmd_config->cpu_config, MAX_CONFIG_LEN - 1, "%s", tmp_value);
					lpmd_config->cpu_config[MAX_CONFIG_LEN - 1] = '\0';
				}

				if (strncmp ((const char*) cur_node->name, "State", strlen ("State")))
					continue;

				if (lpmd_config->config_state_count >= MAX_CONFIG_STATES)
					break;
				lpmd_parse_state (doc, cur_node->children, &lpmd_config->config_states[config_state_count]);
				config_state_count++;
			}
		}
	}
	lpmd_config->config_state_count = config_state_count;
}

static int lpmd_fill_config(xmlDoc *doc, xmlNode *a_node, lpmd_config_t *lpmd_config)
{
	xmlNode *cur_node = NULL;
	char *tmp_value;
	char *pos;

	if (!doc || !a_node || !lpmd_config)
		return LPMD_ERROR;

	lpmd_config->performance_def = lpmd_config->balanced_def = lpmd_config->powersaver_def = LPM_FORCE_OFF;
	lpmd_config->lp_mode_epp = -1;
	for (cur_node = a_node; cur_node; cur_node = cur_node->next) {
		if (cur_node->type == XML_ELEMENT_NODE) {
			tmp_value = (char*) xmlNodeListGetString (doc, cur_node->xmlChildrenNode, 1);
			if (tmp_value) {
				if (!strncmp((const char*)cur_node->name, "Mode", strlen("Mode"))) {
					errno = 0;
					lpmd_config->mode = strtol (tmp_value, &pos, 10);
					if (errno || *pos != '\0' || lpmd_config->mode > LPM_CPU_MODE_MAX
							|| lpmd_config->mode < 0)
						goto err;
				}
				else if (!strncmp((const char*)cur_node->name, "HfiLpmEnable", strlen("HfiEnable"))) {
					errno = 0;
					lpmd_config->hfi_lpm_enable = strtol (tmp_value, &pos, 10);
					if (errno || *pos != '\0'
							|| (lpmd_config->hfi_lpm_enable != 1 && lpmd_config->hfi_lpm_enable != 0))
						goto err;
				}
				else if (!strncmp((const char*)cur_node->name, "HfiSuvEnable", strlen("HfiEnable"))) {
					errno = 0;
					lpmd_config->hfi_suv_enable = strtol (tmp_value, &pos, 10);
					if (errno || *pos != '\0'
							|| (lpmd_config->hfi_suv_enable != 1 && lpmd_config->hfi_suv_enable != 0))
						goto err;
				}
				else if (!strncmp((const char*)cur_node->name, "EntryDelayMS", strlen ("EntryDelayMS"))) {
					errno = 0;
					lpmd_config->util_entry_delay = strtol (tmp_value, &pos, 10);
					if (errno
							|| *pos
									!= '\0'|| lpmd_config->util_entry_delay < 0 || lpmd_config->util_entry_delay > UTIL_DELAY_MAX)
						goto err;
				}
				else if (!strncmp((const char*)cur_node->name, "ExitDelayMS", strlen ("ExitDelayMS"))) {
					errno = 0;
					lpmd_config->util_exit_delay = strtol (tmp_value, &pos, 10);
					if (errno
							|| *pos
									!= '\0'|| lpmd_config->util_exit_delay < 0 || lpmd_config->util_exit_delay > UTIL_DELAY_MAX)
						goto err;
				}
				else if (!strncmp((const char*)cur_node->name, "util_entry_threshold",
									strlen ("util_entry_threshold"))) {
					errno = 0;
					lpmd_config->util_entry_threshold = strtol (tmp_value, &pos, 10);
					if (errno || *pos != '\0' || lpmd_config->util_entry_threshold < 0
							|| lpmd_config->util_entry_threshold > 100)
						goto err;
				}
				else if (!strncmp((const char*)cur_node->name, "util_exit_threshold",
									strlen ("util_exit_threshold"))) {
					errno = 0;
					lpmd_config->util_exit_threshold = strtol (tmp_value, &pos, 10);
					if (errno || *pos != '\0' || lpmd_config->util_exit_threshold < 0
							|| lpmd_config->util_exit_threshold > 100)
						goto err;
				}
				else if (!strncmp((const char*)cur_node->name, "EntryHystMS", strlen ("EntryHystMS"))) {
					errno = 0;
					lpmd_config->util_entry_hyst = strtol (tmp_value, &pos, 10);
					if (errno
							|| *pos
									!= '\0'|| lpmd_config->util_entry_hyst < 0 || lpmd_config->util_entry_hyst > UTIL_HYST_MAX)
						goto err;
				}
				else if (!strncmp((const char*)cur_node->name, "ExitHystMS", strlen ("ExitHystMS"))) {
					errno = 0;
					lpmd_config->util_exit_hyst = strtol (tmp_value, &pos, 10);
					if (errno
							|| *pos
									!= '\0'|| lpmd_config->util_exit_hyst < 0 || lpmd_config->util_exit_hyst > UTIL_HYST_MAX)
						goto err;
				}
				else if (!strncmp((const char*)cur_node->name, "lp_mode_epp", strlen ("lp_mode_epp"))) {
					errno = 0;
					lpmd_config->lp_mode_epp = strtol (tmp_value, &pos, 10);
					if (errno
							|| *pos
									!= '\0'|| lpmd_config->lp_mode_epp > 255 || lpmd_config->lp_mode_epp < -1)
						goto err;
					if (lpmd_config->lp_mode_epp < 0)
						lpmd_config->lp_mode_epp = -1;
				}
				else if (!strncmp((const char*)cur_node->name, "IgnoreITMT", strlen ("IgnoreITMT"))) {
					errno = 0;
					lpmd_config->ignore_itmt = strtol (tmp_value, &pos, 10);
					if (errno
							|| *pos
									!= '\0'|| lpmd_config->ignore_itmt < 0 || lpmd_config->ignore_itmt > 1)
						goto err;
				}
				else if (!strncmp((const char*)cur_node->name, "lp_mode_cpus", strlen ("lp_mode_cpus"))) {
					if (!strncmp (tmp_value, "-1", strlen ("-1")))
						lpmd_config->lp_mode_cpus[0] = '\0';
					else
						snprintf (lpmd_config->lp_mode_cpus, sizeof(lpmd_config->lp_mode_cpus),
									"%s", tmp_value);
				}
				else if (!strncmp((const char*)cur_node->name, "PerformanceDef", strlen ("PerformanceDef"))) {
					errno = 0;
					lpmd_config->performance_def = strtol (tmp_value, &pos, 10);
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
				}
				else if (!strncmp((const char*)cur_node->name, "BalancedDef", strlen ("BalancedDef"))) {
					errno = 0;
					lpmd_config->balanced_def = strtol (tmp_value, &pos, 10);
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
				}
				else if (!strncmp((const char*)cur_node->name, "PowersaverDef", strlen ("PowersaverDef"))) {
					errno = 0;
					lpmd_config->powersaver_def = strtol (tmp_value, &pos, 10);
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
				}
				else if (!strncmp((const char*)cur_node->name, "States", strlen ("States"))) {
					errno = 0;
					lpmd_parse_states(doc, cur_node->children, lpmd_config);
				}
				else {
					lpmd_log_info ("Invalid configuration data\n");
					goto err;
				}
				xmlFree (tmp_value);
				continue;
err:			xmlFree (tmp_value);
				lpmd_log_error ("node type: Element, name: %s value: %s\n", cur_node->name,
								tmp_value);
				return LPMD_ERROR;
			}
		}
	}

	/* use entry_threshold == 0 or exit_threshold == 0 to effectively disable util monitor */
	if (lpmd_config->util_entry_threshold && lpmd_config->util_exit_threshold)
		lpmd_config->util_enable = 1;
	else
		lpmd_config->util_enable = 0;

	return LPMD_SUCCESS;
}

int lpmd_get_config(lpmd_config_t *lpmd_config)
{
	char file_name[MAX_FILE_NAME_PATH];
	xmlNode *root_element;
	xmlNode *cur_node;
	struct stat s;
	xmlDoc *doc;

	if (!lpmd_config)
		return LPMD_ERROR;

	snprintf (file_name, MAX_FILE_NAME_PATH, "%s/%s", TDCONFDIR, CONFIG_FILE_NAME);

	lpmd_log_msg ("Reading configuration file %s\n", file_name);

	if (stat (file_name, &s)) {
		lpmd_log_msg ("error: could not find file %s\n", file_name);
		return LPMD_ERROR;
	}

	doc = xmlReadFile (file_name, NULL, 0);
	if (doc == NULL) {
		lpmd_log_msg ("error: could not parse file %s\n", file_name);
		return LPMD_ERROR;
	}

	root_element = xmlDocGetRootElement (doc);
	if (root_element == NULL) {
		lpmd_log_warn ("error: could not get root element \n");
		return LPMD_ERROR;
	}

	cur_node = NULL;

	for (cur_node = root_element; cur_node; cur_node = cur_node->next) {
		if (cur_node->type == XML_ELEMENT_NODE) {
			if (!strncmp ((const char*) cur_node->name, "Configuration",
							strlen ("Configuration"))) {
				if (lpmd_fill_config (doc, cur_node->children, lpmd_config) != LPMD_SUCCESS) {
					xmlFreeDoc (doc);
					return LPMD_ERROR;
				}
			}
		}
	}

	xmlFreeDoc (doc);

	lpmd_dump_config (lpmd_config);

	return LPMD_SUCCESS;
}
