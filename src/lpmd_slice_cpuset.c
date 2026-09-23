// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * lpmd_slice_cpuset.c: slice / unit keyed CPU affinity policy
 *
 * Copyright (C) 2026 Intel Corporation. All rights reserved.
 *
 * The systemd-native counterpart to lpmd_process_cpuset.c. Policy is keyed
 * on the systemd unit or slice a task already lives in rather than on
 * /proc/<pid>/comm, and is applied with SetUnitProperties(AllowedCPUs=) to
 * that existing unit. Nothing is ever migrated between cgroups, and unit
 * identity cannot be forged by the task.
 *
 * Three things this module deliberately does not assume:
 *
 *  - That a unit accepting AllowedCPUs= means it enforces it. cpuset is
 *    not in DelegateControllers for user@<uid>.service, so the property is
 *    accepted and silently inert on app.slice / background.slice /
 *    session.slice. Enforceability is probed, not assumed.
 *
 *  - That the configured mask is what a task ends up with. The kernel gives
 *    effective = requested & cpuset-of-cgroup, and cgroup v2 falls back to
 *    the parent's set rather than leaving an effective set empty -- so a
 *    mask disjoint from the parent's does not narrow anything, it hands the
 *    unit the parent's whole set. Masks are intersected before being sent.
 *
 *  - That policy should be applied twice. A task covered by an enforceable
 *    entry is reported as covered so the per-task path can leave it alone.
 */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include <systemd/sd-bus.h>

#include "lpmd.h"
#include "process_cpuset.h"

#include <ctype.h>
#include <dirent.h>
#include <fnmatch.h>
#include <sched.h>
#include <string.h>

#include <libxml/parser.h>
#include <libxml/tree.h>

#define SLICE_CPUSET_CONFIG_FILE "slice.xml"

/* Same cap process_cpuset.c uses; masks are handed to systemd as a byte
 * array so the only requirement is that it covers get_max_cpus(). */
#define SLICE_MAX_CPUS 1024
#define CPUMASK_BYTES (SLICE_MAX_CPUS / 8)

#define SLICE_MAX_ENTRIES 128
#define SLICE_NAME_MAX 128
#define SLICE_CPULIST_MAX 256
#define SLICE_CGPATH_MAX 512

enum slice_scope {
	SLICE_SCOPE_SYSTEM = 0, /* owned by the system manager */
	SLICE_SCOPE_USER, /* inside user@<uid>.service */
};

enum slice_fallback {
	SLICE_FALLBACK_NONE = 0,
	SLICE_FALLBACK_AFFINITY,
};

struct slice_entry {
	char name[SLICE_NAME_MAX]; /* unit/slice name, fnmatch pattern */
	char cls[32]; /* classification name, as configured */
	char cores[SLICE_CPULIST_MAX]; /* explicit <Cores> override, or "" */
	enum slice_scope scope;
	enum slice_fallback fallback;
};

static struct slice_entry g_entries[SLICE_MAX_ENTRIES];
static int g_n_entries;

/*
 * Units this module has actually written, with the AllowedCPUs= value each
 * one had beforehand. restore() puts exactly that value back, so a unit
 * carrying an administrator's own AllowedCPUs= keeps it, and a unit we
 * never wrote is never touched.
 */
#define SLICE_MAX_APPLIED 256

struct slice_applied {
	char unit[SLICE_NAME_MAX];
	/* Which manager owns it: 0 = system, else that user's manager. The
	 * same unit name exists once per logged-in user (every session has
	 * its own app.slice), so the name alone does not identify what was
	 * written. */
	uid_t uid;
	uint8_t prev[CPUMASK_BYTES];
	size_t prev_len; /* 0 = property was unset */
};

static struct slice_applied g_applied[SLICE_MAX_APPLIED];
static int g_n_applied;

static struct slice_applied *applied_find(const char *unit, uid_t uid)
{
	int i;

	for (i = 0; i < g_n_applied; i++) {
		if (g_applied[i].uid == uid && !strcmp(g_applied[i].unit, unit))
			return &g_applied[i];
	}
	return NULL;
}

static void applied_record(const char *unit, uid_t uid, const uint8_t *prev,
			   size_t len)
{
	struct slice_applied *a;
	size_t ulen = strlen(unit);

	if (g_n_applied >= SLICE_MAX_APPLIED) {
		lpmd_log_warn(
			"slice_cpuset: applied-unit table full; %s will not be restored\n",
			unit);
		return;
	}
	/*
	 * Never keep a shortened name. It would not match the unit that was
	 * actually written, so restore() would put nothing back and leave that
	 * one confined after we exit. Callers hand us SLICE_NAME_MAX buffers,
	 * so this cannot trigger today; refuse loudly if that ever changes.
	 */
	if (ulen >= SLICE_NAME_MAX) {
		lpmd_log_warn(
			"slice_cpuset: unit name over %d bytes; %s will not be restored\n",
			SLICE_NAME_MAX - 1, unit);
		return;
	}
	a = &g_applied[g_n_applied++];
	memset(a, 0, sizeof(*a));
	memcpy(a->unit, unit, ulen + 1);
	a->uid = uid;
	if (prev && len) {
		if (len > sizeof(a->prev))
			len = sizeof(a->prev);
		memcpy(a->prev, prev, len);
		a->prev_len = len;
	}
}

/* Resolver-only process_cpuset context: used exclusively to turn a
 * classification into a CPU list under the active P/E/LP-E core sets.
 * It never loads a process list and never applies anything. */
static process_cpuset_t *g_resolver;

static int g_active; /* config parsed and resolver seeded */
/*
 * <UseSliceClassification>. Off by default: turning it on lets slice
 * membership override the curated process_cpuset.xml name list for any task
 * inside a classified slice, which is a deliberate policy change and not
 * something to inherit silently on upgrade.
 */
static int g_intent_classification;

/* ------------------------------------------------------------------ */
/* config parsing						      */
/* ------------------------------------------------------------------ */

static char *node_text(xmlNode *node)
{
	return (char *)xmlNodeGetContent(node);
}

static void trim(char *s)
{
	size_t len;
	char *p = s;

	while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')
		p++;
	if (p != s)
		memmove(s, p, strlen(p) + 1);
	len = strlen(s);
	while (len && (s[len - 1] == ' ' || s[len - 1] == '\t' ||
		       s[len - 1] == '\n' || s[len - 1] == '\r'))
		s[--len] = '\0';
}

static int parse_unit_node(xmlNode *unit, struct slice_entry *e)
{
	xmlNode *cur;

	memset(e, 0, sizeof(*e));
	e->scope = SLICE_SCOPE_SYSTEM;
	e->fallback = SLICE_FALLBACK_NONE;

	for (cur = unit->children; cur; cur = cur->next) {
		char *val;

		if (cur->type != XML_ELEMENT_NODE)
			continue;
		val = node_text(cur);
		if (!val)
			continue;
		trim(val);

		if (!strcmp((const char *)cur->name, "Name"))
			snprintf(e->name, sizeof(e->name), "%s", val);
		else if (!strcmp((const char *)cur->name, "Classification"))
			snprintf(e->cls, sizeof(e->cls), "%s", val);
		else if (!strcmp((const char *)cur->name, "Cores"))
			snprintf(e->cores, sizeof(e->cores), "%s", val);
		else if (!strcmp((const char *)cur->name, "Scope"))
			e->scope = !strcasecmp(val, "user") ? SLICE_SCOPE_USER :
							     SLICE_SCOPE_SYSTEM;
		else if (!strcmp((const char *)cur->name, "Fallback"))
			e->fallback = !strcasecmp(val, "affinity") ?
					      SLICE_FALLBACK_AFFINITY :
					      SLICE_FALLBACK_NONE;

		xmlFree(val);
	}

	if (!e->name[0]) {
		lpmd_log_warn("slice_cpuset: <Unit> without <Name>, ignored\n");
		return -1;
	}
	if (!e->cls[0] && !e->cores[0]) {
		lpmd_log_warn(
			"slice_cpuset: %s has neither <Classification> nor <Cores>, ignored\n",
			e->name);
		return -1;
	}
	return 0;
}

static int load_config(const char *path)
{
	xmlDoc *doc;
	xmlNode *root, *cur;

	doc = xmlReadFile(path, NULL, XML_PARSE_NOBLANKS | XML_PARSE_NOERROR);
	if (!doc)
		return -1;

	root = xmlDocGetRootElement(doc);
	if (!root || strcmp((const char *)root->name, "SliceCpusetConfig")) {
		lpmd_log_warn("slice_cpuset: %s: root is not <SliceCpusetConfig>\n",
			      path);
		xmlFreeDoc(doc);
		return -1;
	}

	g_n_entries = 0;
	for (cur = root->children; cur; cur = cur->next) {
		if (cur->type != XML_ELEMENT_NODE)
			continue;
		if (strcmp((const char *)cur->name, "Unit"))
			continue;
		if (g_n_entries >= SLICE_MAX_ENTRIES) {
			lpmd_log_warn("slice_cpuset: more than %d entries, rest ignored\n",
				      SLICE_MAX_ENTRIES);
			break;
		}
		if (parse_unit_node(cur, &g_entries[g_n_entries]) == 0)
			g_n_entries++;
	}

	xmlFreeDoc(doc);
	return 0;
}

