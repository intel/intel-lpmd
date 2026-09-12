// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * intel_lpmd_focus_helper.c: KDE Plasma focused-window PID relay
 *
 * Copyright (C) 2026 Intel Corporation. All rights reserved.
 *
 * KDE Plasma focused-window PID relay for intel_lpmd.
 *
 * Watches the active/focused window in a KDE Plasma session and calls
 * the system-bus method
 *     org.freedesktop.intel_lpmd.LPM_SET_FOCUS_PID(i pid)
 * whenever it changes. intel_lpmd then promotes that PID (and its
 * descendant TGIDs) to the "user_interactive" CPU set and demotes
 * them back on focus loss.
 *
 * Runs as the user (not root). It connects to:
 *   * The system bus to invoke LPM_SET_FOCUS_PID.
 *   * The session bus to register a tiny KWin JavaScript
 *     (workspace.windowActivated) that calls back via DBus to this
 *     helper with the focused window's PID.
 *
 * Only the KDE backend is shipped here -- it is the only compositor
 * pathway that proved reliable across desktops. Experimental X11 and
 * GNOME backends are kept in intel_lpmd_focus_helper_poc.c (not
 * built by default) for future consideration.
 *
 * Requires KWin >= 5.20 (kwin_wayland or kwin_x11).
 */

#define _GNU_SOURCE
#include <errno.h>
#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <unistd.h>

#include <gio/gio.h>
#include <glib.h>

#define LPMD_BUS_NAME "org.freedesktop.intel_lpmd"
#define LPMD_OBJ_PATH "/org/freedesktop/intel_lpmd"
#define LPMD_IFACE "org.freedesktop.intel_lpmd"

static gboolean opt_verbose;

static GDBusConnection *g_sysbus;
static pid_t g_last_pid = -1;

/* ---------- helpers ---------- */

static void log_msg(const char *fmt, ...)
{
	if (!opt_verbose || !fmt)
		return;
	va_list ap;
	memset(&ap, 0, sizeof(ap));
	va_start(ap, fmt);
	vfprintf(stderr, fmt, ap);
	va_end(ap);
	fputc('\n', stderr);
}

static void emit_focus_pid(pid_t pid)
{
	GError *err = NULL;
	GVariant *res;

	if (pid == g_last_pid)
		return;
	g_last_pid = pid;

	log_msg("focus -> pid %d", (int)pid);

	res = g_dbus_connection_call_sync(
		g_sysbus, LPMD_BUS_NAME, LPMD_OBJ_PATH, LPMD_IFACE,
		"LPM_SET_FOCUS_PID", g_variant_new("(i)", (gint)pid),
		G_VARIANT_TYPE("(i)"), G_DBUS_CALL_FLAGS_NONE, -1, NULL, &err);
	if (err) {
		g_warning("LPM_SET_FOCUS_PID(%d) failed: %s", (int)pid,
			  err->message);
		g_error_free(err);
		return;
	}
	if (res)
		g_variant_unref(res);
}

/* ==================================================================
 *                        KDE Wayland backend
 *
 * KWin doesn't expose focus + PID directly over DBus. We work around
 * that by registering a tiny KWin JavaScript that fires on
 * workspace.windowActivated and calls back via callDBus() to our own
 * helper-registered name "org.freedesktop.intel_lpmd_focus_helper",
 * passing the active window's PID. We then forward it to lpmd.
 *
 * Requires KWin >= 5.20 (kwin_wayland/kwin_x11).
 * ================================================================== */

#define KDE_HELPER_NAME "org.freedesktop.intel_lpmd_focus_helper"
#define KDE_HELPER_OBJ "/Focus"
#define KDE_HELPER_IFACE "org.freedesktop.intel_lpmd_focus_helper.Focus"

