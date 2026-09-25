// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * process_cpuset.c: Library implementation for per-process CPU affinity
 *
 * Copyright (C) 2026 Intel Corporation. All rights reserved.
 *
 * Library implementation: per-process CPU affinity via sched_setaffinity(2).
 *
 * Public API: process_cpuset.h
 * CLI driver: process_cpuset_main.c
 *
 * Mirrors the XML-config pattern used by intel_lpmd (see
 * src/lpmd_config.c).
 *
 * sched_setaffinity(2) is the only mechanism here. This file opens no
 * cgroup file for writing and has no code path that could place a task
 * anywhere other than where it already is. Everything cgroup-related
 * lives in lpmd_cgroup.c (whole-slice cpuset) and lpmd_slice_cpuset.c
 * (per-unit AllowedCPUs=), which act on cgroups that already exist and
 * never move a task into one.
 *
 * It has no sd-bus dependency and contacts no daemon. The one systemd
 * interface it uses is sd_pid_get_unit()/sd_pid_get_user_unit(), for
 * <Unit> matching, and those are sd-login calls: inside libsystemd they
 * are a read-only parse of /proc/<pid>/cgroup, not a bus round trip.
 * Checked rather than assumed -- with /run/systemd and /run/dbus masked
 * by a tmpfs and the bus environment unset, both still answer
 * correctly.
 *
 * What apply_once does:
 *   1. Walks /proc once, deciding each PID's policy as it goes: a <Process>
 *      entry matched by systemd unit name, cgroup path or comm name (see
 *      find_entry_for_pid), or -- with <UseSliceClassification> on -- the
 *      classification the PID's slice implies, which outranks a comm or
 *      cgroup match but not a unit match or a hand-curated entry (see
 *      resolve_policy_for_pid).
 *   2. For each matched PID not yet handled, applies sched_setaffinity(2)
 *      to the task where it already is, using the CPU list for its
 *      classification intersected with the CPUs its current cgroup
 *      allows.
 *
 * What it deliberately does NOT do:
 *   - It never moves a task between cgroups.
 *   - It never creates a cgroup or a transient scope unit.
 *   - It never sets a cgroup cpuset (AllowedCPUs=) on anything.
 *   - It never touches a task that already has an affinity mask of its
 *     own, and never restores one it did not set. A mask a task merely
 *     inherited from a parent lpmd narrowed is not the task's own; see
 *     mask_inherited_from_lpmd().
 *   - It yields to slice.xml: a task whose cgroup is already governed by
 *     an enforceable <Unit> entry is left to that policy.
 *
 * Because fork(2) copies the affinity mask and execve(2) preserves it,
 * narrowing a task implicitly narrows every descendant it later spawns.
 * That is handled on both ends: the bind path recognises an inherited mask
 * rather than mistaking it for a deliberate self-placement, and release
 * puts back descendants that are still wearing a mask lpmd wrote even
 * though they were never matched by a <Process> entry themselves.
 *
 * Earlier revisions attached each PID to a transient
 * proc_cpuset_<comm>_<pid>.scope, which migrated the task out of its own
 * service's cgroup, and then steered it by rewriting that scope's
 * AllowedCPUs=. Both halves are gone; see the "no process migration"
 * note further down for why.
 */

#define _GNU_SOURCE
#include "process_cpuset.h"
#include "lpmd.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <fnmatch.h>
#include <sched.h>
#include <linux/sched.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/syscall.h>
#include <sys/stat.h>
#include <sys/types.h>

#include <libxml/parser.h>
#include <libxml/tree.h>
#include <systemd/sd-login.h>

#define MAX_NAME 64
#define MAX_CPULIST 256
#define MAX_PROCS 512
/*
 * A cgroup v2 path can nest arbitrarily deep, but the ones worth matching on
 * bottom out around user.slice/user-N.slice/user@N.service/app.slice/x.scope.
 */
#define MAX_CGROUP_PATH 256
#define MAX_CGROUPS_PER_ENTRY 8
/*
 * A systemd unit name is bounded by NAME_MAX, but the long ones in practice are
 * escaped D-Bus-activated services like
 * "dbus-:1.20-org.a11y.atspi.Registry@0.service" at 44 characters. 128 leaves
 * room without carrying 255 bytes per pattern through a 512-entry table.
 *
 * Fewer alternatives per entry than <Cgroup> gets, because a unit pattern needs
 * fewer: one name is one unit, where a path glob often needs a variant per
 * nesting depth.
 */
#define MAX_UNIT_NAME 128
#define MAX_UNITS_PER_ENTRY 4
#define MAX_CPUS 1024 /* cap for the POC */
#define CPUMASK_BYTES (MAX_CPUS / 8)

#define UCLAMP_UNSET (-1)
#define UCLAMP_CLAMP_MIN 0
#define UCLAMP_CLAMP_MAX 1024

#ifndef SCHED_FLAG_KEEP_POLICY
#define SCHED_FLAG_KEEP_POLICY 0x08
#endif

#ifndef SCHED_FLAG_KEEP_PARAMS
#define SCHED_FLAG_KEEP_PARAMS 0x10
#endif

#ifndef SCHED_FLAG_UTIL_CLAMP_MIN
#define SCHED_FLAG_UTIL_CLAMP_MIN 0x20
#endif

#ifndef SCHED_FLAG_UTIL_CLAMP_MAX
#define SCHED_FLAG_UTIL_CLAMP_MAX 0x40
#endif

#if !defined(SYS_sched_setattr) && defined(__NR_sched_setattr)
#define SYS_sched_setattr __NR_sched_setattr
#endif

#ifndef SCHED_ATTR_SIZE_VER0
struct sched_attr {
	uint32_t size;
	uint32_t sched_policy;
	uint64_t sched_flags;
	int32_t sched_nice;
	uint32_t sched_priority;
	uint64_t sched_runtime;
	uint64_t sched_deadline;
	uint64_t sched_period;
	uint32_t sched_util_min;
	uint32_t sched_util_max;
};
#endif

enum classification {
	CLASS_BACKGROUND = 0,
	/* Strictly higher priority than background; lower than user_*. */
	CLASS_UTILITY,
	/* Catch-all tier for processes that do not match named entries. */
	CLASS_UNCLASSIFIED,
	/* User-launched workloads that should run quickly but are not
     * actively driving the UI (e.g. a build kicked off from a
     * terminal). Higher priority than utility, lower than
     * user_interactive. */
	CLASS_USER_INITIATED,
	/* User-facing UI / display / shell. Renamed from "foreground";
     * the legacy name is still accepted by the XML parser as an
     * alias. */
	CLASS_USER_INTERACTIVE,
	CLASS_REALTIME,
	/*
     * Reserved game-profile classifications. Wired through the parser
     * and ClassDefaults so XML configs can already reference them; the
     * runtime policy that actually distinguishes their behaviour is
     * added later. For now they behave like any other classification:
     * either the per-process <ActiveCores> or the matching
     * <ClassDefaults> entry decides the CPU group mask.
     */
	CLASS_GAME_PROFILE_CPU,
	CLASS_GAME_PROFILE_GPU,
	CLASS_GAME_PROFILE_HYBRID,
	CLASS_CUSTOM_PROFILE_0,
	CLASS_CUSTOM_PROFILE_1,
	CLASS_CUSTOM_PROFILE_2,
	CLASS_INVALID,
};

static int reapply_attached_class(process_cpuset_t *ctx,
				  enum classification target_cls);

/* Bitmask of CPU groups a process is allowed to use. */
#define GROUP_PCORES (1u << 0)
#define GROUP_ECORES (1u << 1)
#define GROUP_LCORES (1u << 2)

/* Resolved CPU specification: a mix of named groups (P/E/LP-E cores)
 * and an optional literal cpuset-style list of CPU numbers/ranges. The
 * effective AllowedCPUs mask is the OR of all selected groups plus the
 * literal list. */
struct core_spec {
	unsigned int groups; /* GROUP_* mask */
	char cpulist[MAX_CPULIST]; /* literal "0-3,5,8" (may be empty) */
};

static int core_spec_is_set(const struct core_spec *s)
{
	return s && (s->groups != 0 || s->cpulist[0] != '\0');
}

struct proc_entry {
	/*
	 * <Name> matches /proc/<pid>/comm, which the kernel truncates to 15
	 * characters and which the process itself can rewrite with
	 * prctl(PR_SET_NAME). That makes it both collision-prone (the three
	 * xdg-desktop-portal* services all report "xdg-desktop-por") and not
	 * something to lean on for identity. Optional when at least one
	 * <Cgroup> is present.
	 */
	char name[MAX_NAME];
	/*
	 * <Cgroup> patterns matched against the process' cgroup v2 path with
	 * the leading '/' stripped, e.g.
	 * "system.slice/xdg-desktop-portal-gnome.service". Globs as <Name>
	 * does, except '*' also crosses '/', so "system.slice/*" means
	 * everything under that slice however deeply nested.
	 *
	 * An entry matches a PID if its <Name> matches OR any of its
	 * <Cgroup> patterns match, so adding one to an existing entry only
	 * widens it. A cgroup match outranks a comm match when a PID is
	 * eligible for more than one entry: it names a systemd unit, not a
	 * 15-character prefix.
	 */
	char cgroups[MAX_CGROUPS_PER_ENTRY][MAX_CGROUP_PATH];
	int n_cgroups;
	/*
	 * <Unit> patterns matched against the systemd unit name owning the task,
	 * as systemd itself reports it: sd_pid_get_unit() for the system
	 * manager's answer and sd_pid_get_user_unit() for the user manager's. A
	 * pattern is tried against both, so "gnome-remote-desktop.service"
	 * matches whether that unit is a system service or a user one.
	 *
	 * This is the strongest of the three matchers and outranks them both,
	 * for two reasons:
	 *
	 *  - It is the task's real identity. <Name> is /proc/<pid>/comm, which
	 *    the process can rewrite at will; a unit name comes from the cgroup
	 *    path, which only the managing systemd writes. Nothing a process
	 *    does to itself changes the answer.
	 *  - It resolves delegated subtrees. A task in
	 *    "system.slice/systemd-udevd.service/udev" reports unit
	 *    "systemd-udevd.service", because sd_pid_get_unit() walks up to the
	 *    owning unit. The <Cgroup> pattern for that same service does not
	 *    match the nested path unless whoever wrote it remembered a trailing
	 *    glob -- a footgun a unit name does not have.
	 *
	 * Optional, like <Cgroup>: an entry needs any one of the three.
	 */
	char units[MAX_UNITS_PER_ENTRY][MAX_UNIT_NAME];
	int n_units;
	enum classification cls;
	/* Resolved spec used at apply time (per-process explicit value if
     * present, otherwise the matching ClassDefaults value). */
	struct core_spec resolved;
	/* The per-process <ActiveCores> value as parsed; .groups==0 and
     * .cpulist[0]=='\0' means "fall back to ClassDefaults". */
	struct core_spec explicit_spec;
	/* If non-zero, PIDs of this comm that live inside a login-session
     * scope (gdm-*, sshd-session, sudo, session-N.scope) are NOT
     * skipped: they get sched_setaffinity() applied. Use this for
     * games / apps you knowingly launch from a terminal or .desktop
     * file under a login session and want pinned. Set via the
     * <AllowSession> tag in process_cpuset.xml. */
	int allow_session;
	/* If non-zero, walk /proc/<pid>/task/ and apply the mask to every
     * TID instead of just the leader.
     *
     * Defaults to 1, and you almost never want 0. sched_setaffinity(2)
     * targets a *thread*, so binding only the leader leaves every
     * already-running worker on its old (usually all-CPUs) mask: a
     * process whose leader sits on the LP-E cores keeps executing on
     * the P-cores through its thread pool, and the classification
     * achieves nothing. The cgroup path this library used to take
     * constrained all threads implicitly, because cgroup membership is
     * per-process; leader-only affinity does not, so the default has
     * to cover threads to preserve that behaviour.
     *
     * Set <AffinityAllThreads>0</AffinityAllThreads> to opt out for an
     * entry whose workers are deliberately placed by the application
     * itself. */
	int affinity_all_threads;
};

/* Global CPU groups parsed from <CpuGroups> in the XML. The per-process
 * <ActiveCores> tag selects which of these groups apply to that process. */
struct cpu_groups {
	char p_cores[MAX_CPULIST];
	char e_cores[MAX_CPULIST];
	char l_cores[MAX_CPULIST];
};

/* Tracked PIDs lpmd has already bound, so a later pass does not
 * re-evaluate (and so re-decide) a task it is already responsible for.
 * cls/groups capture the classification and resolved CPU group mask
 * used at attach time so introspection (LIST-BOUND) can show them
 * without re-parsing the XML.
 *
 * There is no unit name here. Binding is always sched_setaffinity(2) on
 * the task in place, so there is no scope unit to remember and nothing
 * to stop on release -- only a mask to put back. */
struct attached_entry {
	pid_t pid;
	enum classification cls;
	unsigned int groups;
	uid_t owner_uid; /* 0 = system manager, else user manager */
	int all_threads; /* affinity was applied to every TID, not just the leader */
	/* The classification came from the task's slice rather than from a
	 * <Process> entry. Reported by LIST-BOUND so an operator can see which
	 * of the two policies actually decided this PID. */
	int from_intent;
	/* The mask came from the entry's own <ActiveCores>, not from its
	 * class, so a change to the class's <ClassDefaults> must not touch
	 * it. */
	int explicit_cores;

	/* Exact-restore bookkeeping. orig_* is the mask the task had before
	 * lpmd touched it; set_* is what lpmd wrote. On release we only
	 * restore orig_* if the task's current mask still equals set_*:
	 * anything else means the task (or an admin) changed its own
	 * affinity after we bound it, and overwriting that would be worse
	 * than leaving it. start_time guards against PID reuse between
	 * attach and release. */
	uint8_t orig_mask[CPUMASK_BYTES];
	size_t orig_mlen;
	uint8_t set_mask[CPUMASK_BYTES];
	size_t set_mlen;
	unsigned long long start_time;
};
struct pid_set {
	struct attached_entry *items;
	size_t n;
	size_t cap;
};

/* One entry per descendant TGID promoted alongside the focused PID.
 * Remembers everything needed to revert that descendant to its prior
 * state when focus moves away: mask + classification + groups. */
#define FOCUS_DESC_MAX 256
struct focus_desc {
	pid_t pid;
	int had_entry; /* 1 = was in attached set */
	uint8_t orig_mask[CPUMASK_BYTES];
	size_t orig_mlen;
	enum classification orig_cls;
	unsigned orig_groups;
};

struct process_cpuset_ctx {
	struct proc_entry entries[MAX_PROCS];
	int n_entries;

	struct cpu_groups groups;

	/* Per-classification defaults; indexed by enum classification.
     * Sized to CLASS_INVALID + 1 so every enumerator (including the
     * GameProfile* placeholders) has a slot. */
	struct core_spec class_defaults[CLASS_INVALID + 1];
	/* Per-classification uclamp defaults. UCLAMP_UNSET means disabled. */
	int class_uclamp_min[CLASS_INVALID + 1];
	int class_uclamp_max[CLASS_INVALID + 1];

	/* Optional <DefaultProcess> entry: catch-all for any user-session
     * PID whose comm is not matched by ctx->entries[]. .cls is set
     * to a valid classification iff the XML provided one. */
	struct proc_entry default_entry;
	int has_default_entry;

	struct pid_set attached;

	/* Currently focused PID (set by process_cpuset_set_focus_pid()).
     * When non-zero, this PID is treated as user_interactive even if
     * its <Process> entry classifies it differently. focus_orig_* let
     * us revert the change cleanly on demotion. */
	pid_t focus_pid;
	int focus_all_threads;
	uint8_t focus_orig_mask[CPUMASK_BYTES];
	size_t focus_orig_mlen;
	enum classification focus_orig_cls;
	unsigned focus_orig_groups;

	/* Descendant TGIDs that were promoted together with focus_pid
     * (e.g. browser content/GPU/RDD sub-processes). Reverted in
     * reverse order on demote. */
	struct focus_desc focus_descs[FOCUS_DESC_MAX];
	size_t focus_desc_n;

	/* Focus-helper handshake. Set by
     * process_cpuset_set_focus_helper_present() when a user-session
     * relay (e.g. intel_lpmd_focus_helper) signals it has registered
     * with the compositor and will be sending focus-PID events.
     *
     * While 0 (default at startup), default_spec_for(USER_INITIATED)
     * transparently returns the USER_INTERACTIVE spec, so every
     * user-session app gets the larger cpuset -- there's no point
     * starving them when we have no signal about which one is
     * focused. When the helper flips this to 1, USER_INITIATED
     * starts honoring its own <ClassDefaults>, freeing the
     * USER_INTERACTIVE cpuset for the genuinely focused PID. */
	int focus_helper_present;
};

/* ---------- small helpers ---------- */

static const struct core_spec *default_spec_for(const process_cpuset_t *ctx,
						enum classification c)
{
	static const struct core_spec empty;

	if ((int)c < 0 || (size_t)c >= sizeof(ctx->class_defaults) /
					       sizeof(ctx->class_defaults[0]))
		return &empty;
	/* Until the focus helper announces itself, user_initiated mirrors
     * user_interactive so user-session apps don't get penalised in
     * the common case where lpmd has no focus signal at all. */
	if (c == CLASS_USER_INITIATED && !ctx->focus_helper_present)
		return &ctx->class_defaults[CLASS_USER_INTERACTIVE];
	return &ctx->class_defaults[c];
}

/* Append @add to the cpuset-style list in @dst (size @cap), inserting a
 * comma separator if @dst is non-empty. Silently truncates on overflow. */
static void cpulist_append(char *dst, size_t cap, const char *add,
			   size_t add_len)
{
	size_t cur = strlen(dst);
	size_t need = (cur ? 1 : 0) + add_len;

	if (cur + need + 1 > cap)
		return; /* no room; drop this token */
	if (cur)
		dst[cur++] = ',';
	memcpy(dst + cur, add, add_len);
	dst[cur + add_len] = '\0';
}

/* True if @tok (length @len) looks like a cpuset list element:
 * digits, with optional '-' or ".." range and trailing digits. */
static int token_is_cpu_literal(const char *tok, size_t len)
{
	size_t i;
	int saw_digit = 0;

	if (!len)
		return 0;
	for (i = 0; i < len; i++) {
		char ch = tok[i];
		if (ch >= '0' && ch <= '9') {
			saw_digit = 1;
			continue;
		}
		if (ch == '-')
			continue;
		if (ch == '.' && i + 1 < len && tok[i + 1] == '.') {
			i++;
			continue;
		}
		return 0;
	}
	return saw_digit;
}

/* Parse a token list (used by <ActiveCores> and every <ClassDefaults>
 * child). Recognized named groups (case-insensitive):
 *   ActivePcores / ActiveEcores / ActiveLcores
 * Anything that looks like a cpuset list element (digits, optional
 * '-' or ".." ranges) is appended to @out->cpulist as a literal.
 * Tokens that match neither are reported and skipped. */
static void parse_core_spec(const char *s, struct core_spec *out)
{
	const char *p = s;

	if (!out)
		return;
	out->groups = 0;
	out->cpulist[0] = '\0';
	if (!s)
		return;

	while (*p) {
		const char *tok;
		size_t len;

		while (*p &&
		       (isspace((unsigned char)*p) || *p == ',' || *p == '|'))
			p++;
		if (!*p)
			break;
		tok = p;
		while (*p && !isspace((unsigned char)*p) && *p != ',' &&
		       *p != '|')
			p++;
		len = (size_t)(p - tok);

		if (len == strlen("ActivePcores") &&
		    !strncasecmp(tok, "ActivePcores", len))
			out->groups |= GROUP_PCORES;
		else if (len == strlen("ActiveEcores") &&
			 !strncasecmp(tok, "ActiveEcores", len))
			out->groups |= GROUP_ECORES;
		else if (len == strlen("ActiveLcores") &&
			 !strncasecmp(tok, "ActiveLcores", len))
			out->groups |= GROUP_LCORES;
		else if (token_is_cpu_literal(tok, len))
			cpulist_append(out->cpulist, sizeof(out->cpulist), tok,
				       len);
		else
			lpmd_log_debug(
				"warning: unknown ActiveCores token '%.*s'\n",
				(int)len, tok);
	}
}

