// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * process_cpuset.h: Library API for per-process CPU affinity
 *
 * Copyright (C) 2026 Intel Corporation. All rights reserved.
 *
 * Library API for per-process CPU affinity via systemd transient scopes
 * (cpuset / AllowedCPUs).
 *
 * Designed so it can be linked into intel_lpmd (or any other daemon) and
 * driven from existing event loops, instead of being shipped only as a
 * standalone CLI. The bundled CLI (process_cpuset_main.c) is a thin
 * wrapper around this API.
 *
 * Typical integration sketch:
 *
 *     process_cpuset_t *ctx = process_cpuset_new();
 *     process_cpuset_load_config(ctx, "/etc/intel_lpmd/process_cpuset.xml");
 *     // optional: override CPU groups from LPMD's current view
 *     process_cpuset_set_groups(ctx, "0-3", "4-11", "12-13");
 *     process_cpuset_apply_once(ctx, 0);
 *     ...
 *     process_cpuset_free(ctx);
 *
 * The library is single-threaded; serialize calls externally if needed.
 */

#ifndef PROCESS_CPUSET_H
#define PROCESS_CPUSET_H

#define _GNU_SOURCE
#include <sched.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct process_cpuset_ctx process_cpuset_t;

/* Create / destroy a context. Returns NULL on allocation failure. */
process_cpuset_t *process_cpuset_new(void);
void process_cpuset_free(process_cpuset_t *ctx);

/*
 * Load configuration from an XML file (same schema as process_cpuset.xml).
 * Replaces any previously loaded entries / groups / class defaults.
 * Returns the number of <Process> entries loaded, or -1 on failure.
 */
int process_cpuset_load_config(process_cpuset_t *ctx, const char *path);

/*
 * Additive load: parse @path and merge its <Process> entries on top of
 * already-loaded ones. Entries whose <Name> matches an existing entry
 * are replaced; new names are appended. <CpuGroups>, <ClassDefaults>,
 * and <DefaultProcess>, if present, also override.
 *
 * Intended for layering a small user-editable XML on top of the system
 * one. Returns the number of entries added or replaced, 0 if the file
 * is missing/empty (treated as success), or -1 on hard parse error.
 */
int process_cpuset_load_config_overlay(process_cpuset_t *ctx, const char *path);

/*
 * Add (or replace) a single <Process> entry at runtime. The CPU mask
 * is taken from the current <ClassDefaults> for @classification.
 *
 * Returns 1 if the entry was newly added, 0 if it replaced an existing
 * entry with the same @name, -1 on error (NULL args, unknown
 * classification, table full).
 */
int process_cpuset_add_entry_ex(process_cpuset_t *ctx, const char *name,
				const char *classification,
				int allow_session);
int process_cpuset_add_entry(process_cpuset_t *ctx, const char *name,
			     const char *classification);

/*
 * Override the CPU group lists at runtime. Any argument may be NULL to
 * leave that group unchanged. Strings are cpuset-style ("0-3,5,8-11").
 * Useful for LPMD which already knows the active P/E/LP-E core sets.
 * Returns 0 on success.
 */
int process_cpuset_set_groups(process_cpuset_t *ctx, const char *p_cores,
			      const char *e_cores, const char *l_cores);

/*
 * Same as process_cpuset_set_groups(), but takes already-built cpu_set_t
 * masks (as produced by sched.h / CPU_SET()). This is the natural fit for
 * intel_lpmd, whose lpmd_cpumask layer already maintains the active P/E/
 * LP-E core sets as cpu_set_t.
 *
 * Any argument may be NULL to leave that group unchanged. setsize is the
 * size in bytes returned by CPU_ALLOC_SIZE(n) for the largest set passed
 * in; pass 0 to use sizeof(cpu_set_t).
 *
 * Returns 0 on success, -1 on error.
 */
int process_cpuset_set_groups_cpuset(process_cpuset_t *ctx,
				     const cpu_set_t *p_cores,
				     const cpu_set_t *e_cores,
				     const cpu_set_t *l_cores, size_t setsize);

/*
 * Override one or more <ClassDefaults> values at runtime, after
 * process_cpuset_load_config() has parsed the file. Each string is a
 * comma- or whitespace-separated list of "ActivePcores" /
 * "ActiveEcores" / "ActiveLcores" tokens (same syntax as the XML).
 * Pass NULL or "" to leave that classification's default unchanged.
 *
 * Loaded <Process> entries that did NOT specify <ActiveCores> have
 * their resolved CPU group mask recomputed against the new defaults.
 * Entries with an explicit per-process <ActiveCores> are left alone.
 *
 * Intended for callers (e.g. intel_lpmd) that want to override the
 * shared process_cpuset.xml defaults from a CPU-model-specific config.
 *
 * Returns 0 on success, -1 on error.
 */