/* ------------------------------------------------------------------ */
/* mask helpers							      */
/* ------------------------------------------------------------------ */

/* Parse a cpuset-style list into a byte mask. Returns the number of
 * significant bytes, or 0 on error. */
static size_t cpulist_to_bytes(const char *list, uint8_t *out, size_t cap)
{
	const char *p = list;
	size_t used = 0;

	memset(out, 0, cap);
	if (!list || !*list || !strcmp(list, "-"))
		return 0;

	while (*p) {
		char *end;
		long a, b;

		while (*p == ',' || *p == ' ')
			p++;
		if (!*p)
			break;
		a = strtol(p, &end, 10);
		if (end == p)
			return 0;
		p = end;
		b = a;
		if (*p == '-') {
			p++;
			b = strtol(p, &end, 10);
			if (end == p)
				return 0;
			p = end;
		}
		if (a < 0 || b < a)
			return 0;
		for (; a <= b; a++) {
			size_t byte = (size_t)a / 8;

			if (byte >= cap)
				return 0;
			out[byte] |= (uint8_t)(1u << ((unsigned)a % 8));
			if (byte + 1 > used)
				used = byte + 1;
		}
	}
	return used;
}

static void bytes_to_cpulist(const uint8_t *mask, size_t len, char *buf,
			     size_t cap)
{
	size_t off = 0;
	int nbits = (int)(len * 8);
	int i = 0;

	buf[0] = '\0';
	while (i < nbits && off + 1 < cap) {
		int first, last, w;

		if (!(mask[i / 8] & (1u << (i % 8)))) {
			i++;
			continue;
		}
		first = last = i;
		while (last + 1 < nbits &&
		       (mask[(last + 1) / 8] & (1u << ((last + 1) % 8))))
			last++;
		if (first == last)
			w = snprintf(buf + off, cap - off, "%s%d",
				     off ? "," : "", first);
		else
			w = snprintf(buf + off, cap - off, "%s%d-%d",
				     off ? "," : "", first, last);
		if (w < 0 || (size_t)w >= cap - off)
			break;
		off += w;
		i = last + 1;
	}
	if (!off)
		snprintf(buf, cap, "-");
}

/* Read a cgroup file and parse it as a cpuset list. */
static int read_cgroup_cpulist(const char *cgpath, const char *file, char *out,
			       size_t cap)
{
	char path[SLICE_CGPATH_MAX + 64];
	FILE *f;

	snprintf(path, sizeof(path), "/sys/fs/cgroup%s/%s",
		 strcmp(cgpath, "/") ? cgpath : "", file);
	f = fopen(path, "r");
	if (!f)
		return -1;
	out[0] = '\0';
	if (fgets(out, cap, f)) {
		size_t len = strlen(out);

		if (len && out[len - 1] == '\n')
			out[len - 1] = '\0';
	}
	fclose(f);
	return out[0] ? 0 : -1;
}

/* ------------------------------------------------------------------ */
/* unit -> cgroup path						      */
/* ------------------------------------------------------------------ */

/*
 * D-Bus interface exposing ControlGroup for a unit.
 *
 * ControlGroup is NOT on org.freedesktop.systemd1.Unit -- it lives on the
 * type-specific interface, so the right one has to be picked from the unit
 * suffix. Unit types with no cgroup of their own (.target, .timer, .path,
 * .device, .automount) return NULL and are reported as not applicable.
 */
/* ------------------------------------------------------------------ */
/* reaching the right manager					      */
/* ------------------------------------------------------------------ */

/*
 * Every unit belongs to exactly one systemd manager, and only that manager
 * can be asked about it or told to change it. @uid names the manager: 0 is
 * the system manager, anything else the per-user manager running as that
 * uid.
 *
 * Reachability and enforceability are separate questions and this module
 * answers them separately: reachability here, enforceability in
 * unit_cpuset_enforceable(), which probes the cgroup rather than assuming.
 * A user manager currently fails both -- see the body for reachability, and
 * the missing cpuset delegation for the other.
 */

/*
 * A per-uid negative cache for the above. Keyed by uid, aged out so a user
 * manager that starts accepting us (or a different one reusing the uid after
 * a re-login) is not written off for the life of the daemon.
 */
#define BUS_REFUSED_TTL 60 /* seconds */

/*
 * Returned when the cache answered and no connection was attempted, so a
 * caller can say so rather than reporting a refusal that never happened.
 * ESHUTDOWN is not something connect(2) on an AF_UNIX socket or sd_bus_start
 * produces, so it cannot be confused with a real failure.
 */
#define SLICE_EBUS_CACHED ESHUTDOWN

static struct {
	uid_t uid;
	time_t when;
} g_bus_refused[8];

static int bus_refused_recently(uid_t uid)
{
	time_t now = time(NULL);
	size_t i;

	for (i = 0; i < sizeof(g_bus_refused) / sizeof(g_bus_refused[0]); i++) {
		if (!g_bus_refused[i].when || g_bus_refused[i].uid != uid)
			continue;
		if (now - g_bus_refused[i].when < BUS_REFUSED_TTL)
			return 1;
		g_bus_refused[i].when = 0; /* expired: let it retry */
		return 0;
	}
	return 0;
}

static void bus_refused_note(uid_t uid)
{
	size_t n = sizeof(g_bus_refused) / sizeof(g_bus_refused[0]);
	size_t oldest = 0;
	size_t i;

	for (i = 0; i < n; i++) {
		if (!g_bus_refused[i].when || g_bus_refused[i].uid == uid) {
			g_bus_refused[i].uid = uid;
			g_bus_refused[i].when = time(NULL);
			return;
		}
		if (g_bus_refused[i].when < g_bus_refused[oldest].when)
			oldest = i;
	}
	/* Full: evict the stalest entry rather than stop caching. */
	g_bus_refused[oldest].uid = uid;
	g_bus_refused[oldest].when = time(NULL);
}

static int slice_bus_open(uid_t uid, sd_bus **bus)
{
	char addr[64];
	sd_bus *b = NULL;
	int ret;

	if (!uid)
		return sd_bus_open_system(bus);

	/*
	 * A user manager's bus is reachable in principle -- the socket is
	 * world-writable -- but dbus-broker authenticates the peer uid and
	 * accepts only the bus owner. root included: connecting from here
	 * gets the connection reset during the EXTERNAL handshake (verified
	 * on Fedora 44 / dbus-broker, where even "busctl --user" as root
	 * fails with EPIPE).
	 *
	 * The only supported way in is sd_bus_open_user_machine(), which
	 * spawns "systemd-run -pUser=... -pPAMName=login
	 * systemd-stdio-bridge --user" -- a fork and a PAM login session per
	 * connection. That is not something this daemon should be doing on
	 * every low-power transition, so we do not: user-scope units are
	 * probed through the filesystem instead (see
	 * user_unit_control_group) and written by nobody. Their <Fallback>
	 * governs their tasks.
	 *
	 * The attempt is still made, so the log says what happened rather
	 * than silently assuming, and so this starts working by itself on a
	 * system whose broker does allow it. But it is made sparingly: a
	 * refusal is remembered per uid, because otherwise every unit of
	 * every user entry retries a connection known to fail, on every
	 * transition, and the reason lands in the log each time.
	 */
	if (bus_refused_recently(uid))
		return -SLICE_EBUS_CACHED;

	ret = sd_bus_new(&b);
	if (ret < 0)
		return ret;

	/* sd_bus_open_user() would find *our* bus, not this uid's. */
	snprintf(addr, sizeof(addr), "unix:path=/run/user/%u/bus",
		 (unsigned)uid);
	ret = sd_bus_set_address(b, addr);
	if (ret >= 0)
		ret = sd_bus_set_bus_client(b, 1);
	if (ret >= 0)
		ret = sd_bus_start(b);
	if (ret < 0) {
		sd_bus_unref(b);
		bus_refused_note(uid);
		return ret;
	}

	*bus = b;
	return 0;
}

/*
 * The per-user managers we can actually talk to, found from the bus sockets
 * under /run/user. A socket exists exactly while that user's manager is
 * running, which is the condition that matters -- logind would also report
 * users whose manager has already gone away.
 *
 * Returns the number of uids written to @out.
 */