/* Parse a token list (used by <ActiveCores>) and return only the
 * named-group mask. Kept for callers that don't care about literal CPU
 * lists. */
static unsigned int parse_active_cores(const char *s)
{
	struct core_spec spec;
	parse_core_spec(s, &spec);
	return spec.groups;
}

/* ---------- CPU list parsing ---------- */

/*
 * Parse a cpuset-style list ("0,2,4-6,8") and set the corresponding bits in
 * mask (little-endian byte array, bit i = CPU i). Returns 0 on success.
 */
static int cpulist_to_mask(const char *list, uint8_t *mask, size_t mask_bytes)
{
	const char *p = list;
	char *end;
	long a, b;

	memset(mask, 0, mask_bytes);
	if (!list || !*list)
		return 0;

	while (*p) {
		while (*p && (isspace((unsigned char)*p) || *p == ','))
			p++;
		if (!*p)
			break;

		a = strtol(p, &end, 10);
		if (end == p)
			return -1;
		p = end;
		b = a;

		/* support "a-b" and "a..b" */
		if (*p == '-' || (p[0] == '.' && p[1] == '.')) {
			p += (*p == '-') ? 1 : 2;
			b = strtol(p, &end, 10);
			if (end == p)
				return -1;
			p = end;
		}

		if (a < 0 || b < 0 || a >= (long)(mask_bytes * 8) ||
		    b >= (long)(mask_bytes * 8) || a > b)
			return -1;

		for (long c = a; c <= b; c++)
			mask[c / 8] |= (uint8_t)(1u << (c % 8));
	}
	return 0;
}

static void mask_or(uint8_t *dst, const uint8_t *src, size_t n)
{
	for (size_t i = 0; i < n; i++)
		dst[i] |= src[i];
}

/* Trim trailing zero bytes for a slightly tighter sd-bus payload. */
static size_t mask_significant_len(const uint8_t *mask, size_t n)
{
	size_t len = n;
	while (len > 0 && mask[len - 1] == 0)
		len--;
	return len ? len : 1;
}

/*
 * Render a bitmap mask as a cpuset-style range list (e.g. "0-3,8-11").
 * Writes at most cap-1 bytes plus a NUL. Returns the (possibly
 * truncated) length, or 0 if the mask is empty.
 */
static size_t mask_to_cpulist(const uint8_t *mask, size_t mask_bytes, char *out,
			      size_t cap)
{
	size_t pos = 0;
	int first = 1;
	int in_range = 0;
	int range_start = 0, range_end = 0;
	int total_bits = (int)mask_bytes * 8;

	if (!out || cap == 0)
		return 0;
	out[0] = '\0';

#define EMIT(...)                                                     \
	do {                                                          \
		int _n = snprintf(out + pos, cap - pos, __VA_ARGS__); \
		if (_n < 0)                                           \
			return pos;                                   \
		if ((size_t)_n >= cap - pos) {                        \
			pos = cap - 1;                                \
			out[pos] = '\0';                              \
			return pos;                                   \
		}                                                     \
		pos += (size_t)_n;                                    \
	} while (0)

	for (int b = 0; b <= total_bits; b++) {
		int set = (b < total_bits) &&
			  (mask[b / 8] & (uint8_t)(1u << (b % 8)));
		if (set) {
			if (!in_range) {
				range_start = b;
				in_range = 1;
			}
			range_end = b;
		} else if (in_range) {
			if (!first)
				EMIT(",");
			if (range_start == range_end)
				EMIT("%d", range_start);
			else
				EMIT("%d-%d", range_start, range_end);
			first = 0;
			in_range = 0;
		}
	}
#undef EMIT
	return pos;
}

/* ---------- XML parsing (mirrors lpmd_config.c style) ---------- */

static enum classification parse_class(const char *s)
{
	if (!s)
		return CLASS_INVALID;
	if (!strcasecmp(s, "background"))
		return CLASS_BACKGROUND;
	if (!strcasecmp(s, "utility"))
		return CLASS_UTILITY;
	if (!strcasecmp(s, "unclassified") ||
	    !strcasecmp(s, "unlassified"))
		return CLASS_UNCLASSIFIED;
	/* Legacy alias kept for backward compatibility with older
	 * process_cpuset.xml files that used foreground/background. */
	if (!strcasecmp(s, "foreground"))
		return CLASS_USER_INITIATED;
	if (!strcasecmp(s, "user_initiated"))
		return CLASS_USER_INITIATED;
	if (!strcasecmp(s, "user_interactive"))
		return CLASS_USER_INTERACTIVE;
	if (!strcasecmp(s, "realtime"))
		return CLASS_REALTIME;
	if (!strcasecmp(s, "game_profile_cpu") ||
	    !strcasecmp(s, "GameProfileCPU"))
		return CLASS_GAME_PROFILE_CPU;
	if (!strcasecmp(s, "game_profile_gpu") ||
	    !strcasecmp(s, "GameProfileGPU"))
		return CLASS_GAME_PROFILE_GPU;
	if (!strcasecmp(s, "game_profile_mixed") ||
	    !strcasecmp(s, "GameProfileMixed"))
		return CLASS_GAME_PROFILE_HYBRID;
	if (!strcasecmp(s, "custom_profile_0") ||
	    !strcasecmp(s, "CustomProfile0"))
		return CLASS_CUSTOM_PROFILE_0;
	if (!strcasecmp(s, "custom_profile_1") ||
	    !strcasecmp(s, "CustomProfile1"))
		return CLASS_CUSTOM_PROFILE_1;
	if (!strcasecmp(s, "custom_profile_2") ||
	    !strcasecmp(s, "CustomProfile2"))
		return CLASS_CUSTOM_PROFILE_2;
	return CLASS_INVALID;
}

static const char *class_str(enum classification c)
{
	switch (c) {
	case CLASS_BACKGROUND:
		return "background";
	case CLASS_UTILITY:
		return "utility";
	case CLASS_UNCLASSIFIED:
		return "Unclassified";
	case CLASS_USER_INITIATED:
		return "user_initiated";
	case CLASS_USER_INTERACTIVE:
		return "user_interactive";
	case CLASS_REALTIME:
		return "realtime";
	case CLASS_GAME_PROFILE_CPU:
		return "game_profile_cpu";
	case CLASS_GAME_PROFILE_GPU:
		return "game_profile_gpu";
	case CLASS_GAME_PROFILE_HYBRID:
		return "game_profile_mixed";
	case CLASS_CUSTOM_PROFILE_0:
		return "custom_profile_0";
	case CLASS_CUSTOM_PROFILE_1:
		return "custom_profile_1";
	case CLASS_CUSTOM_PROFILE_2:
		return "custom_profile_2";
	default:
		return "invalid";
	}
}

static void copy_text(char *dst, size_t cap, const char *src)
{
	if (!src) {
		dst[0] = '\0';
		return;
	}
	snprintf(dst, cap, "%s", src);
	dst[cap - 1] = '\0';
}

/*
 * copy_text() that drops surrounding whitespace. Used for <Cgroup>, whose
 * values are long enough that they invite being wrapped onto their own
 * indented line; a stored "\n\t\tsystem.slice/foo.service\n\t" would never
 * match anything and give no hint why.
 */
static void copy_text_trim(char *dst, size_t cap, const char *src)
{
	size_t n;

	copy_text(dst, cap, src);

	n = strlen(dst);
	while (n && isspace((unsigned char)dst[n - 1]))
		dst[--n] = '\0';

	if (isspace((unsigned char)dst[0])) {
		char *p = dst;

		while (*p && isspace((unsigned char)*p))
			p++;
		memmove(dst, p, strlen(p) + 1);
	}
}

static void parse_cpu_groups(xmlDoc *doc, xmlNode *node, struct cpu_groups *g)
{
	xmlNode *c;
	char *val;

	for (c = node; c; c = c->next) {
		if (c->type != XML_ELEMENT_NODE)
			continue;
		val = (char *)xmlNodeListGetString(doc, c->xmlChildrenNode, 1);
		if (!val)
			continue;
		if (!strcmp((const char *)c->name, "ActivePcores"))
			copy_text(g->p_cores, sizeof(g->p_cores), val);
		else if (!strcmp((const char *)c->name, "ActiveEcores"))
			copy_text(g->e_cores, sizeof(g->e_cores), val);
		else if (!strcmp((const char *)c->name, "ActiveLcores"))
			copy_text(g->l_cores, sizeof(g->l_cores), val);
		xmlFree(val);
	}
}

static void parse_one_process(const process_cpuset_t *ctx, xmlDoc *doc,
			      xmlNode *node, struct proc_entry *e)
{
	xmlNode *c;
	char *val;

	memset(e, 0, sizeof(*e));
	e->cls = CLASS_INVALID;
	/* Cover every thread unless the entry explicitly opts out: see
	 * affinity_all_threads in struct proc_entry. */
	e->affinity_all_threads = 1;

	for (c = node; c; c = c->next) {
		if (c->type != XML_ELEMENT_NODE)
			continue;
		val = (char *)xmlNodeListGetString(doc, c->xmlChildrenNode, 1);
		if (!val)
			continue;

		if (!strcmp((const char *)c->name, "Name"))
			copy_text(e->name, sizeof(e->name), val);
		else if (!strcmp((const char *)c->name, "Cgroup")) {
			/* Repeatable: each one is an alternative path pattern. */
			if (e->n_cgroups >= MAX_CGROUPS_PER_ENTRY) {
				lpmd_log_debug(
					"Entry '%s': max %d <Cgroup> patterns, ignoring '%s'\n",
					e->name[0] ? e->name : "?",
					MAX_CGROUPS_PER_ENTRY, val);
			} else {
				char *slot = e->cgroups[e->n_cgroups];

				copy_text_trim(slot, MAX_CGROUP_PATH, val);
				/* Don't let an empty tag consume a slot. */
				if (slot[0])
					e->n_cgroups++;
			}
		} else if (!strcmp((const char *)c->name, "Unit")) {
			/* Repeatable, like <Cgroup>: alternatives, not a list. */
			if (e->n_units >= MAX_UNITS_PER_ENTRY) {
				lpmd_log_debug(
					"Entry '%s': max %d <Unit> patterns, ignoring '%s'\n",
					e->name[0] ? e->name : "?",
					MAX_UNITS_PER_ENTRY, val);
			} else {
				char *slot = e->units[e->n_units];

				copy_text_trim(slot, MAX_UNIT_NAME, val);
				if (slot[0])
					e->n_units++;
			}
		} else if (!strcmp((const char *)c->name, "Classification"))
			e->cls = parse_class(val);
		else if (!strcmp((const char *)c->name, "ActiveCores"))
			parse_core_spec(val, &e->explicit_spec);
		else if (!strcmp((const char *)c->name, "AllowSession")) {
			/* Accept 1/true/yes/on (case-insensitive) as enabled. */
			if (!strcasecmp(val, "1") || !strcasecmp(val, "true") ||
			    !strcasecmp(val, "yes") || !strcasecmp(val, "on"))
				e->allow_session = 1;
		} else if (!strcmp((const char *)c->name,
				   "AffinityAllThreads")) {
			/* Defaults to on, so this tag only matters as an
			 * explicit opt-out. Anything unrecognised leaves
			 * the default alone rather than guessing. */
			if (!strcasecmp(val, "0") || !strcasecmp(val, "false") ||
			    !strcasecmp(val, "no") || !strcasecmp(val, "off"))
				e->affinity_all_threads = 0;
			else if (!strcasecmp(val, "1") || !strcasecmp(val, "true") ||
				 !strcasecmp(val, "yes") || !strcasecmp(val, "on"))
				e->affinity_all_threads = 1;
		}

		xmlFree(val);
	}

	/* If no per-process override, use the classification default. */
	if (core_spec_is_set(&e->explicit_spec))
		e->resolved = e->explicit_spec;
	else
		e->resolved = *default_spec_for(ctx, e->cls);
}

/* ---------- /proc PID lookup ---------- */

static int read_comm(pid_t pid, char *out, size_t cap)
{
	char path[64];
	FILE *f;
	size_t n;

	snprintf(path, sizeof(path), "/proc/%d/comm", (int)pid);
	f = fopen(path, "r");
	if (!f)
		return -1;
	if (!fgets(out, cap, f)) {
		fclose(f);
		return -1;
	}
	fclose(f);
	n = strlen(out);
	if (n && out[n - 1] == '\n')
		out[n - 1] = '\0';
	return 0;
}

/* Read @pid's unified (cgroup v2) path into @out, e.g. "/system.slice/foo.service". */
static int pid_cgroup_path(pid_t pid, char *out, size_t cap)
{
	char path[64];
	char line[512];
	FILE *f;
	int rc = -1;

	snprintf(path, sizeof(path), "/proc/%d/cgroup", (int)pid);
	f = fopen(path, "r");
	if (!f)
		return -1;
	while (fgets(line, sizeof(line), f)) {
		size_t l;

		if (strncmp(line, "0::", 3))
			continue;
		l = strlen(line);
		if (l && line[l - 1] == '\n')
			line[--l] = '\0';
		snprintf(out, cap, "%s", line + 3);
		rc = 0;
		break;
	}
	fclose(f);
	return rc;
}

/* Best-effort process name lookup for logs. */
static const char *pid_comm_for_log(pid_t pid, char *buf, size_t cap)
{
	if (!buf || cap == 0)
		return "?";
	if (read_comm(pid, buf, cap) < 0 || buf[0] == '\0')
		snprintf(buf, cap, "?");
	return buf;
}

/* Match a config name against a process comm.
 * Exact names behave as before. If the config name contains '*',
 * treat it as a glob (e.g. Strange*).
 */
static int name_matches_entry(const char *pattern, const char *name)
{
	if (!pattern || !name)
		return 0;
	if (!strchr(pattern, '*'))
		return !strcmp(pattern, name);
	return fnmatch(pattern, name, 0) == 0;
}

static int pid_matches_name(pid_t pid, const char *pattern, const char *leader_comm)
{
	char path[64];
	DIR *task_dir;
	struct dirent *entry;
	char tcomm[MAX_NAME];
	FILE *tf;
	size_t n;

	if (!pattern || !*pattern)
		return 0;

	if (leader_comm && name_matches_entry(pattern, leader_comm))
		return 1;

	if (!strchr(pattern, '*'))
		return 0;

	snprintf(path, sizeof(path), "/proc/%d/task", (int)pid);
	task_dir = opendir(path);
	if (!task_dir)
		return 0;

	while ((entry = readdir(task_dir)) != NULL) {
		pid_t tid;
		char tpath[64];
		char *end;

		if (entry->d_type != DT_DIR)
			continue;
		if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, ".."))
			continue;
		tid = (pid_t)strtol(entry->d_name, &end, 10);
		if (*end != '\0' || tid <= 0)
			continue;
		snprintf(tpath, sizeof(tpath), "/proc/%d/task/%d/comm", (int)pid,
			 (int)tid);
		tf = fopen(tpath, "r");
		if (!tf)
			continue;
		if (fgets(tcomm, sizeof(tcomm), tf)) {
			n = strlen(tcomm);
			if (n && tcomm[n - 1] == '\n')
				tcomm[n - 1] = '\0';
			if (name_matches_entry(pattern, tcomm)) {
				fclose(tf);
				closedir(task_dir);
				return 1;
			}
		}
		fclose(tf);
	}
	closedir(task_dir);
	return 0;
}

/*
 * Match one <Cgroup> pattern against a cgroup v2 path.
 *
 * Both sides are normalised to have no leading '/', so the config can be
 * written the way the paths read in `systemd-cgls` output
 * ("system.slice/foo.service") without having to remember a leading slash.
 *
 * fnmatch is called without FNM_PATHNAME, so '*' crosses '/' just as it does
 * in <Name> globs. That is deliberate: "system.slice/*" should mean everything
 * under system.slice, including units nested below another slice, which is
 * what anyone writing that pattern expects.
 */
static int cgroup_matches_pattern(const char *pattern, const char *cgpath)
{
	if (!pattern || !*pattern || !cgpath)
		return 0;

	while (*pattern == '/')
		pattern++;
	while (*cgpath == '/')
		cgpath++;

	if (!strchr(pattern, '*'))
		return !strcmp(pattern, cgpath);
	return fnmatch(pattern, cgpath, 0) == 0;
}

/* How an entry matched a PID. Higher wins when several entries are eligible. */
enum entry_match {
	ENTRY_MATCH_NONE = 0,
	ENTRY_MATCH_COMM,
	ENTRY_MATCH_CGROUP,
	ENTRY_MATCH_UNIT,
};

/*
 * The unit names owning one task, as systemd reports them. Read once per PID
 * and passed down, because both matchers below want them and each lookup is a
 * /proc read.
 *
 * Either may be empty. A system daemon has @sys only; a task under
 * user@<uid>.service has @sys "user@1000.service" and @usr the unit inside that
 * manager ("app-code-18399.scope"); a kernel thread has neither, because
 * sd_pid_get_unit() answers -ENODATA for a task in the root cgroup.
 */
struct pid_units {
	char sys[MAX_UNIT_NAME];
	char usr[MAX_UNIT_NAME];
};

/*
 * Fill @u for @pid. Never fails: a name we cannot read stays empty, which
 * simply means no <Unit> pattern can match on it.
 *
 * sd_pid_get_unit() and sd_pid_get_user_unit() malloc their result, so each is
 * copied into the caller's fixed buffer and freed here. A name longer than the
 * buffer is dropped rather than truncated: a shortened unit name is a different
 * unit, and matching a pattern against it would claim the wrong task.
 */
static void pid_read_units(pid_t pid, struct pid_units *u)
{
	char *name = NULL;

	u->sys[0] = '\0';
	u->usr[0] = '\0';

	if (sd_pid_get_unit(pid, &name) >= 0 && name) {
		if (strlen(name) < sizeof(u->sys))
			memcpy(u->sys, name, strlen(name) + 1);
		free(name);
		name = NULL;
	}
	if (sd_pid_get_user_unit(pid, &name) >= 0 && name) {
		if (strlen(name) < sizeof(u->usr))
			memcpy(u->usr, name, strlen(name) + 1);
		free(name);
	}
}

/*
 * Match one <Unit> pattern against one unit name.
 *
 * Globs as <Name> and <Cgroup> do, so "app-*.scope" covers every graphical
 * app scope and "dbus-*.service" every D-Bus-activated user service. Unit
 * names contain no '/' -- systemd escapes anything that would -- so there is no
 * FNM_PATHNAME question to answer here.
 */
static int unit_matches_pattern(const char *pattern, const char *unit)
{
	if (!pattern || !*pattern || !unit || !*unit)
		return 0;
	if (!strchr(pattern, '*'))
		return !strcmp(pattern, unit);
	return fnmatch(pattern, unit, 0) == 0;
}

/*
 * Test @pid against one <Process> entry.
 *
 * The three matchers are OR'ed, so an entry that gains a <Cgroup> or a <Unit>
 * keeps matching everything its <Name> used to. The return value distinguishes
 * them so the caller can prefer the more specific one: see the notes on
 * struct proc_entry.cgroups and .units.
 *
 * @cgpath may be NULL (or "") when the caller could not read the PID's cgroup,
 * and @units may be NULL or hold empty names; each matcher simply does not fire.
 */