int process_cpuset_override_class_defaults(
	process_cpuset_t *ctx, const char *realtime,
	const char *user_interactive, const char *user_initiated,
	const char *unclassified,
	const char *utility, const char *background,
	const char *game_profile_cpu, const char *game_profile_gpu,
	const char *game_profile_hybrid,
	const char *custom_profile_0, const char *custom_profile_1,
	const char *custom_profile_2);

/*
 * Override one or more per-class uclamp defaults at runtime.
 * Each value accepts:
 *   -2: keep current value unchanged
 *   -1: disable clamp for that bound
 * 0..1024: explicit sched_util_{min,max} value
 *
 * Returns 0 on success, -1 on error.
 */
int process_cpuset_override_class_uclamp_defaults(
	process_cpuset_t *ctx,
	int realtime_min, int realtime_max,
	int user_interactive_min, int user_interactive_max,
	int user_initiated_min, int user_initiated_max,
	int unclassified_min, int unclassified_max,
	int utility_min, int utility_max,
	int background_min, int background_max,
	int game_profile_cpu_min, int game_profile_cpu_max,
	int game_profile_gpu_min, int game_profile_gpu_max,
	int game_profile_hybrid_min, int game_profile_hybrid_max,
	int custom_profile_0_min, int custom_profile_0_max,
	int custom_profile_1_min, int custom_profile_1_max,
	int custom_profile_2_min, int custom_profile_2_max);

/*
 * Scan /proc once and attach any newly-seen matching PIDs to a transient
 * cpuset scope. PIDs already attached in a previous call are skipped.
 * If dry_run is non-zero, prints what would happen but does not call
 * systemd. Returns the number of new PIDs attached this cycle, or -1.
 */
int process_cpuset_apply_once(process_cpuset_t *ctx, int dry_run);

/*
 * Apply configuration to a single PID: look up its /proc/<pid>/comm,
 * find a matching <Process> entry and attach it to a transient cpuset
 * scope. Intended for event-driven callers (e.g. the kernel proc
 * connector) that already know which PID just appeared.
 *
 * Returns 1 if the PID was newly attached, 0 if no <Process> entry
 * matches / it's already attached / the PID is gone, -1 on error.
 */
int process_cpuset_apply_pid(process_cpuset_t *ctx, pid_t pid, int dry_run);

/*
 * Stop every transient scope unit that this context started, by issuing
 * StopUnit on systemd. Useful on daemon shutdown so processes no longer
 * have an LPMD-imposed AllowedCPUs mask. After this call the attached-
 * PID set is empty. Returns the number of scopes stopped, or -1.
 *
 * WARNING: systemd's default scope KillMode signals all processes in
 * the scope on stop. If you need to release PIDs without killing them,
 * use process_cpuset_release_all() instead.
 */
int process_cpuset_stop_all(process_cpuset_t *ctx);

/*
 * Release every tracked PID without killing it: each PID is migrated
 * out of its proc_cpuset scope into the root cgroup (restoring default
 * affinity), then the now-empty scope unit is stopped. Returns the
 * number of PIDs successfully released, or -1.
 */
int process_cpuset_release_all(process_cpuset_t *ctx);

/* Number of PIDs currently tracked as attached. */
size_t process_cpuset_attached_count(const process_cpuset_t *ctx);

/*
 * Returns non-zero if @pid is currently tracked as attached to a
 * transient cpuset scope by this context, 0 otherwise.
 */
int process_cpuset_is_attached(const process_cpuset_t *ctx, pid_t pid);

/*
 * Read out the attached PID at index @i (0 <= i < attached_count).
 * On success returns 0 and fills *@pid_out (and *@unit_out, if non-NULL,
 * with up to @unit_cap bytes of the systemd scope unit name).
 * Returns -1 on out-of-range or NULL ctx.
 */
int process_cpuset_attached_get(const process_cpuset_t *ctx, size_t i,
				pid_t *pid_out, char *unit_out,
				size_t unit_cap);

/*
 * Extended attached-PID lookup. In addition to the data returned by
 * process_cpuset_attached_get(), also yields the classification name
 * ("background"/"foreground"/"realtime"/"invalid") and three flags
 * indicating which CPU groups (P/E/LP-E cores) form the AllowedCPUs
 * set for this PID. The string returned via *@class_out points into
 * static storage owned by the library; do not free it.
 */