static int active_user_uids(uid_t *out, int max)
{
	DIR *d;
	struct dirent *de;
	int n = 0;

	d = opendir("/run/user");
	if (!d)
		return 0;

	while (n < max && (de = readdir(d))) {
		char path[128];
		unsigned long uid;
		char *end;

		if (!isdigit((unsigned char)de->d_name[0]))
			continue;
		uid = strtoul(de->d_name, &end, 10);
		if (*end || !uid)
			continue;

		snprintf(path, sizeof(path), "/run/user/%lu/bus", uid);
		if (access(path, F_OK) < 0)
			continue;

		out[n++] = (uid_t)uid;
	}
	closedir(d);
	return n;
}

/*
 * The uid of the manager owning the units along @cgpath: a path containing
 * user@<uid>.service belongs to that user's manager, everything else to the
 * system manager.
 */
static uid_t cgpath_manager_uid(const char *cgpath)
{
	const char *p = strstr(cgpath, "/user@");
	unsigned long uid;
	char *end;

	if (!p)
		return 0;
	uid = strtoul(p + 6, &end, 10);
	if (strncmp(end, ".service", 8))
		return 0;
	return (uid_t)uid;
}

/*
 * The set of managers an entry applies to. A system entry names one unit on
 * the system manager; a user entry names one unit *per logged-in user*, so it
 * expands to every active user manager.
 *
 * Returns the number of uids written to @out.
 */
static int entry_manager_uids(const struct slice_entry *e, uid_t *out, int max)
{
	if (e->scope == SLICE_SCOPE_SYSTEM) {
		if (max < 1)
			return 0;
		out[0] = 0;
		return 1;
	}
	return active_user_uids(out, max);
}

static const char *unit_cgroup_interface(const char *unit)
{
	static const struct {
		const char *suffix;
		const char *iface;
	} map[] = {
		{ ".service", "org.freedesktop.systemd1.Service" },
		{ ".slice",   "org.freedesktop.systemd1.Slice"   },
		{ ".scope",   "org.freedesktop.systemd1.Scope"   },
		{ ".socket",  "org.freedesktop.systemd1.Socket"  },
		{ ".mount",   "org.freedesktop.systemd1.Mount"   },
		{ ".swap",    "org.freedesktop.systemd1.Swap"    },
	};
	const char *dot = strrchr(unit, '.');
	size_t i;

	if (!dot)
		return NULL;
	for (i = 0; i < sizeof(map) / sizeof(map[0]); i++) {
		if (!strcmp(dot, map[i].suffix))
			return map[i].iface;
	}
	return NULL;
}

/*
 * Locate a user manager's unit cgroup without asking it, for when its bus
 * will not accept us (the normal case: see slice_bus_open).
 *
 * systemd's naming is mechanical, so the path can be reconstructed: the
 * manager's own cgroup is /user.slice/user-<uid>.slice/user@<uid>.service,
 * and a unit directly under it is a directory of the same name. That covers
 * app.slice, session.slice and background.slice -- the units a desktop
 * policy actually names. A nested unit (app-foo.slice inside app.slice)
 * would need the manager to resolve it, so it is searched for one level
 * down before giving up.
 *
 * Returns 0 and fills @out with a cgroup path that exists, else -1.
 */
static int user_unit_control_group(const char *unit, uid_t uid, char *out,
				  size_t cap)
{
	char base[SLICE_CGPATH_MAX];
	char cg[SLICE_CGPATH_MAX];
	char abs[SLICE_CGPATH_MAX + 32];
	DIR *d;
	struct dirent *de;

	out[0] = '\0';
	snprintf(base, sizeof(base),
		 "/user.slice/user-%u.slice/user@%u.service", (unsigned)uid,
		 (unsigned)uid);

	/* A path that did not fit is a path that cannot exist, so treat
	 * truncation the same as "not there" rather than testing a
	 * shortened name that might match something else. */
	if (snprintf(cg, sizeof(cg), "%s/%s", base, unit) < (int)sizeof(cg)) {
		snprintf(abs, sizeof(abs), "/sys/fs/cgroup%s", cg);
		if (!access(abs, F_OK)) {
			snprintf(out, cap, "%s", cg);
			return 0;
		}
	}

	/* One level down, for units parented to a slice of their own. */
	snprintf(abs, sizeof(abs), "/sys/fs/cgroup%s", base);
	d = opendir(abs);
	if (!d)
		return -1;
	while ((de = readdir(d))) {
		if (de->d_name[0] == '.')
			continue;
		if (snprintf(cg, sizeof(cg), "%s/%s/%s", base, de->d_name,
			     unit) >= (int)sizeof(cg))
			continue;
		snprintf(abs, sizeof(abs), "/sys/fs/cgroup%s", cg);
		if (!access(abs, F_OK)) {
			snprintf(out, cap, "%s", cg);
			closedir(d);
			return 0;
		}
	}
	closedir(d);
	return -1;
}

/*
 * Ask the manager owning @uid's units for a unit's ControlGroup. Returns 0
 * and fills @out on success; -1 if the unit is not loaded, is of a type with
 * no cgroup, or has no cgroup yet.
 */
static int unit_control_group(const char *unit, uid_t uid, char *out,
			      size_t cap)
{
	sd_bus_error error = SD_BUS_ERROR_NULL;
	sd_bus_message *reply = NULL;
	sd_bus *bus = NULL;
	const char *iface;
	const char *cg = NULL;
	char *obj = NULL;
	int ret;

	out[0] = '\0';

	iface = unit_cgroup_interface(unit);
	if (!iface) {
		lpmd_log_debug("slice_cpuset: %s: unit type has no cgroup\n",
			       unit);
		return -1;
	}

	ret = slice_bus_open(uid, &bus);
	if (ret < 0) {
		if (ret == -SLICE_EBUS_CACHED)
			lpmd_log_debug(
				"slice_cpuset: %s: uid %u's bus known unreachable; not retried\n",
				unit, (unsigned)uid);
		else
			lpmd_log_debug("slice_cpuset: %s: bus open (uid=%u): %s\n",
				       unit, (unsigned)uid, strerror(-ret));
		goto fallback;
	}

	ret = sd_bus_call_method(bus, "org.freedesktop.systemd1",
				 "/org/freedesktop/systemd1",
				 "org.freedesktop.systemd1.Manager",
				 "GetUnit", &error, &reply, "s", unit);
	if (ret < 0) {
		lpmd_log_debug("slice_cpuset: %s: GetUnit: %s\n", unit,
			       error.message ? error.message : strerror(-ret));
		goto out;
	}
	ret = sd_bus_message_read(reply, "o", &obj);
	if (ret < 0)
		goto out;

	ret = sd_bus_get_property_string(bus, "org.freedesktop.systemd1", obj,
					 iface, "ControlGroup", &error,
					 (char **)&cg);
	if (ret < 0) {
		lpmd_log_debug("slice_cpuset: %s: get ControlGroup on %s: %s\n",
			       unit, iface,
			       error.message ? error.message : strerror(-ret));
		goto out;
	}

	snprintf(out, cap, "%s", cg && *cg ? cg : "");
	free((void *)cg);
	ret = out[0] ? 0 : -1;

out:
	sd_bus_error_free(&error);
	sd_bus_message_unref(reply);
	sd_bus_unref(bus);
	if (ret >= 0)
		return 0;

fallback:
	/*
	 * A user manager is expected to be unreachable (see slice_bus_open),
	 * so locate the cgroup ourselves rather than reporting the unit as
	 * absent. The failure can surface either at open or at the first
	 * method call -- sd_bus_start does not always complete the handshake
	 * before returning -- so both land here, and both count as a refusal
	 * worth remembering.
	 */
	if (uid) {
		bus_refused_note(uid);
		return user_unit_control_group(unit, uid, out, cap);
	}
	return -1;
}

/* Does @cgpath's cgroup.controllers advertise the cpuset controller? */
static int cgroup_has_cpuset_controller(const char *cgpath)
{
	char list[512];
	char *tok, *save;

	if (read_cgroup_cpulist(cgpath, "cgroup.controllers", list,
				sizeof(list)) < 0)
		return 0;

	for (tok = strtok_r(list, " \t", &save); tok;
	     tok = strtok_r(NULL, " \t", &save)) {
		if (!strcmp(tok, "cpuset"))
			return 1;
	}
	return 0;
}

/*
 * Is AllowedCPUs= on this unit actually enforced?
 *
 * The test is whether the cpuset controller is *available* to the unit's
 * parent, i.e. whether "cpuset" appears in the parent's cgroup.controllers.
 * That is what decides whether systemd can enable cpuset for this unit.
 *
 * Checking for cpuset.cpus files in the unit's own directory is NOT enough:
 * systemd only enables a controller in a subtree once something asks for it,
 * so a unit that has never had AllowedCPUs= set has no cpuset files even
 * though setting the property would work. Verified on Fedora 44 /
 * systemd 259: a service under a slice whose cgroup.controllers lists
 * "cpuset memory pids" has only "memory pids" of its own until written.
 *
 * On a stock system everything under user@<uid>.service reports
 * "cpu io memory pids" with no cpuset at any depth, because systemd's
 * DelegateControllers for that unit excludes cpuset -- there AllowedCPUs=
 * is accepted and silently inert, and this returns 0.
 *
 * That is a property of the running system, not of <Scope>: an admin who
 * adds "Delegate=cpu cpuset io memory pids" to user@.service makes the
 * user slices enforceable, and the same probe then returns 1 for them. So
 * the question is always asked of the cgroup, never inferred from config.
 *
 * Returns 1 enforceable, 0 not enforceable, -1 unit not loaded.
 */