static const char kde_kwin_script[] =
	"function notify(c) {\n"
	"    if (!c) return;\n"
	"    var pid = c.pid;\n"
	"    if (typeof pid === 'undefined' || pid <= 0) return;\n"
	"    callDBus('" KDE_HELPER_NAME "',\n"
	"             '" KDE_HELPER_OBJ "',\n"
	"             '" KDE_HELPER_IFACE "',\n"
	"             'SetPid', pid);\n"
	"}\n"
	"workspace.windowActivated.connect(notify);\n"
	"if (workspace.activeWindow) notify(workspace.activeWindow);\n";

static guint kde_owner_id;

static void kde_method_call(GDBusConnection *c, const gchar *sender,
			    const gchar *obj, const gchar *iface,
			    const gchar *method, GVariant *params,
			    GDBusMethodInvocation *inv, gpointer ud)
{
	(void)c;
	(void)sender;
	(void)obj;
	(void)iface;
	(void)ud;

	if (g_strcmp0(method, "SetPid") == 0) {
		gint pid = 0;
		g_variant_get(params, "(i)", &pid);
		if (pid > 0)
			emit_focus_pid((pid_t)pid);
		g_dbus_method_invocation_return_value(inv, NULL);
		return;
	}
	g_dbus_method_invocation_return_error(inv, G_DBUS_ERROR,
					      G_DBUS_ERROR_UNKNOWN_METHOD,
					      "no such method %s", method);
}

static const GDBusInterfaceVTable kde_vtable = { kde_method_call,
						 NULL,
						 NULL,
						 { 0 } };

static int kde_install_script(GDBusConnection *sess)
{
	GError *err = NULL;
	GVariant *res;
	char path[256];
	FILE *f;

	snprintf(path, sizeof(path), "%s/intel_lpmd_focus_kwin.js",
		 g_get_user_cache_dir());
	f = fopen(path, "w");
	if (!f) {
		log_msg("KDE: cannot write %s: %s", path, strerror(errno));
		return -1;
	}
	fwrite(kde_kwin_script, 1, sizeof(kde_kwin_script) - 1, f);
	fclose(f);

	/* org.kde.kwin.Scripting.loadScript(s path, s name) -> i id
     * then .run() on the returned object path. Different KWin versions
     * expose slightly different signatures; try the common one. */
	res = g_dbus_connection_call_sync(
		sess, "org.kde.KWin", "/Scripting", "org.kde.kwin.Scripting",
		"loadScript", g_variant_new("(ss)", path, "intel_lpmd_focus"),
		G_VARIANT_TYPE("(i)"), G_DBUS_CALL_FLAGS_NONE, -1, NULL, &err);
	if (err) {
		log_msg("KDE: loadScript failed: %s", err->message);
		g_error_free(err);
		return -1;
	}
	g_variant_unref(res);

	res = g_dbus_connection_call_sync(
		sess, "org.kde.KWin", "/Scripting", "org.kde.kwin.Scripting",
		"start", NULL, NULL, G_DBUS_CALL_FLAGS_NONE, -1, NULL, &err);
	if (err) {
		/* "start" may not exist; the script auto-runs after load on
         * newer KWin. Don't treat as fatal. */
		log_msg("KDE: Scripting.start: %s (non-fatal)", err->message);
		g_error_free(err);
	} else if (res) {
		g_variant_unref(res);
	}
	return 0;
}

static void kde_on_name_acquired(GDBusConnection *c, const gchar *n, gpointer u)
{
	(void)c;
	(void)n;
	(void)u;
	log_msg("KDE: helper bus name acquired");
}