static enum entry_match pid_matches_entry(pid_t pid, const struct proc_entry *e,
					  const char *comm, const char *cgpath,
					  const struct pid_units *units)
{
	int i;

	if (!e)
		return ENTRY_MATCH_NONE;

	if (units) {
		for (i = 0; i < e->n_units; i++) {
			if (unit_matches_pattern(e->units[i], units->sys) ||
			    unit_matches_pattern(e->units[i], units->usr))
				return ENTRY_MATCH_UNIT;
		}
	}

	if (cgpath && *cgpath) {
		for (i = 0; i < e->n_cgroups; i++) {
			if (cgroup_matches_pattern(e->cgroups[i], cgpath))
				return ENTRY_MATCH_CGROUP;
		}
	}

	if (e->name[0] && pid_matches_name(pid, e->name, comm))
		return ENTRY_MATCH_COMM;

	return ENTRY_MATCH_NONE;
}

/*
 * Pick the entry that should govern @pid, or NULL if none does.
 *
 * Entries are scanned in config order and the first comm match is remembered,
 * but a later cgroup or unit match replaces it, so an entry keyed on something
 * systemd knows always beats one keyed on a 15-character comm prefix regardless
 * of the order they appear in the file. Among equally specific matches the first
 * wins, which is the behaviour entries had before <Cgroup> existed.
 */
static const struct proc_entry *find_entry_for_pid(const process_cpuset_t *ctx,
						   pid_t pid, const char *comm,
						   const char *cgpath,
						   const struct pid_units *units,
						   enum entry_match *how)
{
	const struct proc_entry *best = NULL;
	enum entry_match best_how = ENTRY_MATCH_NONE;

	for (int i = 0; i < ctx->n_entries; i++) {
		enum entry_match m = pid_matches_entry(pid, &ctx->entries[i],
						       comm, cgpath, units);

		if (m > best_how) {
			best = &ctx->entries[i];
			best_how = m;
			if (best_how == ENTRY_MATCH_UNIT)
				break; /* nothing outranks this */
		}
	}

	if (how)
		*how = best_how;
	return best;
}

/*
 * Whether any entry has a <Cgroup> pattern.
 *
 * Reading /proc/<pid>/cgroup costs an open/read/close per PID, so the /proc
 * sweeps skip it entirely on a config that only uses <Name> -- which is every
 * config predating this feature.
 */
static int entries_use_cgroups(const process_cpuset_t *ctx)
{
	if (!ctx)
		return 0;
	for (int i = 0; i < ctx->n_entries; i++)
		if (ctx->entries[i].n_cgroups)
			return 1;
	return 0;
}

/*
 * Whether any entry has a <Unit> pattern.
 *
 * Same bargain as entries_use_cgroups(): resolving a PID's unit costs two
 * /proc reads, so a config that does not use <Unit> -- which is every config
 * predating this feature -- never pays for it.
 */
static int entries_use_units(const process_cpuset_t *ctx)
{
	if (!ctx)
		return 0;
	for (int i = 0; i < ctx->n_entries; i++)
		if (ctx->entries[i].n_units)
			return 1;
	return 0;
}

/*
 * Log label for an entry: <Name> if it has one, else its first <Unit>, else its
 * first <Cgroup>. Unit before cgroup because an entry carrying both is keyed on
 * the unit -- that is the one that decides.
 */
static const char *entry_label(const struct proc_entry *e)
{
	if (!e)
		return "?";
	if (e->name[0])
		return e->name;
	if (e->n_units)
		return e->units[0];
	if (e->n_cgroups)
		return e->cgroups[0];
	return "?";
}

/*
 * Resolve a possibly-TID to its thread-group leader (TGID) by parsing
 * /proc/<pid>/status's "Tgid:" field. Returns @pid unchanged on
 * error so callers can keep going.
 *
 * Compositors (X11 _NET_WM_PID, Wayland portal helpers) sometimes
 * report whichever TID happened to create the toplevel window
 * rather than the process leader. Acting on a TID directly skips
 * sibling threads; resolving to the TGID first lets the
 * all-threads path constrain every worker.
 */
static pid_t pid_to_tgid(pid_t pid)
{
	char path[64];
	char buf[4096];
	FILE *f;
	char *line, *save;
	size_t nread;
	pid_t tgid = pid;

	snprintf(path, sizeof(path), "/proc/%d/status", (int)pid);
	f = fopen(path, "r");
	if (!f)
		return pid;
	nread = fread(buf, 1, sizeof(buf) - 1, f);
	if (nread == 0) {
		fclose(f);
		return pid;
	}
	fclose(f);
	buf[nread] = '\0';

	for (line = strtok_r(buf, "\n", &save); line;
	     line = strtok_r(NULL, "\n", &save)) {
		if (!strncmp(line, "Tgid:", 5)) {
			long v = strtol(line + 5, NULL, 10);
			if (v > 0)
				tgid = (pid_t)v;
			break;
		}
	}
	return tgid;
}


/*
 * Classify where a PID lives in the cgroup hierarchy.
 *
 * Returns:
 *   PID_LOC_SYSTEM    - PID is in the system slice (or wherever);
 *                       safe to manage via the system systemd manager.
 *   PID_LOC_USER_MGR  - PID is under user@<UID>.service (i.e. owned
 *                       by that user's --user systemd manager).
 *                       *out_uid is set to that UID. Scope creation
 *                       must go through the user manager so the
 *                       cgroup stays inside user@<UID>.service.
 *   PID_LOC_USER_SESS - PID is in user.slice but OUTSIDE user@.service
 *                       (e.g. session-N.scope, sshd-session, sudo,
 *                       gdm-*, login shells). The user manager can't
 *                       attach these (it only owns its own subtree)
 *                       and migrating them via the system manager
 *                       severs logind's session-*.scope ancestry,
 *                       breaking polkit. Do not migrate these PIDs.
 *
 * Reads /proc/<pid>/cgroup. cgroup v2 lines look like:
 *   0::/user.slice/user-1000.slice/user@1000.service/app.slice/...
 *   0::/user.slice/user-1000.slice/session-3.scope
 *   0::/system.slice/foo.service
 */
enum pid_location {
	PID_LOC_SYSTEM = 0,
	PID_LOC_USER_MGR,
	PID_LOC_USER_SESS,
};

static enum pid_location classify_pid_location(pid_t pid, uid_t *out_uid)
{
	char path[64];
	char line[512];
	FILE *f;
	enum pid_location loc = PID_LOC_SYSTEM;

	if (out_uid)
		*out_uid = 0;

	snprintf(path, sizeof(path), "/proc/%d/cgroup", (int)pid);
	f = fopen(path, "r");
	if (!f)
		return PID_LOC_SYSTEM;

	while (fgets(line, sizeof(line), f)) {
		const char *p;

		/* Under user@<uid>.service -> user manager territory. */
		p = strstr(line, "/user@");
		if (p) {
			unsigned uid;
			if (sscanf(p + 6, "%u.service", &uid) == 1) {
				if (out_uid)
					*out_uid = (uid_t)uid;
				loc = PID_LOC_USER_MGR;
				break;
			}
		}
		/* In user.slice but not under user@ -> login-session scope.
         * Don't migrate. */
		if (strstr(line, "/user.slice/") ||
		    strstr(line, "/session.slice/") ||
		    strstr(line, "/session-")) {
			loc = PID_LOC_USER_SESS;
			/* keep scanning: a later line might still expose user@ */
		}
	}
	fclose(f);
	return loc;
}

/*
 * Read the real UID of @pid from /proc/<pid>/status.
 * Returns 0 on success and stores the UID in *out, -1 on error.
 */
static int pid_get_real_uid(pid_t pid, uid_t *out)
{
	char path[64];
	char line[256];
	FILE *f;
	int ok = -1;

	snprintf(path, sizeof(path), "/proc/%d/status", (int)pid);
	f = fopen(path, "r");
	if (!f)
		return -1;
	while (fgets(line, sizeof(line), f)) {
		unsigned ruid;
		if (sscanf(line, "Uid: %u", &ruid) == 1) {
			*out = (uid_t)ruid;
			ok = 0;
			break;
		}
	}
	fclose(f);
	return ok;
}

/*
 * Apply @mask to @pid (and all its threads) using sched_setaffinity(2).
 *
 * This is the only way lpmd binds an individual task. It doesn't touch
 * cgroups at all, so the task stays in its own service's or session's
 * cgroup and systemd accounting, resource limits, systemctl
 * kill/stop semantics and logind / polkit session tracking all remain
 * intact. It also works for tasks under user@<UID>.service, where the
 * user-mode systemd manager normally does not have the cgroup v2
 * cpuset controller delegated and a cgroup-based approach would fail
 * with -EBADR ("Invalid request descriptor") anyway.
 *
 * The effective set is the requested mask intersected with the
 * task's cpuset, so the cgroup it lives in is a hard ceiling;
 * affinity_bind_pid applies that intersection explicitly.
 *
 * Caveat: sched_setaffinity is not inherited by future siblings
 * spawned outside this PID's process tree -- but the proc-connector
 * EXEC listener and the periodic rescan together cover that.
 *
 * Returns 0 on success, -1 on failure (e.g. PID gone).
 */
static int set_pid_affinity_from_mask(pid_t pid, const uint8_t *mask,
				      size_t mask_len)
{
	cpu_set_t *set;
	size_t setsize;
	int n_cpus = (int)(mask_len * 8);
	int r;

	if (n_cpus <= 0)
		return -1;

	set = CPU_ALLOC(n_cpus);
	if (!set)
		return -1;
	setsize = CPU_ALLOC_SIZE(n_cpus);
	CPU_ZERO_S(setsize, set);

	for (size_t b = 0; b < mask_len; b++) {
		for (int bit = 0; bit < 8; bit++) {
			if (mask[b] & (1u << bit))
				CPU_SET_S(b * 8 + bit, setsize, set);
		}
	}

	r = sched_setaffinity(pid, setsize, set);
	CPU_FREE(set);
	if (r < 0) {
		if (errno != ESRCH)
			lpmd_log_debug(
				"sched_setaffinity(pid=%d) failed: %s\n",
				(int)pid, strerror(errno));
		return -1;
	}
	return 0;
}

/*
 * Apply @mask to every TID in /proc/<pid>/task/. Returns the number
 * of threads successfully constrained (0 if the PID is already gone).
 *
 * sched_setaffinity targets a TID, so calling it on the leader only
 * constrains the main thread. For multithreaded apps (games,
 * browsers, media engines) we want every worker pinned. Walking
 * /proc/<pid>/task/ is cheap (~1 syscall per TID); see the
 * <AffinityAllThreads> tag in process_cpuset.xml.
 */
static int set_pid_affinity_all_threads(pid_t pid, const uint8_t *mask,
					size_t mask_len, const char *cls_label)
{
	char path[64];
	char comm[MAX_NAME] = "?";
	char mask_hex[(CPUMASK_BYTES * 2) + 1];
	DIR *d;
	struct dirent *de;
	int n_ok = 0, n_fail = 0;
	const char *cls = cls_label ? cls_label : "?";

	(void)read_comm(pid, comm, sizeof(comm));

	snprintf(path, sizeof(path), "/proc/%d/task", (int)pid);
	d = opendir(path);
	if (!d) {
		/* Fall back to leader-only if /proc/<pid>/task vanished. */
		lpmd_log_debug("all-threads pid=%d comm=%s class=%s: opendir(%s) failed: %s; falling back to leader-only\n",
			       (int)pid, comm, cls, path, strerror(errno));
		return set_pid_affinity_from_mask(pid, mask, mask_len) == 0 ?
			       1 :
			       0;
	}

	for (size_t k = 0; k < mask_len && (k * 2 + 1) < sizeof(mask_hex); k++)
		snprintf(mask_hex + (k * 2), sizeof(mask_hex) - (k * 2), "%02x",
			 mask[k]);
	mask_hex[mask_len * 2] = '\0';
	lpmd_log_debug("all-threads pid=%d comm=%s class=%s: scanning %s mask=%s\n",
		       (int)pid, comm, cls, path, mask_hex);

	while ((de = readdir(d))) {
		char *end;
		long tid = strtol(de->d_name, &end, 10);
		char tcomm[MAX_NAME] = "?";
		char tpath[80];
		FILE *tf;
		if (*end != '\0' || tid <= 0)
			continue;
		snprintf(tpath, sizeof(tpath), "/proc/%d/task/%ld/comm",
			 (int)pid, tid);
		tf = fopen(tpath, "r");
		if (tf) {
			if (fgets(tcomm, sizeof(tcomm), tf)) {
				size_t tn = strlen(tcomm);
				if (tn && tcomm[tn - 1] == '\n')
					tcomm[tn - 1] = '\0';
			}
			fclose(tf);
		}
		if (set_pid_affinity_from_mask((pid_t)tid, mask, mask_len) ==
		    0) {
			n_ok++;
			lpmd_log_debug("  tid %ld comm=%s class=%s OK\n", tid,
				       tcomm, cls);
		} else {
			n_fail++;
			lpmd_log_debug("  tid %ld comm=%s class=%s FAIL (%s)\n",
				       tid, tcomm, cls, strerror(errno));
		}
	}
	closedir(d);
	lpmd_log_debug("all-threads pid=%d comm=%s class=%s: %d ok, %d failed\n",
		       (int)pid, comm, cls, n_ok, n_fail);
	return n_ok;
}

static int class_uclamp_get(const process_cpuset_t *ctx,
			   enum classification cls,
			   int *uclamp_min, int *uclamp_max)
{
	int min_v, max_v;

	if (!ctx || !uclamp_min || !uclamp_max)
		return -1;
	if ((int)cls < 0 || cls > CLASS_INVALID)
		return -1;

	min_v = ctx->class_uclamp_min[cls];
	max_v = ctx->class_uclamp_max[cls];

	/* Disabled for this class. */
	if (min_v == UCLAMP_UNSET && max_v == UCLAMP_UNSET)
		return 1;

	if (min_v == UCLAMP_UNSET)
		min_v = UCLAMP_CLAMP_MIN;
	if (max_v == UCLAMP_UNSET)
		max_v = UCLAMP_CLAMP_MAX;

	if (min_v > max_v)
		return -1;

	*uclamp_min = min_v;
	*uclamp_max = max_v;
	return 0;
}

static int sched_setattr_pid(pid_t pid, const struct sched_attr *attr,
			    unsigned int flags)
{
#ifdef SYS_sched_setattr
	return syscall(SYS_sched_setattr, pid, attr, flags);
#else
	(void)pid;
	(void)attr;
	(void)flags;
	errno = ENOSYS;
	return -1;
#endif
}

static int apply_pid_uclamp_single(pid_t pid, enum classification cls,
				  const char *cls_label,
				  int min_v, int max_v)
{
	struct sched_attr attr;

	memset(&attr, 0, sizeof(attr));
	attr.size = sizeof(attr);
	attr.sched_flags = SCHED_FLAG_KEEP_POLICY | SCHED_FLAG_KEEP_PARAMS |
			   SCHED_FLAG_UTIL_CLAMP_MIN |
			   SCHED_FLAG_UTIL_CLAMP_MAX;
	attr.sched_util_min = (uint32_t)min_v;
	attr.sched_util_max = (uint32_t)max_v;

	if (sched_setattr_pid(pid, &attr, 0) < 0) {
		if (errno != ESRCH)
			lpmd_log_debug(
				"uclamp: sched_setattr tid=%d class=%s min=%d max=%d failed: %s\n",
				(int)pid,
				cls_label ? cls_label : class_str(cls), min_v,
				max_v, strerror(errno));
		return -1;
	}
	return 0;
}

static int apply_pid_uclamp(process_cpuset_t *ctx, pid_t pid,
			   enum classification cls,
			   const char *cls_label,
			   int all_threads)
{
	char path[64];
	char comm[MAX_NAME] = "?";
	DIR *d;
	struct dirent *de;
	int min_v, max_v;
	int rc;

	/* A class <ClassDefaults> does not set applies nothing, uclamp
	 * included: process_cpuset_log_class_defaults() warns about it. */
	if (!core_spec_is_set(default_spec_for(ctx, cls)))
		return 0;
	rc = class_uclamp_get(ctx, cls, &min_v, &max_v);
	if (rc == 1)
		return 0; /* not configured */
	if (rc < 0) {
		lpmd_log_debug(
			"uclamp: invalid config for class=%s, skip pid=%d\n",
			cls_label ? cls_label : class_str(cls), (int)pid);
		return -1;
	}

	if (!all_threads) {
		rc = apply_pid_uclamp_single(pid, cls, cls_label, min_v,
					     max_v);
		if (rc == 0)
			lpmd_log_debug(
				"uclamp: pid=%d class=%s min=%d max=%d\n",
				(int)pid,
				cls_label ? cls_label : class_str(cls), min_v,
				max_v);
		return rc;
	}

	(void)read_comm(pid, comm, sizeof(comm));
	snprintf(path, sizeof(path), "/proc/%d/task", (int)pid);
	d = opendir(path);
	if (!d) {
		if (errno != ESRCH)
			lpmd_log_debug(
				"uclamp: all-threads pid=%d comm=%s class=%s: opendir(%s) failed: %s; falling back to leader-only\n",
				(int)pid, comm,
				cls_label ? cls_label : class_str(cls), path,
				strerror(errno));
		rc = apply_pid_uclamp_single(pid, cls, cls_label, min_v,
					     max_v);
		if (rc == 0)
			lpmd_log_debug(
				"uclamp: pid=%d class=%s min=%d max=%d (leader-only fallback)\n",
				(int)pid,
				cls_label ? cls_label : class_str(cls), min_v,
				max_v);
		return rc;
	}

	lpmd_log_debug(
		"uclamp: all-threads pid=%d comm=%s class=%s min=%d max=%d scanning %s\n",
		(int)pid, comm, cls_label ? cls_label : class_str(cls), min_v,
		max_v, path);

	{
		int n_ok = 0, n_fail = 0;
		while ((de = readdir(d))) {
			char *end;
			long tid = strtol(de->d_name, &end, 10);

			if (*end != '\0' || tid <= 0)
				continue;

			if (apply_pid_uclamp_single((pid_t)tid, cls,
						    cls_label, min_v,
						    max_v) == 0) {
				n_ok++;
				lpmd_log_debug("  tid %ld class=%s OK\n", tid,
					       cls_label ? cls_label :
							   class_str(cls));
			} else {
				n_fail++;
				lpmd_log_debug("  tid %ld class=%s FAIL (%s)\n",
					       tid,
					       cls_label ? cls_label :
							   class_str(cls),
					       strerror(errno));
			}
		}
		closedir(d);
		lpmd_log_debug(
			"uclamp: all-threads pid=%d comm=%s class=%s min=%d max=%d: %d ok, %d failed\n",
			(int)pid, comm, cls_label ? cls_label : class_str(cls),
			min_v, max_v, n_ok, n_fail);
		return n_ok > 0 ? 0 : -1;
	}
}