int process_cpuset_attached_get_ex(const process_cpuset_t *ctx, size_t i,
				   pid_t *pid_out, char *unit_out,
				   size_t unit_cap, const char **class_out,
				   int *use_pcores, int *use_ecores,
				   int *use_lcores);

/*
 * Retrieve the active CPU group lists (cpuset-style strings as given
 * via process_cpuset_set_groups()) for this context. Any of the output
 * pointers may be NULL. Returns 0 on success.
 */
int process_cpuset_groups_get(const process_cpuset_t *ctx, char *p_out,
			      size_t p_cap, char *e_out, size_t e_cap,
			      char *l_out, size_t l_cap);

/* Number of <Process> entries currently loaded. */
int process_cpuset_entry_count(const process_cpuset_t *ctx);

/*
 * Promote @pid to the "user_interactive" CPU set (typically the
 * window/tab currently holding compositor focus). Internally the
 * context remembers the PID's previous mask; calling this again with
 * a different @pid reverts the previous one before promoting the new
 * one. Passing @pid == 0 just demotes the current focus, if any.
 *
 * Only PIDs already tracked in the attached set are acted on. PIDs
 * not yet attached are silently ignored - the proc-connector / rescan
 * will pick them up shortly under their default class, and the next
 * focus event will re-promote them.
 *
 * Returns 0 on success (including the "not attached / no-op" case),
 * -1 on sd-bus or parameter error.
 */
int process_cpuset_set_focus_pid(process_cpuset_t *ctx, pid_t pid);

/*
 * Tell the library whether a user-session focus relay (e.g.
 * intel_lpmd_focus_helper) is currently present and will be sending
 * focus-PID events via process_cpuset_set_focus_pid().
 *
 * While no helper is present (the default at startup), the resolved
 * cpuset for the USER_INITIATED class transparently mirrors the
 * USER_INTERACTIVE class -- there is no point starving user-session
 * apps when we have no signal about which one is focused. When the
 * helper announces itself with @present=1, USER_INITIATED starts
 * honoring its own <ClassDefaults>, freeing the USER_INTERACTIVE
 * cpuset for the actually focused PID.
 *
 * On every state change attached USER_INITIATED PIDs are re-pinned
 * to the new mask. Returns 0 on success, -1 on parameter error.
 */
int process_cpuset_set_focus_helper_present(process_cpuset_t *ctx, int present);

/*
 * Log (stderr) the resolved AllowedCPUs cpuset for every
 * classification, plus the active P/E/LP-E core lists. Useful right
 * after process_cpuset_set_groups_cpuset() /
 * process_cpuset_override_class_defaults() to verify what each tier
 * maps to.
 */
void process_cpuset_log_class_defaults(const process_cpuset_t *ctx);

/*
 * Look up a process name in the loaded config and return its
 * classification and resolved CPU affinity string.
 *
 * @name       : process comm (silently truncated to 15 chars to mirror
 *               the /proc/<pid>/comm kernel limit). Case-insensitive.
 * @class_out  : if non-NULL, set to a static classification name string
 *               (e.g. "background"). Do not free.
 * @cpus_out   : if non-NULL, filled with a cpuset-style list of the
 *               resolved AllowedCPUs (e.g. "0-3,8-11"). Falls back to a
 *               symbolic group expression (e.g. "Pcores+Ecores") when the
 *               active CPU groups have not been configured yet.
 * @cpus_cap   : capacity of @cpus_out including the NUL terminator.
 *
 * Returns:
 *   1  named <Process> entry matched
 *   0  no named entry matched; <DefaultProcess> was used
 *  -1  not found (no match and no <DefaultProcess>)
 */
int process_cpuset_classify_name(const process_cpuset_t *ctx,
				 const char *name,
				 const char **class_out,
				 char *cpus_out, size_t cpus_cap);

/*
 * Resolve a classification's effective default AllowedCPUs list under
 * the current CPU groups and class-default table.
 *
 * @classification accepts the same aliases as XML (for example
 * "GameProfileGPU" / "game_profile_gpu").
 *
 * Returns 0 on success, -1 on invalid input / unknown class / mask
 * build error.
 */
int process_cpuset_get_class_cpulist(const process_cpuset_t *ctx,
				     const char *classification,
				     char *cpus_out, size_t cpus_cap);

#ifdef __cplusplus
}
#endif

#endif /* PROCESS_CPUSET_H */
