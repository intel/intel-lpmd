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
	if (!lpmd_config)
		return;

	lpmd_log_info ("Mode:%d\n", lpmd_config->mode);
	lpmd_log_info ("HFI LPM Enable:%d\n", lpmd_config->hfi_lpm_enable);
	lpmd_log_info ("HFI SUV Enable:%d\n", lpmd_config->hfi_suv_enable);
	lpmd_log_info ("Util entry threshold:%d\n", lpmd_config->util_entry_threshold);
	lpmd_log_info ("Util exit threshold:%d\n", lpmd_config->util_exit_threshold);
	lpmd_log_info ("Util LP Mode CPUs:%s\n", lpmd_config->lp_mode_cpus);
}

static int lpmd_fill_config(xmlDoc *doc, xmlNode *a_node, lpmd_config_t *lpmd_config)
{
	xmlNode *cur_node = NULL;
	char *tmp_value;
	char *pos;

	if (!doc || !a_node || !lpmd_config)
		return LPMD_ERROR;

	lpmd_config->performance_def = lpmd_config->balanced_def = lpmd_config->powersaver_def = LPM_FORCE_OFF;
	for (cur_node = a_node; cur_node; cur_node = cur_node->next) {
		if (cur_node->type == XML_ELEMENT_NODE) {
			tmp_value = (char*) xmlNodeListGetString (doc, cur_node->xmlChildrenNode, 1);
			if (tmp_value) {
				lpmd_log_info ("node type: Element, name: %s, value: %s\n", cur_node->name,
								tmp_value);
				if (!strncmp((const char*)cur_node->name, "Mode", strlen("Mode"))) {
					errno = 0;
					lpmd_config->mode = strtol (tmp_value, &pos, 10);
					lpmd_log_info ("mode %d, errno %d, tmp_value %p, pos %p\n", lpmd_config->mode,
					errno,
									tmp_value, pos);
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