/* ---------- no process migration -------------------------------------
 *
 * Earlier revisions attached each matched PID to a transient scope unit
 * (proc_cpuset_<comm>_<pid>.scope) created with StartTransientUnit and
 * the PIDs= property, which *moves* the task out of its own service's
 * cgroup. That was removed deliberately. Moving a task out of, say,
 * systemd-journald.service breaks everything keyed on cgroup membership:
 * systemd's own accounting and resource limits (the service's cgroup is
 * left empty while MainPID still points at the moved task), systemctl
 * kill/stop semantics, logind/polkit session tracking, and any
 * cgroup-based observability the admin has set up. It also destroys the
 * unit identity that slice.xml policy keys on, so the two features
 * defeated each other. It is also what stranded tasks in the root
 * cgroup on shutdown: the release path wrote each PID to
 * /sys/fs/cgroup/cgroup.procs, which is not where it came from, so the
 * damage outlived the daemon.
 *
 * The scope machinery came in two halves and both are now gone. The
 * first half created the scope and moved the task into it. The second
 * half steered the task afterwards by rewriting that scope's
 * AllowedCPUs= over sd-bus (SetUnitProperties with runtime=true), used
 * by the focus-promotion path and by release. Removing only the first
 * half left the second as dead-but-loaded code: every call site was
 * guarded by a non-empty unit name that nothing could set any more, so
 * a single future assignment would have quietly re-enabled cgroup
 * cpuset writes against live service units. The guards, the state they
 * tested, and the sd-bus calls behind them are deleted, which is why
 * this file no longer includes sd-bus at all.
 *
 * So: this file only ever calls sched_setaffinity(2) on the task where
 * it already is (see affinity_bind_pid). No task is moved between
 * cgroups, no cgroup is created, and no cgroup's cpuset is written.
 *
 * Setting AllowedCPUs= on a unit that already exists is still a
 * legitimate mechanism, but it belongs to slice.xml and lives in
 * lpmd_slice_cpuset.c, keyed on units rather than on PIDs. That module
 * does not move tasks either.
 * -------------------------------------------------------------------- */


/* ---------- per-classification mask ---------- */

static int build_mask_for(const process_cpuset_t *ctx,
			  const struct proc_entry *e, uint8_t *mask,
			  size_t mask_bytes)
{
	uint8_t tmp[CPUMASK_BYTES];
	const struct core_spec *spec = &e->resolved;

	memset(mask, 0, mask_bytes);

	if (e->cls == CLASS_INVALID)
		return -1;

	if (spec->groups & GROUP_PCORES) {
		if (cpulist_to_mask(ctx->groups.p_cores, tmp, mask_bytes) < 0)
			return -1;
		mask_or(mask, tmp, mask_bytes);
	}
	if (spec->groups & GROUP_ECORES) {
		if (cpulist_to_mask(ctx->groups.e_cores, tmp, mask_bytes) < 0)
			return -1;
		mask_or(mask, tmp, mask_bytes);
	}
	if (spec->groups & GROUP_LCORES) {
		if (cpulist_to_mask(ctx->groups.l_cores, tmp, mask_bytes) < 0)
			return -1;
		mask_or(mask, tmp, mask_bytes);
	}
	/* Literal CPU list specified directly in <ActiveCores> or
     * <ClassDefaults>, e.g. "0-3,5,8". */
	if (spec->cpulist[0]) {
		if (cpulist_to_mask(spec->cpulist, tmp, mask_bytes) < 0)
			return -1;
		mask_or(mask, tmp, mask_bytes);
	}
	return 0;
}

/*
 * Read /proc/<pid>/stat field 22 (process start time, in clock ticks
 * since boot). This is unique per process invocation across the
 * lifetime of the kernel, so pairing it with the PID distinguishes a
 * tracked task from an unrelated one that later reused its PID.
 *
 * Returns the value on success, 0 if anything fails (in which case the
 * caller simply skips the PID-reuse check).
 *
 * /proc/<pid>/stat format: pid (comm) state ppid ... where (comm)
 * itself may contain spaces or ')', so we have to skip past the
 * LAST ')' before counting whitespace-delimited fields.
 */
static unsigned long long pid_start_time(pid_t pid)
{
	char path[64];
	char buf[2048];
	FILE *f;
	size_t n;
	char *p;
	int field;
	unsigned long long starttime = 0;

	snprintf(path, sizeof(path), "/proc/%d/stat", (int)pid);
	f = fopen(path, "r");
	if (!f)
		return 0;
	n = fread(buf, 1, sizeof(buf) - 1, f);
	fclose(f);
	if (n == 0)
		return 0;
	buf[n] = '\0';

	p = strrchr(buf, ')');
	if (!p)
		return 0;
	p++; /* skip ')' itself */
	/* From here, field index 3 (state) through 22 (starttime).
     * After ')' we're between field 2 and 3; advance 19 spaces to
     * reach the start of field 22. */
	field = 2;
	while (*p && field < 22) {
		if (*p == ' ')
			field++;
		p++;
	}
	if (field != 22)
		return 0;
	starttime = strtoull(p, NULL, 10);
	return starttime;
}

/*
 * Parent PID of @pid, or 0 if it could not be read. Same /proc/<pid>/stat
 * quirk as pid_start_time(): the comm field can contain spaces and ')',
 * so field counting has to start after the LAST ')'. ppid is field 4.
 */
static pid_t pid_ppid(pid_t pid)
{
	char path[64];
	char buf[2048];
	FILE *f;
	size_t n;
	char *p;

	if (pid <= 0)
		return 0;
	snprintf(path, sizeof(path), "/proc/%d/stat", (int)pid);
	f = fopen(path, "r");
	if (!f)
		return 0;
	n = fread(buf, 1, sizeof(buf) - 1, f);
	fclose(f);
	if (n == 0)
		return 0;
	buf[n] = '\0';

	p = strrchr(buf, ')');
	if (!p)
		return 0;
	p++; /* now between field 2 (comm) and field 3 (state) */
	while (*p == ' ')
		p++;
	/* field 3 is the single-letter state; skip it and its separator */
	while (*p && *p != ' ')
		p++;
	while (*p == ' ')
		p++;
	if (!*p)
		return 0;
	return (pid_t)strtol(p, NULL, 10);
}

/* ---------- attached-PID tracking ---------- */

static int pidset_contains(const struct pid_set *s, pid_t p)
{
	for (size_t i = 0; i < s->n; i++)
		if (s->items[i].pid == p)
			return 1;
	return 0;
}

/*
 * Record @p as attached. @orig/@set carry the exact-restore bookkeeping;
 * pass NULL/0 for both when there is nothing to put back (a PID we
 * reclaimed rather than bound ourselves).
 */
static int pidset_add_full(struct pid_set *s, pid_t p,
			   enum classification cls, unsigned int groups,
			   uid_t owner_uid, const uint8_t *orig,
			   size_t orig_len, const uint8_t *set, size_t set_len,
			   unsigned long long start_time)
{
	struct attached_entry *it;

	if (s->n == s->cap) {
		size_t ncap = s->cap ? s->cap * 2 : 64;
		struct attached_entry *np =
			realloc(s->items, ncap * sizeof(*np));
		if (!np)
			return -1;
		s->items = np;
		s->cap = ncap;
	}
	it = &s->items[s->n];
	memset(it, 0, sizeof(*it));
	it->pid = p;
	it->cls = cls;
	it->groups = groups;
	it->owner_uid = owner_uid;
	it->start_time = start_time;
	if (orig && orig_len) {
		if (orig_len > sizeof(it->orig_mask))
			orig_len = sizeof(it->orig_mask);
		memcpy(it->orig_mask, orig, orig_len);
		it->orig_mlen = orig_len;
	}
	if (set && set_len) {
		if (set_len > sizeof(it->set_mask))
			set_len = sizeof(it->set_mask);
		memcpy(it->set_mask, set, set_len);
		it->set_mlen = set_len;
	}
	s->n++;
	return 0;
}

static int pidset_add(struct pid_set *s, pid_t p, enum classification cls,
		      unsigned int groups, uid_t owner_uid)
{
	return pidset_add_full(s, p, cls, groups, owner_uid, NULL, 0, NULL, 0,
			       0);
}

static void pidset_prune_dead(struct pid_set *s)
{
	char path[64];
	struct stat st;
	size_t w = 0;
	for (size_t r = 0; r < s->n; r++) {
		snprintf(path, sizeof(path), "/proc/%d", (int)s->items[r].pid);
		if (stat(path, &st) == 0) {
			if (w != r)
				s->items[w] = s->items[r];
			w++;
		}
	}
	s->n = w;
}

static void pidset_free(struct pid_set *s)
{
	free(s->items);
	s->items = NULL;
	s->n = s->cap = 0;
}

/* ---------- public API ---------- */

process_cpuset_t *process_cpuset_new(void)
{
	process_cpuset_t *ctx = calloc(1, sizeof(*ctx));
	if (!ctx)
		return NULL;

	/*
     * There is no built-in class -> core-group mapping. calloc() left
     * every slot unset (groups 0, empty cpulist), and an unset class
     * means "lpmd does not touch this task": no affinity is written, no
     * uclamp is applied, a slice keeps whatever AllowedCPUs= it had. The
     * only way to give a class CPUs is the <ClassDefaults> overlay in
     * the matching intel_lpmd_config_*.xml, applied on top by
     * process_cpuset_override_class_defaults(), because only a config
     * that knows the part can say what a class should run on. Neither
     * process_cpuset.xml nor process_cpuset_user.xml may set it: the
     * mapping is not process-list data, and both the process path and
     * the slice path need the same answer for the same classification.
     *
     * A table used to live here. It decided placement on every model
     * without a <ClassDefaults> block -- background silently confined to
     * the LP-E cores, and resolving to nothing at all on parts that have
     * none -- which is a policy no per-model config had asked for.
     */
	for (size_t i = 0;
	     i < sizeof(ctx->class_uclamp_min) / sizeof(ctx->class_uclamp_min[0]);
	     i++) {
		ctx->class_uclamp_min[i] = UCLAMP_UNSET;
		ctx->class_uclamp_max[i] = UCLAMP_UNSET;
	}
	return ctx;
}

void process_cpuset_free(process_cpuset_t *ctx)
{
	if (!ctx)
		return;
	pidset_free(&ctx->attached);
	free(ctx);
}

int process_cpuset_entry_count(const process_cpuset_t *ctx)
{
	return ctx ? ctx->n_entries : 0;
}

size_t process_cpuset_attached_count(const process_cpuset_t *ctx)
{
	return ctx ? ctx->attached.n : 0;
}

int process_cpuset_is_attached(const process_cpuset_t *ctx, pid_t pid)
{
	if (!ctx)
		return 0;
	return pidset_contains(&ctx->attached, pid);
}

int process_cpuset_attached_get(const process_cpuset_t *ctx, size_t i,
				pid_t *pid_out)
{
	if (!ctx || i >= ctx->attached.n)
		return -1;
	if (pid_out)
		*pid_out = ctx->attached.items[i].pid;
	return 0;
}

int process_cpuset_attached_get_ex(const process_cpuset_t *ctx, size_t i,
				   pid_t *pid_out, const char **class_out,
				   int *use_pcores, int *use_ecores,
				   int *use_lcores)
{
	const struct attached_entry *e;

	if (!ctx || i >= ctx->attached.n)
		return -1;
	e = &ctx->attached.items[i];
	if (pid_out)
		*pid_out = e->pid;
	if (class_out)
		*class_out = class_str(e->cls);
	if (use_pcores)
		*use_pcores = !!(e->groups & GROUP_PCORES);
	if (use_ecores)
		*use_ecores = !!(e->groups & GROUP_ECORES);
	if (use_lcores)
		*use_lcores = !!(e->groups & GROUP_LCORES);
	return 0;
}

int process_cpuset_attached_from_intent(const process_cpuset_t *ctx, pid_t pid)
{
	if (!ctx)
		return 0;
	for (size_t i = 0; i < ctx->attached.n; i++) {
		if (ctx->attached.items[i].pid == pid)
			return ctx->attached.items[i].from_intent;
	}
	return 0;
}

int process_cpuset_groups_get(const process_cpuset_t *ctx, char *p_out,
			      size_t p_cap, char *e_out, size_t e_cap,
			      char *l_out, size_t l_cap)
{
	if (!ctx)
		return -1;
	if (p_out && p_cap)
		snprintf(p_out, p_cap, "%s", ctx->groups.p_cores);
	if (e_out && e_cap)
		snprintf(e_out, e_cap, "%s", ctx->groups.e_cores);
	if (l_out && l_cap)
		snprintf(l_out, l_cap, "%s", ctx->groups.l_cores);
	return 0;
}

int process_cpuset_set_groups(process_cpuset_t *ctx, const char *p_cores,
			      const char *e_cores, const char *l_cores)
{
	if (!ctx)
		return -1;
	if (p_cores)
		copy_text(ctx->groups.p_cores, sizeof(ctx->groups.p_cores),
			  p_cores);
	if (e_cores)
		copy_text(ctx->groups.e_cores, sizeof(ctx->groups.e_cores),
			  e_cores);
	if (l_cores)
		copy_text(ctx->groups.l_cores, sizeof(ctx->groups.l_cores),
			  l_cores);
	return 0;
}

/*
 * Convert a cpu_set_t into a compact cpuset-style string ("0-3,5,8-11").
 * Internally stored that way so apply_once()'s existing cpulist_to_mask()
 * code path keeps working unchanged.
 */
static int cpuset_to_str(const cpu_set_t *set, size_t setsize, char *out,
			 size_t cap)
{
	int n_cpus = (int)(setsize * 8);
	int written = 0;
	int run_start = -1;
	int i;

	if (!set || cap == 0)
		return -1;
	out[0] = '\0';
	if (n_cpus <= 0 || n_cpus > MAX_CPUS)
		n_cpus = MAX_CPUS;

	for (i = 0; i <= n_cpus; i++) {
		int present = (i < n_cpus) && CPU_ISSET_S(i, setsize, set);

		if (present && run_start < 0) {
			run_start = i;
		} else if (!present && run_start >= 0) {
			int run_end = i - 1;
			int n;

			if (run_end == run_start)
				n = snprintf(out + written, cap - written,
					     "%s%d", written ? "," : "",
					     run_start);
			else
				n = snprintf(out + written, cap - written,
					     "%s%d-%d", written ? "," : "",
					     run_start, run_end);
			if (n < 0 || (size_t)n >= cap - written)
				return -1;
			written += n;
			run_start = -1;
		}
	}
	return 0;
}

int process_cpuset_set_groups_cpuset(process_cpuset_t *ctx,
				     const cpu_set_t *p_cores,
				     const cpu_set_t *e_cores,
				     const cpu_set_t *l_cores, size_t setsize)
{
	if (!ctx)
		return -1;
	if (setsize == 0)
		setsize = sizeof(cpu_set_t);

	if (p_cores && cpuset_to_str(p_cores, setsize, ctx->groups.p_cores,
				     sizeof(ctx->groups.p_cores)) < 0)
		return -1;
	if (e_cores && cpuset_to_str(e_cores, setsize, ctx->groups.e_cores,
				     sizeof(ctx->groups.e_cores)) < 0)
		return -1;
	if (l_cores && cpuset_to_str(l_cores, setsize, ctx->groups.l_cores,
				     sizeof(ctx->groups.l_cores)) < 0)
		return -1;
	return 0;
}

int process_cpuset_override_class_defaults(
	process_cpuset_t *ctx, const char *realtime,
	const char *user_interactive, const char *user_initiated,
	const char *unclassified,
	const char *utility, const char *background,
	const char *game_profile_cpu, const char *game_profile_gpu,
	const char *game_profile_hybrid,
	const char *custom_profile_0, const char *custom_profile_1,
	const char *custom_profile_2)
{
	const struct {
		enum classification cls;
		const char *str;
	} overrides[] = {
		{ CLASS_REALTIME, realtime },
		{ CLASS_USER_INTERACTIVE, user_interactive },
		{ CLASS_USER_INITIATED, user_initiated },
		{ CLASS_UNCLASSIFIED, unclassified },
		{ CLASS_UTILITY, utility },
		{ CLASS_BACKGROUND, background },
		{ CLASS_GAME_PROFILE_CPU, game_profile_cpu },
		{ CLASS_GAME_PROFILE_GPU, game_profile_gpu },
		{ CLASS_GAME_PROFILE_HYBRID, game_profile_hybrid },
		{ CLASS_CUSTOM_PROFILE_0, custom_profile_0 },
		{ CLASS_CUSTOM_PROFILE_1, custom_profile_1 },
		{ CLASS_CUSTOM_PROFILE_2, custom_profile_2 },
	};
	int updated = 0;

	if (!ctx)
		return -1;

	for (size_t i = 0; i < sizeof(overrides) / sizeof(overrides[0]); i++) {
		if (!overrides[i].str || !overrides[i].str[0])
			continue;
		parse_core_spec(overrides[i].str,
				&ctx->class_defaults[overrides[i].cls]);
		updated++;
	}

	if (!updated)
		return 0;

	/* Re-resolve every loaded entry so previously class-default-based
     * masks reflect the new defaults. Entries with an explicit
     * <ActiveCores> tag are left alone. */
	for (int i = 0; i < ctx->n_entries; i++) {
		struct proc_entry *e = &ctx->entries[i];
		if (core_spec_is_set(&e->explicit_spec))
			continue;
		e->resolved = *default_spec_for(ctx, e->cls);
	}
	return 0;
}

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
	int custom_profile_2_min, int custom_profile_2_max)
{
	const struct {
		enum classification cls;
		int min_v;
		int max_v;
	} overrides[] = {
		{ CLASS_REALTIME, realtime_min, realtime_max },
		{ CLASS_USER_INTERACTIVE, user_interactive_min,
		  user_interactive_max },
		{ CLASS_USER_INITIATED, user_initiated_min, user_initiated_max },
		{ CLASS_UNCLASSIFIED, unclassified_min, unclassified_max },
		{ CLASS_UTILITY, utility_min, utility_max },
		{ CLASS_BACKGROUND, background_min, background_max },
		{ CLASS_GAME_PROFILE_CPU, game_profile_cpu_min,
		  game_profile_cpu_max },
		{ CLASS_GAME_PROFILE_GPU, game_profile_gpu_min,
		  game_profile_gpu_max },
		{ CLASS_GAME_PROFILE_HYBRID, game_profile_hybrid_min,
		  game_profile_hybrid_max },
		{ CLASS_CUSTOM_PROFILE_0, custom_profile_0_min,
		  custom_profile_0_max },
		{ CLASS_CUSTOM_PROFILE_1, custom_profile_1_min,
		  custom_profile_1_max },
		{ CLASS_CUSTOM_PROFILE_2, custom_profile_2_min,
		  custom_profile_2_max },
	};
	int touched = 0;

	if (!ctx)
		return -1;

	for (size_t i = 0; i < sizeof(overrides) / sizeof(overrides[0]); i++) {
		int min_v = overrides[i].min_v;
		int max_v = overrides[i].max_v;

		if (min_v != LPMD_UCLAMP_INHERIT) {
			if (min_v < UCLAMP_UNSET || min_v > UCLAMP_CLAMP_MAX)
				return -1;
			ctx->class_uclamp_min[overrides[i].cls] = min_v;
			touched = 1;
		}

		if (max_v != LPMD_UCLAMP_INHERIT) {
			if (max_v < UCLAMP_UNSET || max_v > UCLAMP_CLAMP_MAX)
				return -1;
			ctx->class_uclamp_max[overrides[i].cls] = max_v;
			touched = 1;
		}

		if (min_v != LPMD_UCLAMP_INHERIT &&
		    max_v != LPMD_UCLAMP_INHERIT &&
		    min_v != UCLAMP_UNSET && max_v != UCLAMP_UNSET &&
		    min_v > max_v)
			return -1;
	}

	if (!touched)
		return 0;

	/* Re-apply on all classes to pick up changed clamps for attached PIDs. */
	for (size_t i = 0; i < sizeof(overrides) / sizeof(overrides[0]); i++)
		(void)reapply_attached_class(ctx, overrides[i].cls);

	return 0;
}