static int unit_cpuset_enforceable(const char *unit, uid_t uid, char *cgpath,
				   size_t cap)
{
	char parent[SLICE_CGPATH_MAX];
	char *slash;

	if (unit_control_group(unit, uid, cgpath, cap) < 0)
		return -1;

	/* Already has its own cpuset files: unambiguously enforceable. */
	if (cgroup_has_cpuset_controller(cgpath))
		return 1;

	snprintf(parent, sizeof(parent), "%s", cgpath);
	slash = strrchr(parent, '/');
	if (!slash)
		return 0;
	if (slash == parent)
		parent[1] = '\0'; /* root */
	else
		*slash = '\0';

	return cgroup_has_cpuset_controller(parent);
}

/*
 * The same question, asked of a cgroup path instead of a unit name: is
 * AllowedCPUs= enforced on the cgroup @unit owns, where @unit matched a
 * component of @cgpath?
 *
 * The PID-keyed callers below already read the task's cgroup path out of
 * /proc, and the unit they matched is by construction a component of it, so
 * truncating the path after that component gives the unit's own cgroup
 * directly. That spares a GetUnit round trip per PID -- on a /proc sweep of a
 * few hundred tasks the D-Bus traffic is the whole cost of the question -- and
 * it answers it about the cgroup the task is actually in rather than about
 * whatever GetUnit resolves the name to.
 *
 * Returns 1 enforceable, 0 not.
 */
static int cgpath_cpuset_enforceable(const char *cgpath, const char *unit)
{
	char own[SLICE_CGPATH_MAX];
	char parent[SLICE_CGPATH_MAX];
	char *p, *slash;
	size_t ulen;

	if (!cgpath || !*cgpath || !unit || !*unit)
		return 0;

	ulen = strlen(unit);
	snprintf(own, sizeof(own), "%s", cgpath);

	/* Cut after @unit where it appears as a whole component, so a unit
	 * whose name is a substring of a longer one cannot match. */
	for (p = own; (p = strstr(p, unit)); p++) {
		if (p != own && p[-1] != '/')
			continue;
		if (p[ulen] != '/' && p[ulen] != '\0')
			continue;
		p[ulen] = '\0';
		break;
	}
	if (!p)
		return 0;

	if (cgroup_has_cpuset_controller(own))
		return 1;

	snprintf(parent, sizeof(parent), "%s", own);
	slash = strrchr(parent, '/');
	if (!slash)
		return 0;
	if (slash == parent)
		parent[1] = '\0'; /* root */
	else
		*slash = '\0';

	return cgroup_has_cpuset_controller(parent);
}

/* ------------------------------------------------------------------ */
/* resolving an entry to a CPU list				      */
/* ------------------------------------------------------------------ */

/*
 * Expand an explicit <Cores> value: a comma- or space-separated mix of the
 * named groups (ActivePcores / ActiveEcores / ActiveLcores) and literal
 * cpuset lists, unioned together. Same vocabulary <ActiveCores> accepts in
 * process_cpuset.xml.
 */
static int expand_cores_spec(const char *spec, char *out, size_t cap)
{
	char p_cores[SLICE_CPULIST_MAX] = { 0 };
	char e_cores[SLICE_CPULIST_MAX] = { 0 };
	char l_cores[SLICE_CPULIST_MAX] = { 0 };
	uint8_t acc[CPUMASK_BYTES] = { 0 };
	char work[SLICE_CPULIST_MAX];
	size_t used = 0, i;
	char *tok, *save;

	if (process_cpuset_groups_get(g_resolver, p_cores, sizeof(p_cores),
				      e_cores, sizeof(e_cores), l_cores,
				      sizeof(l_cores)) < 0)
		return -1;

	snprintf(work, sizeof(work), "%s", spec);
	for (tok = strtok_r(work, ", \t", &save); tok;
	     tok = strtok_r(NULL, ", \t", &save)) {
		uint8_t m[CPUMASK_BYTES];
		const char *list = tok;
		size_t len;

		if (!strcasecmp(tok, "ActivePcores") || !strcasecmp(tok, "Pcores"))
			list = p_cores;
		else if (!strcasecmp(tok, "ActiveEcores") ||
			 !strcasecmp(tok, "Ecores"))
			list = e_cores;
		else if (!strcasecmp(tok, "ActiveLcores") ||
			 !strcasecmp(tok, "LPEcores") ||
			 !strcasecmp(tok, "Lcores"))
			list = l_cores;

		len = cpulist_to_bytes(list, m, sizeof(m));
		for (i = 0; i < len; i++) {
			acc[i] |= m[i];
			if (acc[i] && i + 1 > used)
				used = i + 1;
		}
	}

	if (!used)
		return -1;
	bytes_to_cpulist(acc, used, out, cap);
	return 0;
}

/*
 * Resolve the CPU list an entry asks for: an explicit <Cores> value wins,
 * otherwise the classification's default under the active core groups.
 */
static int entry_want_cpulist(const struct slice_entry *e, char *out,
			      size_t cap)
{
	if (!g_resolver)
		return -1;
	if (e->cores[0])
		return expand_cores_spec(e->cores, out, cap);
	return process_cpuset_get_class_cpulist(g_resolver, e->cls, out, cap);
}

/*
 * Intersect @want with the ceiling the parent cgroup already imposes.
 *
 * This matters because the kernel does not fail a disjoint request: cgroup
 * v2 refuses to leave a cgroup with an empty effective set and falls back
 * to the parent's, so writing a disjoint mask hands the unit the parent's
 * *whole* set -- the opposite of the intent. An empty intersection is
 * reported as such so the caller can skip the write entirely.
 *
 * Returns the number of significant bytes in @out, or 0 if the
 * intersection is empty.
 */
static size_t clamp_to_parent(const char *cgpath, const char *want,
			      uint8_t *out, size_t cap, int *clamped)
{
	uint8_t want_mask[CPUMASK_BYTES] = { 0 };
	uint8_t ceil_mask[CPUMASK_BYTES] = { 0 };
	char parent[SLICE_CGPATH_MAX];
	char ceil_list[SLICE_CPULIST_MAX];
	size_t want_len, ceil_len, i, used = 0;
	char *slash;

	*clamped = 0;
	want_len = cpulist_to_bytes(want, want_mask, sizeof(want_mask));
	if (!want_len)
		return 0;

	/* Walk up from the unit's own cgroup to the nearest ancestor that
	 * has a cpuset; that is the ceiling that will actually apply. */
	snprintf(parent, sizeof(parent), "%s", cgpath);
	for (;;) {
		if (!strcmp(parent, "/")) {
			if (read_cgroup_cpulist(parent, "cpuset.cpus.effective",
						ceil_list,
						sizeof(ceil_list)) < 0)
				ceil_list[0] = '\0';
			break;
		}
		slash = strrchr(parent, '/');
		if (!slash)
			break;
		if (slash == parent)
			parent[1] = '\0';
		else
			*slash = '\0';
		if (read_cgroup_cpulist(parent, "cpuset.cpus.effective",
					ceil_list, sizeof(ceil_list)) == 0)
			break;
	}

	ceil_len = ceil_list[0] ?
			   cpulist_to_bytes(ceil_list, ceil_mask,
					    sizeof(ceil_mask)) :
			   0;
	if (!ceil_len) {
		/* No ceiling found: pass the request through unchanged. */
		memcpy(out, want_mask, cap < sizeof(want_mask) ?
						cap :
						sizeof(want_mask));
		return want_len;
	}

	memset(out, 0, cap);
	for (i = 0; i < cap && i < sizeof(want_mask); i++) {
		uint8_t c = i < ceil_len ? ceil_mask[i] : 0;

		out[i] = (uint8_t)(want_mask[i] & c);
		if (want_mask[i] & (uint8_t)~c)
			*clamped = 1;
		if (out[i])
			used = i + 1;
	}
	return used;
}

/* ------------------------------------------------------------------ */
/* applying							      */
/* ------------------------------------------------------------------ */

/*
 * Read a unit's current AllowedCPUs=. Returns the number of bytes written
 * to @out (0 when the property is unset, which is the common case), or -1
 * if it cannot be read at all.
 */
