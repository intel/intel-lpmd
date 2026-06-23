// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * lpmd_process_cpuset.c: LPMD integration for process cpuset management
 *
 * Copyright (C) 2026 Intel Corporation. All rights reserved.
 *
 * Glue between LPMD and the process_cpuset library.
 *
 * Owns a single process_cpuset_t context that LPMD can lazily create when
 * the <UseProcessCPUSet> XML tag is set. The active P-core / E-core /
 * LP-E-core sets are sourced from lpmd_config_t::core_type_masks, which
 * use the same bit layout as cpu_set_t and can be passed through directly
 * to process_cpuset_set_groups_cpuset().
 */

#include "lpmd.h"
#include "process_cpuset.h"

#include <dirent.h>
#include <errno.h>
#include <linux/cn_proc.h>
#include <linux/connector.h>
#include <linux/netlink.h>
#include <signal.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

#include <libxml/parser.h>
#include <libxml/tree.h>

#define PROCESS_CPUSET_CONFIG_FILE "process_cpuset.xml"
#define PROCESS_CPUSET_USER_CONFIG_FILE "process_cpuset_user.xml"

#define PATH_INTEL_PSTATE_MAX_PERF_PCT "/sys/devices/system/cpu/intel_pstate/max_perf_pct"
#define PATH_SOC_BALANCE_SLIDER "/sys/module/processor_thermal_soc_slider/parameters/slider_balance"
#define PATH_SOC_OFFSET "/sys/module/processor_thermal_soc_slider/parameters/slider_offset"

struct class_tune_owner_t {
	char owner_class[64];
	const char *field_name;
	const char *reset_desc;
	int (*class_has_override)(const struct lpmd_config_t *config,
				      const char *cls);
	void (*apply_override)(const struct lpmd_config_t *config,
			       const char *cls);
	void (*capture_restore_state)(void);
	void (*reset_override)(void);
};

static process_cpuset_t *g_pc_ctx;
static int g_pc_connector_fd = -1;
static struct class_tune_owner_t g_min_perf_owner;
static struct class_tune_owner_t g_max_perf_owner;
static struct class_tune_owner_t g_balance_slider_owner;
static struct class_tune_owner_t g_slider_offset_owner;

static int g_saved_max_perf_pct = SETTING_IGNORE;
static int g_saved_balance_slider = -1;
static int g_saved_slider_offset = -1;

static const struct lpmd_class_tuning_override_t *class_tuning_for_name(
	const struct lpmd_config_t *config, const char *cls)
{
	if (!config || !cls)
		return NULL;
	if (!strcasecmp(cls, "realtime"))
		return &config->pc_class_tuning_realtime;
	if (!strcasecmp(cls, "user_interactive"))
		return &config->pc_class_tuning_user_interactive;
	if (!strcasecmp(cls, "user_initiated"))
		return &config->pc_class_tuning_user_initiated;
	if (!strcasecmp(cls, "Unclassified"))
		return &config->pc_class_tuning_unclassified;
	if (!strcasecmp(cls, "utility"))
		return &config->pc_class_tuning_utility;
	if (!strcasecmp(cls, "background"))
		return &config->pc_class_tuning_background;
	if (!strcasecmp(cls, "GameProfileCPU"))
		return &config->pc_class_tuning_gp_cpu;
	if (!strcasecmp(cls, "GameProfileGPU"))
		return &config->pc_class_tuning_gp_gpu;
	if (!strcasecmp(cls, "GameProfileMixed"))
		return &config->pc_class_tuning_gp_hybrid;
	return NULL;
}

static void apply_class_min_perf_override(const struct lpmd_config_t *config,
					  const char *cls)
{
	const struct lpmd_class_tuning_override_t *ovr;
	struct lpmd_config_state_t tmp_state;
	int on_battery;

	ovr = class_tuning_for_name(config, cls);
	if (!ovr || !ovr->present_mask)
		return;

	on_battery = is_on_battery();
	if (on_battery) {
		if (!(ovr->present_mask & LPMD_CLASS_TUNE_MIN_PERF_PCT_DC))
			return;
	} else {
		if (!(ovr->present_mask & LPMD_CLASS_TUNE_MIN_PERF_PCT_AC))
			return;
	}

	lpmd_init_config_state(&tmp_state);
	if (ovr->present_mask & LPMD_CLASS_TUNE_MIN_PERF_PCT_AC)
		tmp_state.min_perf_pct_ac = ovr->min_perf_pct_ac;
	if (ovr->present_mask & LPMD_CLASS_TUNE_MIN_PERF_PCT_DC)
		tmp_state.min_perf_pct_dc = ovr->min_perf_pct_dc;

	if (process_min_perf_pct_override(&tmp_state))
		lpmd_log_warn("process_cpuset: class=%s min_perf_pct apply failed\n",
			      cls ? cls : "?");
}

static void apply_class_max_perf_override(const struct lpmd_config_t *config,
					  const char *cls)
{
	const struct lpmd_class_tuning_override_t *ovr;
	struct lpmd_config_state_t tmp_state;
	int on_battery;

	ovr = class_tuning_for_name(config, cls);
	if (!ovr || !ovr->present_mask)
		return;

	on_battery = is_on_battery();
	if (on_battery) {
		if (!(ovr->present_mask & LPMD_CLASS_TUNE_MAX_PERF_PCT_DC))
			return;
	} else {
		if (!(ovr->present_mask & LPMD_CLASS_TUNE_MAX_PERF_PCT_AC))
			return;
	}

	lpmd_init_config_state(&tmp_state);
	if (ovr->present_mask & LPMD_CLASS_TUNE_MAX_PERF_PCT_AC)
		tmp_state.max_perf_pct_ac = ovr->max_perf_pct_ac;
	if (ovr->present_mask & LPMD_CLASS_TUNE_MAX_PERF_PCT_DC)
		tmp_state.max_perf_pct_dc = ovr->max_perf_pct_dc;

	if (process_max_perf_pct(&tmp_state))
		lpmd_log_warn("process_cpuset: class=%s max_perf_pct apply failed\n",
			      cls ? cls : "?");
}

static void apply_class_balance_slider_override(const struct lpmd_config_t *config,
						const char *cls)
{
	const struct lpmd_class_tuning_override_t *ovr;
	struct lpmd_config_state_t tmp_state;
	int on_battery;

	if (!config)
		return;

	ovr = class_tuning_for_name(config, cls);
	if (!ovr || !ovr->present_mask)
		return;

	on_battery = is_on_battery();
	if (on_battery) {
		if (!(ovr->present_mask & LPMD_CLASS_TUNE_BALANCE_SLIDER_DC))
			return;
	} else {
		if (!(ovr->present_mask & LPMD_CLASS_TUNE_BALANCE_SLIDER_AC))
			return;
	}

	lpmd_init_config_state(&tmp_state);
	if (ovr->present_mask & LPMD_CLASS_TUNE_BALANCE_SLIDER_AC)
		tmp_state.balance_slider_ac = ovr->balance_slider_ac;
	if (ovr->present_mask & LPMD_CLASS_TUNE_BALANCE_SLIDER_DC)
		tmp_state.balance_slider_dc = ovr->balance_slider_dc;

	if (process_balance_slider_only((struct lpmd_config_t *)config, &tmp_state))
		lpmd_log_warn("process_cpuset: class=%s balance_slider apply failed\n",
			      cls ? cls : "?");
}

