// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * process_cpuset.c: Library implementation for per-process CPU affinity
 *
 * Copyright (C) 2026 Intel Corporation. All rights reserved.
 *
 * Library implementation: per-process CPU affinity via systemd transient
 * scopes (cpuset / AllowedCPUs).
 *
 * Public API: process_cpuset.h
 * CLI driver: process_cpuset_main.c
 *
 * Mirrors the XML-config + sd-bus pattern used by intel_lpmd
 * (see src/lpmd_config.c and src/lpmd_cgroup.c).
 *
 * What apply_once does:
 *   1. For every loaded <Process>, scans /proc to find matching PIDs by
 *      comm name.
 *   2. For each matched PID not yet attached, asks systemd to create a
 *      transient scope unit (proc_cpuset_<name>_<pid>.scope) containing
 *      that PID, with the AllowedCPUs cpuset bitmask derived from the
 *      chosen CPU list.
 *
 * NOTE: This is intentionally minimal POC code. Scopes created here are
 *       not stopped on context destruction; they die naturally with their
 *       PID.
 */

#define _GNU_SOURCE
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
#include <systemd/sd-bus.h>

#define MAX_NAME 64
#define MAX_CPULIST 256
#define MAX_PROCS 512
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
	char name[MAX_NAME];
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
	/* If non-zero, the affinity path (sched_setaffinity) walks
     * /proc/<pid>/task/ and applies the mask to every TID instead of
     * just the leader. sched_setaffinity targets a TID, so the
     * default per-PID call only constrains the main thread; threads
     * that already exist keep their previous (usually all-CPUs)
     * mask. Use this for multithreaded apps -- games, browsers,
     * media engines -- where you want every worker pinned. Set via
     * the <AffinityAllThreads> tag. Has no effect on PIDs that go
     * through the cgroup transient-scope path (cgroups already
     * apply to every thread). */
	int affinity_all_threads;
};

/* Global CPU groups parsed from <CpuGroups> in the XML. The per-process
 * <ActiveCores> tag selects which of these groups apply to that process. */
struct cpu_groups {
	char p_cores[MAX_CPULIST];
	char e_cores[MAX_CPULIST];
	char l_cores[MAX_CPULIST];
};

/* Tracked PIDs already attached to a scope (avoids re-issuing
 * StartTransientUnit, which systemd would refuse). The unit name is
 * remembered so we can later stop the transient scope on shutdown.
 * cls/groups capture the classification and resolved CPU group mask
 * used at attach time so introspection (LIST-BOUND) can show them
 * without re-parsing the XML. */
struct attached_entry {
	pid_t pid;
	char unit[128];
	enum classification cls;
	unsigned int groups;
	uid_t owner_uid; /* 0 = system manager, else user manager */
};
struct pid_set {
	struct attached_entry *items;
	size_t n;
	size_t cap;
};

/* One entry per descendant TGID promoted alongside the focused PID.
 * Remembers everything needed to revert that descendant to its prior
 * state when focus moves away (mask + classification + groups +
 * whether we used the cgroup-scope or sched_setaffinity path). */
#define FOCUS_DESC_MAX 256
struct focus_desc {
	pid_t pid;
	int used_scope; /* 1 = had a tracked scope unit */
	int had_entry; /* 1 = was in attached set */
	char unit[128];
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
     * its <Process> entry classifies it differently. focus_unit /
     * focus_used_scope let us revert the change cleanly on demotion. */
	pid_t focus_pid;
	char focus_unit[128];
	int focus_used_scope; /* 1 = cgroup scope, 0 = sched_setaffinity */
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

static int parse_uclamp_value(const char *s, int *out)
{
	char *end;
	long v;

	if (!s || !out)
		return -1;

	errno = 0;
	v = strtol(s, &end, 10);
	if (errno || end == s || *end != '\0')
		return -1;

	if (v == UCLAMP_UNSET ||
	    (v >= UCLAMP_CLAMP_MIN && v <= UCLAMP_CLAMP_MAX)) {
		*out = (int)v;
		return 0;
	}

	return -1;
}

static enum classification classdefaults_tag_to_class(const char *tag)
{
	if (!tag)
		return CLASS_INVALID;
	if (!strcasecmp(tag, "Realtime"))
		return CLASS_REALTIME;
	if (!strcasecmp(tag, "Foreground"))
		return CLASS_USER_INTERACTIVE;
	if (!strcasecmp(tag, "UserInteractive"))
		return CLASS_USER_INTERACTIVE;
	if (!strcasecmp(tag, "UserInitiated"))
		return CLASS_USER_INITIATED;
	if (!strcasecmp(tag, "Utility"))
		return CLASS_UTILITY;
	if (!strcasecmp(tag, "Unclassified"))
		return CLASS_UNCLASSIFIED;
	if (!strcasecmp(tag, "Background"))
		return CLASS_BACKGROUND;
	if (!strcasecmp(tag, "game_profile_cpu") ||
	    !strcasecmp(tag, "GameProfileCPU"))
		return CLASS_GAME_PROFILE_CPU;
	if (!strcasecmp(tag, "game_profile_gpu") ||
	    !strcasecmp(tag, "GameProfileGPU"))
		return CLASS_GAME_PROFILE_GPU;
	if (!strcasecmp(tag, "game_profile_mixed") ||
	    !strcasecmp(tag, "GameProfileMixed"))
		return CLASS_GAME_PROFILE_HYBRID;
	if (!strcasecmp(tag, "custom_profile_0") ||
	    !strcasecmp(tag, "CustomProfile0"))
		return CLASS_CUSTOM_PROFILE_0;
	if (!strcasecmp(tag, "custom_profile_1") ||
	    !strcasecmp(tag, "CustomProfile1"))
		return CLASS_CUSTOM_PROFILE_1;
	if (!strcasecmp(tag, "custom_profile_2") ||
	    !strcasecmp(tag, "CustomProfile2"))
		return CLASS_CUSTOM_PROFILE_2;