/*
 * <ClassDefaults> is no longer honored in a policy file; see the comment in
 * process_cpuset_new(). Say so rather than ignore it silently: a file left
 * over from an earlier build would otherwise look like it still had an
 * effect. Both loaders report it identically.
 *
 * The child tags (<Cores>, <UClampMin>, <UClampMax>) are spelled the same
 * in the config file, so a moved block is mostly valid there -- which is
 * why the three things that are not get named individually.
 */
static void warn_classdefaults_ignored(const char *path)
{
	fprintf(stderr,
		"warning: %s: <ClassDefaults> is ignored in this file; set it in the <States> stanza of intel_lpmd_config.xml instead.\n"
		"warning: %s: class names are matched case-sensitively there, snake_case aliases (game_profile_cpu) are not accepted, and there is no <Foreground> alias, so a moved block may need its class names edited.\n",
		path, path);
}

int process_cpuset_load_config(process_cpuset_t *ctx, const char *path)
{
	xmlDoc *doc;
	xmlNode *root, *cur;
	int n = 0;

	if (!ctx || !path)
		return -1;

	doc = xmlReadFile(path, NULL, 0);
	if (!doc) {
		lpmd_log_debug( "Failed to parse XML: %s\n", path);
		return -1;
	}
	root = xmlDocGetRootElement(doc);
	if (!root) {
		xmlFreeDoc(doc);
		return -1;
	}

	/* Reset entries & groups being replaced. The attached-PID set is left
     * intact: a config reload shouldn't make us re-attach already-handled
     * PIDs. */
	ctx->n_entries = 0;
	memset(&ctx->groups, 0, sizeof(ctx->groups));
	memset(&ctx->default_entry, 0, sizeof(ctx->default_entry));
	ctx->default_entry.cls = CLASS_INVALID;
	ctx->has_default_entry = 0;

	/* First pass: pick up <CpuGroups> and <DefaultProcess> regardless of
     * where they appear. */
	for (cur = root->children; cur; cur = cur->next) {
		if (cur->type != XML_ELEMENT_NODE)
			continue;
		if (!strcmp((const char *)cur->name, "CpuGroups"))
			parse_cpu_groups(doc, cur->children, &ctx->groups);
		else if (!strcmp((const char *)cur->name, "ClassDefaults"))
			warn_classdefaults_ignored(path);
		else if (!strcmp((const char *)cur->name, "DefaultProcess")) {
			parse_one_process(ctx, doc, cur->children,
					  &ctx->default_entry);
			if (ctx->default_entry.cls != CLASS_INVALID) {
				snprintf(ctx->default_entry.name,
					 sizeof(ctx->default_entry.name),
					 "*default*");
				ctx->has_default_entry = 1;
			} else {
				lpmd_log_debug(
					"Ignoring <DefaultProcess>: missing/invalid <Classification>\n");
			}
		}
	}
	if (!ctx->groups.p_cores[0] && !ctx->groups.e_cores[0] &&
	    !ctx->groups.l_cores[0])
		lpmd_log_debug(
			"warning: no <CpuGroups> defined; per-process tags must supply CPUs\n");

	for (cur = root->children; cur && n < MAX_PROCS; cur = cur->next) {
		if (cur->type != XML_ELEMENT_NODE)
			continue;
		if (strcmp((const char *)cur->name, "Process"))
			continue;
		parse_one_process(ctx, doc, cur->children, &ctx->entries[n]);
		/* Any one of <Name>, <Unit> or <Cgroup> identifies an entry. */
		if ((ctx->entries[n].name[0] || ctx->entries[n].n_units ||
		     ctx->entries[n].n_cgroups) &&
		    ctx->entries[n].cls != CLASS_INVALID)
			n++;
		else
			lpmd_log_debug( "Skipping invalid <Process> entry\n");
	}

	ctx->n_entries = n;
	xmlFreeDoc(doc);
	return n;
}

/*
 * Find an existing <Process> entry by comm name, or -1 if none.
 */
static int find_entry_index_by_name(const process_cpuset_t *ctx, const char *name)
{
	if (!ctx || !name || !*name)
		return -1;
	for (int i = 0; i < ctx->n_entries; i++)
		if (!strcmp(ctx->entries[i].name, name))
			return i;
	return -1;
}

/*
 * Find the existing entry that @e overrides, or -1 if it is a new one.
 *
 * <Name> remains the identity when the entry has one, so overlays keep
 * overriding the entries they always did. An entry without a name keys on its
 * first <Unit> pattern, or failing that its first <Cgroup>: that is what makes
 * an overlay able to replace a unit- or cgroup-keyed entry from the system file
 * instead of silently adding a second entry matching the same processes.
 *
 * Unit before cgroup here for the same reason entry_label() prefers it: an
 * entry carrying both is understood to be keyed on the unit.
 */
static int find_entry_index(const process_cpuset_t *ctx,
			    const struct proc_entry *e)
{
	if (!ctx || !e)
		return -1;

	if (e->name[0])
		return find_entry_index_by_name(ctx, e->name);

	if (e->n_units) {
		for (int i = 0; i < ctx->n_entries; i++) {
			const struct proc_entry *cand = &ctx->entries[i];

			if (cand->name[0] || !cand->n_units)
				continue;
			if (!strcmp(cand->units[0], e->units[0]))
				return i;
		}
		return -1;
	}

	if (!e->n_cgroups)
		return -1;

	for (int i = 0; i < ctx->n_entries; i++) {
		const struct proc_entry *cand = &ctx->entries[i];

		if (cand->name[0] || cand->n_units || !cand->n_cgroups)
			continue;
		if (!strcmp(cand->cgroups[0], e->cgroups[0]))
			return i;
	}
	return -1;
}

/*
 * Additive load: parse @path and merge its <Process> entries on top of
 * the current ctx state. Entries whose <Name> already exists in the ctx
 * are replaced (so the user file overrides the system file). <CpuGroups>,
 * if present in the overlay, also overrides. <ClassDefaults> does not:
 * see the comment in process_cpuset_new().
 *
 * Unlike process_cpuset_load_config(), the existing entries array is
 * NOT cleared; this is intended for layering a user-editable XML on
 * top of the system one.
 *
 * Returns the number of entries added or replaced, 0 if @path is
 * missing/empty (treated as success), or -1 on hard parse error.
 */
int process_cpuset_load_config_overlay(process_cpuset_t *ctx, const char *path)
{
	xmlDoc *doc;
	xmlNode *root, *cur;
	int touched = 0;

	if (!ctx || !path)
		return -1;

	doc = xmlReadFile(path, NULL, 0);
	if (!doc) {
		/* Missing/unreadable overlay is not an error; the caller may
         * decide whether to log it. */
		return 0;
	}
	root = xmlDocGetRootElement(doc);
	if (!root) {
		xmlFreeDoc(doc);
		return 0;
	}

	/* Optional CpuGroups / DefaultProcess overrides. */
	for (cur = root->children; cur; cur = cur->next) {
		if (cur->type != XML_ELEMENT_NODE)
			continue;
		if (!strcmp((const char *)cur->name, "CpuGroups"))
			parse_cpu_groups(doc, cur->children, &ctx->groups);
		else if (!strcmp((const char *)cur->name, "ClassDefaults"))
			warn_classdefaults_ignored(path);
		else if (!strcmp((const char *)cur->name, "DefaultProcess")) {
			struct proc_entry tmp;
			parse_one_process(ctx, doc, cur->children, &tmp);
			if (tmp.cls != CLASS_INVALID) {
				snprintf(tmp.name, sizeof(tmp.name),
					 "*default*");
				ctx->default_entry = tmp;
				ctx->has_default_entry = 1;
			}
		}
	}

	for (cur = root->children; cur; cur = cur->next) {
		struct proc_entry tmp;
		int idx;

		if (cur->type != XML_ELEMENT_NODE)
			continue;
		if (strcmp((const char *)cur->name, "Process"))
			continue;

		parse_one_process(ctx, doc, cur->children, &tmp);
		if ((!tmp.name[0] && !tmp.n_units && !tmp.n_cgroups) ||
		    tmp.cls == CLASS_INVALID) {
			lpmd_log_debug(
				"Skipping invalid <Process> entry in overlay\n");
			continue;
		}

		idx = find_entry_index(ctx, &tmp);
		if (idx >= 0) {
			ctx->entries[idx] = tmp;
			touched++;
			continue;
		}
		if (ctx->n_entries >= MAX_PROCS) {
			lpmd_log_debug(
				"Overlay: max entries (%d) reached, dropping '%s'\n",
				MAX_PROCS, tmp.name);
			continue;
		}
		ctx->entries[ctx->n_entries++] = tmp;
		touched++;
	}

	xmlFreeDoc(doc);
	return touched;
}

/*
 * Add (or replace) a single <Process> entry at runtime.
 *
 * @name           : matched against /proc/<pid>/comm (15-char limit).
 * @classification : "background" / "foreground" / "realtime" /
 *                   "game_profile_cpu" / "game_profile_gpu" /
 *                   "game_profile_mixed" (case-insensitive).
 *
 * Resolves the CPU mask from the current <ClassDefaults>; literal
 * <ActiveCores> / <AllowSession> / <AffinityAllThreads> are not
 * exposed via this single-shot path -- callers needing them should
 * persist a richer XML overlay and reload it.
 *
 * Returns 1 if the entry was newly added, 0 if it replaced an
 * existing one with the same name, -1 on error.
 */
int process_cpuset_add_entry_ex(process_cpuset_t *ctx, const char *name,
				const char *classification,
				int allow_session)
{
	struct proc_entry e;
	enum classification cls;
	int idx;

	if (!ctx || !name || !*name || !classification)
		return -1;

	cls = parse_class(classification);
	if (cls == CLASS_INVALID)
		return -1;

	memset(&e, 0, sizeof(e));
	snprintf(e.name, sizeof(e.name), "%s", name);
	e.cls = cls;
	e.allow_session = allow_session ? 1 : 0;
	/* Same default as an XML entry with no <AffinityAllThreads> tag: cover
	 * every thread. memset() alone would silently mean the opposite, so an
	 * entry added at runtime would constrain only the group leader while an
	 * identical entry in the file constrained the whole process. */
	e.affinity_all_threads = 1;
	e.resolved = *default_spec_for(ctx, cls);

	idx = find_entry_index_by_name(ctx, e.name);
	if (idx >= 0) {
		ctx->entries[idx] = e;
		return 0;
	}
	if (ctx->n_entries >= MAX_PROCS)
		return -1;
	ctx->entries[ctx->n_entries++] = e;
	return 1;
}

int process_cpuset_add_entry(process_cpuset_t *ctx, const char *name,
			     const char *classification)
{
	return process_cpuset_add_entry_ex(ctx, name, classification, 0);
}

/* ---------- focus-driven user_interactive promotion ---------- */

/* Locate the attached_entry for @pid, NULL if not tracked. */
static struct attached_entry *attached_find_mut(process_cpuset_t *ctx,
						pid_t pid)
{
	if (!ctx)
		return NULL;
	for (size_t i = 0; i < ctx->attached.n; i++)
		if (ctx->attached.items[i].pid == pid)
			return &ctx->attached.items[i];
	return NULL;
}

/*
 * Read the PID's current CPU affinity mask into the AllowedCPUs
 * little-endian byte layout used by all the other paths. Returns the
 * number of significant bytes (1..CPUMASK_BYTES), or 0 on error /
 * dead PID. Used to remember the pre-focus mask so demotion can
 * restore it exactly.
 */
static size_t snapshot_pid_mask(pid_t pid, uint8_t *out, size_t out_cap)
{
	cpu_set_t *set;
	size_t setsize = CPU_ALLOC_SIZE(MAX_CPUS);
	size_t bytes = setsize;
	size_t i;

	if (bytes > out_cap)
		bytes = out_cap;
	set = CPU_ALLOC(MAX_CPUS);
	if (!set)
		return 0;
	if (sched_getaffinity(pid, setsize, set) != 0) {
		CPU_FREE(set);
		return 0;
	}
	memcpy(out, set, bytes);
	CPU_FREE(set);

	/* Trim trailing zero bytes. */
	while (bytes > 1 && out[bytes - 1] == 0)
		bytes--;
	return bytes;
}

/* Apply @mask to @pid, optionally walking /proc/<pid>/task/.
 * Returns 0 on success, -1 if even the leader could not be set. */
static int affinity_apply(pid_t pid, int all_threads, const uint8_t *mask,
			  size_t mask_len, const char *cls_label)
{
	char dbg[MAX_CPULIST] = { 0 };
	int rc;
	const char *cls = cls_label ? cls_label : "?";

	mask_to_cpulist(mask, mask_len, dbg, sizeof(dbg));
	lpmd_log_debug(
		"affinity_apply: pid=%d class=%s all_threads=%d cpus=[%s] mlen=%zu\n",
		(int)pid, cls, all_threads, dbg[0] ? dbg : "<empty>", mask_len);

	if (all_threads) {
		int n = set_pid_affinity_all_threads(pid, mask, mask_len,
						     cls_label);
		if (n <= 0) {
			lpmd_log_debug(
				"affinity_apply: pid=%d all-threads FAILED (n=%d, %s)\n",
				(int)pid, n,
				n == 0 ? "PID gone or empty task dir" :
					 strerror(errno));
			return -1;
		}
		lpmd_log_debug(
			"affinity_apply: pid=%d set affinity on %d thread(s)\n",
			(int)pid, n);
		return 0;
	}
	rc = set_pid_affinity_from_mask(pid, mask, mask_len);
	if (rc < 0) {
		lpmd_log_debug(
			"affinity_apply: pid=%d leader-only sched_setaffinity failed: %s\n",
			(int)pid, strerror(errno));
	}
	return rc;
}

/* ---------- cgroup ceiling / hands-off detection ---------------------
 *
 * lpmd never moves a task between cgroups. Whatever cgroup a task is in
 * is where it stays, which means the CPUs its cgroup allows are a hard
 * ceiling on anything we can usefully ask for: sched_setaffinity's
 * effective set is (requested AND cpuset), so a request reaching outside
 * the cgroup is silently trimmed by the kernel anyway. We do the
 * intersection ourselves so the logs say what actually happened.
 * -------------------------------------------------------------------- */

static void mask_and(uint8_t *dst, const uint8_t *a, const uint8_t *b, size_t n)
{
	size_t i;

	for (i = 0; i < n; i++)
		dst[i] = a[i] & b[i];
}

static int mask_is_empty(const uint8_t *m, size_t n)
{
	size_t i;

	for (i = 0; i < n; i++)
		if (m[i])
			return 0;
	return 1;
}

/* 1 if every bit of @a is in @b and @a != @b (i.e. @a is strictly narrower). */
static int mask_is_strict_subset(const uint8_t *a, const uint8_t *b, size_t n)
{
	int differs = 0;
	size_t i;

	for (i = 0; i < n; i++) {
		if (a[i] & ~b[i])
			return 0; /* a has a bit b lacks: not a subset */
		if (a[i] != b[i])
			differs = 1;
	}
	return differs;
}

/*
 * Effective CPU set of cgroup @cgpath. cpuset.cpus.effective only exists
 * where the cpuset controller is enabled, so walk towards the root until
 * a readable one is found; the root always has it. Returns 0 on success
 * (@mask filled), -1 if no readable cpuset was found anywhere up the tree.
 */
static int cgroup_effective_cpus(const char *cgpath, uint8_t *mask, size_t cap)
{
	char work[512];
	char path[600];
	char buf[MAX_CPULIST];

	snprintf(work, sizeof(work), "%s", cgpath);
	for (;;) {
		char *slash;
		FILE *f;

		snprintf(path, sizeof(path),
			 "/sys/fs/cgroup%s/cpuset.cpus.effective",
			 strcmp(work, "/") ? work : "");
		f = fopen(path, "r");
		if (f) {
			char *got = fgets(buf, sizeof(buf), f);
			fclose(f);
			if (got) {
				size_t l = strlen(buf);

				if (l && buf[l - 1] == '\n')
					buf[--l] = '\0';
				/* cpulist_to_mask returns 0 on success. An
				 * empty file is not an answer -- keep walking. */
				if (l && cpulist_to_mask(buf, mask, cap) == 0 &&
				    !mask_is_empty(mask, cap))
					return 0;
			}
		}

		slash = strrchr(work, '/');
		if (!slash)
			return -1;
		if (slash == work) {
			if (!strcmp(work, "/"))
				return -1; /* root had nothing readable */
			work[1] = '\0';
		} else {
			*slash = '\0';
		}
	}
}

/*
 * Decide whether @pid has an affinity mask of its own that lpmd must not
 * disturb.
 *
 * A task that has never called sched_setaffinity(2) reports exactly its
 * cgroup's effective CPU set. A mask strictly narrower than that set has
 * two possible origins: the task itself (or something acting on its
 * behalf, e.g. taskset), or a fork(2) from a parent that already carried
 * a narrower mask -- masks are inherited by children and preserved across
 * execve(2). Only the first is a deliberate placement to be left alone;
 * see mask_inherited_from_lpmd() for how the caller tells the two apart.
 *
 * Fills @cur with the current mask (@cur must be CPUMASK_BYTES wide) and
 * returns 1 for "narrower than the cgroup allows", 0 for "exactly the
 * cgroup's set, so untouched by anyone", -1 if the mask could not be read.
 */
static int pid_has_own_affinity(pid_t pid, const uint8_t *ceiling,
			       uint8_t *cur, size_t *cur_len)
{
	size_t len;

	memset(cur, 0, CPUMASK_BYTES);
	len = snapshot_pid_mask(pid, cur, CPUMASK_BYTES);
	if (!len)
		return -1;
	if (cur_len)
		*cur_len = len;
	return mask_is_strict_subset(cur, ceiling, CPUMASK_BYTES);
}

/* How far up the parent chain to look for the task we inherited from.
 * A shell pipeline or a service's helper script is a handful of levels at
 * most; the bound is here so a /proc race that reports a cycle cannot
 * spin us forever. */
#define INHERIT_MAX_DEPTH 16

/*
 * Decide whether @pid's mask @cur was inherited across fork(2) from a task
 * lpmd itself bound, rather than set by the task or an admin.
 *
 * fork(2) copies the parent's affinity mask and execve(2) preserves it, so
 * a child of a task lpmd narrowed comes up already wearing lpmd's mask
 * while never having called sched_setaffinity(2) itself. To the "is this
 * mask narrower than the cgroup allows?" test that is indistinguishable
 * from a deliberate self-placement -- which would mean lpmd refuses to
 * apply the child's own <Process> policy and, because it never records the
 * child, never restores it either. The narrowing would then outlive the
 * daemon.
 *
 * So walk up the parent chain and look for a tracked ancestor whose mask
 * lpmd wrote and which @cur matches exactly. Exact match matters: if the
 * child has since narrowed further, that part IS its own decision and the
 * hands-off rule still applies.
 *
 * Returns the ancestor's entry (whose orig_mask is the mask the child
 * would have had if lpmd had never run, and so the right thing to restore
 * on release), or NULL if the mask did not come from us.
 *
 * Residual ambiguity, accepted deliberately: a task that deliberately
 * sets itself to exactly the mask lpmd gave its parent is read as having
 * inherited it. The alternative is to leave inherited masks in place
 * permanently, which is the worse failure.
 */
static struct attached_entry *
mask_inherited_from_lpmd(process_cpuset_t *ctx, pid_t pid, const uint8_t *cur)
{
	pid_t walk = pid;

	if (!ctx || !cur)
		return NULL;

	for (int depth = 0; depth < INHERIT_MAX_DEPTH; depth++) {
		struct attached_entry *anc;

		walk = pid_ppid(walk);
		if (walk <= 1)
			return NULL; /* reached init / unreadable */

		anc = attached_find_mut(ctx, walk);
		if (!anc || !anc->set_mlen)
			continue; /* not ours, but its parent still might be */
		if (memcmp(cur, anc->set_mask, CPUMASK_BYTES) == 0)
			return anc;
	}
	return NULL;
}