static void apply_class_slider_offset_override(const struct lpmd_config_t *config,
					      const char *cls)
{
	const struct lpmd_class_tuning_override_t *ovr;
	struct lpmd_config_state_t tmp_state;
	int on_battery;

	if (!config)
		return;

	ovr = class_tuning_for_name(config, cls);
	if (!ovr || !ovr->present_mask)
		return;

	on_battery = is_on_battery();
	if (on_battery) {
		if (!(ovr->present_mask & LPMD_CLASS_TUNE_SLIDER_OFFSET_DC))
			return;
	} else {
		if (!(ovr->present_mask & LPMD_CLASS_TUNE_SLIDER_OFFSET_AC))
			return;
	}

	lpmd_init_config_state(&tmp_state);
	if (ovr->present_mask & LPMD_CLASS_TUNE_SLIDER_OFFSET_AC)
		tmp_state.slider_offset_ac = ovr->slider_offset_ac;
	if (ovr->present_mask & LPMD_CLASS_TUNE_SLIDER_OFFSET_DC)
		tmp_state.slider_offset_dc = ovr->slider_offset_dc;

	if (process_slider_offset_only((struct lpmd_config_t *)config, &tmp_state))
		lpmd_log_warn("process_cpuset: class=%s slider_offset apply failed\n",
			      cls ? cls : "?");
}

static int pid_is_live(pid_t pid)
{
	if (pid <= 0)
		return 0;

	if (kill(pid, 0) == 0)
		return 1;

	if (errno == EPERM)
		return 1;

	return 0;
}

static int class_has_live_attached_pid(const char *cls)
{
	size_t n, i;

	if (!g_pc_ctx || !cls || !*cls)
		return 0;

	n = process_cpuset_attached_count(g_pc_ctx);
	for (i = 0; i < n; i++) {
		pid_t pid = 0;
		char unit[128] = { 0 };
		const char *item_cls = NULL;
		int use_p = 0, use_e = 0, use_l = 0;

		if (process_cpuset_attached_get_ex(g_pc_ctx, i, &pid, unit,
					   sizeof(unit), &item_cls,
					   &use_p, &use_e, &use_l) < 0)
			continue;

		if (!item_cls || strcasecmp(item_cls, cls))
			continue;

		if (pid_is_live(pid))
			return 1;
	}

	return 0;
}

static int class_has_min_perf_override(const struct lpmd_config_t *config,
					 const char *cls)
{
	const struct lpmd_class_tuning_override_t *ovr;
	int on_battery;

	if (!config || !cls || !*cls)
		return 0;

	ovr = class_tuning_for_name(config, cls);
	if (!ovr || !ovr->present_mask)
		return 0;

	on_battery = is_on_battery();
	if (on_battery)
		return !!(ovr->present_mask & LPMD_CLASS_TUNE_MIN_PERF_PCT_DC);

	return !!(ovr->present_mask & LPMD_CLASS_TUNE_MIN_PERF_PCT_AC);
}

static int class_has_max_perf_override(const struct lpmd_config_t *config,
					 const char *cls)
{
	const struct lpmd_class_tuning_override_t *ovr;
	int on_battery;

	if (!config || !cls || !*cls)
		return 0;

	ovr = class_tuning_for_name(config, cls);
	if (!ovr || !ovr->present_mask)
		return 0;

	on_battery = is_on_battery();
	if (on_battery)
		return !!(ovr->present_mask & LPMD_CLASS_TUNE_MAX_PERF_PCT_DC);

	return !!(ovr->present_mask & LPMD_CLASS_TUNE_MAX_PERF_PCT_AC);
}

static int class_has_balance_slider_override(const struct lpmd_config_t *config,
					      const char *cls)
{
	const struct lpmd_class_tuning_override_t *ovr;
	int on_battery;

	if (!config || !cls || !*cls)
		return 0;

	ovr = class_tuning_for_name(config, cls);
	if (!ovr || !ovr->present_mask)
		return 0;

	on_battery = is_on_battery();
	if (on_battery)
		return !!(ovr->present_mask & LPMD_CLASS_TUNE_BALANCE_SLIDER_DC);

	return !!(ovr->present_mask & LPMD_CLASS_TUNE_BALANCE_SLIDER_AC);
}

static int class_has_slider_offset_override(const struct lpmd_config_t *config,
					     const char *cls)
{
	const struct lpmd_class_tuning_override_t *ovr;
	int on_battery;

	if (!config || !cls || !*cls)
		return 0;

	ovr = class_tuning_for_name(config, cls);
	if (!ovr || !ovr->present_mask)
		return 0;

	on_battery = is_on_battery();
	if (on_battery)
		return !!(ovr->present_mask & LPMD_CLASS_TUNE_SLIDER_OFFSET_DC);

	return !!(ovr->present_mask & LPMD_CLASS_TUNE_SLIDER_OFFSET_AC);
}

static void reset_owned_min_perf_pct_to_zero(void)
{
	struct lpmd_config_state_t tmp_state;

	lpmd_init_config_state(&tmp_state);
	tmp_state.min_perf_pct_ac = 0;
	tmp_state.min_perf_pct_dc = 0;
	if (process_min_perf_pct(&tmp_state))
		lpmd_log_warn("process_cpuset: failed to reset owned min_perf_pct to 0\n");
}

static void capture_owned_max_perf_pct_state(void)
{
	if (lpmd_read_int(PATH_INTEL_PSTATE_MAX_PERF_PCT, &g_saved_max_perf_pct, -1)) {
		g_saved_max_perf_pct = SETTING_IGNORE;
		lpmd_log_warn("process_cpuset: failed to read current max_perf_pct for restore\n");
	}
}

static void reset_owned_max_perf_pct_to_zero(void)
{
	struct lpmd_config_state_t tmp_state;

	if (g_saved_max_perf_pct == SETTING_IGNORE)
		return;

	lpmd_init_config_state(&tmp_state);
	tmp_state.max_perf_pct_ac = g_saved_max_perf_pct;
	tmp_state.max_perf_pct_dc = g_saved_max_perf_pct;
	if (process_max_perf_pct(&tmp_state))
		lpmd_log_warn("process_cpuset: failed to restore owned max_perf_pct\n");
}

static void capture_owned_balance_slider_state(void)
{
	if (lpmd_read_int(PATH_SOC_BALANCE_SLIDER, &g_saved_balance_slider, -1)) {
		g_saved_balance_slider = -1;
		lpmd_log_warn("process_cpuset: failed to read current balance_slider for restore\n");
	}
}

static void reset_owned_balance_slider_to_default(void)
{
	struct lpmd_config_t *config = get_lpmd_config();
	struct lpmd_config_state_t tmp_state;

	if (!config)
		return;

	if (g_saved_balance_slider >= 0) {
		lpmd_init_config_state(&tmp_state);
		tmp_state.balance_slider_ac = g_saved_balance_slider;
		tmp_state.balance_slider_dc = g_saved_balance_slider;
		if (!process_balance_slider_only(config, &tmp_state))
			return;
	}

	process_balance_slider_default_update(config);
}

static void capture_owned_slider_offset_state(void)
{
	if (lpmd_read_int(PATH_SOC_OFFSET, &g_saved_slider_offset, -1)) {
		g_saved_slider_offset = -1;
		lpmd_log_warn("process_cpuset: failed to read current slider_offset for restore\n");
	}
}

static void reset_owned_slider_offset_to_default(void)
{
	struct lpmd_config_t *config = get_lpmd_config();
	struct lpmd_config_state_t tmp_state;

	if (!config)
		return;

	if (g_saved_slider_offset >= 0) {
		lpmd_init_config_state(&tmp_state);
		tmp_state.slider_offset_ac = g_saved_slider_offset;
		tmp_state.slider_offset_dc = g_saved_slider_offset;
		if (!process_slider_offset_only(config, &tmp_state))
			return;
	}

	process_slider_offset_default_update(config);
}

static void clear_class_tune_owner(struct class_tune_owner_t *owner,
				    int reset_field)
{
	if (!owner || !owner->owner_class[0])
		return;

	if (reset_field && owner->reset_override)
		owner->reset_override();

	owner->owner_class[0] = '\0';
}