static ssize_t unit_get_allowed_cpus(const char *unit, uid_t uid, uint8_t *out,
				     size_t cap)
{
	sd_bus_error error = SD_BUS_ERROR_NULL;
	sd_bus_message *reply = NULL;
	sd_bus *bus = NULL;
	const char *iface;
	const void *data = NULL;
	char *obj = NULL;
	size_t n = 0;
	ssize_t res = -1;
	int ret;

	iface = unit_cgroup_interface(unit);
	if (!iface)
		return -1;
	if (slice_bus_open(uid, &bus) < 0)
		return -1;

	ret = sd_bus_call_method(bus, "org.freedesktop.systemd1",
				 "/org/freedesktop/systemd1",
				 "org.freedesktop.systemd1.Manager", "GetUnit",
				 &error, &reply, "s", unit);
	if (ret < 0)
		goto out;
	if (sd_bus_message_read(reply, "o", &obj) < 0)
		goto out;
	sd_bus_message_unref(reply);
	reply = NULL;

	ret = sd_bus_get_property(bus, "org.freedesktop.systemd1", obj, iface,
				  "AllowedCPUs", &error, &reply, "ay");
	if (ret < 0) {
		lpmd_log_debug("slice_cpuset: %s: get AllowedCPUs: %s\n", unit,
			       error.message ? error.message : strerror(-ret));
		goto out;
	}
	if (sd_bus_message_read_array(reply, 'y', &data, &n) < 0)
		goto out;

	if (n > cap)
		n = cap;
	memset(out, 0, cap);
	if (n)
		memcpy(out, data, n);
	res = (ssize_t)n;

out:
	sd_bus_error_free(&error);
	sd_bus_message_unref(reply);
	sd_bus_unref(bus);
	return res;
}

/*
 * SetUnitProperties(unit, AllowedCPUs=mask) with runtime=true, so nothing
 * is persisted and a reboot returns the system to distro policy.
 */
static int set_unit_allowed_cpus(const char *unit, uid_t uid,
				 const uint8_t *vals, size_t size)
{
	sd_bus_error error = SD_BUS_ERROR_NULL;
	sd_bus_message *m = NULL;
	sd_bus *bus = NULL;
	int ret;

	ret = slice_bus_open(uid, &bus);
	if (ret < 0)
		goto finish;

	ret = sd_bus_message_new_method_call(
		bus, &m, "org.freedesktop.systemd1",
		"/org/freedesktop/systemd1",
		"org.freedesktop.systemd1.Manager", "SetUnitProperties");
	if (ret < 0)
		goto finish;

	/* runtime = 1: never write a persistent drop-in. */
	ret = sd_bus_message_append(m, "sb", unit, 1);
	if (ret < 0)
		goto finish;
	ret = sd_bus_message_open_container(m, SD_BUS_TYPE_ARRAY, "(sv)");
	if (ret < 0)
		goto finish;
	ret = sd_bus_message_open_container(m, SD_BUS_TYPE_STRUCT, "sv");
	if (ret < 0)
		goto finish;
	ret = sd_bus_message_append_basic(m, SD_BUS_TYPE_STRING, "AllowedCPUs");
	if (ret < 0)
		goto finish;
	ret = sd_bus_message_open_container(m, 'v', "ay");
	if (ret < 0)
		goto finish;
	ret = sd_bus_message_append_array(m, 'y', vals, size);
	if (ret < 0)
		goto finish;

	sd_bus_message_close_container(m); /* v */
	sd_bus_message_close_container(m); /* sv */
	sd_bus_message_close_container(m); /* a(sv) */

	ret = sd_bus_call(bus, m, 0, &error, NULL);
	if (ret < 0)
		lpmd_log_info(
			"slice_cpuset: %s (uid=%u): SetUnitProperties failed: %s\n",
			unit, (unsigned)uid,
			error.message ? error.message : "?");

finish:
	sd_bus_error_free(&error);
	sd_bus_message_unref(m);
	sd_bus_unref(bus);
	return ret < 0 ? -1 : 0;
}

/*
 * Expand an entry's <Name> to the loaded units it matches on the manager
 * owning @uid's units. A literal name resolves to itself; a glob is matched
 * against that manager's unit list. Returns the number of names written to
 * @out.
 */
static int expand_entry_units(const struct slice_entry *e, uid_t uid,
			      char out[][SLICE_NAME_MAX], int max)
{
	sd_bus_error error = SD_BUS_ERROR_NULL;
	sd_bus_message *reply = NULL;
	sd_bus *bus = NULL;
	int n = 0;

	if (!strpbrk(e->name, "*?[")) {
		snprintf(out[0], SLICE_NAME_MAX, "%s", e->name);
		return 1;
	}

	if (slice_bus_open(uid, &bus) < 0)
		goto out;
	if (sd_bus_call_method(bus, "org.freedesktop.systemd1",
			       "/org/freedesktop/systemd1",
			       "org.freedesktop.systemd1.Manager", "ListUnits",
			       &error, &reply, "") < 0)
		goto out;
	if (sd_bus_message_enter_container(reply, SD_BUS_TYPE_ARRAY,
					   "(ssssssouso)") < 0)
		goto out;

	while (n < max) {
		const char *name = NULL;
		int r;

		r = sd_bus_message_read(reply, "(ssssssouso)", &name, NULL,
					NULL, NULL, NULL, NULL, NULL, NULL,
					NULL, NULL);
		if (r <= 0)
			break;
		if (name && !fnmatch(e->name, name, 0))
			snprintf(out[n++], SLICE_NAME_MAX, "%s", name);
	}
	sd_bus_message_exit_container(reply);

out:
	sd_bus_error_free(&error);
	sd_bus_message_unref(reply);
	sd_bus_unref(bus);
	return n;
}

/* ------------------------------------------------------------------ */
/* public API							      */
/* ------------------------------------------------------------------ */

int lpmd_slice_cpuset_init(struct lpmd_config_t *config)
{
	char path[MAX_STR_LENGTH];

	if (!config || !config->use_slice_cpuset) {
		/* Intent classification reads slice.xml, so it cannot do
		 * anything on its own. Say so rather than look enabled. */
		if (config && config->use_slice_classification)
			lpmd_log_warn(
				"slice_cpuset: UseSliceClassification needs UseSliceCpuset=1; ignored\n");
		return 0;
	}

	snprintf(path, sizeof(path), "%s/%s", TDCONFDIR,
		 SLICE_CPUSET_CONFIG_FILE);
	if (load_config(path) < 0) {
		lpmd_log_info("slice_cpuset: no usable %s; disabled\n", path);
		return 0;
	}

	/*
	 * Build the resolver the same way the process path does, so that a
	 * <Classification> here means exactly what the same name means in
	 * process_cpuset.xml. No <Process> entries are wanted: this resolver
	 * exists only to turn a class name into a cpulist.
	 *
	 * This used to be a bare process_cpuset_new() plus the core masks,
	 * which skipped the per-CPU-model <ClassDefaults> overlay. An entry
	 * was then written to the cgroup using the built-in mask for its tier
	 * even where the config had overridden that tier -- so gdm.service
	 * and pipewire.service got masks nobody had configured, and
	 * LIST-SLICES reported them as if they were the configured ones.
	 */
	g_resolver = lpmd_class_resolver_new(config, NULL, NULL, NULL);
	if (!g_resolver) {
		g_n_entries = 0;
		return -1;
	}

	g_active = 1;
	g_intent_classification = config->use_slice_classification ? 1 : 0;
	lpmd_log_info("slice_cpuset: %d entries from %s\n", g_n_entries, path);
	if (g_intent_classification && !config->use_process_cpuset) {
		/* Nothing acts on an intent but the per-task path, so without it
		 * the setting is inert. Say so rather than look enabled. */
		lpmd_log_warn(
			"slice_cpuset: UseSliceClassification needs UseProcessCPUSet=1 to have any effect\n");
		g_intent_classification = 0;
	}
	if (g_intent_classification)
		lpmd_log_info(
			"slice_cpuset: intent classification on; slice membership outranks process names\n");
	return 0;
}

void lpmd_slice_cpuset_uninit(void)
{
	if (g_resolver) {
		process_cpuset_free(g_resolver);
		g_resolver = NULL;
	}
	g_n_entries = 0;
	g_active = 0;
	g_intent_classification = 0;
}

/*
 * Resolve which configured entry covers @pid: the longest unit name
 * appearing in the PID's cgroup path wins, so a leaf service beats its
 * slice.
 *
 * Only entries whose <Scope> matches the manager that owns the task's cgroup
 * are considered. Unit names are not unique across managers -- background.slice
 * and session.slice exist in the user manager, and a distro may run pipewire as
 * a system service while a desktop runs it as a user one -- so a system entry
 * must not answer for a user task. Without this the longest-name rule picks
 * whichever entry has the longer name, which is how a system
 * <Name>pipewire.service</Name> entry came to outrank the session.slice entry
 * that slice.xml says should cover the user-manager copy.
 *
 * On success returns the entry and writes the matched unit name to
 * @unit_out and the PID's cgroup path to @cg_out (either may be NULL).
 * Returns NULL when no entry matches, when the cgroup cannot be read, or
 * when slice policy is inactive -- callers that must tell those apart
 * check g_active themselves.
 */
