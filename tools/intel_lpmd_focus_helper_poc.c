// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * intel_lpmd_focus_helper_poc.c: Experimental focus-relay backends
 *
 * Copyright (C) 2026 Intel Corporation. All rights reserved.
 *
 * Experimental focus-relay backends (X11 + GNOME Wayland).
 *
 * Proof-of-concept code retained for future consideration. NOT built
 * by default: kept out of bin_PROGRAMS in tools/Makefile.am and only
 * shipped via EXTRA_DIST. The productized helper
 * (intel_lpmd_focus_helper.c) keeps only the KDE Plasma backend,
 * which is the only one that proved reliable in practice.
 *
 * Why these backends were demoted to POC status:
 *
 *   X11 / Xwayland
 *     _NET_WM_PID is set by the *thread* that called XSetProperty,
 *     not necessarily the process leader. For multithreaded apps
 *     (notably browsers) this returned worker-TIDs that compositor
 *     callers couldn't sensibly act on. Resolving to TGID in lpmd
 *     mitigates this, but the helper still misses windows whose
 *     creator never sets _NET_WM_PID.
 *
 *   GNOME Wayland
 *     The built-in org.gnome.Shell.Introspect doesn't expose PIDs.
 *     We have to depend on a third-party Shell extension (Window
 *     Calls) being installed, whose JSON schema varies by fork and
 *     often reports focus:false on every window during Wayland
 *     compositor transitions. Too fragile for production.
 *
 *   KDE Plasma (kept in intel_lpmd_focus_helper.c)
 *     KWin Scripting fires windowActivated synchronously with the
 *     compositor's focus change and exposes client.pid directly.
 *
 * To compile this POC standalone for experimentation:
 *
 *     gcc -O2 -Wall $(pkg-config --cflags --libs gio-2.0) -ldl \
 *         tools/intel_lpmd_focus_helper_poc.c \
 *         -o intel_lpmd_focus_helper_poc
 */

#define _GNU_SOURCE
#include <dlfcn.h>
#include <errno.h>
#include <stdio.h>
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
static gboolean opt_once;
static gchar *opt_force_backend; /* "x11" | "gnome" | NULL */

static GDBusConnection *g_sysbus;
static pid_t g_last_pid = -1;

/* ---------- helpers ---------- */

