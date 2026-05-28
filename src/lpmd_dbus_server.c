// SPDX-License-Identifier: GPL-2.0-or-later
/* Copyright (C) 2026 Intel Corporation */

#include <gio/gio.h>
#include <glib.h>
#include <glib/gprintf.h>
#include <glib-object.h>

#include "lpmd.h"

struct _PrefObject {
	GObject parent;
};

#define PREF_TYPE_OBJECT (pref_object_get_type())
G_DECLARE_FINAL_TYPE(PrefObject, pref_object, PREF, OBJECT, GObject)

#define MAX_DBUS_REPLY_STR_LEN	100
G_DEFINE_TYPE(PrefObject, pref_object, G_TYPE_OBJECT)

static gboolean
dbus_interface_terminate(PrefObject *obj, GError **error);

static gboolean
dbus_interface_l_pm__fo_rc_e__on(PrefObject *obj, GError **error);

static gboolean
dbus_interface_l_pm__fo_rc_e__of_f(PrefObject *obj, GError **error);

static gboolean
dbus_interface_l_pm__au_to(PrefObject *obj, GError **error);

static gboolean
dbus_interface_l_pm__ba_si_c(PrefObject *obj, GError **error);

static gboolean
(*intel_lpmd_dbus_exit_callback)(void);

/*
 * Tracking for the user-session focus helper (intel_lpmd_focus_helper).
 * When the helper calls LPM_FOCUS_HELPER_READY(1) we record its unique
 * bus name and start a NameOwner watch. If that name disappears
 * (helper crashed / exited / lost the session) we automatically flip
 * the focus-helper-present flag back to 0 so USER_INITIATED reverts
 * to mirroring USER_INTERACTIVE.
 */
static guint  g_focus_helper_watch_id;
static gchar *g_focus_helper_owner;

static void
focus_helper_vanished(GDBusConnection *connection,
		      const gchar     *name,
		      gpointer         user_data)
{
	(void)connection;
	(void)user_data;
	lpmd_log_info("focus helper %s vanished; reverting\n",
		      name ? name : "(unknown)");
	(void)lpmd_process_cpuset_set_focus_helper_present(0);
	if (g_focus_helper_watch_id) {
		g_bus_unwatch_name(g_focus_helper_watch_id);
		g_focus_helper_watch_id = 0;
	}
	g_clear_pointer(&g_focus_helper_owner, g_free);
}

static void
focus_helper_track(GDBusConnection *connection, const gchar *sender)
{
	/* Drop any prior watch -- a fresh READY supersedes the previous
	 * helper instance. */
	if (g_focus_helper_watch_id) {
		g_bus_unwatch_name(g_focus_helper_watch_id);
		g_focus_helper_watch_id = 0;
	}
	g_clear_pointer(&g_focus_helper_owner, g_free);

	if (!connection || !sender || !*sender)
		return;

	g_focus_helper_owner = g_strdup(sender);
	g_focus_helper_watch_id = g_bus_watch_name_on_connection(
		connection, sender,
		G_BUS_NAME_WATCHER_FLAGS_NONE,
		NULL,			/* name_appeared (already here) */
		focus_helper_vanished,
		NULL, NULL);
	lpmd_log_debug("focus helper tracked: %s (watch_id=%u)\n",
		       sender, g_focus_helper_watch_id);
}

static void
focus_helper_untrack(void)
{
	if (g_focus_helper_watch_id) {
		g_bus_unwatch_name(g_focus_helper_watch_id);
		g_focus_helper_watch_id = 0;
	}
	g_clear_pointer(&g_focus_helper_owner, g_free);
}

// Dbus object initialization
static void pref_object_init(PrefObject *obj)
{
	g_assert(obj);
}

// Dbus object class initialization
static void pref_object_class_init(PrefObjectClass *_class)
{
	g_assert(_class);
}

static gboolean dbus_interface_terminate(PrefObject *obj, GError **error)
{
	lpmd_log_debug("intel_lpmd_dbus_interface_terminate\n");
	lpmd_terminate();
	if (intel_lpmd_dbus_exit_callback)
		intel_lpmd_dbus_exit_callback();

	return TRUE;
}

static gboolean dbus_interface_l_pm__fo_rc_e__on(PrefObject *obj, GError **error)
{
	lpmd_log_debug("intel_lpmd_dbus_interface_lpm_enter\n");
	lpmd_force_on();

	return TRUE;
}

static gboolean dbus_interface_l_pm__fo_rc_e__of_f(PrefObject *obj, GError **error)
{
	lpmd_log_debug("intel_lpmd_dbus_interface_lpm_exit\n");
	lpmd_force_off();

	return TRUE;
}