static void sync_class_tune_owner(struct class_tune_owner_t *owner,
				 const struct lpmd_config_t *config,
				 const char *candidate_cls)
{
	if (!owner)
		return;

	if (owner->owner_class[0] &&
	    !class_has_live_attached_pid(owner->owner_class)) {
		if (owner->reset_override)
			owner->reset_override();
		lpmd_log_info("process_cpuset: owner class %s has no live tasks, %s %s\n",
			      owner->owner_class,
			      owner->field_name ? owner->field_name : "tuning field",
			      owner->reset_desc ? owner->reset_desc : "reset");
		owner->owner_class[0] = '\0';
	}

	if (!candidate_cls || !*candidate_cls)
		return;

	if (!owner->class_has_override ||
	    !owner->class_has_override(config, candidate_cls))
		return;

	if (owner->owner_class[0] &&
	    strcasecmp(owner->owner_class, candidate_cls))
		return;

	if (!owner->owner_class[0]) {
		if (owner->capture_restore_state)
			owner->capture_restore_state();

		snprintf(owner->owner_class, sizeof(owner->owner_class),
			 "%s", candidate_cls);
		lpmd_log_info("process_cpuset: class %s is now %s owner\n",
			      owner->owner_class,
			      owner->field_name ? owner->field_name : "tuning field");
	}

	if (owner->apply_override)
		owner->apply_override(config, candidate_cls);
}

static void sync_min_perf_owner(const struct lpmd_config_t *config,
			      const char *candidate_cls)
{
	sync_class_tune_owner(&g_min_perf_owner, config, candidate_cls);
}

static void sync_max_perf_owner(const struct lpmd_config_t *config,
			      const char *candidate_cls)
{
	sync_class_tune_owner(&g_max_perf_owner, config, candidate_cls);
}

static void sync_balance_slider_owner(const struct lpmd_config_t *config,
				  const char *candidate_cls)
{
	sync_class_tune_owner(&g_balance_slider_owner, config, candidate_cls);
}

static void sync_slider_offset_owner(const struct lpmd_config_t *config,
				 const char *candidate_cls)
{
	sync_class_tune_owner(&g_slider_offset_owner, config, candidate_cls);
}

static void apply_class_tuning_override_for_pid(
	const struct lpmd_config_t *config, pid_t pid,
	struct class_tune_owner_t *owner)
{
	size_t n, i;

	if (!g_pc_ctx || !config || pid <= 0 || !owner)
		return;

	n = process_cpuset_attached_count(g_pc_ctx);
	for (i = 0; i < n; i++) {
		pid_t item_pid = 0;
		char unit[128] = { 0 };
		const char *cls = NULL;
		int use_p = 0, use_e = 0, use_l = 0;

		if (process_cpuset_attached_get_ex(g_pc_ctx, i, &item_pid, unit,
					   sizeof(unit), &cls, &use_p,
					   &use_e, &use_l) < 0)
			continue;
		if (item_pid != pid)
			continue;

		sync_class_tune_owner(owner, config, cls);
		return;
	}

	sync_class_tune_owner(owner, config, NULL);
}

static void apply_class_min_perf_override_for_pid(
	const struct lpmd_config_t *config, pid_t pid)
{
	apply_class_tuning_override_for_pid(config, pid, &g_min_perf_owner);
}

static void apply_class_max_perf_override_for_pid(
	const struct lpmd_config_t *config, pid_t pid)
{
	apply_class_tuning_override_for_pid(config, pid, &g_max_perf_owner);
}

static void apply_class_balance_slider_override_for_pid(
	const struct lpmd_config_t *config, pid_t pid)
{
	apply_class_tuning_override_for_pid(config, pid, &g_balance_slider_owner);
}

static void apply_class_slider_offset_override_for_pid(
	const struct lpmd_config_t *config, pid_t pid)
{
	apply_class_tuning_override_for_pid(config, pid, &g_slider_offset_owner);
}

static struct class_tune_owner_t g_min_perf_owner = {
	.field_name = "min_perf_pct",
	.reset_desc = "reset to 0",
	.class_has_override = class_has_min_perf_override,
	.apply_override = apply_class_min_perf_override,
	.capture_restore_state = NULL,
	.reset_override = reset_owned_min_perf_pct_to_zero,
};

static struct class_tune_owner_t g_max_perf_owner = {
	.field_name = "max_perf_pct",
	.reset_desc = "restored",
	.class_has_override = class_has_max_perf_override,
	.apply_override = apply_class_max_perf_override,
	.capture_restore_state = capture_owned_max_perf_pct_state,
	.reset_override = reset_owned_max_perf_pct_to_zero,
};

static struct class_tune_owner_t g_balance_slider_owner = {
	.field_name = "balance_slider",
	.reset_desc = "restored",
	.class_has_override = class_has_balance_slider_override,
	.apply_override = apply_class_balance_slider_override,
	.capture_restore_state = capture_owned_balance_slider_state,
	.reset_override = reset_owned_balance_slider_to_default,
};

static struct class_tune_owner_t g_slider_offset_owner = {
	.field_name = "slider_offset",
	.reset_desc = "restored",
	.class_has_override = class_has_slider_offset_override,
	.apply_override = apply_class_slider_offset_override,
	.capture_restore_state = capture_owned_slider_offset_state,
	.reset_override = reset_owned_slider_offset_to_default,
};

static int class_tune_owner_is_locked(const struct class_tune_owner_t *owner,
				      const struct lpmd_config_t *config)
{
	if (!owner || !config || !owner->owner_class[0])
		return 0;

	if (!class_has_live_attached_pid(owner->owner_class))
		return 0;

	if (!owner->class_has_override)
		return 0;

	return owner->class_has_override(config, owner->owner_class);
}

int lpmd_process_cpuset_min_perf_pct_locked(void)
{
	struct lpmd_config_t *config = get_lpmd_config();

	if (!g_pc_ctx)
		return 0;

	return class_tune_owner_is_locked(&g_min_perf_owner, config);
}

int lpmd_process_cpuset_balance_slider_locked(void)
{
	struct lpmd_config_t *config = get_lpmd_config();

	if (!g_pc_ctx)
		return 0;

	return class_tune_owner_is_locked(&g_balance_slider_owner, config);
}

int lpmd_process_cpuset_slider_offset_locked(void)
{
	struct lpmd_config_t *config = get_lpmd_config();

	if (!g_pc_ctx)
		return 0;

	return class_tune_owner_is_locked(&g_slider_offset_owner, config);
}

const char *lpmd_process_cpuset_min_perf_pct_owner(void)
{
	if (!g_pc_ctx)
		return NULL;

	return g_min_perf_owner.owner_class[0] ? g_min_perf_owner.owner_class : NULL;
}

const char *lpmd_process_cpuset_balance_slider_owner(void)
{
	if (!g_pc_ctx)
		return NULL;

	return g_balance_slider_owner.owner_class[0] ? g_balance_slider_owner.owner_class : NULL;
}

const char *lpmd_process_cpuset_slider_offset_owner(void)
{
	if (!g_pc_ctx)
		return NULL;

	return g_slider_offset_owner.owner_class[0] ? g_slider_offset_owner.owner_class : NULL;
}

static void user_xml_path(char *out, size_t cap)
{
	snprintf(out, cap, "%s/%s", TDCONFDIR, PROCESS_CPUSET_USER_CONFIG_FILE);
}

/*
 * Append a <Process> entry to the user-editable XML file. If the file
 * doesn't exist yet, create a minimal one with a single
 * <ProcessCpusetConfig> root. If an entry with the same <Name> already
 * exists, replace it. Returns 0 on success, -1 on error.
 */