static void log_msg(const char *fmt, ...)
{
	va_list ap;
	if (!opt_verbose)
		return;
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
 *                          X11 backend
 *
 * Loaded via dlopen so the helper still builds and runs on Wayland-
 * only systems without libX11.
 * ================================================================== */

typedef struct _Display Display;
typedef unsigned long XID;
typedef XID Window;
typedef XID Atom;
typedef int Bool;
typedef int Status;

#define X11_None 0L
#define X11_Success 0
#define X11_AnyPropertyType 0L
#define X11_PropertyChangeMask (1L << 22)
#define X11_PropertyNotify 28

typedef union _XEvent XEvent;
struct XAnyEvent {
	int type;
	unsigned long serial;
	Bool send_event;
	Display *display;
	Window window;
};
struct XPropertyEvent {
	int type;
	unsigned long serial;
	Bool send_event;
	Display *display;
	Window window;
	Atom atom;
	unsigned long time;
	int state;
};
union _XEvent {
	int type;
	struct XAnyEvent xany;
	struct XPropertyEvent xproperty;
	long pad[24];
};

struct x11_api {
	Display *(*XOpenDisplay)(const char *);
	int (*XCloseDisplay)(Display *);
	int (*XConnectionNumber)(Display *);
	Window (*XDefaultRootWindow)(Display *);
	Atom (*XInternAtom)(Display *, const char *, Bool);
	int (*XSelectInput)(Display *, Window, long);
	int (*XNextEvent)(Display *, XEvent *);
	int (*XPending)(Display *);
	int (*XGetWindowProperty)(Display *, Window, Atom, long, long, Bool,
				  Atom, Atom *, int *, unsigned long *,
				  unsigned long *, unsigned char **);
	int (*XFree)(void *);
	int (*XSetErrorHandler)(int (*)(Display *, void *));
	int (*XSync)(Display *, Bool);
};

static struct x11_api X;
static void *x11_dl;
static Display *x11_dpy;
static Window x11_root;
static Atom x11_active, x11_wmpid;

static int x11_error_handler(Display *d, void *e)
{
	(void)d;
	(void)e;
	return 0; /* swallow BadWindow on races */
}

static int x11_load(void)
{
	x11_dl = dlopen("libX11.so.6", RTLD_NOW);
	if (!x11_dl)
		x11_dl = dlopen("libX11.so", RTLD_NOW);
	if (!x11_dl)
		return -1;

#define LOAD(sym)                                       \
	do {                                            \
		*(void **)&X.sym = dlsym(x11_dl, #sym); \
		if (!X.sym) {                           \
			dlclose(x11_dl);                \
			x11_dl = NULL;                  \
			return -1;                      \
		}                                       \
	} while (0)

	LOAD(XOpenDisplay);
	LOAD(XCloseDisplay);
	LOAD(XConnectionNumber);
	LOAD(XDefaultRootWindow);
	LOAD(XInternAtom);
	LOAD(XSelectInput);
	LOAD(XNextEvent);
	LOAD(XPending);
	LOAD(XGetWindowProperty);
	LOAD(XFree);
	LOAD(XSetErrorHandler);
	LOAD(XSync);
#undef LOAD
	return 0;
}

static Window x11_get_active_window(void)
{
	Atom actual_type;
	int actual_format;
	unsigned long nitems, bytes_after;
	unsigned char *prop = NULL;
	Window w = X11_None;

	if (X.XGetWindowProperty(x11_dpy, x11_root, x11_active, 0, 1, 0,
				 X11_AnyPropertyType, &actual_type,
				 &actual_format, &nitems, &bytes_after,
				 &prop) != X11_Success)
		return X11_None;
	if (prop && nitems >= 1)
		w = *(Window *)prop;
	if (prop)
		X.XFree(prop);
	return w;
}

static pid_t x11_get_window_pid(Window w)
{
	Atom actual_type;
	int actual_format;
	unsigned long nitems, bytes_after;
	unsigned char *prop = NULL;
	pid_t pid = 0;

	if (w == X11_None)
		return 0;
	if (X.XGetWindowProperty(x11_dpy, w, x11_wmpid, 0, 1, 0,
				 X11_AnyPropertyType, &actual_type,
				 &actual_format, &nitems, &bytes_after,
				 &prop) != X11_Success)
		return 0;
	if (prop && nitems >= 1)
		pid = (pid_t)(*(unsigned long *)prop);
	if (prop)
		X.XFree(prop);
	return pid;
}

static gboolean x11_on_readable(GIOChannel *src, GIOCondition cond, gpointer ud)
{
	(void)src;
	(void)cond;
	(void)ud;
	XEvent ev;

	while (X.XPending(x11_dpy)) {
		X.XNextEvent(x11_dpy, &ev);
		if (ev.type == X11_PropertyNotify &&
		    ev.xproperty.window == x11_root &&
		    ev.xproperty.atom == x11_active) {
			Window w = x11_get_active_window();
			pid_t pid = x11_get_window_pid(w);
			if (pid > 0)
				emit_focus_pid(pid);
			if (opt_once) {
				g_main_loop_quit((GMainLoop *)ud);
				return FALSE;
			}
		}
	}
	return TRUE;
}

static int x11_start(GMainLoop *loop)
{
	int fd;

	if (x11_load() < 0) {
		log_msg("X11: libX11 not available");
		return -1;
	}
	x11_dpy = X.XOpenDisplay(NULL);
	if (!x11_dpy) {
		log_msg("X11: XOpenDisplay failed");
		return -1;
	}
	X.XSetErrorHandler(x11_error_handler);
	x11_root = X.XDefaultRootWindow(x11_dpy);
	x11_active = X.XInternAtom(x11_dpy, "_NET_ACTIVE_WINDOW", 0);
	x11_wmpid = X.XInternAtom(x11_dpy, "_NET_WM_PID", 0);

	if (x11_active == X11_None || x11_wmpid == X11_None) {
		log_msg("X11: EWMH atoms missing");
		X.XCloseDisplay(x11_dpy);
		x11_dpy = NULL;
		return -1;
	}

	X.XSelectInput(x11_dpy, x11_root, X11_PropertyChangeMask);
	X.XSync(x11_dpy, 0);

	/* Prime current focus. */
	{
		Window w = x11_get_active_window();
		pid_t pid = x11_get_window_pid(w);
		if (pid > 0)
			emit_focus_pid(pid);
	}
	if (opt_once)
		return 0;

	fd = X.XConnectionNumber(x11_dpy);
	GIOChannel *ch = g_io_channel_unix_new(fd);
	g_io_add_watch(ch, G_IO_IN, x11_on_readable, loop);
	g_io_channel_unref(ch);

	log_msg("X11 backend active (fd=%d)", fd);
	return 0;
}

/* ==================================================================
 *                       GNOME Wayland backend
 *
 * GNOME's built-in org.gnome.Shell.Introspect.GetWindows() does NOT
 * expose PIDs (only app-id / wm-class / title / has-focus). To recover
 * a PID for the focused window we therefore prefer, in order:
 *
 *   1. "Window Calls" extension (ickyicky/window-calls):
 *        bus    org.gnome.Shell
 *        path   /org/gnome/Shell/Extensions/Windows
 *        iface  org.gnome.Shell.Extensions.Windows
 *        method List() -> (s)         JSON array, each entry has
 *                                     {id,pid,focus,wm_class,...}
 *        method GetFocus() -> (s)     (newer versions) JSON of focused
 *
 *   2. Built-in Introspect.GetWindows(): we still iterate it as a
 *      best-effort, in case a downstream patch added "pid".
 *
 * We always subscribe to the built-in WindowsChanged signal — gnome-
 * shell emits it whether or not the extension is loaded — and re-poll
 * on every notification.
 * ================================================================== */

#define GNOME_BUS "org.gnome.Shell"

#define GNOME_INTRO_OBJ "/org/gnome/Shell/Introspect"
#define GNOME_INTRO_IFACE "org.gnome.Shell.Introspect"

#define GNOME_WINCALLS_OBJ "/org/gnome/Shell/Extensions/Windows"
#define GNOME_WINCALLS_IFACE "org.gnome.Shell.Extensions.Windows"

static GDBusConnection *gnome_sess;
static guint gnome_sub_id;
static gboolean gnome_have_wincalls;
static gboolean gnome_have_introspect;

/* Find the unsigned integer value of a JSON key in a flat object slice.
 * Looks for "<key>": <digits>   (whitespace tolerated). Returns 0 if not
 * found. Good enough for the simple objects "Window Calls" emits. */
static guint32 gnome_json_find_uint(const char *s, const char *end,
				    const char *key)
{
	char pat[64];
	const char *p;
	int klen;

	klen = g_snprintf(pat, sizeof(pat), "\"%s\"", key);
	if (klen <= 0 || klen >= (int)sizeof(pat))
		return 0;

	p = s;
	while (p < end && (p = memmem(p, end - p, pat, klen)) != NULL) {
		const char *q = p + klen;
		while (q < end && (*q == ' ' || *q == '\t' || *q == ':'))
			q++;
		if (q < end && *q >= '0' && *q <= '9') {
			guint64 v = 0;
			while (q < end && *q >= '0' && *q <= '9') {
				v = v * 10 + (guint)(*q - '0');
				q++;
			}
			return (guint32)v;
		}
		p += klen;
	}
	return 0;
}

/* Find a boolean-ish JSON value for key. Accepts true/false and 1/0.
 * Returns 1 (true), 0 (false), or -1 (key absent). */
static int gnome_json_find_bool(const char *s, const char *end, const char *key)
{
	char pat[64];
	const char *p;
	int klen;

	klen = g_snprintf(pat, sizeof(pat), "\"%s\"", key);
	if (klen <= 0 || klen >= (int)sizeof(pat))
		return -1;

	p = s;
	while (p < end && (p = memmem(p, end - p, pat, klen)) != NULL) {
		const char *q = p + klen;
		while (q < end && (*q == ' ' || *q == '\t' || *q == ':'))
			q++;
		if (q + 4 <= end && !strncmp(q, "true", 4))
			return 1;
		if (q + 5 <= end && !strncmp(q, "false", 5))
			return 0;
		if (q < end && *q == '1')
			return 1;
		if (q < end && *q == '0')
			return 0;
		p += klen;
	}
	return -1;
}

/* Try several spellings the various Window Calls forks use. */
static int gnome_json_find_focus(const char *s, const char *end)
{
	int v;
	static const char *const keys[] = { "focus",	  "has_focus",
					    "has-focus",  "focused",
					    "is_focused", NULL };
	int i;
	for (i = 0; keys[i]; i++) {
		v = gnome_json_find_bool(s, end, keys[i]);
		if (v != -1)
			return v;
	}
	return -1;
}

/* Look up the pid for a specific window id in a Window Calls JSON array.
 * Returns 0 if id not present or no pid recorded. */
static guint32 gnome_json_pid_for_id(const char *json, guint64 want_id)
{
	const char *p = json;
	const char *end = json + strlen(json);

	while (p < end) {
		const char *obj_start, *obj_end;
		int depth;
		guint32 id_lo, pid;

		while (p < end && *p != '{')
			p++;
		if (p >= end)
			break;
		obj_start = p;
		depth = 0;
		while (p < end) {
			if (*p == '{')
				depth++;
			else if (*p == '}') {
				depth--;
				if (depth == 0) {
					p++;
					break;
				}
			}
			p++;
		}
		obj_end = p;
		if (obj_end <= obj_start)
			break;

		id_lo = gnome_json_find_uint(obj_start, obj_end, "id");
		if (id_lo != 0 && (guint64)id_lo == (want_id & 0xffffffffULL)) {
			pid = gnome_json_find_uint(obj_start, obj_end, "pid");
			return pid;
		}
	}
	return 0;
}

/* Walk a JSON array of flat objects and pick the focused one. */
static pid_t gnome_parse_wincalls_json(const char *json)
{
	const char *p = json;
	const char *end = json + strlen(json);
	pid_t found = 0;
	int objs = 0, focused_seen = 0;

	while (p < end) {
		const char *obj_start, *obj_end;
		int depth;

		while (p < end && *p != '{')
			p++;
		if (p >= end)
			break;
		obj_start = p;
		depth = 0;
		while (p < end) {
			if (*p == '{')
				depth++;
			else if (*p == '}') {
				depth--;
				if (depth == 0) {
					p++;
					break;
				}
			}
			p++;
		}
		obj_end = p;
		if (obj_end <= obj_start)
			break;
		objs++;

		if (gnome_json_find_focus(obj_start, obj_end) == 1) {
			guint32 pid =
				gnome_json_find_uint(obj_start, obj_end, "pid");
			focused_seen++;
			if (pid > 0) {
				found = (pid_t)pid;
				break;
			}
		}
	}

	(void)objs;
	(void)focused_seen;
	return found;
}

static gchar *gnome_fetch_wincalls_json(void)
{
	GError *err = NULL;
	GVariant *res;
	const gchar *json = NULL;
	gchar *copy = NULL;

	if (!gnome_have_wincalls)
		return NULL;

	res = g_dbus_connection_call_sync(
		gnome_sess, GNOME_BUS, GNOME_WINCALLS_OBJ, GNOME_WINCALLS_IFACE,
		"List", NULL, G_VARIANT_TYPE("(s)"), G_DBUS_CALL_FLAGS_NONE, -1,
		NULL, &err);
	if (err) {
		log_msg("GNOME: Window Calls List() failed: %s", err->message);
		g_error_free(err);
		return NULL;
	}
	g_variant_get(res, "(&s)", &json);
	if (json)
		copy = g_strdup(json);
	g_variant_unref(res);
	return copy;
}

static pid_t gnome_refresh_introspect(guint64 *focused_id)
{
	GError *err = NULL;
	GVariant *res, *windows;
	GVariantIter it;
	guint64 wid;
	GVariant *dict;
	pid_t found = 0;
	guint64 found_id = 0;

	if (focused_id)
		*focused_id = 0;
	if (!gnome_have_introspect)
		return 0;

	res = g_dbus_connection_call_sync(
		gnome_sess, GNOME_BUS, GNOME_INTRO_OBJ, GNOME_INTRO_IFACE,
		"GetWindows", NULL, G_VARIANT_TYPE("(a{ta{sv}})"),
		G_DBUS_CALL_FLAGS_NONE, -1, NULL, &err);
	if (err) {
		log_msg("GNOME: Introspect GetWindows failed: %s",
			err->message);
		g_error_free(err);
		return 0;
	}

	windows = g_variant_get_child_value(res, 0);
	g_variant_iter_init(&it, windows);
	while (g_variant_iter_loop(&it, "{t@a{sv}}", &wid, &dict)) {
		gboolean focus = FALSE;
		guint32 pid = 0;
		GVariant *vf = g_variant_lookup_value(dict, "has-focus",
						      G_VARIANT_TYPE_BOOLEAN);
		GVariant *vp = g_variant_lookup_value(dict, "pid",
						      G_VARIANT_TYPE_UINT32);
		if (vf) {
			focus = g_variant_get_boolean(vf);
			g_variant_unref(vf);
		}
		if (vp) {
			pid = g_variant_get_uint32(vp);
			g_variant_unref(vp);
		}
		if (focus) {
			found_id = wid;
			if (pid > 0)
				found = (pid_t)pid;
			break;
		}
	}
	g_variant_unref(windows);
	g_variant_unref(res);

	if (focused_id)
		*focused_id = found_id;
	return found;
}

static void gnome_refresh(void)
{
	pid_t pid = 0;
	guint64 focused_id = 0;
	gchar *json = NULL;

	pid = gnome_refresh_introspect(&focused_id);

	if (pid == 0 && focused_id != 0 && gnome_have_wincalls) {
		json = gnome_fetch_wincalls_json();
		if (json) {
			guint32 p = gnome_json_pid_for_id(json, focused_id);
			if (p > 0)
				pid = (pid_t)p;
		}
	}

	if (pid == 0 && gnome_have_wincalls) {
		if (!json)
			json = gnome_fetch_wincalls_json();
		if (json)
			pid = gnome_parse_wincalls_json(json);
	}

	if (pid > 0)
		emit_focus_pid(pid);

	g_free(json);
}

static void gnome_on_signal(GDBusConnection *c, const gchar *sender,
			    const gchar *obj, const gchar *iface,
			    const gchar *sig, GVariant *params, gpointer ud)
{
	(void)c;
	(void)sender;
	(void)obj;
	(void)iface;
	(void)sig;
	(void)params;
	(void)ud;
	gnome_refresh();
}

static gboolean gnome_periodic_poll(gpointer ud)
{
	(void)ud;
	gnome_refresh();
	return G_SOURCE_CONTINUE;
}

static int gnome_start(GMainLoop *loop)
{
	GError *err = NULL;
	GVariant *res;

	(void)loop;

	gnome_sess = g_bus_get_sync(G_BUS_TYPE_SESSION, NULL, &err);
	if (!gnome_sess) {
		log_msg("GNOME: no session bus: %s", err ? err->message : "?");
		if (err)
			g_error_free(err);
		return -1;
	}

	res = g_dbus_connection_call_sync(
		gnome_sess, GNOME_BUS, GNOME_WINCALLS_OBJ, GNOME_WINCALLS_IFACE,
		"List", NULL, G_VARIANT_TYPE("(s)"), G_DBUS_CALL_FLAGS_NONE, -1,
		NULL, &err);
	if (err) {
		log_msg("GNOME: Window Calls extension not present: %s",
			err->message);
		g_error_free(err);
		err = NULL;
	} else {
		gnome_have_wincalls = TRUE;
		g_variant_unref(res);
		log_msg("GNOME: Window Calls extension detected");
	}

	res = g_dbus_connection_call_sync(
		gnome_sess, GNOME_BUS, GNOME_INTRO_OBJ, GNOME_INTRO_IFACE,
		"GetWindows", NULL, G_VARIANT_TYPE("(a{ta{sv}})"),
		G_DBUS_CALL_FLAGS_NONE, -1, NULL, &err);
	if (err) {
		log_msg("GNOME: Introspect GetWindows unavailable: %s",
			err->message);
		g_error_free(err);
		err = NULL;
	} else {
		gnome_have_introspect = TRUE;
		g_variant_unref(res);
	}

	if (!gnome_have_wincalls && !gnome_have_introspect) {
		g_object_unref(gnome_sess);
		gnome_sess = NULL;
		return -1;
	}

	gnome_sub_id = g_dbus_connection_signal_subscribe(
		gnome_sess, GNOME_BUS, GNOME_INTRO_IFACE, "WindowsChanged",
		GNOME_INTRO_OBJ, NULL, G_DBUS_SIGNAL_FLAGS_NONE,
		gnome_on_signal, NULL, NULL);

	g_timeout_add_seconds(1, gnome_periodic_poll, NULL);

	gnome_refresh();
	log_msg("GNOME Wayland backend active (wincalls=%d introspect=%d)",
		gnome_have_wincalls, gnome_have_introspect);
	return 0;
}

/* ==================================================================
 *                          backend autodetect
 * ================================================================== */

static int try_backend(const char *name, GMainLoop *loop)
{
	if (!strcmp(name, "x11"))
		return x11_start(loop);
	if (!strcmp(name, "gnome"))
		return gnome_start(loop);
	return -1;
}

static int autodetect(GMainLoop *loop)
{
	const char *xdg = g_getenv("XDG_SESSION_TYPE");
	const char *desktop = g_getenv("XDG_CURRENT_DESKTOP");
	const char *display = g_getenv("DISPLAY");
	int wayland = xdg && !g_strcmp0(xdg, "wayland");

	if (wayland && desktop && strstr(desktop, "GNOME") &&
	    gnome_start(loop) == 0)
		return 0;
	if (display && x11_start(loop) == 0)
		return 0;
	if (gnome_start(loop) == 0)
		return 0;
	return -1;
}

/* ==================================================================
 *                              main
 * ================================================================== */

static GOptionEntry entries[] = {
	{ "verbose", 'v', 0, G_OPTION_ARG_NONE, &opt_verbose,
	  "Log focus changes to stderr", NULL },
	{ "once", 'o', 0, G_OPTION_ARG_NONE, &opt_once,
	  "Emit one focus update then exit (X11 only)", NULL },
	{ "backend", 'b', 0, G_OPTION_ARG_STRING, &opt_force_backend,
	  "Force backend: x11 | gnome", "BACKEND" },
	{ NULL }
};

int main(int argc, char **argv)
{
	GError *err = NULL;
	GOptionContext *opt;
	GMainLoop *loop;

	opt = g_option_context_new("- intel_lpmd focus relay POC (X11/GNOME)");
	g_option_context_add_main_entries(opt, entries, NULL);
	if (!g_option_context_parse(opt, &argc, &argv, &err)) {
		g_printerr("option parse: %s\n", err->message);
		g_error_free(err);
		return 1;
	}
	g_option_context_free(opt);

	g_sysbus = g_bus_get_sync(G_BUS_TYPE_SYSTEM, NULL, &err);
	if (!g_sysbus) {
		g_printerr("system bus: %s\n", err ? err->message : "?");
		if (err)
			g_error_free(err);
		return 1;
	}

	loop = g_main_loop_new(NULL, FALSE);

	if (opt_force_backend) {
		if (try_backend(opt_force_backend, loop) < 0) {
			g_printerr("backend '%s' unavailable\n",
				   opt_force_backend);
			return 2;
		}
	} else if (autodetect(loop) < 0) {
		g_printerr("no focus backend available "
			   "(no X11 DISPLAY, no GNOME Introspect)\n");
		return 2;
	}

	if (opt_once)
		return 0;

	g_main_loop_run(loop);
	g_main_loop_unref(loop);
	return 0;
}
