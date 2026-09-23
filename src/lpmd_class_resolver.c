// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * lpmd_class_resolver.c: the one way to turn a <Classification> into CPUs
 *
 * Copyright (C) 2026 Intel Corporation. All rights reserved.
 *
 * Both policy files name classifications: process_cpuset.xml says which
 * class a process is, slice.xml says which class a systemd unit is. What a
 * class *means* in CPUs is a third thing, shared by both, and it lives in
 * exactly two places -- the built-in table in process_cpuset_new() and the
 * <ClassDefaults> overlay in the matching <States> stanza of
 * intel_lpmd_config_*.xml.
 *
 * This file exists so that "build a context that resolves classes" is one
 * function rather than a sequence each caller reproduces. It used to be
 * three sequences, and they disagreed: the slice path did
 * process_cpuset_new() plus the core masks and skipped the config overlay
 * entirely, so a class name resolved one way in slice.xml and another in
 * process_cpuset.xml. gdm.service and pipewire.service were written masks
 * the config had explicitly overridden, and LIST-SLICES reported those
 * masks as though they were the configured ones.
 *
 * Kept free of daemon dependencies (no D-Bus, no cgroups, no proc
 * connector) so the standalone tests under tests/ can link the real
 * function instead of a stub that could drift from it -- which is the
 * failure this file is here to prevent.
 */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "lpmd.h"
#include "process_cpuset.h"

#include <sched.h>
#include <stddef.h>

/*
 * Build a resolver whose class -> core-group mapping is the one the whole
 * daemon agrees on: the built-in table from process_cpuset_new(), then the
 * per-CPU-model <ClassDefaults> and uclamp overlays parsed from the
 * matching <States> stanza of intel_lpmd_config_*.xml, then the active
 * P/E/LP-E sets.
 *
 * Every consumer of a <Classification> has to build its resolver through
 * here. The slice path used to call process_cpuset_new() and hand it
 * nothing but the core masks, so one class name resolved two ways: gdm.service
 * and pipewire.service were given the built-in mask for their tier while the
 * config had explicitly overridden it, and nothing said so.
 *
 * @sys_xml and @user_xml are optional. NULL for both yields the class
 * mapping with no <Process> entries at all, which is everything a caller
 * that only turns a class name into a cpulist needs.
 *
 * The order here is not arbitrary. process_cpuset_load_config() clears the
 * entry array and the groups, the overrides re-resolve every entry already
 * loaded, and process_cpuset_set_groups_cpuset() must come last because it
 * is what turns named groups into real CPUs.
 *
 * Must be called AFTER lpmd_build_config_states() (so the active core sets
 * are valid) and BEFORE free_cpu_type_masks() (so core_type_masks[] are
 * still alive).
 *
 * Returns the new context, or NULL on failure. @n_entries, when non-NULL,
 * receives the number of <Process> entries loaded from @sys_xml.
 */
