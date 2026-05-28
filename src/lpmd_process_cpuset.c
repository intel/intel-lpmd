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
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

#include <libxml/parser.h>
#include <libxml/tree.h>

#define PROCESS_CPUSET_CONFIG_FILE "process_cpuset.xml"
#define PROCESS_CPUSET_USER_CONFIG_FILE "process_cpuset_user.xml"

static process_cpuset_t *g_pc_ctx;
static int g_pc_connector_fd = -1;

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
	    config->pc_class_default_utility[0] ||
	    config->pc_class_default_background[0] ||
	    config->pc_class_default_gp_cpu[0] ||
	    config->pc_class_default_gp_gpu[0] ||
	    config->pc_class_default_gp_hybrid[0]) {
		if (process_cpuset_override_class_defaults(
			    g_pc_ctx, config->pc_class_default_realtime,
			    config->pc_class_default_user_interactive,
			    config->pc_class_default_user_initiated,
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
		case PROC_EVENT_COMM:
			/* A process renamed itself (e.g. via prctl(PR_SET_NAME)).
             * comm now matches a config entry that didn't match at
             * exec time, so re-evaluate. */
			pid = msg.ev.event_data.comm.process_pid;
			break;
		default:
			continue;
		}

		(void)process_cpuset_apply_pid(g_pc_ctx, pid, 0);
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
		FILE *f;
		size_t n;
		int is_k;

		pid = strtol(de->d_name, &end, 10);
		if (*end != '\0' || pid <= 0)
			continue;

		total++;
		if (g_pc_ctx &&
		    process_cpuset_is_attached(g_pc_ctx, (pid_t)pid))
			continue;
		/* Also skip PIDs already inside a proc_cpuset_*.scope created by
         * a prior daemon run; those are bound even though this process's
         * attached set doesn't track them. */
		if (pid_in_proc_cpuset_scope(pid) == 1)
			continue;

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

		lpmd_log_msg("  pid=%ld %s comm=%s\n", pid,
			     is_k == 1 ? "[k]" : "[u]", comm);
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
		"process_cpuset: %zu PIDs bound to transient cpuset scopes\n",
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

		if (process_cpuset_attached_get_ex(g_pc_ctx, i, &pid, unit,
						   sizeof(unit), &cls, &use_p,
						   &use_e, &use_l) < 0)
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

		lpmd_log_msg(
			"  pid=%d comm=%s class=%s groups=%s cpus=[%s] unit=%s\n",
			(int)pid, comm, cls, groups_buf, cpus_buf,
			unit[0] ? unit : "<affinity>");
	}
}