static int user_xml_upsert_process(const char *path, const char *name,
				   const char *classification)
{
	xmlDoc *doc = NULL;
	xmlNode *root = NULL;
	xmlNode *cur, *new_proc;
	struct stat st;
	int rc = -1;

	if (!path || !name || !*name || !classification || !*classification)
		return -1;

	if (stat(path, &st) == 0) {
		doc = xmlReadFile(path, NULL, XML_PARSE_NOBLANKS);
		if (doc)
			root = xmlDocGetRootElement(doc);
		if (!root ||
		    strcmp((const char *)root->name, "ProcessCpusetConfig")) {
			lpmd_log_warn(
				"process_cpuset: %s missing/invalid root, recreating\n",
				path);
			if (doc)
				xmlFreeDoc(doc);
			doc = NULL;
			root = NULL;
		}
	}

	if (!doc) {
		doc = xmlNewDoc(BAD_CAST "1.0");
		if (!doc)
			return -1;
		root = xmlNewNode(NULL, BAD_CAST "ProcessCpusetConfig");
		if (!root) {
			xmlFreeDoc(doc);
			return -1;
		}
		xmlDocSetRootElement(doc, root);
	}

	/* Replace any existing <Process> with the same <Name>. */
	for (cur = root->children; cur;) {
		xmlNode *next = cur->next;
		if (cur->type == XML_ELEMENT_NODE &&
		    !strcmp((const char *)cur->name, "Process")) {
			xmlNode *kid;
			for (kid = cur->children; kid; kid = kid->next) {
				if (kid->type != XML_ELEMENT_NODE)
					continue;
				if (strcmp((const char *)kid->name, "Name"))
					continue;
				xmlChar *val = xmlNodeListGetString(
					doc, kid->xmlChildrenNode, 1);
				if (val && !strcmp((const char *)val, name)) {
					xmlUnlinkNode(cur);
					xmlFreeNode(cur);
				}
				if (val)
					xmlFree(val);
				break;
			}
		}
		cur = next;
	}

	new_proc = xmlNewChild(root, NULL, BAD_CAST "Process", NULL);
	if (!new_proc)
		goto out;
	if (!xmlNewChild(new_proc, NULL, BAD_CAST "Name", BAD_CAST name))
		goto out;
	if (!xmlNewChild(new_proc, NULL, BAD_CAST "Classification",
			 BAD_CAST classification))
		goto out;

	if (xmlSaveFormatFileEnc(path, doc, "UTF-8", 1) < 0) {
		lpmd_log_warn("process_cpuset: failed to write %s\n", path);
		goto out;
	}
	rc = 0;
out:
	xmlFreeDoc(doc);
	return rc;
}

/*
 * Initialize the per-process cpuset manager from LPMD state.
 *
 * Must be called AFTER lpmd_build_config_states() (so the active core
 * sets are valid) and BEFORE free_cpu_type_masks() (so core_type_masks[]
 * are still alive). No-op if <UseProcessCPUSet> is not set.
 */
int lpmd_process_cpuset_init(struct lpmd_config_t *config)
{
	char path[MAX_FILE_NAME_PATH];
	size_t setsize;
	int n;

	if (!config || !config->use_process_cpuset)
		return 0;

	if (g_pc_ctx) {
		lpmd_log_warn("process_cpuset already initialized\n");
		return 0;
	}

	g_pc_ctx = process_cpuset_new();
	if (!g_pc_ctx) {
		lpmd_log_error("process_cpuset_new failed\n");
		return -1;
	}

	snprintf(path, sizeof(path), "%s/%s", TDCONFDIR,
		 PROCESS_CPUSET_CONFIG_FILE);
	n = process_cpuset_load_config(g_pc_ctx, path);
	if (n < 0) {
		lpmd_log_error("process_cpuset_load_config(%s) failed\n", path);
		process_cpuset_free(g_pc_ctx);
		g_pc_ctx = NULL;
		return -1;
	}
	lpmd_log_info("process_cpuset: loaded %d entries from %s\n", n, path);

	/* Layer the user-editable overlay (process_cpuset_user.xml) on top.
     * Missing file is fine and is silently ignored. */
	{
		char user_path[MAX_FILE_NAME_PATH];
		int un;

		user_xml_path(user_path, sizeof(user_path));
		un = process_cpuset_load_config_overlay(g_pc_ctx, user_path);
		if (un > 0)
			lpmd_log_info(
				"process_cpuset: merged %d user entries from %s\n",
				un, user_path);
		else if (un < 0)
			lpmd_log_warn(
				"process_cpuset: parse error in %s (ignored)\n",
				user_path);
	}

	/*
     * Apply CPU-model-specific <ClassDefaults> overrides parsed from
     * the matching <States> stanza of intel_lpmd_config_*.xml. Empty
     * fields are left as configured by process_cpuset.xml.
     */
	if (config->pc_class_default_realtime[0] ||
	    config->pc_class_default_user_interactive[0] ||
	    config->pc_class_default_user_initiated[0] ||
	    config->pc_class_default_unclassified[0] ||
	    config->pc_class_default_utility[0] ||
	    config->pc_class_default_background[0] ||
	    config->pc_class_default_gp_cpu[0] ||
	    config->pc_class_default_gp_gpu[0] ||
	    config->pc_class_default_gp_hybrid[0]) {
		if (process_cpuset_override_class_defaults(
			    g_pc_ctx, config->pc_class_default_realtime,
			    config->pc_class_default_user_interactive,
			    config->pc_class_default_user_initiated,
			    config->pc_class_default_unclassified,
			    config->pc_class_default_utility,
			    config->pc_class_default_background,
			    config->pc_class_default_gp_cpu,
			    config->pc_class_default_gp_gpu,
			    config->pc_class_default_gp_hybrid) < 0) {
			lpmd_log_warn(
				"process_cpuset: ClassDefaults override failed\n");
		} else {
			lpmd_log_info(
				"process_cpuset: applied per-CPU ClassDefaults override\n");
		}
	}

	/* core_type_masks[] uses the same little-endian bit layout as
     * cpu_set_t, so it's safe to cast and pass straight through. */
	setsize = (size_t)(get_max_cpus() / 8);
	if (process_cpuset_set_groups_cpuset(
		    g_pc_ctx,
		    (const cpu_set_t *)config->core_type_masks[P_CORE],
		    (const cpu_set_t *)config->core_type_masks[E_CORE],
		    (const cpu_set_t *)config->core_type_masks[L_CORE],
		    setsize) < 0) {
		lpmd_log_error("process_cpuset_set_groups_cpuset failed\n");
		process_cpuset_free(g_pc_ctx);
		g_pc_ctx = NULL;
		return -1;
	}

	/* Debug: log per-classification cpusets so the operator can
     * see what each tier maps to under the active P/E/LP-E sets. */
	process_cpuset_log_class_defaults(g_pc_ctx);

	/*
     * Do NOT perform the initial bind here: the daemon's state at
     * startup is LPMD_OFF, and OFF must be fully inert (no transient
     * cpuset scopes). The first transition to AUTO/PROCESS-PRECONFIG/ON triggers
     * a rescan that attaches matching PIDs.
     */
	return 0;
}

/*
 * Stop every transient cpuset scope started by this context, then drop
 * the context. Safe to call even if init was never run.
 *
 * Uses the non-killing release path so processes survive intel_lpmd
 * shutdown / restart with their default (system-inherited) affinity.
 */
void lpmd_process_cpuset_uninit(void)
{
	clear_class_tune_owner(&g_min_perf_owner, 1);
	clear_class_tune_owner(&g_max_perf_owner, 1);
	clear_class_tune_owner(&g_balance_slider_owner, 1);
	clear_class_tune_owner(&g_slider_offset_owner, 1);

	if (!g_pc_ctx)
		return;

	lpmd_process_cpuset_proc_connector_uninit();
	process_cpuset_release_all(g_pc_ctx);
	process_cpuset_free(g_pc_ctx);
	g_pc_ctx = NULL;
}

/*
 * Stop every transient cpuset scope started by this context but keep
 * the context alive (config, groups, class defaults are preserved).
 * After this call no PIDs are bound; a subsequent
 * lpmd_process_cpuset_rescan() (or the periodic rescan) will re-attach
 * matching PIDs from <Process> entries again.
 */