static const struct slice_entry *slice_entry_for_pid(pid_t pid, char *unit_out,
						     size_t unit_cap,
						     char *cg_out,
						     size_t cg_cap)
{
	char cgpath[SLICE_CGPATH_MAX];
	char best_unit[SLICE_NAME_MAX] = { 0 };
	const struct slice_entry *best = NULL;
	enum slice_scope scope;
	size_t best_len = 0;
	char line[SLICE_CGPATH_MAX];
	char path[64];
	FILE *f;
	int i;

	if (!g_active)
		return NULL;

	snprintf(path, sizeof(path), "/proc/%d/cgroup", (int)pid);
	f = fopen(path, "r");
	if (!f)
		return NULL;
	cgpath[0] = '\0';
	while (fgets(line, sizeof(line), f)) {
		size_t len;

		if (strncmp(line, "0::", 3))
			continue;
		len = strlen(line);
		if (len && line[len - 1] == '\n')
			line[len - 1] = '\0';
		snprintf(cgpath, sizeof(cgpath), "%s", line + 3);
		break;
	}
	fclose(f);
	if (!cgpath[0])
		return NULL;

	/* Inside user@<uid>.service the user's manager owns everything below;
	 * anywhere else the system manager does. */
	scope = cgpath_manager_uid(cgpath) ? SLICE_SCOPE_USER :
					     SLICE_SCOPE_SYSTEM;

	/* Match each entry against every path component. */
	for (i = 0; i < g_n_entries; i++) {
		char work[SLICE_CGPATH_MAX];
		char *tok, *save;

		if (g_entries[i].scope != scope)
			continue;

		snprintf(work, sizeof(work), "%s", cgpath);
		for (tok = strtok_r(work, "/", &save); tok;
		     tok = strtok_r(NULL, "/", &save)) {
			if (fnmatch(g_entries[i].name, tok, 0))
				continue;
			if (strlen(tok) >= best_len) {
				best_len = strlen(tok);
				best = &g_entries[i];
				snprintf(best_unit, sizeof(best_unit), "%s",
					 tok);
			}
		}
	}

	if (!best)
		return NULL;
	if (unit_out)
		snprintf(unit_out, unit_cap, "%s", best_unit);
	if (cg_out)
		snprintf(cg_out, cg_cap, "%s", cgpath);
	return best;
}

/*
 * Find the entry covering @pid, if any: the longest unit name appearing in
 * the PID's cgroup path wins, so a leaf service beats its slice.
 *
 * Returns 1 and fills the out params when covered, 0 when not, -1 when
 * slice policy is inactive or the cgroup cannot be read.
 */
int lpmd_slice_cpuset_pid_coverage(pid_t pid, char *unit_out, size_t unit_cap,
				   char *cls_out, size_t cls_cap,
				   int *enforceable_out)
{
	char cgpath[SLICE_CGPATH_MAX];
	char best_unit[SLICE_NAME_MAX] = { 0 };
	const struct slice_entry *best;

	if (!g_active)
		return -1;

	best = slice_entry_for_pid(pid, best_unit, sizeof(best_unit), cgpath,
				   sizeof(cgpath));
	if (!best)
		return 0;

	if (unit_out)
		snprintf(unit_out, unit_cap, "%s", best_unit);
	if (cls_out)
		snprintf(cls_out, cls_cap, "%s",
			 best->cores[0] ? best->cores : best->cls);
	if (enforceable_out) {
		uid_t owner = cgpath_manager_uid(cgpath);

		/*
		 * This flag tells the per-task path "slice policy has this
		 * one, leave it alone", so it has to mean *both* that the
		 * cgroup enforces AllowedCPUs= and that lpmd can set it.
		 * Claiming coverage we cannot deliver would leave the task
		 * with no policy at all: not bound by its slice, and skipped
		 * by the affinity path that would otherwise have handled it.
		 *
		 * A task under user@<uid>.service fails the second half: only
		 * that user's manager can set its units and its bus refuses a
		 * system daemon (see slice_bus_open). Such tasks are reported
		 * as not covered, so <Fallback>affinity</Fallback> applies --
		 * which is the behaviour that actually confines them.
		 */
		*enforceable_out =
			owner == 0 &&
			cgpath_cpuset_enforceable(cgpath, best_unit) == 1;
	}
	return 1;
}

/*
 * Intent-based classification: what does @pid's slice membership say the
 * task *is*, for a task the cgroup itself will not constrain?
 *
 * The premise is that a desktop which places work in background.slice has
 * already declared that work to be background, and that declaration is
 * better evidence than a process name: systemd assigns it, the task cannot
 * forge it, and it covers programs nobody thought to list. Where the
 * cgroup can enforce that itself, it does (see pid_coverage above and the
 * <Fallback> discussion in slice.xml) and this function stays out of the
 * way; where it cannot -- every slice inside user@<uid>.service, because
 * cpuset is not delegated there -- the classification is still good
 * information, and sched_setaffinity(2) can act on it.
 *
 * So this deliberately reports only the non-enforceable case, and only for
 * entries whose <Fallback> asked for affinity. An entry with
 * <Fallback>none</Fallback> is declaring "leave my tasks alone", which is
 * an answer, not an omission.
 *
 * Fills @cls_out with the configured classification name and @cores_out
 * with the entry's explicit <Cores> override ("" if it has none) -- kept
 * separate because pid_coverage() conflates them for reporting and a
 * caller acting on the result needs to tell a class from a literal CPU
 * list. @cores_out is expanded to a literal list here, so the caller does
 * not have to understand this file's <Cores> vocabulary (which accepts
 * short aliases <ActiveCores> does not); a classification name, by
 * contrast, is passed through unresolved so it resolves against the
 * caller's own <ClassDefaults> rather than this file's resolver.
 *
 * Returns 1 when an intent applies, 0 when none does, -1 when slice
 * policy is inactive or intent classification is switched off.
 */
int lpmd_slice_cpuset_pid_intent(pid_t pid, char *unit_out, size_t unit_cap,
				 char *cls_out, size_t cls_cap, char *cores_out,
				 size_t cores_cap)
{
	char cgpath[SLICE_CGPATH_MAX];
	char best_unit[SLICE_NAME_MAX] = { 0 };
	char cpus[SLICE_CPULIST_MAX];
	const struct slice_entry *best;
	uid_t owner;

	if (unit_out && unit_cap)
		unit_out[0] = '\0';
	if (cls_out && cls_cap)
		cls_out[0] = '\0';
	if (cores_out && cores_cap)
		cores_out[0] = '\0';

	if (!g_active || !g_intent_classification)
		return -1;

	best = slice_entry_for_pid(pid, best_unit, sizeof(best_unit), cgpath,
				   sizeof(cgpath));
	if (!best)
		return 0;
	if (best->fallback != SLICE_FALLBACK_AFFINITY)
		return 0; /* entry asked for its tasks to be left alone */
	if (!best->cls[0] && !best->cores[0])
		return 0; /* nothing to say about this one */

	/* Where the cgroup will do it, let the cgroup do it: applying a task
	 * affinity on top of an enforced AllowedCPUs= could only conflict. */
	owner = cgpath_manager_uid(cgpath);
	if (owner == 0 && cgpath_cpuset_enforceable(cgpath, best_unit) == 1)
		return 0;

	cpus[0] = '\0';
	if (best->cores[0] &&
	    expand_cores_spec(best->cores, cpus, sizeof(cpus)) < 0) {
		/* Unusable <Cores>. Fall back to the classification if the
		 * entry has one; otherwise there is nothing left to report. */
		cpus[0] = '\0';
		if (!best->cls[0])
			return 0;
	}

	if (unit_out)
		snprintf(unit_out, unit_cap, "%s", best_unit);
	if (cls_out)
		snprintf(cls_out, cls_cap, "%s", best->cls);
	if (cores_out)
		snprintf(cores_out, cores_cap, "%s", cpus);
	return 1;
}

/*
 * A dry run is LIST-SLICES answering a question the administrator just
 * asked, so everything it has to say goes out at message level -- an
 * explicitly requested report must not come back empty because the daemon
 * happens to be running without --loglevel=info. The same lines during a
 * real transition would repeat on every low-power entry, so there they keep
 * the level they merit on their own: info for a write, debug for a skip.
 */
#define slice_log_skip(dry_run, ...)                                           \
	do {                                                                   \
		if (dry_run)                                                   \
			lpmd_log_msg(__VA_ARGS__);                             \
		else                                                           \
			lpmd_log_debug(__VA_ARGS__);                           \
	} while (0)