	return CLASS_INVALID;
}

/* Parse a token list (used by <ActiveCores> and every <ClassDefaults>
 * child) and return only the named-group mask. Kept for callers that
 * don't care about literal CPU lists. */
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

/* Parse <ClassDefaults> children: <Realtime>/<Foreground>/<Background>/
 * <GameProfile*>, each containing the same token list accepted by
 * <ActiveCores> (named groups and/or literal cpuset list).
 * Any class not mentioned keeps its built-in default. */
static void parse_class_defaults(xmlDoc *doc, xmlNode *node,
				 struct core_spec defaults[],
				 int uclamp_min[], int uclamp_max[])
{
	xmlNode *c, *child_node;
	char *val;

	for (c = node; c; c = c->next) {
		enum classification cls;

		if (c->type != XML_ELEMENT_NODE)
			continue;

		cls = classdefaults_tag_to_class((const char *)c->name);
		if (cls == CLASS_INVALID) {
			lpmd_log_debug(
				"warning: unknown <ClassDefaults> child '%s'\n",
				c->name);
			continue;
		}

		/* Legacy alias: map Foreground to both modern user tiers. */
		if (!strcasecmp((const char *)c->name, "Foreground")) {
			for (child_node = c->children; child_node;
			     child_node = child_node->next) {
				int parsed;

				if (child_node->type != XML_ELEMENT_NODE ||
				    !child_node->name)
					continue;
				val = (char *)xmlNodeListGetString(
					doc, child_node->xmlChildrenNode, 1);
				if (!val)
					continue;

				if (!strcasecmp((const char *)child_node->name,
					       "Cores")) {
					parse_core_spec(
						val,
						&defaults[CLASS_USER_INTERACTIVE]);
					parse_core_spec(
						val,
						&defaults[CLASS_USER_INITIATED]);
				} else if (!strcasecmp((const char *)child_node->name,
						      "UClampMin") ||
					   !strcasecmp((const char *)child_node->name,
						      "uclamp_min")) {
					if (parse_uclamp_value(val, &parsed) == 0) {
						uclamp_min[CLASS_USER_INTERACTIVE] =
							parsed;
						uclamp_min[CLASS_USER_INITIATED] =
							parsed;
					}
				} else if (!strcasecmp((const char *)child_node->name,
						      "UClampMax") ||
					   !strcasecmp((const char *)child_node->name,
						      "uclamp_max")) {
					if (parse_uclamp_value(val, &parsed) == 0) {
						uclamp_max[CLASS_USER_INTERACTIVE] =
							parsed;
						uclamp_max[CLASS_USER_INITIATED] =
							parsed;
					}
				}

				xmlFree(val);
			}
			continue;
		}

		for (child_node = c->children; child_node; child_node = child_node->next) {
			int parsed;

			if (child_node->type == XML_ELEMENT_NODE &&
			    !child_node->name)
				continue;

			val = (char *)xmlNodeListGetString(
				doc, child_node->xmlChildrenNode, 1);
			if (!val)
				continue;

			if (!strcasecmp((const char *)child_node->name, "Cores")) {
				parse_core_spec(val, &defaults[cls]);
			} else if (!strcasecmp((const char *)child_node->name,
					      "UClampMin") ||
				   !strcasecmp((const char *)child_node->name,
					      "uclamp_min")) {
				if (parse_uclamp_value(val, &parsed) == 0)
					uclamp_min[cls] = parsed;
			} else if (!strcasecmp((const char *)child_node->name,
					      "UClampMax") ||
				   !strcasecmp((const char *)child_node->name,
					      "uclamp_max")) {
				if (parse_uclamp_value(val, &parsed) == 0)
					uclamp_max[cls] = parsed;
			}

			xmlFree(val);
		}
	}
}

static void parse_one_process(const process_cpuset_t *ctx, xmlDoc *doc,
			      xmlNode *node, struct proc_entry *e)
{
	xmlNode *c;
	char *val;

	memset(e, 0, sizeof(*e));
	e->cls = CLASS_INVALID;

	for (c = node; c; c = c->next) {
		if (c->type != XML_ELEMENT_NODE)
			continue;
		val = (char *)xmlNodeListGetString(doc, c->xmlChildrenNode, 1);
		if (!val)
			continue;

		if (!strcmp((const char *)c->name, "Name"))
			copy_text(e->name, sizeof(e->name), val);
		else if (!strcmp((const char *)c->name, "Classification"))
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
			if (!strcasecmp(val, "1") || !strcasecmp(val, "true") ||
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
 * If @pid currently lives inside a transient "proc_cpuset_*.scope"
 * cgroup (created by any prior run of this library, including an
 * earlier daemon instance whose in-memory attached set is gone),
 * copy that scope's unit name into @unit_out and return 1.
 * Returns 0 if no such scope is found, -1 on read error.
 */
static int pid_existing_proc_cpuset_unit(pid_t pid, char *unit_out, size_t cap)
{
	char path[64];
	char line[512];
	FILE *f;
	int found = 0;

	if (!unit_out || cap == 0)
		return -1;
	snprintf(path, sizeof(path), "/proc/%d/cgroup", (int)pid);
	f = fopen(path, "r");
	if (!f)
		return -1;
	while (fgets(line, sizeof(line), f)) {
		char *p = strstr(line, "proc_cpuset_");
		char *e;
		size_t len;
		if (!p)
			continue;
		e = strstr(p, ".scope");
		if (!e)
			continue;
		len = (size_t)(e - p) + sizeof(".scope") - 1;
		if (len >= cap)
			len = cap - 1;
		memcpy(unit_out, p, len);
		unit_out[len] = '\0';
		found = 1;
		break;
	}
	fclose(f);
	return found;
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

static int find_pids_by_name(const char *name, pid_t *out, int max)
{
	DIR *d = opendir("/proc");
	struct dirent *de;
	char comm[MAX_NAME];
	int n = 0;
	pid_t self = getpid();

	if (!d)
		return -1;

	while ((de = readdir(d)) && n < max) {
		char *end;
		long pid = strtol(de->d_name, &end, 10);
		if (*end != '\0' || pid <= 0)
			continue;
		/*
         * Never bind our own PID into a transient cpuset scope.
         * Stopping a scope unit sends SIGTERM to the processes in it,
         * so binding ourselves would let process_cpuset_stop_all()
         * (UNBIND-ALL / shutdown) kill the daemon.
         */
		if ((pid_t)pid == self)
			continue;
		if (read_comm((pid_t)pid, comm, sizeof(comm)) < 0)
			continue;
		if (!pid_matches_name((pid_t)pid, name, comm))
			continue;
		out[n++] = (pid_t)pid;
	}
	closedir(d);
	return n;
}

/*
 * Apply @mask to @pid (and all its threads) using sched_setaffinity(2).
 *
 * Used as an alternative to creating a transient cpuset scope for
 * PIDs that live inside a user@<UID>.service cgroup. The user-mode
 * systemd manager normally does NOT have the cgroup v2 cpuset
 * controller delegated, so calling StartTransientUnit on it with
 * AllowedCPUs= fails with -EBADR ("Invalid request descriptor").
 * sched_setaffinity has no such limitation, doesn't touch cgroups
 * at all, and so leaves the systemd-logind / polkit session
 * tracking intact.
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

/* ---------- systemd: StartTransientUnit with AllowedCPUs + PIDs ----------
 *
 * Equivalent to:
 *   systemd-run --scope -p AllowedCPUs=<mask> --unit=<name> \
 *               <pid attached via PIDs property>
 *
 * but we need to attach an *existing* PID, so we use the
 * StartTransientUnit DBus method directly. Always issued against the
 * system manager: PIDs in user@<UID>.service must NOT take this path
 * (the system manager rejects them with -EBADR because they belong
 * to a delegated cgroup subtree).
 */
static void reset_failed_unit(const char *unit); /* fwd, defined below */

static int start_scope_for_pid(const char *unit, pid_t pid, const uint8_t *mask,
			       size_t mask_len)
{
	sd_bus_error err = SD_BUS_ERROR_NULL;
	sd_bus_message *m = NULL;
	sd_bus *bus = NULL;
	uint32_t pid_arr[1] = { (uint32_t)pid };
	int r;

	r = sd_bus_open_system(&bus);
	if (r < 0) {
		lpmd_log_debug( "sd_bus_open_system: %s\n", strerror(-r));
		goto out;
	}

	r = sd_bus_message_new_method_call(bus, &m, "org.freedesktop.systemd1",
					   "/org/freedesktop/systemd1",
					   "org.freedesktop.systemd1.Manager",
					   "StartTransientUnit");
	if (r < 0)
		goto out;

	/* name, mode */
	r = sd_bus_message_append(m, "ss", unit, "replace");
	if (r < 0)
		goto out;

	/* a(sv) properties */
	r = sd_bus_message_open_container(m, 'a', "(sv)");
	if (r < 0)
		goto out;

	/* Description */
	r = sd_bus_message_append(m, "(sv)", "Description", "s",
				  "POC: per-process cpuset scope");
	if (r < 0)
		goto out;

	/* PIDs = au */
	r = sd_bus_message_open_container(m, 'r', "sv");
	if (r < 0)
		goto out;
	r = sd_bus_message_append(m, "s", "PIDs");
	if (r < 0)
		goto out;
	r = sd_bus_message_open_container(m, 'v', "au");
	if (r < 0)
		goto out;
	r = sd_bus_message_append_array(m, 'u', pid_arr, sizeof(pid_arr));
	if (r < 0)
		goto out;
	sd_bus_message_close_container(m); /* v */
	sd_bus_message_close_container(m); /* (sv) */

	/* AllowedCPUs = ay (cpuset bitmask) */
	if (mask_len > 0) {
		r = sd_bus_message_open_container(m, 'r', "sv");
		if (r < 0)
			goto out;
		r = sd_bus_message_append(m, "s", "AllowedCPUs");
		if (r < 0)
			goto out;
		r = sd_bus_message_open_container(m, 'v', "ay");
		if (r < 0)
			goto out;
		r = sd_bus_message_append_array(m, 'y', mask, mask_len);
		if (r < 0)
			goto out;
		sd_bus_message_close_container(m); /* v */
		sd_bus_message_close_container(m); /* (sv) */
	}

	sd_bus_message_close_container(m); /* a(sv) */

	/* aux a(sa(sv)) - empty */
	r = sd_bus_message_open_container(m, 'a', "(sa(sv))");
	if (r < 0)
		goto out;
	sd_bus_message_close_container(m);

	r = sd_bus_call(bus, m, 0, &err, NULL);
	if (r < 0) {
		const char *msg = err.message ? err.message : strerror(-r);
		/* systemd refuses to load a transient unit whose name is
         * already known (e.g. a previous instance lingering in the
         * "failed"/"loaded-but-inactive" state). Retry once after
         * ResetFailedUnit. */
		if (msg && (strstr(msg, "already loaded") ||
			    strstr(msg, "already exists") ||
			    strstr(msg, "AlreadyExists"))) {
			reset_failed_unit(unit);
			sd_bus_error_free(&err);
			err = SD_BUS_ERROR_NULL;
			sd_bus_message_unref(m);
			m = NULL;
			r = sd_bus_message_new_method_call(
				bus, &m, "org.freedesktop.systemd1",
				"/org/freedesktop/systemd1",
				"org.freedesktop.systemd1.Manager",
				"StartTransientUnit");
			if (r < 0)
				goto out;
			r = sd_bus_message_append(m, "ss", unit, "replace");
			if (r < 0)
				goto out;
			r = sd_bus_message_open_container(m, 'a', "(sv)");
			if (r < 0)
				goto out;
			r = sd_bus_message_append(
				m, "(sv)", "Description", "s",
				"POC: per-process cpuset scope");
			if (r < 0)
				goto out;
			r = sd_bus_message_open_container(m, 'r', "sv");
			if (r < 0)
				goto out;
			r = sd_bus_message_append(m, "s", "PIDs");
			if (r < 0)
				goto out;
			r = sd_bus_message_open_container(m, 'v', "au");
			if (r < 0)
				goto out;
			r = sd_bus_message_append_array(m, 'u', pid_arr,
							sizeof(pid_arr));
			if (r < 0)
				goto out;
			sd_bus_message_close_container(m); /* v */
			sd_bus_message_close_container(m); /* (sv) */
			if (mask_len > 0) {
				r = sd_bus_message_open_container(m, 'r', "sv");
				if (r < 0)
					goto out;
				r = sd_bus_message_append(m, "s",
							  "AllowedCPUs");
				if (r < 0)
					goto out;
				r = sd_bus_message_open_container(m, 'v', "ay");
				if (r < 0)
					goto out;
				r = sd_bus_message_append_array(m, 'y', mask,
								mask_len);
				if (r < 0)
					goto out;
				sd_bus_message_close_container(m); /* v */
				sd_bus_message_close_container(m); /* (sv) */
			}
			sd_bus_message_close_container(m); /* a(sv) */
			r = sd_bus_message_open_container(m, 'a', "(sa(sv))");
			if (r < 0)
				goto out;
			sd_bus_message_close_container(m);
			r = sd_bus_call(bus, m, 0, &err, NULL);
			if (r < 0)
				lpmd_log_debug(
					"StartTransientUnit(%s) retry failed: %s\n",
					unit,
					err.message ? err.message :
						      strerror(-r));
		} else {
			lpmd_log_debug( "StartTransientUnit(%s) failed: %s\n",
				unit, msg);
		}
	}

out:
	sd_bus_error_free(&err);
	sd_bus_message_unref(m);
	sd_bus_unref(bus);
	return r < 0 ? -1 : 0;
}

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

static void sanitize_unit_name(const char *in, char *out, size_t cap)
{
	size_t i;
	for (i = 0; in[i] && i + 1 < cap; i++) {
		char c = in[i];
		out[i] = (isalnum((unsigned char)c) || c == '_' || c == '-') ?
				 c :
				 '_';
	}
	out[i] = '\0';
}

/*
 * Read /proc/<pid>/stat field 22 (process start time, in clock ticks
 * since boot). This is unique per process invocation across the
 * lifetime of the kernel, so combining it with the PID guarantees
 * a globally-unique transient-unit name even when PIDs get recycled.
 *
 * Returns the value on success, 0 if anything fails (in which case
 * the caller should fall back to PID-only naming).
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
 * Tell the system manager to forget about a failed/exited transient
 * unit, so a subsequent StartTransientUnit with the same name will
 * succeed instead of returning "already loaded".
 *
 * Best effort: failures are ignored.
 */
static void reset_failed_unit(const char *unit)
{
	sd_bus_error err = SD_BUS_ERROR_NULL;
	sd_bus *bus = NULL;

	if (sd_bus_open_system(&bus) < 0)
		goto out;
	(void)sd_bus_call_method(bus, "org.freedesktop.systemd1",
				 "/org/freedesktop/systemd1",
				 "org.freedesktop.systemd1.Manager",
				 "ResetFailedUnit", &err, NULL, "s", unit);
out:
	sd_bus_error_free(&err);
	sd_bus_unref(bus);
}

/* ---------- attached-PID tracking ---------- */

static int pidset_contains(const struct pid_set *s, pid_t p)
{
	for (size_t i = 0; i < s->n; i++)
		if (s->items[i].pid == p)
			return 1;
	return 0;
}

static int pidset_add(struct pid_set *s, pid_t p, const char *unit,
		      enum classification cls, unsigned int groups,
		      uid_t owner_uid)
{
	if (s->n == s->cap) {
		size_t ncap = s->cap ? s->cap * 2 : 64;
		struct attached_entry *np =
			realloc(s->items, ncap * sizeof(*np));
		if (!np)
			return -1;
		s->items = np;
		s->cap = ncap;
	}
	s->items[s->n].pid = p;
	snprintf(s->items[s->n].unit, sizeof(s->items[s->n].unit), "%s",
		 unit ? unit : "");
	s->items[s->n].cls = cls;
	s->items[s->n].groups = groups;
	s->items[s->n].owner_uid = owner_uid;
	s->n++;
	return 0;
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

	/* Built-in fallback class defaults. calloc() already zeroed every
     * slot's cpulist; only the named-group masks need to be set. */
	ctx->class_defaults[CLASS_INVALID].groups = 0;
	ctx->class_defaults[CLASS_REALTIME].groups = GROUP_PCORES;
	ctx->class_defaults[CLASS_USER_INTERACTIVE].groups =
		GROUP_PCORES | GROUP_ECORES;
	ctx->class_defaults[CLASS_USER_INITIATED].groups =
		GROUP_PCORES | GROUP_ECORES;
	ctx->class_defaults[CLASS_UTILITY].groups = GROUP_ECORES | GROUP_LCORES;
	ctx->class_defaults[CLASS_UNCLASSIFIED].groups =
		GROUP_PCORES | GROUP_ECORES | GROUP_LCORES;
	ctx->class_defaults[CLASS_BACKGROUND].groups = GROUP_LCORES;
	/* GameProfile* placeholders: pick sensible defaults until the
     * runtime grows policy specific to each profile. */
	ctx->class_defaults[CLASS_GAME_PROFILE_CPU].groups = GROUP_PCORES;
	ctx->class_defaults[CLASS_GAME_PROFILE_GPU].groups =
		GROUP_PCORES | GROUP_ECORES;
	ctx->class_defaults[CLASS_GAME_PROFILE_HYBRID].groups =
		GROUP_PCORES | GROUP_ECORES;
	ctx->class_defaults[CLASS_CUSTOM_PROFILE_0].groups =
		GROUP_PCORES | GROUP_ECORES | GROUP_LCORES;
	ctx->class_defaults[CLASS_CUSTOM_PROFILE_1].groups =
		GROUP_PCORES | GROUP_ECORES | GROUP_LCORES;
	ctx->class_defaults[CLASS_CUSTOM_PROFILE_2].groups =
		GROUP_PCORES | GROUP_ECORES | GROUP_LCORES;

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
				pid_t *pid_out, char *unit_out, size_t unit_cap)
{
	if (!ctx || i >= ctx->attached.n)
		return -1;
	if (pid_out)
		*pid_out = ctx->attached.items[i].pid;
	if (unit_out && unit_cap)
		snprintf(unit_out, unit_cap, "%s", ctx->attached.items[i].unit);
	return 0;
}

int process_cpuset_attached_get_ex(const process_cpuset_t *ctx, size_t i,
				   pid_t *pid_out, char *unit_out,
				   size_t unit_cap, const char **class_out,
				   int *use_pcores, int *use_ecores,
				   int *use_lcores)
{
	const struct attached_entry *e;

	if (!ctx || i >= ctx->attached.n)
		return -1;
	e = &ctx->attached.items[i];
	if (pid_out)
		*pid_out = e->pid;
	if (unit_out && unit_cap)
		snprintf(unit_out, unit_cap, "%s", e->unit);
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

	/* First pass: pick up <CpuGroups>, <ClassDefaults>, and
     * <DefaultProcess> regardless of where they appear. */
	for (cur = root->children; cur; cur = cur->next) {
		if (cur->type != XML_ELEMENT_NODE)
			continue;
		if (!strcmp((const char *)cur->name, "CpuGroups"))
			parse_cpu_groups(doc, cur->children, &ctx->groups);
		else if (!strcmp((const char *)cur->name, "ClassDefaults"))
			parse_class_defaults(doc, cur->children,
					     ctx->class_defaults,
					     ctx->class_uclamp_min,
					     ctx->class_uclamp_max);
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
		if (ctx->entries[n].name[0] &&
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
static int find_entry_index(const process_cpuset_t *ctx, const char *name)
{
	if (!ctx || !name || !*name)
		return -1;
	for (int i = 0; i < ctx->n_entries; i++)
		if (!strcmp(ctx->entries[i].name, name))
			return i;
	return -1;
}

/*
 * Additive load: parse @path and merge its <Process> entries on top of
 * the current ctx state. Entries whose <Name> already exists in the ctx
 * are replaced (so the user file overrides the system file). <CpuGroups>
 * and <ClassDefaults>, if present in the overlay, also override.
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

	/* Optional CpuGroups / ClassDefaults / DefaultProcess overrides. */
	for (cur = root->children; cur; cur = cur->next) {
		if (cur->type != XML_ELEMENT_NODE)
			continue;
		if (!strcmp((const char *)cur->name, "CpuGroups"))
			parse_cpu_groups(doc, cur->children, &ctx->groups);
		else if (!strcmp((const char *)cur->name, "ClassDefaults"))
			parse_class_defaults(doc, cur->children,
					     ctx->class_defaults,
					     ctx->class_uclamp_min,
					     ctx->class_uclamp_max);
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
		if (!tmp.name[0] || tmp.cls == CLASS_INVALID) {
			lpmd_log_debug(
				"Skipping invalid <Process> entry in overlay\n");
			continue;
		}

		idx = find_entry_index(ctx, tmp.name);
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
	e.resolved = *default_spec_for(ctx, cls);

	idx = find_entry_index(ctx, e.name);
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

/*
 * Update an existing transient cpuset scope's AllowedCPUs at runtime
 * via systemd's SetUnitProperties("runtime"=true). Best effort: a
 * non-zero return is logged at the call site.
 */
static int set_scope_allowed_cpus(const char *unit, const uint8_t *mask,
				  size_t mask_len)
{
	sd_bus_error err = SD_BUS_ERROR_NULL;
	sd_bus_message *m = NULL;
	sd_bus *bus = NULL;
	char dbg[MAX_CPULIST] = { 0 };
	int r;

	if (!unit || !*unit) {
		lpmd_log_debug(
			"set_scope_allowed_cpus: empty unit name, skipped\n");
		return -1;
	}

	mask_to_cpulist(mask, mask_len, dbg, sizeof(dbg));
	lpmd_log_debug(
		"set_scope_allowed_cpus: unit='%s' cpus=[%s] mlen=%zu\n", unit,
		dbg[0] ? dbg : "<empty>", mask_len);

	r = sd_bus_open_system(&bus);
	if (r < 0) {
		lpmd_log_debug(
			"set_scope_allowed_cpus: sd_bus_open_system failed: %s\n",
			strerror(-r));
		goto out;
	}

	r = sd_bus_message_new_method_call(bus, &m, "org.freedesktop.systemd1",
					   "/org/freedesktop/systemd1",
					   "org.freedesktop.systemd1.Manager",
					   "SetUnitProperties");
	if (r < 0)
		goto out;

	/* SetUnitProperties(in s name, in b runtime, in a(sv) properties) */
	r = sd_bus_message_append(m, "sb", unit, 1);
	if (r < 0)
		goto out;

	r = sd_bus_message_open_container(m, 'a', "(sv)");
	if (r < 0)
		goto out;
	r = sd_bus_message_open_container(m, 'r', "sv");
	if (r < 0)
		goto out;
	r = sd_bus_message_append(m, "s", "AllowedCPUs");
	if (r < 0)
		goto out;
	r = sd_bus_message_open_container(m, 'v', "ay");
	if (r < 0)
		goto out;
	r = sd_bus_message_append_array(m, 'y', mask, mask_len);
	if (r < 0)
		goto out;
	sd_bus_message_close_container(m); /* v */
	sd_bus_message_close_container(m); /* (sv) */
	sd_bus_message_close_container(m); /* a(sv) */

	r = sd_bus_call(bus, m, 0, &err, NULL);
	if (r < 0) {
		lpmd_log_debug(
			"SetUnitProperties(%s, AllowedCPUs=[%s]) failed: %s\n",
			unit, dbg[0] ? dbg : "<empty>",
			err.message ? err.message : strerror(-r));
	} else {
		lpmd_log_debug(
			"set_scope_allowed_cpus: unit='%s' cpus=[%s] applied OK\n",
			unit, dbg[0] ? dbg : "<empty>");
	}
out:
	sd_bus_error_free(&err);
	sd_bus_message_unref(m);
	sd_bus_unref(bus);
	return r < 0 ? -1 : 0;
}

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
		if (ent->unit[0]) {
			desc->used_scope = 1;
			snprintf(desc->unit, sizeof(desc->unit), "%s",
				 ent->unit);
		}
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
			"unit='%s' cpus [%s] -> [%s]\n",
			(int)pid, comm,
			desc->used_scope ? desc->unit : "(affinity)",
			old_list[0] ? old_list : "?",
			ui_list[0] ? ui_list : "?");
	}

	if (desc->used_scope) {
		if (set_scope_allowed_cpus(desc->unit, ui_mask, ui_mlen) < 0)
			return -1;
		(void)apply_pid_uclamp(ctx, pid, CLASS_USER_INTERACTIVE,
			       class_str(CLASS_USER_INTERACTIVE), 0);
		return 0;
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
	lpmd_log_debug(
		"process_cpuset: focus: demote-desc pid=%d unit='%s' "
		"cls->%s\n",
		(int)desc->pid, desc->used_scope ? desc->unit : "(affinity)",
		class_str(desc->orig_cls));
	if (desc->used_scope && desc->unit[0]) {
		(void)set_scope_allowed_cpus(desc->unit, desc->orig_mask,
					     desc->orig_mlen);
		if (desc->orig_cls != CLASS_INVALID)
			(void)apply_pid_uclamp(ctx, desc->pid, desc->orig_cls,
				       class_str(desc->orig_cls), 0);
	} else {
		(void)affinity_apply(desc->pid, /*all_threads=*/1,
				     desc->orig_mask, desc->orig_mlen,
				     class_str(desc->orig_cls));
		if (desc->orig_cls != CLASS_INVALID)
			(void)apply_pid_uclamp(ctx, desc->pid, desc->orig_cls,
				       class_str(desc->orig_cls), 1);
	}
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
 * Promotion strategy:
 *   - System-slice PID with a tracked scope unit:
 *       SetUnitProperties(unit, AllowedCPUs=<UI mask>, runtime=true)
 *   - User-session PID (affinity-only, unit==""):
 *       sched_setaffinity(pid, <UI mask>) (all-threads if entry asked)
 *   - PID not in attached set yet: no-op (the proc-connector / rescan
 *     will attach it shortly with its default class).
 *
 * On the next call (with a different @pid, or 0) the previous focus
 * PID is reverted to its remembered mask.
 *
 * Returns 0 on success, -1 on parameter / sd-bus error.
 */
int process_cpuset_set_focus_pid(process_cpuset_t *ctx, pid_t pid)
{
	uint8_t mask[CPUMASK_BYTES];
	size_t mlen;
	struct attached_entry *ent;
	char comm[MAX_NAME] = "";

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
		lpmd_log_debug(
			"process_cpuset: focus: demote pid=%d unit=%s scope=%d "
			"cls->%s\n",
			(int)prev,
			ctx->focus_unit[0] ? ctx->focus_unit : "(none)",
			ctx->focus_used_scope, class_str(ctx->focus_orig_cls));
		if (ctx->focus_used_scope && ctx->focus_unit[0]) {
			set_scope_allowed_cpus(ctx->focus_unit,
					       ctx->focus_orig_mask,
					       ctx->focus_orig_mlen);
			if (ctx->focus_orig_cls != CLASS_INVALID)
				(void)apply_pid_uclamp(ctx, prev,
					       ctx->focus_orig_cls,
					       class_str(ctx->focus_orig_cls), 0);
		} else {
			/* Affinity-only path: best effort, the PID may be gone. */
			(void)affinity_apply(prev, ctx->focus_all_threads,
					     ctx->focus_orig_mask,
					     ctx->focus_orig_mlen,
					     class_str(ctx->focus_orig_cls));
			if (ctx->focus_orig_cls != CLASS_INVALID)
				(void)apply_pid_uclamp(ctx, prev,
					       ctx->focus_orig_cls,
					       class_str(ctx->focus_orig_cls),
					       ctx->focus_all_threads);
		}
		ctx->focus_pid = 0;
		ctx->focus_unit[0] = '\0';
		ctx->focus_used_scope = 0;
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

	mlen = build_user_interactive_mask(ctx, mask, sizeof(mask));
	if (!mlen) {
		lpmd_log_debug(
			"process_cpuset: focus: user_interactive mask is empty\n");
		return -1;
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
			"process_cpuset: focus: pid=%d comm='%s' unit='%s': "
			"user_interactive mask matches current mask exactly "
			"(no visible change). Update <ClassDefaults> so "
			"<UserInteractive> differs from the PID's current class.\n",
			(int)pid, comm,
			ent->unit[0] ? ent->unit : "(affinity)");
	} else {
		char ui_list[MAX_CPULIST] = { 0 };
		char old_list[MAX_CPULIST] = { 0 };
		mask_to_cpulist(mask, mlen, ui_list, sizeof(ui_list));
		mask_to_cpulist(ctx->focus_orig_mask, ctx->focus_orig_mlen,
				old_list, sizeof(old_list));
		lpmd_log_debug(
			"process_cpuset: focus: promote pid=%d comm='%s' "
			"unit='%s' cpus [%s] -> [%s]\n",
			(int)pid, comm, ent->unit[0] ? ent->unit : "(affinity)",
			old_list[0] ? old_list : "?",
			ui_list[0] ? ui_list : "?");
	}

	ctx->focus_pid = pid;
	ctx->focus_used_scope = (ent->unit[0] != '\0');
	snprintf(ctx->focus_unit, sizeof(ctx->focus_unit), "%s", ent->unit);
	ctx->focus_orig_cls = ent->cls;
	ctx->focus_orig_groups = ent->groups;
	ent->cls = CLASS_USER_INTERACTIVE;
	ent->groups = ctx->class_defaults[CLASS_USER_INTERACTIVE].groups;

	if (ctx->focus_used_scope) {
		if (set_scope_allowed_cpus(ent->unit, mask, mlen) < 0)
			return -1;
		(void)apply_pid_uclamp(ctx, pid, CLASS_USER_INTERACTIVE,
			       class_str(CLASS_USER_INTERACTIVE), 0);
	} else {
		/* Affinity-only. Focus promotion is a whole-process semantic:
         * always cover every TID in the leader's task list, regardless
         * of the matching entry's <AffinityAllThreads> setting. This is
         * especially important for browsers, whose top-level window PID
         * is often a worker TID rather than the process leader. */
		ctx->focus_all_threads = 1;
		if (affinity_apply(pid, /*all_threads=*/1, mask, mlen,
				   class_str(CLASS_USER_INTERACTIVE)) < 0)
			return -1;
		(void)apply_pid_uclamp(ctx, pid, CLASS_USER_INTERACTIVE,
			       class_str(CLASS_USER_INTERACTIVE), 1);
	}

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
 * the eventual revert via focus_orig_*.
 *
 * Returns the number of PIDs touched, or -1 on parameter error.
 */
static int reapply_attached_class(process_cpuset_t *ctx,
				  enum classification target_cls)
{
	uint8_t mask[CPUMASK_BYTES];
	size_t mlen;
	struct proc_entry e;
	int n = 0;

	if (!ctx)
		return -1;

	memset(&e, 0, sizeof(e));
	e.cls = target_cls;
	e.resolved = *default_spec_for(ctx, target_cls);
	if (build_mask_for(ctx, &e, mask, sizeof(mask)) < 0)
		return -1;
	mlen = mask_significant_len(mask, sizeof(mask));
	if (!mlen)
		return -1;

	for (size_t i = 0; i < ctx->attached.n; i++) {
		struct attached_entry *ent = &ctx->attached.items[i];
		if (ent->cls != target_cls)
			continue;
		if (ctx->focus_pid > 0 && ent->pid == ctx->focus_pid)
			continue;
		if (ent->unit[0]) {
			(void)set_scope_allowed_cpus(ent->unit, mask, mlen);
			(void)apply_pid_uclamp(ctx, ent->pid, target_cls,
				       class_str(target_cls), 0);
		} else {
			(void)affinity_apply(ent->pid, /*all_threads=*/1, mask,
					     mlen, class_str(target_cls));
			(void)apply_pid_uclamp(ctx, ent->pid, target_cls,
				       class_str(target_cls), 1);
		}
		/* Keep the stored groups in sync so LIST-BOUND displays the
		 * correct CPU group names after a class-default change
		 * (e.g. focus-helper handshake flipping USER_INITIATED). */
		ent->groups = e.resolved.groups;
		n++;
	}
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
		if (build_mask_for(ctx, &e, mask, sizeof(mask)) < 0) {
			lpmd_log_debug(
				"process_cpuset: defaults: %-18s = <build error>\n",
				class_str(order[i]));
			continue;
		}
		mask_to_cpulist(mask, sizeof(mask), list, sizeof(list));
		(void)class_uclamp_get(ctx, order[i],
				      &uclamp_min, &uclamp_max);
		lpmd_log_debug(
			"process_cpuset: defaults: %-18s = [%s] uclamp_min=%d uclamp_max=%d\n",
			class_str(order[i]), list[0] ? list : "<empty>",
			uclamp_min, uclamp_max);
	}
}

/* Forward decls for catch-all <DefaultProcess> helpers (defined below). */
static int apply_default_to_pid(process_cpuset_t *ctx, pid_t pid,
				int from_event, int dry_run);
static int pid_in_entries(const process_cpuset_t *ctx, pid_t pid,
				  const char *comm);

int process_cpuset_apply_once(process_cpuset_t *ctx, int dry_run)
{
	uint8_t mask[CPUMASK_BYTES];
	int total_new = 0;

	if (!ctx)
		return -1;

	pidset_prune_dead(&ctx->attached);

	for (int i = 0; i < ctx->n_entries; i++) {
		const struct proc_entry *e = &ctx->entries[i];
		pid_t pids[256];
		int npids, new_here = 0;
		size_t mlen;
		char safe_name[MAX_NAME];

		if (build_mask_for(ctx, e, mask, sizeof(mask)) < 0) {
			lpmd_log_debug( "[%s] invalid CPU list, skipping\n",
				e->name);
			continue;
		}
		mlen = mask_significant_len(mask, sizeof(mask));

		npids = find_pids_by_name(e->name, pids, 256);
		if (npids <= 0)
			continue;

		sanitize_unit_name(e->name, safe_name, sizeof(safe_name));

		for (int j = 0; j < npids; j++) {
			char unit[128];
			char comm[MAX_NAME];
			uid_t owner_uid = 0;
			enum pid_location loc;

			(void)pid_comm_for_log(pids[j], comm, sizeof(comm));

			if (pidset_contains(&ctx->attached, pids[j]))
				continue; /* already handled in a previous cycle */

			loc = classify_pid_location(pids[j], &owner_uid);
			/* Login-session PIDs (sshd-session, sudo, gdm-*,
             * session-N.scope contents) are normally skipped:
             * migrating them breaks logind/polkit session tracking
             * and silently constraining a login shell is surprising.
             * The <AllowSession> tag opts a specific comm into
             * sched_setaffinity-based handling -- intended for games
             * or apps the user knowingly launches from the session
             * and wants pinned. */
			if (loc == PID_LOC_USER_SESS) {
				if (!e->allow_session) {
					lpmd_log_debug("[%s] skip pid %d (login-session scope)\n",
						       e->name, (int)pids[j]);
					continue;
				}
				/* Treat as user-session for affinity application. */
				loc = PID_LOC_USER_MGR;
				owner_uid = 0;
			}

			new_here++;
			total_new++;

			/* User-session PIDs (under user@<UID>.service): apply
             * affinity directly. The user systemd manager doesn't
             * have the cpuset cgroup controller delegated, so we
             * can't use a transient scope there. */
			if (loc == PID_LOC_USER_MGR) {
				if (dry_run) {
					lpmd_log_debug("[%s] (dry) would set affinity pid %d comm=%s (uid=%u%s)\n",
						       e->name, (int)pids[j],
						       comm,
						       (unsigned)owner_uid,
						       e->affinity_all_threads ?
							       " all-threads" :
							       "");
					continue;
				}
				if (e->affinity_all_threads) {
					int n = set_pid_affinity_all_threads(
						pids[j], mask, mlen,
						class_str(e->cls));
					if (n > 0) {
						lpmd_log_debug("[%s] affinity pid %d comm=%s (uid=%u tids=%d)\n",
							       e->name, (int)pids[j],
							       comm,
							       (unsigned)owner_uid, n);
						pidset_add(&ctx->attached,
							   pids[j], "", e->cls,
							   e->resolved.groups,
							   owner_uid);
						(void)apply_pid_uclamp(ctx, pids[j],
							       e->cls,
						       class_str(e->cls), 1);
					}
				} else if (set_pid_affinity_from_mask(
						   pids[j], mask, mlen) == 0) {
					lpmd_log_debug("[%s] affinity pid %d comm=%s (uid=%u)\n",
						       e->name, (int)pids[j],
						       comm,
						       (unsigned)owner_uid);
					/* unit="" marks this as affinity-only (no scope
                     * to stop on shutdown). */
					pidset_add(&ctx->attached, pids[j], "",
						   e->cls, e->resolved.groups,
						   owner_uid);
					(void)apply_pid_uclamp(ctx, pids[j], e->cls,
						       class_str(e->cls), 0);
				}
				continue;
			}

			/* System slice: create a transient cpuset scope. */
			{
				unsigned long long st = pid_start_time(pids[j]);
				if (st)
					snprintf(unit, sizeof(unit),
						 "proc_cpuset_%s_%d_%llu.scope",
						 safe_name, (int)pids[j], st);
				else
					snprintf(unit, sizeof(unit),
						 "proc_cpuset_%s_%d.scope",
						 safe_name, (int)pids[j]);
			}

			/* If the PID is already inside a proc_cpuset_*.scope (e.g.
             * left over from a previous daemon run), just reclaim it
             * into the in-memory tracking set. Otherwise systemd would
             * reject the StartTransientUnit with "already loaded". */
			{
				char existing[128];
				if (pid_existing_proc_cpuset_unit(
					    pids[j], existing,
					    sizeof(existing)) == 1) {
					if (dry_run)
						lpmd_log_debug("[%s] (dry) reattach pid %d comm=%s -> %s\n",
							       e->name, (int)pids[j],
							       comm,
							       existing);
					else
						lpmd_log_debug("[%s] reattached pid %d comm=%s -> %s\n",
							       e->name, (int)pids[j],
							       comm,
							       existing);
					pidset_add(&ctx->attached, pids[j],
						   existing, e->cls,
						   e->resolved.groups, 0);
					(void)apply_pid_uclamp(ctx, pids[j], e->cls,
						       class_str(e->cls), 0);
					continue;
				}
			}

			if (dry_run) {
				char mask_hex[(CPUMASK_BYTES * 2) + 1];

				for (size_t k = 0;
				     k < mlen && (k * 2 + 1) < sizeof(mask_hex); k++)
					snprintf(mask_hex + (k * 2),
						 sizeof(mask_hex) - (k * 2),
						 "%02x", mask[k]);
				mask_hex[mlen * 2] = '\0';
				lpmd_log_debug("[%s] (dry) would attach pid %d comm=%s -> %s (mask=%s)\n",
					       e->name, (int)pids[j], comm, unit, mask_hex);
				continue;
			}

			if (start_scope_for_pid(unit, pids[j], mask, mlen) ==
			    0) {
				lpmd_log_debug("[%s] attached pid %d comm=%s -> %s\n", e->name,
					       (int)pids[j], comm, unit);
				pidset_add(&ctx->attached, pids[j], unit,
					   e->cls, e->resolved.groups, 0);
				(void)apply_pid_uclamp(ctx, pids[j], e->cls,
					       class_str(e->cls), 0);
			}
		}

		if (new_here)
			lpmd_log_debug("[%s] class=%s new=%d total_matches=%d\n",
				       e->name, class_str(e->cls), new_here, npids);
	}

	/* Catch-all pass: apply <DefaultProcess> to user-session PIDs
     * whose comm isn't matched by any named entry. This is what
     * lets e.g. a kernel-build (cc1/ld/make), which inherits an
     * affinity mask from its gnome-terminal ancestor, get reset to
     * the default (typically all CPUs). */
	if (ctx->has_default_entry) {
		DIR *d = opendir("/proc");
		struct dirent *de;
		pid_t self = getpid();
		int n_def = 0;

		if (d) {
			while ((de = readdir(d))) {
				char *end;
				long pid = strtol(de->d_name, &end, 10);
				char comm[MAX_NAME];

				if (*end != '\0' || pid <= 0)
					continue;
				if ((pid_t)pid == self)
					continue;
				if (pidset_contains(&ctx->attached, (pid_t)pid))
					continue;
				if (read_comm((pid_t)pid, comm, sizeof(comm)) <
				    0)
					continue;
				if (pid_in_entries(ctx, (pid_t)pid, comm))
					continue; /* will be / was handled by name */
				if (apply_default_to_pid(ctx, (pid_t)pid, 0,
							 dry_run) == 1) {
					n_def++;
					total_new++;
				}
			}
			closedir(d);
		}
		if (n_def)
			lpmd_log_debug("[*default*] class=%s new=%d\n",
				       class_str(ctx->default_entry.cls), n_def);
	}

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
	char safe_name[MAX_NAME];
	char unit[128];
	size_t mlen;
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
     * matching <Process> entry, so the per-comm <AllowSession> tag
     * can override the default skip. */
	uid_t owner_uid = 0;
	enum pid_location loc = classify_pid_location(pid, &owner_uid);

	for (int i = 0; i < ctx->n_entries; i++) {
		const struct proc_entry *e = &ctx->entries[i];
		if (!pid_matches_name(pid, e->name, comm))
			continue;

		if (loc == PID_LOC_USER_SESS) {
			if (!e->allow_session)
				return 0; /* skip login-session PIDs */
			loc = PID_LOC_USER_MGR;
			owner_uid = 0;
		}

		if (build_mask_for(ctx, e, mask, sizeof(mask)) < 0)
			return -1;
		mlen = mask_significant_len(mask, sizeof(mask));

		/* User-session PIDs: sched_setaffinity, no cgroup. */
		if (loc == PID_LOC_USER_MGR) {
			if (dry_run) {
				lpmd_log_debug("[%s] (dry) would set affinity pid %d comm=%s (event uid=%u%s)\n",
					       e->name, (int)pid,
					       comm,
					       (unsigned)owner_uid,
					       e->affinity_all_threads ?
						       " all-threads" :
						       "");
				return 1;
			}
			if (e->affinity_all_threads) {
				int n = set_pid_affinity_all_threads(
					pid, mask, mlen, class_str(e->cls));
				if (n > 0) {
					lpmd_log_debug("[%s] affinity pid %d comm=%s (event uid=%u tids=%d)\n",
						       e->name, (int)pid,
						       comm,
						       (unsigned)owner_uid, n);
					pidset_add(&ctx->attached, pid, "",
						   e->cls, e->resolved.groups,
						   owner_uid);
					(void)apply_pid_uclamp(ctx, pid, e->cls,
						       class_str(e->cls), 1);
					return 1;
				}
			} else if (set_pid_affinity_from_mask(pid, mask,
							      mlen) == 0) {
				lpmd_log_debug("[%s] affinity pid %d comm=%s (event uid=%u)\n",
					       e->name, (int)pid,
					       comm,
					       (unsigned)owner_uid);
				pidset_add(&ctx->attached, pid, "", e->cls,
					   e->resolved.groups, owner_uid);
				(void)apply_pid_uclamp(ctx, pid, e->cls,
					       class_str(e->cls), 0);
				return 1;
			}
			return -1;
		}

		/* System slice: transient cpuset scope. */
		sanitize_unit_name(e->name, safe_name, sizeof(safe_name));
		{
			unsigned long long st = pid_start_time(pid);
			if (st)
				snprintf(unit, sizeof(unit),
					 "proc_cpuset_%s_%d_%llu.scope",
					 safe_name, (int)pid, st);
			else
				snprintf(unit, sizeof(unit),
					 "proc_cpuset_%s_%d.scope", safe_name,
					 (int)pid);
		}

		/* Reclaim an existing proc_cpuset_*.scope (e.g. from a prior
         * daemon run) instead of asking systemd to start one with the
         * same name, which would fail with "already loaded". */
		{
			char existing[128];
			if (pid_existing_proc_cpuset_unit(
				    pid, existing, sizeof(existing)) == 1) {
				if (dry_run)
					lpmd_log_debug("[%s] (dry) reattach pid %d comm=%s -> %s\n",
						       e->name, (int)pid,
						       comm,
						       existing);
				else
					lpmd_log_debug("[%s] reattached pid %d comm=%s -> %s (event)\n",
						       e->name, (int)pid,
						       comm,
						       existing);
				pidset_add(&ctx->attached, pid, existing,
					   e->cls, e->resolved.groups, 0);
				(void)apply_pid_uclamp(ctx, pid, e->cls,
					       class_str(e->cls), 0);
				return 1;
			}
		}

		if (dry_run) {
			lpmd_log_debug("[%s] (dry) would attach pid %d comm=%s -> %s\n",
				       e->name, (int)pid, comm, unit);
			return 1;
		}

		if (start_scope_for_pid(unit, pid, mask, mlen) == 0) {
			lpmd_log_debug("[%s] attached pid %d comm=%s -> %s (event)\n",
				       e->name, (int)pid, comm, unit);
			pidset_add(&ctx->attached, pid, unit, e->cls,
				   e->resolved.groups, 0);
			(void)apply_pid_uclamp(ctx, pid, e->cls,
			       class_str(e->cls), 0);
			return 1;
		}
		return -1;
	}
	/* No named entry matched -- try the catch-all <DefaultProcess>. */
	return apply_default_to_pid(ctx, pid, 1, dry_run);
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
			pidset_add(&ctx->attached, pid, "", e->cls,
				   e->resolved.groups, owner_uid);
			(void)apply_pid_uclamp(ctx, pid, e->cls,
			       class_str(e->cls), 1);
			return 1;
		}
	} else if (set_pid_affinity_from_mask(pid, mask, mlen) == 0) {
		lpmd_log_debug("[*default*] affinity pid %d comm=%s (uid=%u%s)\n",
			       (int)pid, comm,
			       (unsigned)owner_uid,
			       from_event ? " event" : "");
		pidset_add(&ctx->attached, pid, "", e->cls, e->resolved.groups,
			   owner_uid);
		(void)apply_pid_uclamp(ctx, pid, e->cls, class_str(e->cls), 0);
		return 1;
	}
	return -1;
}

/* Returns 1 if @pid/@comm matches the <Name> of any explicit <Process> entry. */
static int pid_in_entries(const process_cpuset_t *ctx, pid_t pid,
				  const char *comm)
{
	for (int i = 0; i < ctx->n_entries; i++)
		if (pid_matches_name(pid, ctx->entries[i].name, comm))
			return 1;
	return 0;
}

/* ---------- shutdown: stop transient scopes ---------- */

static int stop_scope_unit(const char *unit)
{
	sd_bus_error err = SD_BUS_ERROR_NULL;
	sd_bus *bus = NULL;
	int r;

	r = sd_bus_open_system(&bus);
	if (r < 0) {
		lpmd_log_debug( "sd_bus_open_system: %s\n", strerror(-r));
		goto out;
	}

	r = sd_bus_call_method(bus, "org.freedesktop.systemd1",
			       "/org/freedesktop/systemd1",
			       "org.freedesktop.systemd1.Manager", "StopUnit",
			       &err, NULL, "ss", unit, "replace");
	if (r < 0) {
		/* Unit may already be gone (process exited): not fatal. */
		const char *msg = err.message ? err.message : strerror(-r);
		if (!strstr(msg, "not loaded"))
			lpmd_log_debug( "StopUnit(%s) failed: %s\n", unit, msg);
	}

out:
	sd_bus_error_free(&err);
	sd_bus_unref(bus);
	return r < 0 ? -1 : 0;
}

int process_cpuset_stop_all(process_cpuset_t *ctx)
{
	int stopped = 0;

	if (!ctx)
		return -1;

	for (size_t i = 0; i < ctx->attached.n; i++) {
		if (ctx->attached.items[i].unit[0] &&
		    stop_scope_unit(ctx->attached.items[i].unit) == 0)
			stopped++;
	}

	/* Clear the tracking set; the scopes (if still alive) are gone now. */
	ctx->attached.n = 0;
	return stopped;
}

/*
 * Move a single PID out of its current cgroup back to the root cgroup,
 * which removes it from the proc_cpuset scope without killing it.
 * Returns 0 on success, -1 on failure (PID may already be gone).
 *
 * Uses cgroup v2 unified hierarchy (/sys/fs/cgroup/cgroup.procs).
 * Writing a single PID atomically migrates it; this restores its
 * inherited (system-default) cpu affinity since the root cgroup
 * imposes no AllowedCPUs constraint.
 */
static int migrate_pid_to_root_cgroup(pid_t pid)
{
	static const char root_procs[] = "/sys/fs/cgroup/cgroup.procs";
	char buf[32];
	int fd, n, w;

	fd = open(root_procs, O_WRONLY | O_CLOEXEC);
	if (fd < 0)
		return -1;

	n = snprintf(buf, sizeof(buf), "%d\n", (int)pid);
	if (n < 0 || n >= (int)sizeof(buf)) {
		close(fd);
		return -1;
	}

	do {
		w = write(fd, buf, (size_t)n);
	} while (w < 0 && errno == EINTR);
	close(fd);

	/* ESRCH = process already gone; treat as success. */
	if (w < 0 && errno != ESRCH)
		return -1;
	return 0;
}

/*
 * Release every tracked PID back to default (no AllowedCPUs) without
 * killing it. For each scope:
 *   1. Move its PID to the root cgroup (drops the cpuset constraint).
 *   2. Stop the now-empty scope unit (no SIGTERM is delivered because
 *      cgroup.procs is empty).
 * Clears the tracking set on completion. Returns the number of PIDs
 * successfully released, or -1 on a NULL context.
 */
int process_cpuset_release_all(process_cpuset_t *ctx)
{
	int released = 0;

	if (!ctx)
		return -1;

	for (size_t i = 0; i < ctx->attached.n; i++) {
		pid_t pid = ctx->attached.items[i].pid;
		const char *unit = ctx->attached.items[i].unit;

		if (unit[0]) {
			/* Scope-managed PID: drop cpuset constraint, then stop
             * the (now empty) scope. */
			if (migrate_pid_to_root_cgroup(pid) == 0)
				released++;
			(void)stop_scope_unit(unit);
		} else {
			/* Affinity-only PID: re-allow all online CPUs. */
			cpu_set_t *all;
			long ncpus = sysconf(_SC_NPROCESSORS_ONLN);
			size_t setsize;
			if (ncpus <= 0)
				ncpus = 1;
			/* Guard CPU_ALLOC/CPU_ALLOC_SIZE math against bogus sysconf values. */
			if (ncpus > MAX_CPUS)
				ncpus = MAX_CPUS;
			all = CPU_ALLOC(ncpus);
			if (all) {
				setsize = CPU_ALLOC_SIZE(ncpus);
				CPU_ZERO_S(setsize, all);
				for (long c = 0; c < ncpus; c++)
					CPU_SET_S(c, setsize, all);
				if (sched_setaffinity(pid, setsize, all) == 0)
					released++;
				CPU_FREE(all);
			}
		}
	}

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