void lpmd_process_cpuset_unbind_all(void)
{
	int n;

	clear_class_tune_owner(&g_min_perf_owner, 1);
	clear_class_tune_owner(&g_max_perf_owner, 1);
	clear_class_tune_owner(&g_balance_slider_owner, 1);
	clear_class_tune_owner(&g_slider_offset_owner, 1);

	if (!g_pc_ctx) {
		lpmd_log_msg("process_cpuset: not active; nothing to unbind\n");
		return;
	}

	/*
     * Use the release path (migrate PID to root cgroup, then stop the
     * empty scope) instead of process_cpuset_stop_all(), which would
     * send SIGTERM to every bound process via systemd's default
     * scope KillMode.
     */
	n = process_cpuset_release_all(g_pc_ctx);
	if (n < 0)
		lpmd_log_warn("process_cpuset: unbind-all failed\n");
	else
		lpmd_log_msg(
			"process_cpuset: released %d PID(s) from transient cpuset scopes\n",
			n);
}

/*
 * Periodic rescan of /proc to attach any newly-spawned matching PIDs
 * that didn't exist when lpmd_process_cpuset_init() ran. No-op if
 * <UseProcessCPUSet> is disabled.
 */
void lpmd_process_cpuset_rescan(void)
{
	int n;
	size_t i, attached_n;
	struct lpmd_config_t *config;

	if (!g_pc_ctx)
		return;

	/* OFF means truly nothing: do not bind any PID. */
	if (get_lpmd_state() == LPMD_OFF)
		return;

	n = process_cpuset_apply_once(g_pc_ctx, 0);
	if (n > 0)
		lpmd_log_info("process_cpuset: rescan attached %d new PIDs\n",
			      n);
	else if (n < 0)
		lpmd_log_warn("process_cpuset: rescan failed\n");

	if (n <= 0) {
		sync_min_perf_owner(get_lpmd_config(), NULL);
		sync_max_perf_owner(get_lpmd_config(), NULL);
		sync_balance_slider_owner(get_lpmd_config(), NULL);
		sync_slider_offset_owner(get_lpmd_config(), NULL);
		return;
	}

	config = get_lpmd_config();
	if (!config)
		return;

	attached_n = process_cpuset_attached_count(g_pc_ctx);
	for (i = 0; i < attached_n; i++) {
		pid_t pid = 0;
		char unit[128] = { 0 };
		const char *cls = NULL;
		int use_p = 0, use_e = 0, use_l = 0;

		if (process_cpuset_attached_get_ex(g_pc_ctx, i, &pid, unit,
						   sizeof(unit), &cls, &use_p,
						   &use_e, &use_l) < 0)
			continue;
		sync_min_perf_owner(config, cls);
		sync_max_perf_owner(config, cls);
		sync_balance_slider_owner(config, cls);
		sync_slider_offset_owner(config, cls);
	}

	sync_min_perf_owner(config, NULL);
	sync_max_perf_owner(config, NULL);
	sync_balance_slider_owner(config, NULL);
	sync_slider_offset_owner(config, NULL);
}

/*
 * Runtime add of a <Process> entry. Updates the in-memory ctx so the
 * next rescan binds matching PIDs, and persists to the user-editable
 * XML overlay so the entry survives daemon restart.
 *
 * Returns 0 on success (entry added or replaced), -1 on validation /
 * I/O failure. Safe to call when process_cpuset is disabled (returns
 * -1 with a warning).
 */
int lpmd_process_cpuset_add_process(const char *name,
				    const char *classification)
{
	char user_path[MAX_FILE_NAME_PATH];
	int rc;

	if (!g_pc_ctx) {
		lpmd_log_warn("process_cpuset: not active; cannot add '%s'\n",
			      name ? name : "(null)");
		return -1;
	}
	if (!name || !*name || !classification || !*classification) {
		lpmd_log_warn(
			"process_cpuset: add: missing name or classification\n");
		return -1;
	}

	rc = process_cpuset_add_entry(g_pc_ctx, name, classification);
	if (rc < 0) {
		lpmd_log_warn(
			"process_cpuset: add: rejected name='%s' class='%s' "
			"(invalid classification or table full)\n",
			name, classification);
		return -1;
	}

	user_xml_path(user_path, sizeof(user_path));
	if (user_xml_upsert_process(user_path, name, classification) < 0) {
		lpmd_log_warn(
			"process_cpuset: add: in-memory updated but persist to %s failed\n",
			user_path);
		return -1;
	}

	lpmd_log_msg("process_cpuset: %s '%s' as %s (persisted to %s)\n",
		     rc == 1 ? "added" : "updated", name, classification,
		     user_path);

	/* Bind any matching PIDs that already exist. Honors the OFF gate
     * inside lpmd_process_cpuset_rescan(). */
	lpmd_process_cpuset_rescan();
	return 0;
}

/*
 * Promote @pid to user_interactive (typically the focused window/tab),
 * or demote the previously-promoted PID when @pid == 0. Thin wrapper
 * around process_cpuset_set_focus_pid() with logging and the OFF gate.
 *
 * Returns 0 on success (including no-op when focus pid is not
 * attached yet), -1 on error.
 */
int lpmd_process_cpuset_set_focus_pid(pid_t pid)
{
	if (!g_pc_ctx) {
		lpmd_log_warn(
			"process_cpuset: not active; ignoring focus pid %d\n",
			(int)pid);
		return -1;
	}
	if (process_cpuset_set_focus_pid(g_pc_ctx, pid) < 0) {
		lpmd_log_warn("process_cpuset: set_focus_pid(%d) failed\n",
			      (int)pid);
		return -1;
	}
	lpmd_log_debug("process_cpuset: focus pid = %d\n", (int)pid);
	return 0;
}

/*
 * Announce / withdraw a user-session focus relay (e.g.
 * intel_lpmd_focus_helper). Until the helper calls this with
 * @present=1, user_initiated mirrors user_interactive so user-session
 * apps aren't penalised in the absence of any focus signal.
 */
int lpmd_process_cpuset_set_focus_helper_present(int present)
{
	if (!g_pc_ctx) {
		lpmd_log_warn(
			"process_cpuset: not active; ignoring focus helper "
			"%s\n",
			present ? "present" : "absent");
		return -1;
	}
	if (process_cpuset_set_focus_helper_present(g_pc_ctx, present) < 0) {
		lpmd_log_warn("process_cpuset: set_focus_helper_present(%d) "
			      "failed\n",
			      present);
		return -1;
	}
	lpmd_log_info("process_cpuset: focus helper %s\n",
		      present ? "PRESENT" : "ABSENT");
	return 0;
}

/*
 * ---------- Kernel proc connector (event-driven classification) ----------
 *
 * Subscribe to the kernel's process events multicast group so we can
 * attach matching PIDs the moment they exec(), instead of waiting for
 * the periodic /proc rescan. Requires CAP_NET_ADMIN; if the socket
 * cannot be opened we silently fall back to the rescan-only path.
 */

/* Wire payload for the PROC_CN_MCAST_LISTEN/IGNORE control messages and
 * for incoming events. Keeps the cn_msg's flexible array trailing the
 * proc_event payload so the kernel sees a single contiguous buffer. */
struct pc_cn_subscribe_msg {
	struct nlmsghdr nl;
	struct cn_msg cn;
	enum proc_cn_mcast_op op;
} __attribute__((packed));

struct pc_cn_event_msg {
	struct nlmsghdr nl;
	struct cn_msg cn;
	struct proc_event ev;
} __attribute__((packed));

static int proc_connector_send_op(int fd, enum proc_cn_mcast_op op)
{
	struct pc_cn_subscribe_msg msg;

	memset(&msg, 0, sizeof(msg));
	msg.nl.nlmsg_len = sizeof(msg);
	msg.nl.nlmsg_type = NLMSG_DONE;
	msg.nl.nlmsg_pid = getpid();
	msg.cn.id.idx = CN_IDX_PROC;
	msg.cn.id.val = CN_VAL_PROC;
	msg.cn.len = sizeof(op);
	msg.op = op;

	if (send(fd, &msg, sizeof(msg), 0) != (ssize_t)sizeof(msg)) {
		lpmd_log_warn(
			"process_cpuset: connector send(op=%d) failed: %s\n",
			(int)op, strerror(errno));
		return -1;
	}
	return 0;
}