struct process_cpuset_ctx *lpmd_class_resolver_new(struct lpmd_config_t *config,
						   const char *sys_xml,
						   const char *user_xml,
						   int *n_entries)
{
	process_cpuset_t *ctx;
	size_t setsize;
	int n = 0;

	if (!config)
		return NULL;

	ctx = process_cpuset_new();
	if (!ctx) {
		lpmd_log_error("process_cpuset_new failed\n");
		return NULL;
	}

	if (sys_xml) {
		n = process_cpuset_load_config(ctx, sys_xml);
		if (n < 0) {
			lpmd_log_error("process_cpuset_load_config(%s) failed\n",
				       sys_xml);
			process_cpuset_free(ctx);
			return NULL;
		}
		lpmd_log_info("process_cpuset: loaded %d entries from %s\n", n,
			      sys_xml);
	}

	/* Layer the user-editable overlay (process_cpuset_user.xml) on top.
     * Missing file is fine and is silently ignored. */
	if (user_xml) {
		int un = process_cpuset_load_config_overlay(ctx, user_xml);

		if (un > 0)
			lpmd_log_info(
				"process_cpuset: merged %d user entries from %s\n",
				un, user_xml);
		else if (un < 0)
			lpmd_log_warn(
				"process_cpuset: parse error in %s (ignored)\n",
				user_xml);
	}

	/*
     * Apply the CPU-model-specific <ClassDefaults> overlay. This is the
     * only place a class default can be changed; a class the config leaves
     * empty keeps the built-in value.
     */
	if (config->pc_class_default_realtime[0] ||
	    config->pc_class_default_user_interactive[0] ||
	    config->pc_class_default_user_initiated[0] ||
	    config->pc_class_default_unclassified[0] ||
	    config->pc_class_default_utility[0] ||
	    config->pc_class_default_background[0] ||
	    config->pc_class_default_gp_cpu[0] ||
	    config->pc_class_default_gp_gpu[0] ||
	    config->pc_class_default_gp_hybrid[0] ||
	    config->pc_class_default_custom_profile_0[0] ||
	    config->pc_class_default_custom_profile_1[0] ||
	    config->pc_class_default_custom_profile_2[0]) {
		if (process_cpuset_override_class_defaults(
			    ctx, config->pc_class_default_realtime,
			    config->pc_class_default_user_interactive,
			    config->pc_class_default_user_initiated,
			    config->pc_class_default_unclassified,
			    config->pc_class_default_utility,
			    config->pc_class_default_background,
			    config->pc_class_default_gp_cpu,
			    config->pc_class_default_gp_gpu,
			    config->pc_class_default_gp_hybrid,
			    config->pc_class_default_custom_profile_0,
			    config->pc_class_default_custom_profile_1,
			    config->pc_class_default_custom_profile_2) < 0) {
			lpmd_log_warn(
				"process_cpuset: ClassDefaults override failed\n");
		} else {
			lpmd_log_info(
				"process_cpuset: applied per-CPU ClassDefaults override\n");
		}
	}

	if (config->pc_class_uclamp_min_realtime != LPMD_UCLAMP_INHERIT ||
	    config->pc_class_uclamp_max_realtime != LPMD_UCLAMP_INHERIT ||
	    config->pc_class_uclamp_min_user_interactive !=
		    LPMD_UCLAMP_INHERIT ||
	    config->pc_class_uclamp_max_user_interactive !=
		    LPMD_UCLAMP_INHERIT ||
	    config->pc_class_uclamp_min_user_initiated != LPMD_UCLAMP_INHERIT ||
	    config->pc_class_uclamp_max_user_initiated != LPMD_UCLAMP_INHERIT ||
	    config->pc_class_uclamp_min_unclassified != LPMD_UCLAMP_INHERIT ||
	    config->pc_class_uclamp_max_unclassified != LPMD_UCLAMP_INHERIT ||
	    config->pc_class_uclamp_min_utility != LPMD_UCLAMP_INHERIT ||
	    config->pc_class_uclamp_max_utility != LPMD_UCLAMP_INHERIT ||
	    config->pc_class_uclamp_min_background != LPMD_UCLAMP_INHERIT ||
	    config->pc_class_uclamp_max_background != LPMD_UCLAMP_INHERIT ||
	    config->pc_class_uclamp_min_gp_cpu != LPMD_UCLAMP_INHERIT ||
	    config->pc_class_uclamp_max_gp_cpu != LPMD_UCLAMP_INHERIT ||
	    config->pc_class_uclamp_min_gp_gpu != LPMD_UCLAMP_INHERIT ||
	    config->pc_class_uclamp_max_gp_gpu != LPMD_UCLAMP_INHERIT ||
	    config->pc_class_uclamp_min_gp_hybrid != LPMD_UCLAMP_INHERIT ||
	    config->pc_class_uclamp_max_gp_hybrid != LPMD_UCLAMP_INHERIT ||
	    config->pc_class_uclamp_min_custom_profile_0 != LPMD_UCLAMP_INHERIT ||
	    config->pc_class_uclamp_max_custom_profile_0 != LPMD_UCLAMP_INHERIT ||
	    config->pc_class_uclamp_min_custom_profile_1 != LPMD_UCLAMP_INHERIT ||
	    config->pc_class_uclamp_max_custom_profile_1 != LPMD_UCLAMP_INHERIT ||
	    config->pc_class_uclamp_min_custom_profile_2 != LPMD_UCLAMP_INHERIT ||
	    config->pc_class_uclamp_max_custom_profile_2 != LPMD_UCLAMP_INHERIT) {
		if (process_cpuset_override_class_uclamp_defaults(
			    ctx,
			    config->pc_class_uclamp_min_realtime,
			    config->pc_class_uclamp_max_realtime,
			    config->pc_class_uclamp_min_user_interactive,
			    config->pc_class_uclamp_max_user_interactive,
			    config->pc_class_uclamp_min_user_initiated,
			    config->pc_class_uclamp_max_user_initiated,
			    config->pc_class_uclamp_min_unclassified,
			    config->pc_class_uclamp_max_unclassified,
			    config->pc_class_uclamp_min_utility,
			    config->pc_class_uclamp_max_utility,
			    config->pc_class_uclamp_min_background,
			    config->pc_class_uclamp_max_background,
			    config->pc_class_uclamp_min_gp_cpu,
			    config->pc_class_uclamp_max_gp_cpu,
			    config->pc_class_uclamp_min_gp_gpu,
			    config->pc_class_uclamp_max_gp_gpu,
			    config->pc_class_uclamp_min_gp_hybrid,
			    config->pc_class_uclamp_max_gp_hybrid,
			    config->pc_class_uclamp_min_custom_profile_0,
			    config->pc_class_uclamp_max_custom_profile_0,
			    config->pc_class_uclamp_min_custom_profile_1,
			    config->pc_class_uclamp_max_custom_profile_1,
			    config->pc_class_uclamp_min_custom_profile_2,
			    config->pc_class_uclamp_max_custom_profile_2) < 0) {
			lpmd_log_warn(
				"process_cpuset: ClassDefaults uclamp override failed\n");
		} else {
			lpmd_log_info(
				"process_cpuset: applied per-CPU ClassDefaults uclamp override\n");
		}
	}

	/* core_type_masks[] uses the same little-endian bit layout as
     * cpu_set_t, so it's safe to cast and pass straight through. */
	setsize = (size_t)(get_max_cpus() / 8);
	if (process_cpuset_set_groups_cpuset(
		    ctx,
		    (const cpu_set_t *)config->core_type_masks[P_CORE],
		    (const cpu_set_t *)config->core_type_masks[E_CORE],
		    (const cpu_set_t *)config->core_type_masks[L_CORE],
		    setsize) < 0) {
		lpmd_log_error("process_cpuset_set_groups_cpuset failed\n");
		process_cpuset_free(ctx);
		return NULL;
	}

	if (n_entries)
		*n_entries = n;
	return ctx;
}