/*
 * Bind @pid to @want using sched_setaffinity(2) only: no transient scope,
 * no cgroup migration, no change to any unit's AllowedCPUs.
 *
 * Precedence and opt-outs, in order:
 *   1. If slice.xml already covers @pid's cgroup with an enforceable
 *      entry, that policy wins and we do nothing -- the cpuset is being
 *      set on the unit itself, which is both cheaper and inherited by
 *      the unit's future children.
 *   2. If @pid carries an affinity mask of its own, leave it alone. A mask
 *      inherited across fork(2) from a task lpmd bound does not count as
 *      the task's own; see mask_inherited_from_lpmd().
 *   3. Otherwise apply (@want AND <the CPUs @pid's cgroup allows>).
 *
 * Returns 1 if applied, 0 if deliberately skipped, -1 on error.
 */
static int affinity_bind_pid(process_cpuset_t *ctx, const struct proc_entry *e,
			     pid_t pid, const char *comm, const uint8_t *want,
			     size_t want_len, uid_t owner_uid, int dry_run)
{
	uint8_t ceiling[CPUMASK_BYTES];
	uint8_t cur[CPUMASK_BYTES];
	uint8_t send[CPUMASK_BYTES];
	char cgpath[512];
	char slice_unit[256];
	char slice_cls[64];
	char dbg_want[MAX_CPULIST];
	char dbg_send[MAX_CPULIST];
	char dbg_ceil[MAX_CPULIST];
	struct attached_entry *inherited = NULL;
	size_t send_len, cur_len = 0;
	int enforceable = 0;
	int have_ceiling = 0;
	int clamped = 0;
	int own;

	if (!ctx || !e || pid <= 0)
		return -1;

	/* 1. slice.xml has precedence. */
	if (lpmd_slice_cpuset_pid_coverage(pid, slice_unit, sizeof(slice_unit),
					   slice_cls, sizeof(slice_cls),
					   &enforceable, NULL) == 1 &&
	    enforceable) {
		lpmd_log_debug(
			"[%s] skip pid %d comm=%s: covered by slice.xml unit=%s class=%s\n",
			entry_label(e), (int)pid, comm, slice_unit, slice_cls);
		return 0;
	}

	memset(ceiling, 0, sizeof(ceiling));
	mask_to_cpulist(want, want_len, dbg_want, sizeof(dbg_want));

	/* 2. Establish the ceiling from the cgroup the task already lives in. */
	if (pid_cgroup_path(pid, cgpath, sizeof(cgpath)) == 0 &&
	    cgroup_effective_cpus(cgpath, ceiling, sizeof(ceiling)) == 0) {
		have_ceiling = 1;
		mask_and(send, want, ceiling, sizeof(send));
		clamped = memcmp(send, want, sizeof(send)) != 0;
		mask_to_cpulist(ceiling, sizeof(ceiling), dbg_ceil,
				sizeof(dbg_ceil));
	} else {
		/* No readable cpuset anywhere up the tree: nothing constrains
		 * the task, so the request stands as-is. Without a ceiling we
		 * cannot tell an inherited mask from a self-set one, so the
		 * hands-off check below is skipped rather than guessed at. */
		memcpy(send, want, sizeof(send));
		snprintf(dbg_ceil, sizeof(dbg_ceil), "unknown");
	}

	if (mask_is_empty(send, sizeof(send))) {
		lpmd_log_debug(
			"[%s] skip pid %d comm=%s: want=[%s] has no CPU in common with cgroup ceiling=[%s]\n",
			entry_label(e), (int)pid, comm, dbg_want, dbg_ceil);
		return 0;
	}
	send_len = mask_significant_len(send, sizeof(send));
	mask_to_cpulist(send, send_len, dbg_send, sizeof(dbg_send));

	/* 3. Hands off anything that placed itself. Only decidable when we
     * know the cgroup's set: a mask equal to it was untouched by anyone,
     * a mask narrower than it came from sched_setaffinity -- either the
     * task's own call, or a fork(2) from a parent we already narrowed. */
	own = pid_has_own_affinity(pid, ceiling, cur, &cur_len);
	if (own < 0)
		return 0; /* gone */
	if (own && !have_ceiling)
		own = 0;
	if (own) {
		char dbg_cur[MAX_CPULIST];

		mask_to_cpulist(cur, cur_len, dbg_cur, sizeof(dbg_cur));
		inherited = mask_inherited_from_lpmd(ctx, pid, cur);
		if (!inherited) {
			lpmd_log_debug(
				"[%s] skip pid %d comm=%s: has own affinity=[%s] (narrower than cgroup ceiling=[%s]); not set, not restored\n",
				entry_label(e), (int)pid, comm, dbg_cur,
				dbg_ceil);
			return 0;
		}
		/* Inherited from a task we bound, so not the child's own
		 * choice. Apply the child's own policy, and remember the
		 * ancestor's pre-lpmd mask as what to put back on release --
		 * that is what this task would have had if lpmd had never
		 * run, and restoring the inherited mask instead would just
		 * leave a narrower leak behind. */
		lpmd_log_debug(
			"[%s] pid %d comm=%s inherited our mask [%s] from pid %d; applying its own policy\n",
			entry_label(e), (int)pid, comm, dbg_cur,
			(int)inherited->pid);
	}

	if (dry_run) {
		lpmd_log_debug(
			"[%s] (dry) would set affinity pid %d comm=%s cpus=[%s]%s (uid=%u%s)\n",
			entry_label(e), (int)pid, comm, dbg_send,
			clamped ? " (clamped to cgroup)" : "",
			(unsigned)owner_uid,
			e->affinity_all_threads ? " all-threads" : "");
		return 1;
	}

	if (affinity_apply(pid, e->affinity_all_threads, send, send_len,
			   class_str(e->cls)) < 0)
		return -1;

	lpmd_log_debug(
		"[%s] affinity pid %d comm=%s cpus=[%s]%s was=[%s] (uid=%u%s)\n",
		entry_label(e), (int)pid, comm, dbg_send,
		clamped ? " (clamped to cgroup)" : "", dbg_want,
		(unsigned)owner_uid,
		e->affinity_all_threads ? " all-threads" : "");

	/* What to put back on release. Normally the mask the task had when we
	 * found it; for an inherited mask, the ancestor's pre-lpmd mask. Copy
	 * it out of the attached array first: pidset_add_full() can realloc,
	 * which would invalidate @inherited. */
	if (inherited && inherited->orig_mlen) {
		memcpy(cur, inherited->orig_mask, CPUMASK_BYTES);
		cur_len = inherited->orig_mlen;
	}

	if (pidset_add_full(&ctx->attached, pid, e->cls, e->resolved.groups,
			    owner_uid, cur, cur_len, send, send_len,
			    pid_start_time(pid)) == 0) {
		struct attached_entry *it =
			&ctx->attached.items[ctx->attached.n - 1];

		it->all_threads = e->affinity_all_threads ? 1 : 0;
		it->explicit_cores = core_spec_is_set(&e->explicit_spec);
	}
	(void)apply_pid_uclamp(ctx, pid, e->cls, class_str(e->cls),
			       e->affinity_all_threads ? 1 : 0);
	return 1;
}

/*
 * Collect every descendant TGID of @root (children, grandchildren, ...)
 * by walking /proc/<pid>/task/<tid>/children. Skips @root itself and
 * dedupes. Returns the number of TGIDs written to @out.
 */
static size_t collect_descendant_tgids(pid_t root, pid_t *out, size_t max)
{
	pid_t queue[FOCUS_DESC_MAX + 1];
	size_t qhead = 0, qtail = 0;
	size_t n = 0;

	if (root <= 0 || !out || max == 0)
		return 0;

	queue[qtail++] = root;

	while (qhead < qtail) {
		pid_t cur = queue[qhead++];
		char taskdir[64];
		DIR *td;
		struct dirent *de;

		snprintf(taskdir, sizeof(taskdir), "/proc/%d/task", (int)cur);
		td = opendir(taskdir);
		if (!td)
			continue;
		while ((de = readdir(td)) != NULL) {
			char cpath[96];
			char cbuf[1024];
			FILE *cf;
			size_t got;
			char *p, *save;
			char *end;
			long tid;

			tid = strtol(de->d_name, &end, 10);
			if (*end != '\0' || tid <= 0)
				continue;
			snprintf(cpath, sizeof(cpath), "/proc/%d/task/%ld/children",
				 (int)cur, tid);
			cf = fopen(cpath, "r");
			if (!cf)
				continue;
			got = fread(cbuf, 1, sizeof(cbuf) - 1, cf);
			fclose(cf);
			if (got == 0)
				continue;
			cbuf[got] = '\0';

			for (p = strtok_r(cbuf, " \t\n", &save); p;
			     p = strtok_r(NULL, " \t\n", &save)) {
				long v = strtol(p, NULL, 10);
				pid_t child, tgid;
				size_t i;
				int seen;

				if (v <= 0 || v == root)
					continue;
				child = (pid_t)v;
				tgid = pid_to_tgid(child);
				if (tgid == cur || tgid == root)
					continue;
				child = tgid;

				seen = 0;
				for (i = 0; i < n; i++) {
					if (out[i] == child) {
						seen = 1;
						break;
					}
				}
				if (seen)
					continue;
				if (n >= max) {
					closedir(td);
					return n;
				}
				out[n++] = child;
				if (qtail < sizeof(queue) / sizeof(queue[0]))
					queue[qtail++] = child;
			}
		}
		closedir(td);
	}
	return n;
}

/*
 * Promote one descendant TGID @pid to @ui_mask and record state in
 * @desc so we can revert later. The TGID need not be in the attached
 * set; if it is not, sched_setaffinity all-threads is still applied.
 */
static int focus_promote_descendant(process_cpuset_t *ctx, pid_t pid,
				    const uint8_t *ui_mask, size_t ui_mlen,
				    struct focus_desc *desc)
{
	struct attached_entry *ent;
	char comm[MAX_NAME] = "";

	memset(desc, 0, sizeof(*desc));
	desc->pid = pid;
	desc->orig_cls = CLASS_INVALID;

	(void)read_comm(pid, comm, sizeof(comm));

	desc->orig_mlen = snapshot_pid_mask(pid, desc->orig_mask,
					    sizeof(desc->orig_mask));
	if (!desc->orig_mlen)
		return -1; /* gone */

	ent = attached_find_mut(ctx, pid);
	if (ent) {
		desc->had_entry = 1;
		desc->orig_cls = ent->cls;
		desc->orig_groups = ent->groups;
		ent->cls = CLASS_USER_INTERACTIVE;
		ent->groups =
			ctx->class_defaults[CLASS_USER_INTERACTIVE].groups;
	}

	{
		char ui_list[MAX_CPULIST] = { 0 };
		char old_list[MAX_CPULIST] = { 0 };
		mask_to_cpulist(ui_mask, ui_mlen, ui_list, sizeof(ui_list));
		mask_to_cpulist(desc->orig_mask, desc->orig_mlen, old_list,
				sizeof(old_list));
		lpmd_log_debug(
			"process_cpuset: focus: promote-desc pid=%d comm='%s' "
			"cpus [%s] -> [%s]\n",
			(int)pid, comm,
			old_list[0] ? old_list : "?",
			ui_list[0] ? ui_list : "?");
	}

	if (affinity_apply(pid, /*all_threads=*/1, ui_mask, ui_mlen,
			  class_str(CLASS_USER_INTERACTIVE)) < 0)
		return -1;
	(void)apply_pid_uclamp(ctx, pid, CLASS_USER_INTERACTIVE,
		       class_str(CLASS_USER_INTERACTIVE), 1);
	return 0;
}

/* Revert one descendant. Best-effort: PID may already be gone. */
static void focus_demote_descendant(process_cpuset_t *ctx,
				    struct focus_desc *desc)
{
	if (desc->had_entry) {
		struct attached_entry *ent = attached_find_mut(ctx, desc->pid);
		if (ent) {
			ent->cls = desc->orig_cls;
			ent->groups = desc->orig_groups;
		}
	}
	lpmd_log_debug("process_cpuset: focus: demote-desc pid=%d cls->%s\n",
		       (int)desc->pid, class_str(desc->orig_cls));
	(void)affinity_apply(desc->pid, /*all_threads=*/1, desc->orig_mask,
			     desc->orig_mlen, class_str(desc->orig_cls));
	if (desc->orig_cls != CLASS_INVALID)
		(void)apply_pid_uclamp(ctx, desc->pid, desc->orig_cls,
				       class_str(desc->orig_cls), 1);
}

/* Build the user_interactive AllowedCPUs mask from current
 * <ClassDefaults>. Returns the significant byte length. */
static size_t build_user_interactive_mask(const process_cpuset_t *ctx,
					  uint8_t *out, size_t out_cap)
{
	struct proc_entry e;

	memset(&e, 0, sizeof(e));
	e.cls = CLASS_USER_INTERACTIVE;
	e.resolved = *default_spec_for(ctx, CLASS_USER_INTERACTIVE);
	if (build_mask_for(ctx, &e, out, out_cap) < 0) {
		memset(out, 0, out_cap);
		return 0;
	}
	return mask_significant_len(out, out_cap);
}

/*
 * Promote @pid to user_interactive (typically the focused window) or,
 * if @pid == 0, demote the previously promoted PID back to its
 * original mask.
 *
 * Promotion is sched_setaffinity(pid, <UI mask>) over every TID, on the
 * task where it already is -- the same mechanism used for every other
 * PID here, and for the same reason: promoting the focused window must
 * not relocate it out of its own service or session cgroup. If
 * <ClassDefaults> does not set user_interactive, the <UI mask> is the
 * task's own pre-lpmd mask: focus lifts its class's placement.
 *
 * A PID that is not in the attached set yet is a no-op; the
 * proc-connector / periodic rescan will attach it shortly with its
 * default class, and the helper's next focus event picks it up.
 *
 * On the next call (with a different @pid, or 0) the previous focus
 * PID is reverted to its remembered mask.
 *
 * Returns 0 on success, -1 on parameter error.
 */
int process_cpuset_set_focus_pid(process_cpuset_t *ctx, pid_t pid)
{
	uint8_t mask[CPUMASK_BYTES];
	size_t mlen;
	struct attached_entry *ent;
	char comm[MAX_NAME] = "";
	int ui_set;

	if (!ctx)
		return -1;

	/* Compositors may report a TID (e.g. the one that called
     * XSetProperty(_NET_WM_PID) inside Firefox). Resolve to TGID so
     * that promotion / all-threads cover the entire process. */
	if (pid > 0) {
		pid_t tgid = pid_to_tgid(pid);
		if (tgid != pid) {
			lpmd_log_debug(
				"process_cpuset: focus: resolved tid=%d -> tgid=%d\n",
				(int)pid, (int)tgid);
			pid = tgid;
		}
	}

	/* Same PID -> nothing to do. */
	if (pid == ctx->focus_pid)
		return 0;

	/* Step 1: revert previously focused PID and its tracked
     * descendants, if any. Descendants are reverted first (reverse
     * order) before the root so visible behaviour mirrors a stack. */
	if (ctx->focus_desc_n > 0) {
		size_t i;
		for (i = ctx->focus_desc_n; i-- > 0;)
			focus_demote_descendant(ctx, &ctx->focus_descs[i]);
		ctx->focus_desc_n = 0;
	}
	if (ctx->focus_pid > 0) {
		pid_t prev = ctx->focus_pid;
		struct attached_entry *prev_ent = attached_find_mut(ctx, prev);
		if (prev_ent) {
			prev_ent->cls = ctx->focus_orig_cls;
			prev_ent->groups = ctx->focus_orig_groups;
		}
		lpmd_log_debug("process_cpuset: focus: demote pid=%d cls->%s\n",
			       (int)prev, class_str(ctx->focus_orig_cls));
		/* Best effort: the PID may be gone. */
		(void)affinity_apply(prev, ctx->focus_all_threads,
				     ctx->focus_orig_mask,
				     ctx->focus_orig_mlen,
				     class_str(ctx->focus_orig_cls));
		if (ctx->focus_orig_cls != CLASS_INVALID)
			(void)apply_pid_uclamp(ctx, prev, ctx->focus_orig_cls,
					       class_str(ctx->focus_orig_cls),
					       ctx->focus_all_threads);
		ctx->focus_pid = 0;
		ctx->focus_all_threads = 0;
		ctx->focus_orig_mlen = 0;
		ctx->focus_orig_cls = CLASS_INVALID;
	}

	/* pid == 0 means "clear focus only". */
	if (pid == 0)
		return 0;

	(void)read_comm(pid, comm, sizeof(comm));

	/* Step 2: locate the PID in the attached set. If not present, the
     * proc-connector / periodic rescan will attach it shortly under
     * its default class; the next focus-change event from the helper
     * will pick it up. */
	ent = attached_find_mut(ctx, pid);
	if (!ent) {
		lpmd_log_debug(
			"process_cpuset: focus: pid=%d comm='%s' NOT in attached "
			"set (no matching <Process>/<DefaultProcess>; ignored)\n",
			(int)pid, comm);
		return 0;
	}

	ui_set = core_spec_is_set(&ctx->class_defaults[CLASS_USER_INTERACTIVE]);
	if (ui_set) {
		mlen = build_user_interactive_mask(ctx, mask, sizeof(mask));
		if (!mlen) {
			lpmd_log_debug(
				"process_cpuset: focus: user_interactive mask is empty\n");
			return -1;
		}
	} else {
		/*
		 * user_interactive is not set in <ClassDefaults>, which means
		 * lpmd has no placement for it. The focused application is
		 * then given back the mask it had before lpmd bound it, so
		 * focus lifts whatever its own class imposed rather than
		 * inventing a mask nobody configured. With no pre-lpmd mask
		 * on record there is nothing to lift.
		 */
		if (!ent->orig_mlen) {
			lpmd_log_debug(
				"process_cpuset: focus: pid=%d comm='%s': "
				"user_interactive not set in <ClassDefaults> and "
				"no pre-lpmd mask recorded; left alone\n",
				(int)pid, comm);
			return 0;
		}
		memset(mask, 0, sizeof(mask));
		memcpy(mask, ent->orig_mask, ent->orig_mlen);
		mlen = ent->orig_mlen;
	}

	/* Remember current (pre-focus) mask so we can revert later. */
	ctx->focus_orig_mlen = snapshot_pid_mask(pid, ctx->focus_orig_mask,
						 sizeof(ctx->focus_orig_mask));
	if (!ctx->focus_orig_mlen) {
		/* PID just exited; nothing to do. */
		return 0;
	}

	/* If the new (user_interactive) mask is identical to what the PID
     * already has, the promotion is a visible no-op. Warn loudly so
     * the user knows to differentiate the <ClassDefaults> for
     * UserInteractive vs UserInitiated. */
	if (mlen == ctx->focus_orig_mlen &&
	    memcmp(mask, ctx->focus_orig_mask, mlen) == 0) {
		lpmd_log_debug(
			"process_cpuset: focus: pid=%d comm='%s': "
			"user_interactive mask matches current mask exactly "
			"(no visible change). Update <ClassDefaults> so "
			"<UserInteractive> differs from the PID's current class.\n",
			(int)pid, comm);
	} else {
		char ui_list[MAX_CPULIST] = { 0 };
		char old_list[MAX_CPULIST] = { 0 };
		mask_to_cpulist(mask, mlen, ui_list, sizeof(ui_list));
		mask_to_cpulist(ctx->focus_orig_mask, ctx->focus_orig_mlen,
				old_list, sizeof(old_list));
		lpmd_log_debug(
			"process_cpuset: focus: promote pid=%d comm='%s' "
			"cpus [%s] -> [%s]\n",
			(int)pid, comm,
			old_list[0] ? old_list : "?",
			ui_list[0] ? ui_list : "?");
	}

	ctx->focus_pid = pid;
	ctx->focus_orig_cls = ent->cls;
	ctx->focus_orig_groups = ent->groups;
	ent->cls = CLASS_USER_INTERACTIVE;
	ent->groups = ctx->class_defaults[CLASS_USER_INTERACTIVE].groups;

	/* Focus promotion is a whole-process semantic: always cover every TID
     * in the leader's task list, regardless of the matching entry's
     * <AffinityAllThreads> setting. This is especially important for
     * browsers, whose top-level window PID is often a worker TID rather
     * than the process leader. */
	ctx->focus_all_threads = 1;
	if (affinity_apply(pid, /*all_threads=*/1, mask, mlen,
			   class_str(CLASS_USER_INTERACTIVE)) < 0)
		return -1;
	(void)apply_pid_uclamp(ctx, pid, CLASS_USER_INTERACTIVE,
			       class_str(CLASS_USER_INTERACTIVE), 1);

	/* Step 4: promote descendant TGIDs (browser content / GPU / RDD
     * sub-processes, etc.). Each gets the same UI mask applied
     * all-threads and is tracked for revert on the next focus
     * change. */
	{
		pid_t kids[FOCUS_DESC_MAX];
		size_t nkids, i;

		nkids = collect_descendant_tgids(pid, kids, FOCUS_DESC_MAX);
		if (nkids) {
			lpmd_log_debug(
				"process_cpuset: focus: pid=%d has %zu descendant "
				"TGID(s) to promote\n",
				(int)pid, nkids);
		}
		for (i = 0; i < nkids && ctx->focus_desc_n < FOCUS_DESC_MAX;
		     i++) {
			struct focus_desc *d =
				&ctx->focus_descs[ctx->focus_desc_n];
			if (focus_promote_descendant(ctx, kids[i], mask, mlen,
						     d) == 0)
				ctx->focus_desc_n++;
		}
	}
	return 0;
}