/*
 * Open and subscribe a NETLINK_CONNECTOR socket for PROC_EVENT_*.
 * Returns the fd on success (>= 0), or -1 on failure (lack of
 * CAP_NET_ADMIN, kernel without CONFIG_PROC_EVENTS, etc.). Idempotent;
 * subsequent calls return the cached fd.
 */
int lpmd_process_cpuset_proc_connector_init(void)
{
	struct sockaddr_nl sa;
	int fd;

	if (!g_pc_ctx)
		return -1;
	if (g_pc_connector_fd >= 0)
		return g_pc_connector_fd;

	fd = socket(PF_NETLINK, SOCK_DGRAM | SOCK_CLOEXEC, NETLINK_CONNECTOR);
	if (fd < 0) {
		lpmd_log_warn(
			"process_cpuset: open NETLINK_CONNECTOR failed: %s\n",
			strerror(errno));
		return -1;
	}

	memset(&sa, 0, sizeof(sa));
	sa.nl_family = AF_NETLINK;
	sa.nl_pid = getpid();
	sa.nl_groups = CN_IDX_PROC;
	if (bind(fd, (struct sockaddr *)&sa, sizeof(sa)) < 0) {
		lpmd_log_warn("process_cpuset: connector bind failed: %s\n",
			      strerror(errno));
		close(fd);
		return -1;
	}

	/* The proc connector emits one event per fork/exec/exit system-wide,
     * which on a busy host (boot, login, build) can easily burst past
     * the default ~208 KiB SO_RCVBUF and cause ENOBUFS drops. Bump the
     * buffer; fall back to the privileged SO_RCVBUFFORCE if the
     * unprivileged setsockopt is capped by net.core.rmem_max. */
	{
		int rcvbuf = 8 * 1024 * 1024; /* 8 MiB */
		if (setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &rcvbuf,
			       sizeof(rcvbuf)) < 0) {
			if (setsockopt(fd, SOL_SOCKET, SO_RCVBUFFORCE, &rcvbuf,
				       sizeof(rcvbuf)) < 0) {
				lpmd_log_info(
					"process_cpuset: SO_RCVBUF bump failed: %s\n",
					strerror(errno));
			}
		}
	}

	if (proc_connector_send_op(fd, PROC_CN_MCAST_LISTEN) < 0) {
		close(fd);
		return -1;
	}

	g_pc_connector_fd = fd;
	lpmd_log_info(
		"process_cpuset: proc-connector listener active (fd=%d)\n", fd);
	return fd;
}

/* Drain one kernel notification batch from the connector socket and
 * dispatch interesting events (exec, fork) to process_cpuset_apply_pid().
 * Safe to call from the main poll loop on POLLIN. */
void lpmd_process_cpuset_proc_connector_handle(void)
{
	/* The connector multiplexes many subsystems; loop until EAGAIN. */
	for (;;) {
		struct pc_cn_event_msg msg;
		ssize_t r;
		pid_t pid;

		if (g_pc_connector_fd < 0)
			return;

		r = recv(g_pc_connector_fd, &msg, sizeof(msg), MSG_DONTWAIT);
		if (r < 0) {
			if (errno == EAGAIN || errno == EWOULDBLOCK)
				return;
			if (errno == EINTR)
				continue;
			if (errno == ENOBUFS) {
				/* Kernel dropped events because our receive buffer
                 * filled up. We've lost notifications, so the only
                 * safe recovery is a full /proc rescan to catch any
                 * matching PIDs spawned during the burst. */
				lpmd_log_info(
					"process_cpuset: connector overflow; "
					"triggering full rescan\n");
				lpmd_process_cpuset_rescan();
				continue;
			}
			lpmd_log_warn("process_cpuset: connector recv: %s\n",
				      strerror(errno));
			return;
		}
		if (r == 0)
			return;
		if ((size_t)r < sizeof(msg))
			continue;
		if (msg.cn.id.idx != CN_IDX_PROC ||
		    msg.cn.id.val != CN_VAL_PROC)
			continue;

		/* Skip when OFF so the daemon can't bind anything in idle mode. */
		if (get_lpmd_state() == LPMD_OFF)
			continue;

		switch (msg.ev.what) {
		case PROC_EVENT_EXEC:
			pid = msg.ev.event_data.exec.process_pid;
			break;
		case PROC_EVENT_EXIT: {
			struct lpmd_config_t *config = get_lpmd_config();

			sync_min_perf_owner(config, NULL);
			sync_max_perf_owner(config, NULL);
			sync_balance_slider_owner(config, NULL);
			sync_slider_offset_owner(config, NULL);
			continue;
		}
		case PROC_EVENT_COMM:
			/* A process renamed itself (e.g. via prctl(PR_SET_NAME)).
             * comm now matches a config entry that didn't match at
             * exec time, so re-evaluate. */
			pid = msg.ev.event_data.comm.process_pid;
			break;
		default:
			continue;
		}

		if (process_cpuset_apply_pid(g_pc_ctx, pid, 0) == 1) {
			struct lpmd_config_t *config = get_lpmd_config();
			if (config) {
				apply_class_min_perf_override_for_pid(config, pid);
				apply_class_max_perf_override_for_pid(config, pid);
				apply_class_balance_slider_override_for_pid(config, pid);
				apply_class_slider_offset_override_for_pid(config, pid);
			}
		}
	}
}

int lpmd_process_cpuset_proc_connector_fd(void)
{
	return g_pc_connector_fd;
}

void lpmd_process_cpuset_proc_connector_uninit(void)
{
	if (g_pc_connector_fd < 0)
		return;
	(void)proc_connector_send_op(g_pc_connector_fd, PROC_CN_MCAST_IGNORE);
	close(g_pc_connector_fd);
	g_pc_connector_fd = -1;
}

/*
 * Returns 1 if /proc/<pid> looks like a kernel thread (empty cmdline),
 * 0 if it looks like a user process, -1 if it could not be determined.
 *
 * Kernel threads always have an empty /proc/<pid>/cmdline. User
 * processes have a non-empty cmdline (argv joined by NULs). Reading
 * cmdline never blocks and works without CAP_SYS_PTRACE, unlike
 * dereferencing /proc/<pid>/exe.
 */
static int pid_is_kernel_thread(long pid)
{
	char path[64];
	FILE *f;
	int c;

	snprintf(path, sizeof(path), "/proc/%ld/cmdline", pid);
	f = fopen(path, "r");
	if (!f)
		return -1;
	c = fgetc(f);
	fclose(f);
	if (c == EOF)
		return 1; /* empty -> kernel thread */
	return 0;
}

/*
 * Return 1 if /proc/<pid>/cgroup mentions a transient cpuset scope
 * named "proc_cpuset_*.scope" (i.e. a scope previously created by any
 * instance of this daemon, including an earlier run whose in-memory
 * attached set is gone). Returns 0 otherwise, or -1 on error.
 */
static int pid_in_proc_cpuset_scope(long pid)
{
	char path[64];
	char line[512];
	FILE *f;
	int found = 0;

	snprintf(path, sizeof(path), "/proc/%ld/cgroup", pid);
	f = fopen(path, "r");
	if (!f)
		return -1;
	while (fgets(line, sizeof(line), f)) {
		if (strstr(line, "proc_cpuset_") && strstr(line, ".scope")) {
			found = 1;
			break;
		}
	}
	fclose(f);
	return found;
}

/*
 * Walk /proc and log every PID that does NOT currently have a transient
 * cpuset scope started by this context (i.e. processes not matched by any
 * <Process> entry in process_cpuset.xml). If process_cpuset is disabled
 * or not yet initialized, every running PID is reported as unbound.
 *
 * @user_only: if non-zero, skip kernel threads (empty /proc/<pid>/cmdline)
 *             and report only userspace PIDs.
 */