static gboolean dbus_interface_l_pm__au_to(PrefObject *obj, GError **error)
{
	lpmd_set_auto();
	return TRUE;
}

static gboolean dbus_interface_l_pm__ba_si_c(PrefObject *obj, GError **error)
{
	lpmd_set_process_preconfig ();
	return TRUE;
}

#pragma GCC diagnostic push

static GDBusInterfaceVTable interface_vtable;

static GDBusNodeInfo *
lpmd_dbus_load_introspection(const gchar *filename, GError **error)
{
	g_autoptr(GBytes) data = NULL;
	g_autofree gchar *path = NULL;

	path = g_build_filename("/org/freedesktop/intel_lpmd", filename, NULL);
	data = g_resources_lookup_data(path, G_RESOURCE_LOOKUP_FLAGS_NONE, error);
	if (!data)
		return NULL;

	return g_dbus_node_info_new_for_xml((gchar *)g_bytes_get_data(data, NULL), error);
}

static void
lpmd_dbus_handle_method_call(GDBusConnection       *connection,
			     const gchar           *sender,
			     const gchar           *object_path,
			     const gchar           *interface_name,
			     const gchar           *method_name,
			     GVariant              *parameters,
			     GDBusMethodInvocation *invocation,
			     gpointer               user_data)
{
	PrefObject *obj = PREF_OBJECT(user_data);
	g_autoptr(GError) error = NULL;

	lpmd_log_debug("Dbus method called %s %s.\n", interface_name, method_name);

	if (g_strcmp0(method_name, "Terminate") == 0) {
		g_dbus_method_invocation_return_value(invocation, NULL);
		dbus_interface_terminate(obj, &error);
		return;
	}

	if (g_strcmp0(method_name, "LPM_FORCE_ON") == 0) {
		g_dbus_method_invocation_return_value(invocation, NULL);
		dbus_interface_l_pm__fo_rc_e__on(obj, &error);
		return;
	}

	if (g_strcmp0(method_name, "LPM_FORCE_OFF") == 0) {
		g_dbus_method_invocation_return_value(invocation, NULL);
		dbus_interface_l_pm__fo_rc_e__of_f(obj, &error);
		return;
	}
	if (g_strcmp0(method_name, "LPM_AUTO") == 0) {
		g_dbus_method_invocation_return_value(invocation, NULL);
		dbus_interface_l_pm__au_to(obj, &error);
		return;
	}

	if (g_strcmp0(method_name, "LPM_PROCESS_PRECONFIG") == 0) {
		g_dbus_method_invocation_return_value(invocation, NULL);
		dbus_interface_l_pm__ba_si_c(obj, &error);
		return;
	}

	if (g_strcmp0(method_name, "GetState") == 0) {
		static const char * const state_names[] = {
			[LPMD_OFF] = "OFF",
			[LPMD_ON] = "ON",
			[LPMD_AUTO] = "AUTO",
			[LPMD_PROCESS_PRECONFIG] = "PROCESS-PRECONFIG",
			[LPMD_FREEZE] = "FREEZE",
			[LPMD_RESTORE] = "RESTORE",
			[LPMD_TERMINATE] = "TERMINATE",
		};
		int state = get_lpmd_state();
		const char *name = (state >= 0 && state <= LPMD_TERMINATE) ? state_names[state] : "UNKNOWN";

		g_dbus_method_invocation_return_value(invocation,
						      g_variant_new("(s)", name));
		return;
	}

	if (g_strcmp0(method_name, "LPM_LIST_UNBOUND_PROCS") == 0) {
		g_dbus_method_invocation_return_value(invocation, NULL);
		lpmd_process_cpuset_print_unbound(0);
		return;
	}

	if (g_strcmp0(method_name, "LPM_LIST_UNBOUND_USER_PROCS") == 0) {
		g_dbus_method_invocation_return_value(invocation, NULL);
		lpmd_process_cpuset_print_unbound(1);
		return;
	}

	if (g_strcmp0(method_name, "LPM_LIST_BOUND_PROCS") == 0) {
		g_dbus_method_invocation_return_value(invocation, NULL);
		lpmd_process_cpuset_print_bound();
		return;
	}

	if (g_strcmp0(method_name, "LPM_UNBIND_ALL") == 0) {
		g_dbus_method_invocation_return_value(invocation, NULL);
		lpmd_process_cpuset_unbind_all();
		return;
	}

	if (g_strcmp0(method_name, "LPM_ADD_NEW_PROCESS") == 0) {
		const gchar *pname = NULL;
		const gchar *pclass = NULL;
		gint result;

		g_variant_get(parameters, "(&s&s)", &pname, &pclass);
		result = lpmd_process_cpuset_add_process(pname, pclass);
		g_dbus_method_invocation_return_value(invocation,
						      g_variant_new("(i)", result));
		return;
	}

	if (g_strcmp0(method_name, "LPM_SET_FOCUS_PID") == 0) {
		gint pid = 0;
		gint result;

		g_variant_get(parameters, "(i)", &pid);
		result = lpmd_process_cpuset_set_focus_pid((pid_t)pid);
		g_dbus_method_invocation_return_value(invocation,
						      g_variant_new("(i)", result));
		return;
	}

	if (g_strcmp0(method_name, "LPM_FOCUS_HELPER_READY") == 0) {
		gint present = 0;
		gint result;

		g_variant_get(parameters, "(i)", &present);
		result = lpmd_process_cpuset_set_focus_helper_present(present);
		/* On success, start (or stop) tracking the helper's unique
		 * bus name so we can auto-revert if it dies. */
		if (result == 0) {
			if (present)
				focus_helper_track(connection, sender);
			else
				focus_helper_untrack();
		}
		g_dbus_method_invocation_return_value(invocation,
						      g_variant_new("(i)", result));
		return;
	}

	g_set_error(&error,
		    G_DBUS_ERROR,
		    G_DBUS_ERROR_UNKNOWN_METHOD,
		    "no such method %s",
		    method_name);
	g_dbus_method_invocation_return_gerror(invocation, error);
}