static int kde_start(GMainLoop *loop)
{
	GError *err = NULL;
	GDBusConnection *sess;
	GDBusNodeInfo *node;
	static const gchar introspect[] =
		"<node>"
		"  <interface name='" KDE_HELPER_IFACE "'>"
		"    <method name='SetPid'>"
		"      <arg type='i' name='pid' direction='in'/>"
		"    </method>"
		"  </interface>"
		"</node>";

	(void)loop;

	sess = g_bus_get_sync(G_BUS_TYPE_SESSION, NULL, &err);
	if (!sess) {
		log_msg("KDE: no session bus: %s", err ? err->message : "?");
		if (err)
			g_error_free(err);
		return -1;
	}

	node = g_dbus_node_info_new_for_xml(introspect, &err);
	if (!node) {
		log_msg("KDE: introspect parse failed: %s", err->message);
		g_error_free(err);
		g_object_unref(sess);
		return -1;
	}
	g_dbus_connection_register_object(sess, KDE_HELPER_OBJ,
					  node->interfaces[0], &kde_vtable,
					  NULL, NULL, &err);
	g_dbus_node_info_unref(node);
	if (err) {
		log_msg("KDE: register_object failed: %s", err->message);
		g_error_free(err);
		g_object_unref(sess);
		return -1;
	}

	kde_owner_id = g_bus_own_name_on_connection(
		sess, KDE_HELPER_NAME, G_BUS_NAME_OWNER_FLAGS_NONE,
		kde_on_name_acquired, NULL, NULL, NULL);

	if (kde_install_script(sess) < 0) {
		g_object_unref(sess);
		return -1;
	}

	log_msg("KDE Wayland backend active");
	/* Keep sess alive; do not unref. */
	return 0;
}

/* ==================================================================
 *                              main
 * ================================================================== */

static GOptionEntry entries[] = { { "verbose", 'v', 0, G_OPTION_ARG_NONE,
				    &opt_verbose, "Log focus changes to stderr",
				    NULL },
				  { NULL } };

int main(int argc, char **argv)
{
	GError *err = NULL;
	GOptionContext *opt;
	GMainLoop *loop;
	const char *desktop;

	opt = g_option_context_new(
		"- intel_lpmd KDE Plasma focused-window relay");
	g_option_context_add_main_entries(opt, entries, NULL);
	if (!g_option_context_parse(opt, &argc, &argv, &err)) {
		g_printerr("option parse: %s\n", err->message);
		g_error_free(err);
		return 1;
	}
	g_option_context_free(opt);

	/* Sanity-check we're in a KDE session. Don't refuse to run on
     * other desktops -- KWin may be present anyway (kwin_x11 inside
     * a nested compositor, etc.) -- but warn. */
	desktop = g_getenv("XDG_CURRENT_DESKTOP");
	if (desktop && !(strstr(desktop, "KDE") || strstr(desktop, "plasma"))) {
		log_msg("warning: XDG_CURRENT_DESKTOP='%s' is not KDE/plasma; "
			"trying KWin anyway",
			desktop);
	}

	g_sysbus = g_bus_get_sync(G_BUS_TYPE_SYSTEM, NULL, &err);
	if (!g_sysbus) {
		g_printerr("system bus: %s\n", err ? err->message : "?");
		if (err)
			g_error_free(err);
		return 1;
	}

	loop = g_main_loop_new(NULL, FALSE);

	if (kde_start(loop) < 0) {
		g_printerr(
			"KDE focus backend unavailable "
			"(KWin Scripting not reachable on the session bus)\n");
		return 2;
	}

	/* Tell intel_lpmd we're up and will be sending focus-PID events.
     * Until this call, lpmd treats user_initiated as user_interactive
     * (no point starving user-session apps when nobody is reporting
     * focus). The reply is fire-and-forget; we don't gate startup on
     * it because lpmd might be slow to come up or temporarily down. */
	{
		GError *rerr = NULL;
		GVariant *res;
		res = g_dbus_connection_call_sync(
			g_sysbus, LPMD_BUS_NAME, LPMD_OBJ_PATH, LPMD_IFACE,
			"LPM_FOCUS_HELPER_READY", g_variant_new("(i)", 1),
			G_VARIANT_TYPE("(i)"), G_DBUS_CALL_FLAGS_NONE, -1, NULL,
			&rerr);
		if (rerr) {
			log_msg("LPM_FOCUS_HELPER_READY: %s (continuing)",
				rerr->message);
			g_error_free(rerr);
		} else {
			log_msg("LPM_FOCUS_HELPER_READY: ack");
			if (res)
				g_variant_unref(res);
		}
	}

	g_main_loop_run(loop);
	g_main_loop_unref(loop);
	return 0;
}