void lpmd_process_cpuset_print_unbound(int user_only)
{
	DIR *d;
	struct dirent *de;
	int total = 0, unbound = 0, kthreads_skipped = 0;

	d = opendir("/proc");
	if (!d) {
		lpmd_log_warn("process_cpuset: opendir(/proc) failed: %s\n",
			      strerror(errno));
		return;
	}

	if (!g_pc_ctx)
		lpmd_log_msg(
			"process_cpuset: not active; listing all %sPIDs as unbound\n",
			user_only ? "user " : "");
	else
		lpmd_log_msg(
			"process_cpuset: %sPIDs with no transient cpuset scope\n",
			user_only ? "user " : "");

	while ((de = readdir(d)) != NULL) {
		char *end;
		long pid;
		char path[64];
		char comm[64];
		const char *cls = NULL;
		char cpus[128] = { 0 };
		FILE *f;
		size_t n;
		int is_k;
		int is_unclassified = 0;

		pid = strtol(de->d_name, &end, 10);
		if (*end != '\0' || pid <= 0)
			continue;

		total++;
		is_k = pid_is_kernel_thread(pid);
		if (user_only && is_k != 0) {
			if (is_k == 1)
				kthreads_skipped++;
			continue;
		}

		snprintf(path, sizeof(path), "/proc/%ld/comm", pid);
		f = fopen(path, "r");
		if (!f)
			continue;
		if (!fgets(comm, sizeof(comm), f)) {
			fclose(f);
			continue;
		}
		fclose(f);
		n = strlen(comm);
		if (n && comm[n - 1] == '\n')
			comm[n - 1] = '\0';

		if (g_pc_ctx &&
		    process_cpuset_classify_name(g_pc_ctx, comm, &cls, cpus,
						 sizeof(cpus)) >= 0 &&
		    cls && !strcasecmp(cls, "Unclassified"))
			is_unclassified = 1;

		if (!is_unclassified) {
			if (g_pc_ctx &&
			    process_cpuset_is_attached(g_pc_ctx, (pid_t)pid))
				continue;
			/* Also skip PIDs already inside a proc_cpuset_*.scope created by
         * a prior daemon run; those are bound even though this process's
         * attached set doesn't track them. */
			if (pid_in_proc_cpuset_scope(pid) == 1)
				continue;
		}

		lpmd_log_msg("  pid=%ld %s comm=%s%s\n", pid,
			     is_k == 1 ? "[k]" : "[u]", comm,
			     is_unclassified ? " class=Unclassified" : "");

		/* List all threads in this process */
		if (is_k != 1) {  /* Only list threads for user processes */
			char task_path[64];
			DIR *task_dir;
			struct dirent *entry;

			snprintf(task_path, sizeof(task_path), "/proc/%ld/task", pid);
			task_dir = opendir(task_path);
			if (task_dir) {
				while ((entry = readdir(task_dir)) != NULL) {
					pid_t tid;
					char tid_comm[64] = { 0 };
					char tid_path[64];
					FILE *tid_f;
					size_t tid_len;

					if (entry->d_type != DT_DIR)
						continue;
					if (!strcmp(entry->d_name, ".") ||
					    !strcmp(entry->d_name, ".."))
						continue;
					tid = (pid_t)atoi(entry->d_name);
					if (tid <= 0 || tid == (pid_t)pid)
						continue;

					/* Read thread name from /proc/<pid>/task/<tid>/comm */
					snprintf(tid_path, sizeof(tid_path),
						 "/proc/%ld/task/%d/comm", pid, (int)tid);
					tid_f = fopen(tid_path, "r");
					if (tid_f) {
						if (fgets(tid_comm, sizeof(tid_comm), tid_f)) {
							tid_len = strlen(tid_comm);
							if (tid_len && tid_comm[tid_len - 1] == '\n')
								tid_comm[tid_len - 1] = '\0';
						}
						fclose(tid_f);
					} else {
						snprintf(tid_comm, sizeof(tid_comm), "<dead>");
					}

					lpmd_log_msg("\t\tTID=%d comm=%s%s\n", (int)tid,
						     tid_comm,
						     is_unclassified ? " class=Unclassified" : "");
				}
				closedir(task_dir);
			}
		}

		unbound++;
	}
	closedir(d);

	if (user_only)
		lpmd_log_msg(
			"process_cpuset: %d/%d user PIDs unbound (%d kernel threads skipped)\n",
			unbound, total, kthreads_skipped);
	else
		lpmd_log_msg("process_cpuset: %d/%d PIDs unbound\n", unbound,
			     total);
}

/*
 * Log every PID currently attached to a transient cpuset scope started
 * by this context (i.e. matched by a <Process> entry in
 * process_cpuset.xml). No-op (with a notice) if process_cpuset is not
 * active.
 */