#define slice_log_set(dry_run, ...)                                            \
	do {                                                                   \
		if (dry_run)                                                   \
			lpmd_log_msg(__VA_ARGS__);                             \
		else                                                           \
			lpmd_log_info(__VA_ARGS__);                            \
	} while (0)

/*
 * Apply every enforceable entry. @dry_run only logs what would be sent.
 *
 * Entries whose unit is not loaded, whose cpuset is not enforceable, or
 * whose requested mask does not intersect the parent's are skipped rather
 * than written: writing them would either do nothing or, in the disjoint
 * case, widen the unit to the parent's full set.
 */
int lpmd_slice_cpuset_apply(int dry_run)
{
	int applied = 0;
	int i;

	if (!g_active)
		return 0;

	for (i = 0; i < g_n_entries; i++) {
		char units[16][SLICE_NAME_MAX];
		char want[SLICE_CPULIST_MAX];
		uid_t uids[16];
		int n_uids, u;
		int n, j;

		if (entry_want_cpulist(&g_entries[i], want, sizeof(want)) < 0) {
			lpmd_log_warn("slice_cpuset: %s: cannot resolve %s\n",
				      g_entries[i].name,
				      g_entries[i].cores[0] ?
					      g_entries[i].cores :
					      g_entries[i].cls);
			continue;
		}

		/*
		 * A class can legitimately map to a core group this machine
		 * does not have (background -> LPEcores with no LP-E cores).
		 * There is nothing to write; writing an empty AllowedCPUs=
		 * would mean "inherit", i.e. a silent no-op.
		 */
		if (!want[0]) {
			slice_log_skip(
				dry_run,
				"slice_cpuset: %s: class %s has no CPUs on this system; skipped\n",
				g_entries[i].name,
				g_entries[i].cores[0] ? g_entries[i].cores :
							g_entries[i].cls);
			continue;
		}

		/*
		 * A system entry names one unit; a user entry names one per
		 * logged-in user, each on its own manager's bus.
		 */
		n_uids = entry_manager_uids(&g_entries[i], uids,
					    (int)(sizeof(uids) / sizeof(uids[0])));
		if (!n_uids) {
			slice_log_skip(
				dry_run,
				"slice_cpuset: %s: no manager to talk to (no active user session?)\n",
				g_entries[i].name);
			continue;
		}

		for (u = 0; u < n_uids; u++) {
		uid_t uid = uids[u];

		n = expand_entry_units(&g_entries[i], uid, units, 16);
		for (j = 0; j < n; j++) {
			uint8_t mask[CPUMASK_BYTES];
			char cgpath[SLICE_CGPATH_MAX];
			char sent[SLICE_CPULIST_MAX];
			size_t len;
			int clamped = 0;
			int enf;

			enf = unit_cpuset_enforceable(units[j], uid, cgpath,
						      sizeof(cgpath));
			if (enf < 0) {
				slice_log_skip(
					dry_run,
					"slice_cpuset: %s (uid=%u): not active (no cgroup); nothing to set\n",
					units[j], (unsigned)uid);
				continue;
			}
			if (enf == 0) {
				/* For a user slice this is the stock state:
				 * cpuset is not in DelegateControllers for
				 * user@<uid>.service, so AllowedCPUs= would
				 * be accepted and silently do nothing. The
				 * entry's <Fallback> governs its tasks. */
				slice_log_skip(
					dry_run,
					"slice_cpuset: %s (uid=%u): cpuset not enforceable (not delegated); skipped\n",
					units[j], (unsigned)uid);
				continue;
			}

			/*
			 * Enforceable but not ours to set: AllowedCPUs= on a
			 * user manager's unit can only be set by that manager,
			 * and its bus refuses us (see slice_bus_open). Say so
			 * once, clearly, instead of attempting a write that
			 * cannot succeed. <Fallback> covers these tasks.
			 */
			if (uid) {
				slice_log_skip(
					dry_run,
					"slice_cpuset: %s (uid=%u): cpuset IS enforceable but only uid %u's manager can set it; "
					"a system daemon cannot -> fallback=%s\n",
					units[j], (unsigned)uid, (unsigned)uid,
					g_entries[i].fallback ==
							SLICE_FALLBACK_AFFINITY ?
						"affinity" :
						"none");
				continue;
			}

			len = clamp_to_parent(cgpath, want, mask, sizeof(mask),
					      &clamped);
			if (!len) {
				slice_log_skip(
					dry_run,
					"slice_cpuset: %s: want=[%s] does not intersect its parent; skipped\n",
					units[j], want);
				continue;
			}
			bytes_to_cpulist(mask, len, sent, sizeof(sent));

			slice_log_set(
				dry_run,
				"slice_cpuset: %s%s%s class=%s want=[%s] send=[%s]%s\n",
				dry_run ? "would set " : "set ", units[j],
				uid ? " (user)" : "",
				g_entries[i].cores[0] ? g_entries[i].cores :
							g_entries[i].cls,
				want, sent, clamped ? " (clamped)" : "");

			if (dry_run) {
				applied++; /* counts planned writes */
				continue;
			}

			/*
			 * Snapshot what this unit had before the first write,
			 * so restore() can put it back rather than blanking
			 * a value the administrator set.
			 */
			if (!applied_find(units[j], uid)) {
				uint8_t prev[CPUMASK_BYTES];
				ssize_t plen;

				plen = unit_get_allowed_cpus(units[j], uid, prev,
							     sizeof(prev));
				applied_record(units[j], uid,
					       plen > 0 ? prev : NULL,
					       plen > 0 ? (size_t)plen : 0);
			}

			if (set_unit_allowed_cpus(units[j], uid, mask,
						  (size_t)(get_max_cpus() / 8)) == 0)
				applied++;
		}
		}
	}
	return applied;
}

/*
 * Put back exactly the AllowedCPUs= each unit had before this module first
 * wrote it. Units never written are not touched; units that had nothing set
 * are unset again (an empty byte array is systemd's "unset", restoring
 * inheritance from the parent slice).
 */
int lpmd_slice_cpuset_restore(void)
{
	int i;

	for (i = 0; i < g_n_applied; i++) {
		set_unit_allowed_cpus(g_applied[i].unit, g_applied[i].uid,
				      g_applied[i].prev_len ?
					      g_applied[i].prev :
					      NULL,
				      g_applied[i].prev_len);
	}
	g_n_applied = 0;
	return 0;
}

/*
 * Report every configured entry against the running system: whether the
 * unit exists, whether its cpuset is actually enforceable, what the entry
 * resolves to, and what the cgroup currently holds.
 */
void lpmd_slice_cpuset_print(void)
{
	int i;

	if (!g_active) {
		lpmd_log_msg(
			"slice_cpuset: not active (<UseSliceCpuset> unset or no slice.xml)\n");
		return;
	}

	lpmd_log_msg("slice_cpuset: %d configured entries\n", g_n_entries);

	for (i = 0; i < g_n_entries; i++) {
		char units[16][SLICE_NAME_MAX];
		char want[SLICE_CPULIST_MAX] = "?";
		uid_t uids[16];
		int n_uids, u;
		int n, j;

		if (entry_want_cpulist(&g_entries[i], want, sizeof(want)) < 0)
			snprintf(want, sizeof(want), "?");

		lpmd_log_msg("  %s scope=%s class=%s want=[%s] fallback=%s\n",
			     g_entries[i].name,
			     g_entries[i].scope == SLICE_SCOPE_USER ? "user" :
								      "system",
			     g_entries[i].cores[0] ? g_entries[i].cores :
						     g_entries[i].cls,
			     want,
			     g_entries[i].fallback == SLICE_FALLBACK_AFFINITY ?
				     "affinity" :
				     "none");

		/*
		 * An empty resolution is not a config error: a class can map
		 * to a core group this machine does not have (background ->
		 * LPEcores on a part with no low-power E-cores). Say so,
		 * because the entry is then skipped entirely.
		 */
		if (!want[0] || !strcmp(want, "?")) {
			lpmd_log_msg(
				"      (class resolves to no CPUs on this system; entry skipped)\n");
			continue;
		}

		n_uids = entry_manager_uids(&g_entries[i], uids,
					   (int)(sizeof(uids) / sizeof(uids[0])));
		if (!n_uids) {
			lpmd_log_msg(
				"      (no active user session; nothing to probe)\n");
			continue;
		}

		for (u = 0; u < n_uids; u++) {
		uid_t uid = uids[u];

		n = expand_entry_units(&g_entries[i], uid, units, 16);
		if (!n) {
			lpmd_log_msg("      (no loaded unit matches)\n");
			continue;
		}
		for (j = 0; j < n; j++) {
			char cgpath[SLICE_CGPATH_MAX];
			char cur[SLICE_CPULIST_MAX];
			char eff[SLICE_CPULIST_MAX];
			char who[32] = "";
			int enf;

			if (uid)
				snprintf(who, sizeof(who), " uid=%u",
					 (unsigned)uid);
			enf = unit_cpuset_enforceable(units[j], uid, cgpath,
						      sizeof(cgpath));

			if (enf < 0) {
				lpmd_log_msg(
					"      %s%s: not active (no cgroup)\n",
					units[j], who);
				continue;
			}
			if (enf == 0) {
				/* Name the remedy, but only where it is the
				 * whole remedy. Delegating cpuset makes the
				 * cgroup enforce AllowedCPUs=; it does not let
				 * this daemon set it on a user manager's unit
				 * (see slice_bus_open), so do not imply that a
				 * drop-in alone would change the outcome. */
				lpmd_log_msg(
					"      %s%s: cg=%s enforceable=NO (cpuset not delegated%s) -> fallback=%s\n",
					units[j], who, cgpath,
					uid ? "; a drop-in adding \"Delegate=cpu cpuset io memory pids\" to user@.service is necessary but not sufficient -- only uid's own manager can set it" :
					      "",
					g_entries[i].fallback ==
							SLICE_FALLBACK_AFFINITY ?
						"affinity" :
						"none");
				continue;
			}
			/* An empty cpuset.cpus is the normal state: nothing
			 * set on this cgroup, inherit from the parent. */
			if (read_cgroup_cpulist(cgpath, "cpuset.cpus", cur,
						sizeof(cur)) < 0)
				snprintf(cur, sizeof(cur), "unset");
			if (read_cgroup_cpulist(cgpath, "cpuset.cpus.effective",
						eff, sizeof(eff)) < 0)
				snprintf(eff, sizeof(eff), "-");
			/* The cgroup enforces it, but for a user manager's unit
			 * lpmd still cannot be the one to ask -- so report that
			 * rather than a bare YES the apply path won't honour. */
			lpmd_log_msg(
				"      %s%s: cg=%s enforceable=YES%s cpuset.cpus=[%s] effective=[%s]\n",
				units[j], who, cgpath,
				uid ? " (but not writable by a system daemon) -> fallback" :
				      "",
				cur, eff);
		}
		}
	}

	/* Then what would actually be written, without writing it. */
	lpmd_log_msg("slice_cpuset: plan for the next low-power transition:\n");
	if (!lpmd_slice_cpuset_apply(1))
		lpmd_log_msg("  (nothing to write on this system)\n");
}