/*
 * Re-apply the @target_cls cpuset to every attached PID currently
 * classified as @target_cls. Called when the resolved default for
 * @target_cls changes (e.g. focus-helper handshake flipping the
 * USER_INITIATED default between "mirror UI" and "own spec").
 *
 * The PID currently held by the focus-promote path is skipped: it
 * already wears the USER_INTERACTIVE mask, and the focus path owns
 * the eventual revert via focus_orig_*. So is any PID whose mask came
 * from its entry's own <ActiveCores>: the class default never applied
 * to it.
 *
 * If @target_cls is now unset, lpmd has no placement for it any more,
 * so its PIDs are released exactly as process_cpuset_release_all()
 * would release them and dropped from the attached set. Leaving them
 * wearing the old mask would keep enforcing a policy that no longer
 * exists.
 *
 * Returns the number of PIDs touched, or -1 on parameter error.
 */
static int release_attached(process_cpuset_t *ctx,
			    const struct attached_entry *it);
static int reapply_attached_class(process_cpuset_t *ctx,
				  enum classification target_cls)
{
	uint8_t mask[CPUMASK_BYTES];
	size_t mlen = 0;
	struct proc_entry e;
	int unset;
	int n = 0;
	size_t w = 0;

	if (!ctx)
		return -1;

	memset(&e, 0, sizeof(e));
	e.cls = target_cls;
	e.resolved = *default_spec_for(ctx, target_cls);
	unset = !core_spec_is_set(&e.resolved);
	if (!unset) {
		if (build_mask_for(ctx, &e, mask, sizeof(mask)) < 0)
			return -1;
		mlen = mask_significant_len(mask, sizeof(mask));
		if (!mlen)
			return -1;
	}

	for (size_t i = 0; i < ctx->attached.n; i++) {
		struct attached_entry *ent = &ctx->attached.items[i];
		int keep = 1;

		if (ent->cls != target_cls || ent->explicit_cores ||
		    (ctx->focus_pid > 0 && ent->pid == ctx->focus_pid)) {
			/* not ours to change */
		} else if (unset) {
			lpmd_log_debug(
				"process_cpuset: class %s no longer set; releasing pid %d\n",
				class_str(target_cls), (int)ent->pid);
			(void)release_attached(ctx, ent);
			keep = 0;
			n++;
		} else {
			(void)affinity_apply(ent->pid, /*all_threads=*/1, mask,
					     mlen, class_str(target_cls));
			(void)apply_pid_uclamp(ctx, ent->pid, target_cls,
					       class_str(target_cls), 1);
			/* Keep the stored groups in sync so LIST-BOUND displays
			 * the correct CPU group names after a class-default
			 * change (e.g. focus-helper handshake flipping
			 * USER_INITIATED). */
			ent->groups = e.resolved.groups;
			n++;
		}
		if (keep) {
			if (w != i)
				ctx->attached.items[w] = *ent;
			w++;
		}
	}
	ctx->attached.n = w;
	return n;
}

/*
 * Toggle the "focus helper is present" flag. The user-session relay
 * (intel_lpmd_focus_helper) calls this via DBus after it successfully
 * registers with the compositor; intel_lpmd then knows it will be
 * receiving focus-PID events and can start enforcing a tighter
 * USER_INITIATED cpuset (vs. the default of mirroring USER_INTERACTIVE
 * when no focus signal is available).
 *
 * On every transition we re-resolve every entry that depends on the
 * USER_INITIATED class default and re-apply masks to attached PIDs
 * of that class.
 *
 * Returns 0 on success, -1 on parameter error.
 */
int process_cpuset_set_focus_helper_present(process_cpuset_t *ctx, int present)
{
	int new_state;

	if (!ctx)
		return -1;
	new_state = present ? 1 : 0;
	if (ctx->focus_helper_present == new_state)
		return 0;

	ctx->focus_helper_present = new_state;
	lpmd_log_debug(
		"process_cpuset: focus helper %s; user_initiated default "
		"will %s\n",
		new_state ? "PRESENT" : "ABSENT",
		new_state ? "honor its own <ClassDefaults>" :
			    "mirror user_interactive");

	/* Re-resolve every loaded entry of class USER_INITIATED that
     * doesn't have an explicit <ActiveCores> override; their
     * .resolved snapshot was captured at parse time and needs to
     * track the new default. */
	for (int i = 0; i < ctx->n_entries; i++) {
		struct proc_entry *e = &ctx->entries[i];
		if (e->cls != CLASS_USER_INITIATED)
			continue;
		if (core_spec_is_set(&e->explicit_spec))
			continue;
		e->resolved = *default_spec_for(ctx, CLASS_USER_INITIATED);
	}
	if (ctx->has_default_entry &&
	    ctx->default_entry.cls == CLASS_USER_INITIATED &&
	    !core_spec_is_set(&ctx->default_entry.explicit_spec)) {
		ctx->default_entry.resolved =
			*default_spec_for(ctx, CLASS_USER_INITIATED);
	}

	/* Re-apply to already-attached USER_INITIATED PIDs. */
	(void)reapply_attached_class(ctx, CLASS_USER_INITIATED);

	/* Dump the full class-defaults table so the operator can see the
     * new resolved cpuset for every classification after the
     * USER_INITIATED <-> USER_INTERACTIVE flip. Same format as the
     * one-shot dump emitted at startup. */
	lpmd_log_debug(
		"process_cpuset: class defaults after focus-helper %s:\n",
		new_state ? "PRESENT" : "ABSENT");
	process_cpuset_log_class_defaults(ctx);
	return 0;
}

/*
 * Debug dump: log the resolved AllowedCPUs mask for each
 * classification, computed from current <ClassDefaults> + active
 * CPU groups. Intended to be called once after config load /
 * group overrides so the user can see which CPUs each tier maps to.
 */
void process_cpuset_log_class_defaults(const process_cpuset_t *ctx)
{
	static const enum classification order[] = {
		CLASS_REALTIME,		CLASS_USER_INTERACTIVE,
		CLASS_USER_INITIATED,	CLASS_UNCLASSIFIED,
		CLASS_UTILITY,
		CLASS_BACKGROUND,	CLASS_GAME_PROFILE_CPU,
		CLASS_GAME_PROFILE_GPU, CLASS_GAME_PROFILE_HYBRID,
		CLASS_CUSTOM_PROFILE_0, CLASS_CUSTOM_PROFILE_1,
		CLASS_CUSTOM_PROFILE_2,
	};
	char list[MAX_CPULIST];
	char gp[MAX_CPULIST], ge[MAX_CPULIST], gl[MAX_CPULIST];

	if (!ctx)
		return;

	snprintf(gp, sizeof(gp), "%s", ctx->groups.p_cores);
	snprintf(ge, sizeof(ge), "%s", ctx->groups.e_cores);
	snprintf(gl, sizeof(gl), "%s", ctx->groups.l_cores);
	lpmd_log_debug(
		"process_cpuset: groups Pcores=[%s] Ecores=[%s] Lcores=[%s]\n",
		gp[0] ? gp : "-", ge[0] ? ge : "-", gl[0] ? gl : "-");

	for (size_t i = 0; i < sizeof(order) / sizeof(order[0]); i++) {
		struct proc_entry e;
		uint8_t mask[CPUMASK_BYTES];
		int uclamp_min = UCLAMP_UNSET;
		int uclamp_max = UCLAMP_UNSET;

		memset(&e, 0, sizeof(e));
		e.cls = order[i];
		e.resolved = *default_spec_for(ctx, order[i]);
		(void)class_uclamp_get(ctx, order[i],
				      &uclamp_min, &uclamp_max);
		if (!core_spec_is_set(&e.resolved)) {
			lpmd_log_debug(
				"process_cpuset: defaults: %-18s = not set (left alone)\n",
				class_str(order[i]));
			/* An unset class applies nothing, uclamp included. */
			if (uclamp_min != UCLAMP_UNSET ||
			    uclamp_max != UCLAMP_UNSET)
				lpmd_log_warn(
					"process_cpuset: <ClassDefaults> gives %s a uclamp but no <Cores>; the uclamp is not applied\n",
					class_str(order[i]));
			continue;
		}
		if (build_mask_for(ctx, &e, mask, sizeof(mask)) < 0) {
			lpmd_log_debug(
				"process_cpuset: defaults: %-18s = <build error>\n",
				class_str(order[i]));
			continue;
		}
		mask_to_cpulist(mask, sizeof(mask), list, sizeof(list));
		lpmd_log_debug(
			"process_cpuset: defaults: %-18s = [%s] uclamp_min=%d uclamp_max=%d\n",
			class_str(order[i]), list[0] ? list : "<empty>",
			uclamp_min, uclamp_max);
	}
}

/* Forward decl for the catch-all <DefaultProcess> helper (defined below). */
static int apply_default_to_pid(process_cpuset_t *ctx, pid_t pid,
				int from_event, int dry_run);

/* ---------- slice-intent classification ---------- */

/*
 * Whether @e says something about this program in particular, as opposed to
 * something a slice could have said just as well.
 *
 * An explicit <ActiveCores> is a hand-written CPU list for one program; the
 * realtime, game-profile and custom-profile classes exist to single out a
 * specific workload. Neither is expressible as "everything in this slice", so
 * neither is something slice membership should overrule. A plain
 * <Name>foo</Name><Classification>background</Classification> entry, by
 * contrast, is a guess at exactly what a slice states outright, and loses.
 */
static int entry_is_curated(const struct proc_entry *e)
{
	if (!e)
		return 0;
	if (core_spec_is_set(&e->explicit_spec))
		return 1;
	switch (e->cls) {
	case CLASS_REALTIME:
	case CLASS_GAME_PROFILE_CPU:
	case CLASS_GAME_PROFILE_GPU:
	case CLASS_GAME_PROFILE_HYBRID:
	case CLASS_CUSTOM_PROFILE_0:
	case CLASS_CUSTOM_PROFILE_1:
	case CLASS_CUSTOM_PROFILE_2:
		return 1;
	default:
		return 0;
	}
}

/*
 * Synthesise an entry from the classification @pid's slice implies, so the
 * rest of this file can apply it through exactly the same path a <Process>
 * entry takes -- masks, cgroup ceiling, hands-off rules, release tracking and
 * uclamp all included.
 *
 * <Name> is filled in with the slice unit so the debug log names the source of
 * the decision; nothing matches against it, since the entry never enters
 * ctx->entries. A <Cores> override arrives already expanded to a literal CPU
 * list (see lpmd_slice_cpuset_pid_intent), and is treated as <ActiveCores>
 * would be; otherwise the classification resolves against this file's
 * <ClassDefaults>, which is what makes an intent behave identically to a
 * <Process> entry of the same class.
 *
 * Returns 1 if @out was filled in, 0 if there is no intent for @pid.
 */
static int intent_entry_for_pid(const process_cpuset_t *ctx, pid_t pid,
				struct proc_entry *out)
{
	char unit[MAX_NAME];
	char cls_name[32];
	char cores[MAX_CPULIST];

	if (!ctx || !out)
		return 0;
	if (lpmd_slice_cpuset_pid_intent(pid, unit, sizeof(unit), cls_name,
					 sizeof(cls_name), cores,
					 sizeof(cores)) != 1)
		return 0;

	memset(out, 0, sizeof(*out));
	snprintf(out->name, sizeof(out->name), "%s", unit);
	/* Same default as an XML entry with no <AffinityAllThreads>. */
	out->affinity_all_threads = 1;
	out->cls = parse_class(cls_name);

	if (cores[0]) {
		parse_core_spec(cores, &out->explicit_spec);
		out->resolved = out->explicit_spec;
		/* A <Cores>-only entry still needs a class for uclamp and for
		 * the attached-set bookkeeping; unclassified is the tier that
		 * claims nothing about the workload. */
		if (out->cls == CLASS_INVALID)
			out->cls = CLASS_UNCLASSIFIED;
		return core_spec_is_set(&out->explicit_spec) ? 1 : 0;
	}

	if (out->cls == CLASS_INVALID)
		return 0; /* classification this build does not know */
	out->resolved = *default_spec_for(ctx, out->cls);
	return 1;
}

/*
 * Pick the policy that governs @pid: a <Process> entry, the classification
 * @pid's slice implies, or neither.
 *
 * Precedence, highest first:
 *   1. A curated <Process> entry -- explicit <ActiveCores>, or a class only a
 *      curated list can mean. See entry_is_curated().
 *   2. A <Process> entry matched by <Unit>.
 *   3. The slice intent, where one applies.
 *   4. Any remaining <Process> match, by <Cgroup> or by <Name>.
 *   5. Nothing: the caller falls back to <DefaultProcess>.
 *
 * Step 3 sitting above step 4 is the whole point of intent classification:
 * where the desktop put a task is better evidence of what it is than a
 * 15-character comm prefix somebody added to a list. It only applies at all
 * when <UseSliceClassification> is on, so a config that does not ask for it
 * sees the old order.
 *
 * Step 2 sits above it because the argument for preferring intent does not
 * apply to a unit name. Intent beats <Name> because a comm is forgeable and
 * imprecise; a unit name is neither -- systemd assigns it, the task cannot
 * change it, and it identifies one unit rather than a slice full of them. Where
 * the config names a specific unit and the slice only implies a default for
 * everything in it, the specific statement is the better evidence. The
 * shipped config keys many entries on <Unit>, and each of those is decided
 * here before intent is consulted.
 *
 * A <Cgroup> match is in step 4, not step 2, even though systemd assigns the
 * path too: a glob over the whole path can span a slice as easily as name one
 * unit. An entry that has to win over its slice needs a <Unit>.
 *
 * @intent_buf is storage the caller owns; the returned pointer is it when the
 * intent won, and *@from_intent says which happened. @how is only meaningful
 * for a <Process> entry.
 */
static const struct proc_entry *
resolve_policy_for_pid(const process_cpuset_t *ctx, pid_t pid, const char *comm,
		       const char *cgpath, const struct pid_units *units,
		       struct proc_entry *intent_buf, enum entry_match *how,
		       int *from_intent)
{
	const struct proc_entry *e;
	enum entry_match m = ENTRY_MATCH_NONE;

	*from_intent = 0;
	e = find_entry_for_pid(ctx, pid, comm, cgpath, units, &m);
	if (how)
		*how = m;
	if (e && (m == ENTRY_MATCH_UNIT || entry_is_curated(e)))
		return e;

	if (intent_entry_for_pid(ctx, pid, intent_buf)) {
		if (how)
			*how = ENTRY_MATCH_NONE;
		*from_intent = 1;
		return intent_buf;
	}

	return e;
}

/* How a PID's policy was chosen, for the debug log. */
static const char *match_str(enum entry_match how, int from_intent)
{
	if (from_intent)
		return "slice-intent";
	switch (how) {
	case ENTRY_MATCH_UNIT:
		return "unit";
	case ENTRY_MATCH_CGROUP:
		return "cgroup";
	default:
		return "name";
	}
}

/*
 * Note on the tracking record that @pid's class came from its slice, so
 * LIST-BOUND can report which policy decided it. A no-op for a dry run, which
 * tracks nothing.
 */
static void attached_mark_intent(process_cpuset_t *ctx, pid_t pid)
{
	struct attached_entry *it = attached_find_mut(ctx, pid);

	if (it)
		it->from_intent = 1;
}