void lpmd_process_cpuset_print_bound(void)
{
	size_t n, i;
	size_t listed = 0;
	char p_cpus[256] = { 0 };
	char e_cpus[256] = { 0 };
	char l_cpus[256] = { 0 };

	if (!g_pc_ctx) {
		lpmd_log_msg("process_cpuset: not active; no bound PIDs\n");
		return;
	}

	process_cpuset_groups_get(g_pc_ctx, p_cpus, sizeof(p_cpus), e_cpus,
				  sizeof(e_cpus), l_cpus, sizeof(l_cpus));

	n = process_cpuset_attached_count(g_pc_ctx);
	lpmd_log_msg(
		"process_cpuset: %zu PIDs bound to transient cpuset scopes (excluding Unclassified)\n",
		n);
	lpmd_log_msg("  groups: Pcores=[%s] Ecores=[%s] LPEcores=[%s]\n",
		     p_cpus[0] ? p_cpus : "-", e_cpus[0] ? e_cpus : "-",
		     l_cpus[0] ? l_cpus : "-");

	for (i = 0; i < n; i++) {
		pid_t pid = 0;
		char unit[128] = { 0 };
		char path[64];
		char comm[64] = { 0 };
		const char *cls = "?";
		int use_p = 0, use_e = 0, use_l = 0;
		char groups_buf[64];
		char cpus_buf[256];
		size_t off;
		FILE *f;
		size_t len;
		DIR *task_dir;
		struct dirent *entry;

		if (process_cpuset_attached_get_ex(g_pc_ctx, i, &pid, unit,
						   sizeof(unit), &cls, &use_p,
						   &use_e, &use_l) < 0)
			continue;
		if (cls && !strcasecmp(cls, "Unclassified"))
			continue;

		snprintf(path, sizeof(path), "/proc/%d/comm", (int)pid);
		f = fopen(path, "r");
		if (f) {
			if (fgets(comm, sizeof(comm), f)) {
				len = strlen(comm);
				if (len && comm[len - 1] == '\n')
					comm[len - 1] = '\0';
			}
			fclose(f);
		} else {
			snprintf(comm, sizeof(comm), "<dead>");
		}

		groups_buf[0] = '\0';
		off = 0;
		if (use_p)
			off += snprintf(groups_buf + off,
					sizeof(groups_buf) - off, "Pcores");
		if (use_e)
			off += snprintf(groups_buf + off,
					sizeof(groups_buf) - off, "%sEcores",
					off ? "+" : "");
		if (use_l)
			off += snprintf(groups_buf + off,
					sizeof(groups_buf) - off, "%sLPEcores",
					off ? "+" : "");
		if (!off)
			snprintf(groups_buf, sizeof(groups_buf), "none");

		cpus_buf[0] = '\0';
		off = 0;
		if (use_p && p_cpus[0])
			off += snprintf(cpus_buf + off, sizeof(cpus_buf) - off,
					"%s", p_cpus);
		if (use_e && e_cpus[0])
			off += snprintf(cpus_buf + off, sizeof(cpus_buf) - off,
					"%s%s", off ? "," : "", e_cpus);
		if (use_l && l_cpus[0])
			off += snprintf(cpus_buf + off, sizeof(cpus_buf) - off,
					"%s%s", off ? "," : "", l_cpus);
		if (!off)
			snprintf(cpus_buf, sizeof(cpus_buf), "-");

		/* Print process line */
		lpmd_log_msg(
			"  PID=%d comm=%s class=%s groups=%s cpus=[%s] unit=%s\n",
			(int)pid, comm, cls, groups_buf, cpus_buf,
			unit[0] ? unit : "<affinity>");

		/* List all threads in this process */
		snprintf(path, sizeof(path), "/proc/%d/task", (int)pid);
		task_dir = opendir(path);
		if (task_dir) {
			while ((entry = readdir(task_dir)) != NULL) {
				pid_t tid;
				char tid_comm[64] = { 0 };
				char tid_path[64];
				char tid_groups_buf[64];
				FILE *tid_f;
				size_t tid_len, tid_off;

				if (entry->d_type != DT_DIR)
					continue;
				if (!strcmp(entry->d_name, ".") ||
				    !strcmp(entry->d_name, ".."))
					continue;
				tid = (pid_t)atoi(entry->d_name);
				if (tid <= 0 || tid == pid)
					continue;

				/* Read thread name from /proc/<pid>/task/<tid>/comm */
				snprintf(tid_path, sizeof(tid_path),
					 "/proc/%d/task/%d/comm", (int)pid, (int)tid);
				tid_f = fopen(tid_path, "r");
				if (tid_f) {
					if (fgets(tid_comm, sizeof(tid_comm), tid_f)) {
						tid_len = strlen(tid_comm);
						if (tid_len && tid_comm[tid_len - 1] == '\n')
							tid_comm[tid_len - 1] = '\0';
					}
					fclose(tid_f);
				} else {
					snprintf(tid_comm, sizeof(tid_comm), "<dead>");
				}

				/* Threads share the same core groups as their parent process */
				tid_groups_buf[0] = '\0';
				tid_off = 0;
				if (use_p)
					tid_off += snprintf(tid_groups_buf + tid_off,
							    sizeof(tid_groups_buf) - tid_off, "Pcores");
				if (use_e)
					tid_off += snprintf(tid_groups_buf + tid_off,
							    sizeof(tid_groups_buf) - tid_off, "%sEcores",
							    tid_off ? "+" : "");
				if (use_l)
					tid_off += snprintf(tid_groups_buf + tid_off,
							    sizeof(tid_groups_buf) - tid_off, "%sLPEcores",
							    tid_off ? "+" : "");
				if (!tid_off)
					snprintf(tid_groups_buf, sizeof(tid_groups_buf), "none");

				lpmd_log_msg("\t\tTID=%d comm=%s class=%s groups=%s\n", (int)tid,
					     tid_comm, cls, tid_groups_buf);
			}
			closedir(task_dir);
		}

		listed++;
	}

	lpmd_log_msg("process_cpuset: listed %zu bound PIDs after Unclassified filter\n",
		     listed);
}

/*
 * Look up @name in the merged process_cpuset config (system + user XML)
 * and fill @result with a human-readable classification summary:
 *
 *   "process:<NAME>, classification:<CLS>, cpuaffinity:<CPUS>"
 *
 * The name is silently truncated to 15 chars to mirror the
 * /proc/<pid>/comm kernel limit; the search is case-insensitive.
 *
 * @result     : output buffer.
 * @result_cap : capacity of @result including the NUL terminator.
 *
 * Returns 0 on success, -1 if process_cpuset is not active or @name
 * was not found in the config (in that case @result is set to an
 * informative "not found" string).
 */
int lpmd_process_cpuset_classify(const char *name, char *result,
				 size_t result_cap)
{
	char trunc[16]; /* /proc/comm limit: 15 chars + NUL */
	const char *cls = NULL;
	struct lpmd_config_t *config = get_lpmd_config();
	char cpus[512] = { 0 };
	char conf_sys[MAX_FILE_NAME_PATH];
	char conf_user[MAX_FILE_NAME_PATH];
	char p_cpus[256] = { 0 }, e_cpus[256] = { 0 }, l_cpus[256] = { 0 };
	process_cpuset_t *lookup_ctx;
	int n;
	int rc;

	if (!result || !result_cap)
		return -1;
	result[0] = '\0';

	if (!name || !*name) {
		lpmd_log_warn("process_cpuset: classify: missing name\n");
		snprintf(result, result_cap,
			 "process:?, classification:not found, cpuaffinity:-");
		return -1;
	}

	/* Truncate to /proc/<pid>/comm's 15-char kernel limit. */
	snprintf(trunc, sizeof(trunc), "%s", name);

	lookup_ctx = process_cpuset_new();
	if (!lookup_ctx) {
		snprintf(result, result_cap,
			 "process:%s, classification:not found, cpuaffinity:-",
			 trunc);
		return -1;
	}

	/*
	 * Always classify from XML config, not from currently attached/running
	 * processes. Use the configured policy files under TDCONFDIR.
	 */
	snprintf(conf_sys, sizeof(conf_sys), "%s/%s", TDCONFDIR,
		 PROCESS_CPUSET_CONFIG_FILE);
	snprintf(conf_user, sizeof(conf_user), "%s/%s", TDCONFDIR,
		 PROCESS_CPUSET_USER_CONFIG_FILE);

	n = process_cpuset_load_config(lookup_ctx, conf_sys);
	if (n < 0) {
		process_cpuset_free(lookup_ctx);
		snprintf(result, result_cap,
			 "process:%s, classification:not found, cpuaffinity:-",
			 trunc);
		return -1;
	}

	(void)process_cpuset_load_config_overlay(lookup_ctx, conf_user);

	if (config && (config->pc_class_default_realtime[0] ||
	    config->pc_class_default_user_interactive[0] ||
	    config->pc_class_default_user_initiated[0] ||
	    config->pc_class_default_unclassified[0] ||
	    config->pc_class_default_utility[0] ||
	    config->pc_class_default_background[0] ||
	    config->pc_class_default_gp_cpu[0] ||
	    config->pc_class_default_gp_gpu[0] ||
	    config->pc_class_default_gp_hybrid[0])) {
		(void)process_cpuset_override_class_defaults(
			lookup_ctx, config->pc_class_default_realtime,
			config->pc_class_default_user_interactive,
			config->pc_class_default_user_initiated,
			config->pc_class_default_unclassified,
			config->pc_class_default_utility,
			config->pc_class_default_background,
			config->pc_class_default_gp_cpu,
			config->pc_class_default_gp_gpu,
			config->pc_class_default_gp_hybrid);
	}

	/* Reuse active runtime P/E/LP-E groups if available. */
	if (g_pc_ctx &&
	    process_cpuset_groups_get(g_pc_ctx, p_cpus, sizeof(p_cpus), e_cpus,
				       sizeof(e_cpus), l_cpus,
				       sizeof(l_cpus)) == 0) {
		(void)process_cpuset_set_groups(lookup_ctx,
					p_cpus[0] ? p_cpus : NULL,
					e_cpus[0] ? e_cpus : NULL,
					l_cpus[0] ? l_cpus : NULL);
	}

	rc = process_cpuset_classify_name(lookup_ctx, trunc, &cls, cpus,
					  sizeof(cpus));
	process_cpuset_free(lookup_ctx);

	if (rc < 0) {
		lpmd_log_info("process_cpuset: classify: '%s' not found\n", trunc);
		snprintf(result, result_cap,
			 "process:%s, classification:not found, cpuaffinity:-",
			 trunc);
		return -1;
	}

	snprintf(result, result_cap,
		 "process:%s, classification:%s, cpuaffinity:%s", trunc,
		 cls ? cls : "unknown", cpus[0] ? cpus : "-");
	return 0;
}