static GVariant *
lpmd_dbus_handle_get_property(GDBusConnection  *connection,
			      const gchar      *sender,
			      const gchar      *object_path,
			      const gchar      *interface_name,
			      const gchar      *property_name,
			      GError          **error,
			      gpointer          user_data)
{
	return NULL;
}

static gboolean
lpmd_dbus_handle_set_property(GDBusConnection  *connection,
			      const gchar      *sender,
			      const gchar      *object_path,
			      const gchar      *interface_name,
			      const gchar      *property_name,
			      GVariant         *value,
			      GError          **error,
			      gpointer          user_data)
{
	return TRUE;
}

static void
lpmd_dbus_on_bus_acquired(GDBusConnection *connection,
			  const gchar     *name,
			  gpointer         user_data)
{
	GDBusNodeInfo *introspection_data = NULL;
	GDBusProxy *proxy_id = NULL;
	guint registration_id;
	GError *error = NULL;

	if (!user_data) {
		lpmd_log_error("user_data is NULL\n");
		return;
	}

	introspection_data = lpmd_dbus_load_introspection("src/intel_lpmd_dbus_interface.xml",
							  &error);
	if (!introspection_data || error) {
		lpmd_log_error("Couldn't create introspection data: %s:\n",
			       error->message);
		return;
	}

	registration_id = g_dbus_connection_register_object(connection,
							    "/org/freedesktop/intel_lpmd",
							    introspection_data->interfaces[0],
							    &interface_vtable,
							    user_data,
							    NULL,
							    &error);

	proxy_id = g_dbus_proxy_new_sync(connection,
					 G_DBUS_PROXY_FLAGS_DO_NOT_LOAD_PROPERTIES,
					 NULL,
					 "org.freedesktop.DBus",
					 "/org/freedesktop/DBus",
					 "org.freedesktop.DBus",
					 NULL,
					 &error);
	g_assert(registration_id > 0);
	g_assert(proxy_id);
}

static void
lpmd_dbus_on_name_acquired(GDBusConnection *connection,
			   const gchar     *name,
			   gpointer         user_data)
{
}

static void
lpmd_dbus_on_name_lost(GDBusConnection *connection,
		       const gchar     *name,
		       gpointer         user_data)
{
	g_warning("Lost the name %s\n", name);
	exit(1);
}

// Set up Dbus server with GDBus
int intel_dbus_server_init(gboolean (*exit_handler)(void))
{
	PrefObject *value_obj;

	intel_lpmd_dbus_exit_callback = exit_handler;

	value_obj = PREF_OBJECT(g_object_new(PREF_TYPE_OBJECT, NULL));
	if (!value_obj) {
		lpmd_log_error("Failed to create one Value instance:\n");
		return LPMD_FATAL_ERROR;
	}

	interface_vtable.method_call = lpmd_dbus_handle_method_call;
	interface_vtable.get_property = lpmd_dbus_handle_get_property;
	interface_vtable.set_property = lpmd_dbus_handle_set_property;

	watcher_id = g_bus_own_name(G_BUS_TYPE_SYSTEM,
				    "org.freedesktop.intel_lpmd",
				    G_BUS_NAME_OWNER_FLAGS_REPLACE,
				    lpmd_dbus_on_bus_acquired,
				    lpmd_dbus_on_name_acquired,
				    lpmd_dbus_on_name_lost,
				    g_object_ref(value_obj),
				    NULL);

	return LPMD_SUCCESS;
}

#pragma GCC diagnostic pop