/*
 * Read a task's current affinity mask into @out. Returns the number of
 * significant bytes, or 0 on failure with @err set to the errno.
 *
 * cpu_set_t is a plain bitmap with the same byte layout used everywhere else
 * in this file, so the result can be compared against a cgroup cpulist
 * without conversion.
 *
 * The cpusetsize has to come from CPU_ALLOC_SIZE() and not from
 * get_max_cpus() / 8. The kernel rejects a cpusetsize that is not a multiple
 * of sizeof(unsigned long), and detect_max_cpus() rounds its answer up to a
 * multiple of 32, which makes get_max_cpus() / 8 equal to 4 on every machine
 * with 32 CPUs or fewer -- so that form fails with EINVAL for every task on
 * an ordinary laptop, not just occasionally. Every other affinity call in the
 * tree already uses CPU_ALLOC_SIZE(); this one was the exception.
 */
static size_t task_affinity_bytes(pid_t tid, uint8_t *out, size_t cap, int *err)
{
	size_t setsize = CPU_ALLOC_SIZE(get_max_cpus());
	size_t used = 0, i;

	*err = 0;
	/* Never read past @out. Keep the multiple-of-long property while
	 * clamping, or the syscall would fail for the reason above. */
	if (setsize > cap)
		setsize = cap & ~(sizeof(unsigned long) - 1);
	if (!setsize) {
		*err = EINVAL;
		return 0;
	}

	memset(out, 0, cap);
	if (sched_getaffinity(tid, setsize, (cpu_set_t *)out) < 0) {
		*err = errno;
		return 0;
	}
	for (i = 0; i < setsize; i++) {
		if (out[i])
			used = i + 1;
	}
	/* A live task always has at least one CPU, so an empty mask is a bug
	 * in this function rather than a fact about the task. */
	if (!used)
		*err = ENODATA;
	return used;
}

/*
 * Report the tasks whose CPU set comes from slice policy.
 *
 * This is the half of LIST-BOUND that has nothing to do with
 * sched_setaffinity: these tasks were never migrated and never had their
 * affinity mask written. Their CPU set is the cpuset of the cgroup they were
 * already in, which the kernel applies to every thread, including threads
 * created later.
 *
 * The verdict per task is the discriminator established for the per-task
 * path: an affinity mask equal to the cgroup's effective set means the task
 * inherited it and nothing called sched_setaffinity on it. A strict subset
 * can only come from a sched_setaffinity call -- by the task itself, or by
 * something else -- and such a task is left alone.
 */
void lpmd_slice_cpuset_print_bound(void)
{
	int i;

	if (!g_active) {
		lpmd_log_msg(
			"slice_cpuset: not active (<UseSliceCpuset> unset or no slice.xml)\n");
		return;
	}

	if (!g_n_applied) {
		lpmd_log_msg(
			"slice_cpuset: no unit currently set by slice policy "
			"(not in low-power mode, or nothing enforceable); "
			"see LIST-SLICES for the plan\n");
		return;
	}

	lpmd_log_msg(
		"slice_cpuset: %d unit(s) bound by cgroup cpuset, no sched_setaffinity\n",
		g_n_applied);

	for (i = 0; i < g_n_applied; i++) {
		const char *unit = g_applied[i].unit;
		char cgpath[SLICE_CGPATH_MAX];
		char cur[SLICE_CPULIST_MAX];
		char eff[SLICE_CPULIST_MAX];
		uint8_t eff_mask[CPUMASK_BYTES];
		char path[SLICE_CGPATH_MAX + 64];
		size_t eff_len;
		FILE *f;
		char line[64];
		int tasks = 0;

		if (unit_control_group(unit, g_applied[i].uid, cgpath,
				       sizeof(cgpath)) < 0) {
			lpmd_log_msg("  %s: gone\n", unit);
			continue;
		}
		if (read_cgroup_cpulist(cgpath, "cpuset.cpus", cur,
					sizeof(cur)) < 0)
			snprintf(cur, sizeof(cur), "unset");
		if (read_cgroup_cpulist(cgpath, "cpuset.cpus.effective", eff,
					sizeof(eff)) < 0)
			snprintf(eff, sizeof(eff), "-");

		lpmd_log_msg(
			"  %s: cg=%s cpuset.cpus=[%s] effective=[%s] source=slice.xml\n",
			unit, cgpath, cur, eff);

		eff_len = cpulist_to_bytes(eff, eff_mask, sizeof(eff_mask));

		snprintf(path, sizeof(path), "/sys/fs/cgroup%s/cgroup.procs",
			 strcmp(cgpath, "/") ? cgpath : "");
		f = fopen(path, "r");
		if (!f) {
			lpmd_log_msg("      (cannot read %s)\n", path);
			continue;
		}
		while (fgets(line, sizeof(line), f)) {
			pid_t pid = (pid_t)strtol(line, NULL, 10);
			uint8_t aff[CPUMASK_BYTES];
			char aff_list[SLICE_CPULIST_MAX];
			char comm[64] = { 0 };
			const char *verdict;
			size_t aff_len;
			int aff_err;
			FILE *cf;

			if (pid <= 0)
				continue;
			tasks++;

			snprintf(path, sizeof(path), "/proc/%d/comm", (int)pid);
			cf = fopen(path, "r");
			if (cf) {
				if (fgets(comm, sizeof(comm), cf)) {
					size_t l = strlen(comm);

					if (l && comm[l - 1] == '\n')
						comm[l - 1] = '\0';
				}
				fclose(cf);
			} else {
				snprintf(comm, sizeof(comm), "<dead>");
			}

			/* Say why. A task that exited between the comm read and
			 * here gives ESRCH; anything else is this daemon's
			 * fault and used to be indistinguishable from it. */
			aff_len = task_affinity_bytes(pid, aff, sizeof(aff),
						      &aff_err);
			if (!aff_len) {
				lpmd_log_msg(
					"      PID=%d comm=%s affinity unreadable: %s\n",
					(int)pid, comm, strerror(aff_err));
				continue;
			}
			bytes_to_cpulist(aff, aff_len, aff_list,
					 sizeof(aff_list));

			if (aff_len == eff_len &&
			    !memcmp(aff, eff_mask, eff_len))
				verdict = "INHERITED (no sched_setaffinity)";
			else
				verdict = "OWN-AFFINITY (sched_setaffinity was called; left alone)";

			lpmd_log_msg(
				"      PID=%d comm=%s affinity=[%s] %s\n",
				(int)pid, comm, aff_list, verdict);
		}
		fclose(f);
		if (!tasks)
			lpmd_log_msg("      (no tasks in this cgroup)\n");
	}
}