int process_cpuset_apply_once(process_cpuset_t *ctx, int dry_run)
{
	uint8_t mask[CPUMASK_BYTES];
	int total_new = 0;
	int n_default = 0;
	int any_cgroups;
	int any_units;
	pid_t self = getpid();
	struct dirent *de;
	DIR *d;

	if (!ctx)
		return -1;

	pidset_prune_dead(&ctx->attached);

	/*
	 * One sweep over /proc, deciding each PID's entry as we see it.
	 *
	 * This used to be the other way round -- a /proc walk per <Process>
	 * entry -- which reopened and reread the whole directory once for each
	 * of the ~300 entries the shipped config carries. It also made
	 * precedence an accident of config order, because whichever entry
	 * reached a PID first put it in ctx->attached and every later entry
	 * skipped it. Deciding per PID lets find_entry_for_pid() apply the real
	 * rule: a <Unit> match outranks a <Cgroup> match outranks a <Name> match
	 * wherever they collide.
	 */
	any_cgroups = entries_use_cgroups(ctx);
	any_units = entries_use_units(ctx);

	/*
	 * Report unusable <ActiveCores> lists once per config, not once per
	 * PID that happens to match the broken entry.
	 */
	for (int i = 0; i < ctx->n_entries; i++) {
		/* An unset class is reported once, by the startup log. */
		if (!core_spec_is_set(&ctx->entries[i].resolved))
			continue;
		if (build_mask_for(ctx, &ctx->entries[i], mask,
				   sizeof(mask)) < 0)
			lpmd_log_debug("[%s] invalid CPU list, skipping\n",
				       entry_label(&ctx->entries[i]));
	}

	d = opendir("/proc");
	if (!d)
		return -1;

	while ((de = readdir(d))) {
		const struct proc_entry *e;
		struct proc_entry intent;
		enum entry_match how;
		char comm[MAX_NAME];
		char cgpath[MAX_CGROUP_PATH];
		struct pid_units units;
		uid_t owner_uid = 0;
		enum pid_location loc;
		int from_intent;
		size_t mlen;
		char *end;
		long pid = strtol(de->d_name, &end, 10);

		if (*end != '\0' || pid <= 0)
			continue;
		/*
		 * Never constrain our own PID: the daemon has to keep
		 * scheduling to be able to undo any of this.
		 */
		if ((pid_t)pid == self)
			continue;
		if (pidset_contains(&ctx->attached, (pid_t)pid))
			continue; /* already handled in a previous cycle */
		if (read_comm((pid_t)pid, comm, sizeof(comm)) < 0)
			continue; /* PID likely already gone */

		cgpath[0] = '\0';
		if (any_cgroups)
			(void)pid_cgroup_path((pid_t)pid, cgpath, sizeof(cgpath));

		units.sys[0] = '\0';
		units.usr[0] = '\0';
		if (any_units)
			pid_read_units((pid_t)pid, &units);

		e = resolve_policy_for_pid(ctx, (pid_t)pid, comm, cgpath, &units,
					   &intent, &how, &from_intent);
		if (!e) {
			/*
			 * Catch-all: apply <DefaultProcess> to anything no
			 * entry claimed. This is what lets e.g. a kernel build
			 * (cc1/ld/make), which inherited an affinity mask from
			 * its gnome-terminal ancestor, get reset to the default
			 * (typically all CPUs).
			 */
			if (ctx->has_default_entry &&
			    apply_default_to_pid(ctx, (pid_t)pid, 0,
						 dry_run) == 1) {
				n_default++;
				total_new++;
			}
			continue;
		}

		/*
		 * Already reported, so just skip it here. An unset class is
		 * not "no CPUs": it means lpmd has no opinion, so the task
		 * keeps whatever affinity it has and is not tracked.
		 */
		if (!core_spec_is_set(&e->resolved))
			continue;
		if (build_mask_for(ctx, e, mask, sizeof(mask)) < 0)
			continue;
		mlen = mask_significant_len(mask, sizeof(mask));

		loc = classify_pid_location((pid_t)pid, &owner_uid);
		/* Login-session PIDs (sshd-session, sudo, gdm-*,
         * session-N.scope contents) are normally skipped:
         * migrating them breaks logind/polkit session tracking
         * and silently constraining a login shell is surprising.
         * The <AllowSession> tag opts a specific entry into
         * sched_setaffinity-based handling -- intended for games
         * or apps the user knowingly launches from the session
         * and wants pinned. */
		if (loc == PID_LOC_USER_SESS) {
			if (!e->allow_session) {
				lpmd_log_debug("[%s] skip pid %d (login-session scope)\n",
					       entry_label(e), (int)pid);
				continue;
			}
			/* Treat as user-session for affinity application. */
			loc = PID_LOC_USER_MGR;
			owner_uid = 0;
		}

		/* Every PID takes the same path now: sched_setaffinity
         * on the task where it already lives. @loc no longer
         * selects a mechanism -- it only decided, above, whether
         * a login-session PID is eligible at all. */
		(void)loc;
		if (affinity_bind_pid(ctx, e, (pid_t)pid, comm, mask, mlen,
				      owner_uid, dry_run) != 1)
			continue; /* slice-covered, own affinity, or no CPU in common */

		total_new++;
		if (from_intent)
			attached_mark_intent(ctx, (pid_t)pid);
		lpmd_log_debug("[%s] class=%s pid=%d comm=%s match=%s\n",
			       entry_label(e), class_str(e->cls), (int)pid, comm,
			       match_str(how, from_intent));
	}
	closedir(d);

	if (n_default)
		lpmd_log_debug("[*default*] class=%s new=%d\n",
			       class_str(ctx->default_entry.cls), n_default);

	return total_new;
}

/*
 * Single-PID variant of process_cpuset_apply_once(): look up @pid's
 * /proc/<pid>/comm, find a matching <Process> entry, and attach. This
 * is what the proc-connector listener calls when a fresh fork/exec
 * event arrives, avoiding a full /proc walk on every event.
 *
 * Returns 1 if @pid was newly attached, 0 if no match / already
 * attached / dead, -1 on error.
 */
int process_cpuset_apply_pid(process_cpuset_t *ctx, pid_t pid, int dry_run)
{
	uint8_t mask[CPUMASK_BYTES];
	char comm[MAX_NAME];
	char cgpath[MAX_CGROUP_PATH];
	struct pid_units units;
	const struct proc_entry *e;
	struct proc_entry intent;
	enum entry_match how;
	int from_intent;
	size_t mlen;
	int rc;
	pid_t self = getpid();

	if (!ctx || pid <= 0)
		return -1;
	if (pid == self)
		return 0;
	if (pidset_contains(&ctx->attached, pid))
		return 0;
	if (read_comm(pid, comm, sizeof(comm)) < 0)
		return 0; /* PID likely already gone */

	/* Decide how this PID should be handled (system-slice scope vs.
     * sched_setaffinity for user-session PIDs vs. skip). The
     * login-session decision is deferred until after we know the
     * matching <Process> entry, so the per-entry <AllowSession> tag
     * can override the default skip. */
	uid_t owner_uid = 0;
	enum pid_location loc = classify_pid_location(pid, &owner_uid);

	cgpath[0] = '\0';
	if (entries_use_cgroups(ctx))
		(void)pid_cgroup_path(pid, cgpath, sizeof(cgpath));

	units.sys[0] = '\0';
	units.usr[0] = '\0';
	if (entries_use_units(ctx))
		pid_read_units(pid, &units);

	e = resolve_policy_for_pid(ctx, pid, comm, cgpath, &units, &intent,
				   &how, &from_intent);
	if (!e) {
		/* Nothing claimed it -- try the catch-all <DefaultProcess>. */
		return apply_default_to_pid(ctx, pid, 1, dry_run);
	}

	if (loc == PID_LOC_USER_SESS) {
		if (!e->allow_session)
			return 0; /* skip login-session PIDs */
		loc = PID_LOC_USER_MGR;
		owner_uid = 0;
	}

	/* Class not set in <ClassDefaults>: not ours to place. */
	if (!core_spec_is_set(&e->resolved))
		return 0;
	if (build_mask_for(ctx, e, mask, sizeof(mask)) < 0)
		return -1;
	mlen = mask_significant_len(mask, sizeof(mask));

	/* One path for every PID: sched_setaffinity where the task
     * already lives. No transient scope, no cgroup change. */
	(void)loc;
	rc = affinity_bind_pid(ctx, e, pid, comm, mask, mlen, owner_uid,
			       dry_run);
	if (rc == 1) {
		if (from_intent)
			attached_mark_intent(ctx, pid);
		lpmd_log_debug("[%s] class=%s pid=%d comm=%s match=%s (event)\n",
			       entry_label(e), class_str(e->cls), (int)pid, comm,
			       match_str(how, from_intent));
	}
	return rc;
}

/*
 * Apply the catch-all <DefaultProcess> spec to @pid.
 *
 * Only user-session PIDs (under user@<UID>.service, or login-session
 * scopes if the default entry has <AllowSession>) are considered.
 * The default is intentionally NOT applied to system-slice PIDs --
 * those daemons either match a named <Process> entry or are left
 * alone. The default uses the sched_setaffinity path; no cgroup is
 * created.
 *
 * Returns 1 if applied, 0 if skipped, -1 on error.
 */
static int apply_default_to_pid(process_cpuset_t *ctx, pid_t pid,
				int from_event, int dry_run)
{
	uint8_t mask[CPUMASK_BYTES];
	size_t mlen;
	char comm[MAX_NAME];
	uid_t owner_uid = 0;
	enum pid_location loc;
	const struct proc_entry *e;

	(void)pid_comm_for_log(pid, comm, sizeof(comm));

	if (!ctx->has_default_entry)
		return 0;
	e = &ctx->default_entry;

	loc = classify_pid_location(pid, &owner_uid);
	if (loc == PID_LOC_USER_SESS) {
		if (!e->allow_session)
			return 0;
		loc = PID_LOC_USER_MGR;
		owner_uid = 0;
	}
	/* The catch-all only handles user-session PIDs. */
	if (loc != PID_LOC_USER_MGR)
		return 0;

	/* A <DefaultProcess> with no <ActiveCores> and an unset class. */
	if (!core_spec_is_set(&e->resolved))
		return 0;
	if (build_mask_for(ctx, e, mask, sizeof(mask)) < 0)
		return -1;
	mlen = mask_significant_len(mask, sizeof(mask));

	if (dry_run) {
		lpmd_log_debug("[*default*] (dry) would set affinity pid %d comm=%s (uid=%u%s%s)\n",
			       (int)pid, comm,
			       (unsigned)owner_uid,
			       from_event ? " event" : "",
			       e->affinity_all_threads ? " all-threads" : "");
		return 1;
	}

	if (e->affinity_all_threads) {
		int n = set_pid_affinity_all_threads(pid, mask, mlen,
						     class_str(e->cls));
		if (n > 0) {
			lpmd_log_debug("[*default*] affinity pid %d comm=%s (uid=%u tids=%d%s)\n",
				       (int)pid, comm,
				       (unsigned)owner_uid, n,
				       from_event ? " event" : "");
			if (pidset_add(&ctx->attached, pid, e->cls,
				       e->resolved.groups, owner_uid) == 0)
				ctx->attached.items[ctx->attached.n - 1]
					.explicit_cores = core_spec_is_set(
					&e->explicit_spec);
			(void)apply_pid_uclamp(ctx, pid, e->cls,
			       class_str(e->cls), 1);
			return 1;
		}
	} else if (set_pid_affinity_from_mask(pid, mask, mlen) == 0) {
		lpmd_log_debug("[*default*] affinity pid %d comm=%s (uid=%u%s)\n",
			       (int)pid, comm,
			       (unsigned)owner_uid,
			       from_event ? " event" : "");
		if (pidset_add(&ctx->attached, pid, e->cls, e->resolved.groups,
			       owner_uid) == 0)
			ctx->attached.items[ctx->attached.n - 1].explicit_cores =
				core_spec_is_set(&e->explicit_spec);
		(void)apply_pid_uclamp(ctx, pid, e->cls, class_str(e->cls), 0);
		return 1;
	}
	return -1;
}

/*
 * Undo the mask that descendants of @it inherited from it across fork(2).
 *
 * A child of a bound task comes up wearing lpmd's mask. If it matches a
 * <Process> entry the bind path tracks it in its own right and the main
 * release loop covers it; if it matches nothing, lpmd never visits it, yet
 * the narrowing is still lpmd's doing and would outlive the daemon. This
 * catches exactly those: descendants that are not tracked themselves and
 * whose current mask is still, byte for byte, what lpmd wrote to @it.
 *
 * That equality test is the whole safety argument. A descendant that has
 * since set its own affinity no longer matches and is left alone, so this
 * can only ever put back a mask lpmd is responsible for.
 *
 * Threads follow @it->all_threads rather than always being included: it
 * mirrors what lpmd would have done to this family had it bound the task
 * directly, and erring that way can leave a sibling thread narrowed but
 * can never overwrite a mask a thread chose for itself.
 *
 * Returns the number of descendants restored.
 */
static int release_inherited_descendants(process_cpuset_t *ctx,
					 const struct attached_entry *it)
{
	pid_t kids[FOCUS_DESC_MAX];
	size_t nkids;
	int restored = 0;

	if (!it->set_mlen || !it->orig_mlen)
		return 0;

	nkids = collect_descendant_tgids(it->pid, kids, FOCUS_DESC_MAX);
	for (size_t k = 0; k < nkids; k++) {
		uint8_t now[CPUMASK_BYTES];
		char comm[MAX_NAME];
		char dbg[MAX_CPULIST];

		/* Tracked in its own right: the main loop owns it. */
		if (pidset_contains(&ctx->attached, kids[k]))
			continue;

		memset(now, 0, sizeof(now));
		if (!snapshot_pid_mask(kids[k], now, sizeof(now)))
			continue; /* gone */
		if (memcmp(now, it->set_mask, CPUMASK_BYTES) != 0)
			continue; /* not our mask; not ours to undo */

		if (affinity_apply(kids[k], it->all_threads, it->orig_mask,
				   it->orig_mlen, class_str(it->cls)) == 0) {
			mask_to_cpulist(it->orig_mask, it->orig_mlen, dbg,
					sizeof(dbg));
			lpmd_log_debug(
				"release: pid %d comm=%s inherited our mask from pid %d; restored to [%s]\n",
				(int)kids[k],
				pid_comm_for_log(kids[k], comm, sizeof(comm)),
				(int)it->pid, dbg);
			restored++;
		}
	}
	return restored;
}

/*
 * Release one tracked PID @it the way process_cpuset_release_all()
 * describes, without removing it from the attached set; the caller does
 * that. Returns the number of PIDs restored (the task and any
 * descendants that inherited its mask).
 */
static int release_attached(process_cpuset_t *ctx,
			    const struct attached_entry *it)
{
	pid_t pid = it->pid;
	uint8_t now[CPUMASK_BYTES];
	char comm[MAX_NAME];
	size_t now_len;
	int released = 0;

	/* Restore the exact mask the PID had before we touched it, but
	 * only if it still carries what we wrote. A different mask means
	 * the task (or an admin) set its own affinity after we bound it
	 * -- that is the task's decision, so leave it. */
	if (it->start_time && pid_start_time(pid) != it->start_time) {
		lpmd_log_debug(
			"release: pid %d start_time changed (PID reused); not restoring\n",
			(int)pid);
		return 0;
	}

	/* Sweep descendants before restoring the PID itself, and do it
	 * even for the cases below that leave the PID alone: a child
	 * that forked before its parent re-pinned itself still carries
	 * our mask, and it is still ours to undo. Children of a PID
	 * that has already exited cannot be reached this way -- they
	 * were reparented, so there is no chain left to walk. */
	released += release_inherited_descendants(ctx, it);

	if (!it->orig_mlen)
		return released; /* nothing recorded: nothing to undo */

	memset(now, 0, sizeof(now));
	now_len = snapshot_pid_mask(pid, now, sizeof(now));
	if (!now_len)
		return released; /* gone */
	if (it->set_mlen && memcmp(now, it->set_mask, sizeof(now)) != 0) {
		char dbg_now[MAX_CPULIST];

		mask_to_cpulist(now, now_len, dbg_now, sizeof(dbg_now));
		lpmd_log_debug(
			"release: pid %d comm=%s affinity=[%s] was changed since we set it; left alone\n",
			(int)pid, pid_comm_for_log(pid, comm, sizeof(comm)),
			dbg_now);
		return released;
	}
	if (affinity_apply(pid, it->all_threads, it->orig_mask, it->orig_mlen,
			   class_str(it->cls)) == 0)
		released++;
	return released;
}

/*
 * Release every tracked PID, restoring exactly what it had before lpmd
 * touched it, then do the same for descendants that inherited an lpmd mask
 * across fork(2) without ever being tracked themselves. Nothing is
 * migrated and nothing is killed: a task that set its own affinity after
 * we bound it keeps that affinity, and a task whose PID was recycled is
 * left alone.
 * Clears the tracking set on completion. Returns the number of PIDs
 * successfully released, or -1 on a NULL context.
 */
int process_cpuset_release_all(process_cpuset_t *ctx)
{
	int released = 0;

	if (!ctx)
		return -1;

	for (size_t i = 0; i < ctx->attached.n; i++)
		released += release_attached(ctx, &ctx->attached.items[i]);

	ctx->attached.n = 0;
	return released;
}

/*
 * Look up a process name in the loaded config and return its
 * classification and resolved CPU affinity string.
 *
 * @name       : process comm (truncated to 15 chars like /proc/<pid>/comm).
 *               The search is case-insensitive.
 * @class_out  : if non-NULL, set to a static classification name string
 *               ("background", "utility", ...). Do not free.
 * @cpus_out   : if non-NULL, filled with a cpuset-style list of the
 *               resolved AllowedCPUs (e.g. "0-3,8-11"). When the active
 *               CPU groups have not been set (no P/E/LP-E core info),
 *               falls back to a symbolic group expression like
 *               "Pcores+Ecores". Writes at most @cpus_cap bytes.
 * @cpus_cap   : capacity of @cpus_out including the NUL terminator.
 *
 * Returns:
 *   1  if a named <Process> entry matched
 *   0  if no named entry matched but <DefaultProcess> was used
 *  -1  if not found (no match and no <DefaultProcess>)
 */
int process_cpuset_classify_name(const process_cpuset_t *ctx,
				 const char *name,
				 const char **class_out,
				 char *cpus_out, size_t cpus_cap)
{
	char trunc[16]; /* /proc/comm limit: 15 chars + NUL */
	const struct proc_entry *e = NULL;
	uint8_t mask[CPUMASK_BYTES];
	int is_default = 0;

	if (!ctx || !name || !*name)
		return -1;

	/* /proc/<pid>/comm truncates comm to 15 chars; mirror that here. */
	snprintf(trunc, sizeof(trunc), "%s", name);

	/* Case-insensitive name search across all loaded entries.
	 * Entries with '*' are glob patterns (e.g. "ShadowOfTheTomb*")
	 * and must be matched with fnmatch, not a plain string compare.
	 */
	for (int i = 0; i < ctx->n_entries; i++) {
		if (name_matches_entry(ctx->entries[i].name, trunc)) {
			e = &ctx->entries[i];
			break;
		}
	}

	if (!e) {
		if (ctx->has_default_entry) {
			e = &ctx->default_entry;
			is_default = 1;
		} else {
			return -1;
		}
	}

	if (class_out)
		*class_out = class_str(e->cls);

	if (cpus_out && cpus_cap) {
		cpus_out[0] = '\0';
		memset(mask, 0, sizeof(mask));

		if (build_mask_for(ctx, e, mask, sizeof(mask)) == 0 &&
		    mask_to_cpulist(mask, sizeof(mask), cpus_out, cpus_cap) > 0) {
			/* Successfully resolved to a real CPU list. */
		} else {
			/*
			 * CPU groups not configured yet (daemon not running or
			 * groups not set). Fall back to a symbolic group
			 * expression so the caller still gets useful info.
			 */
			const struct core_spec *spec = &e->resolved;
			size_t off = 0;

			cpus_out[0] = '\0';
			if (spec->groups & GROUP_PCORES)
				off += snprintf(cpus_out + off, cpus_cap - off,
						"Pcores");
			if (spec->groups & GROUP_ECORES)
				off += snprintf(cpus_out + off, cpus_cap - off,
						"%sEcores", off ? "+" : "");
			if (spec->groups & GROUP_LCORES)
				off += snprintf(cpus_out + off, cpus_cap - off,
						"%sLPEcores", off ? "+" : "");
			if (spec->cpulist[0])
				off += snprintf(cpus_out + off, cpus_cap - off,
						"%s%s", off ? "," : "",
						spec->cpulist);
			if (!off)
				snprintf(cpus_out, cpus_cap, "-");
		}
	}

	return is_default ? 0 : 1;
}

int process_cpuset_class_is_set(const process_cpuset_t *ctx,
				const char *classification)
{
	enum classification cls;

	if (!ctx || !classification)
		return -1;
	cls = parse_class(classification);
	if (cls == CLASS_INVALID)
		return -1;
	return core_spec_is_set(default_spec_for(ctx, cls)) ? 1 : 0;
}

int process_cpuset_get_class_cpulist(const process_cpuset_t *ctx,
				     const char *classification,
				     char *cpus_out, size_t cpus_cap)
{
	struct proc_entry e;
	uint8_t mask[CPUMASK_BYTES];
	enum classification cls;

	if (!ctx || !classification || !cpus_out || cpus_cap == 0)
		return -1;

	cls = parse_class(classification);
	if (cls == CLASS_INVALID)
		return -1;

	memset(&e, 0, sizeof(e));
	e.cls = cls;
	e.resolved = *default_spec_for(ctx, cls);

	if (build_mask_for(ctx, &e, mask, sizeof(mask)) < 0)
		return -1;

	mask_to_cpulist(mask, sizeof(mask), cpus_out, cpus_cap);
	return 0;
}
