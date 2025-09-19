/*
 * lpmd_dbus_server.c: Dbus server for intel_lpmd
 *
 * Copyright (C) 2023 Intel Corporation. All rights reserved.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
 *
 * This file contains function to start dbus server and provide callbacks for
 * dbus messages.
 */

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
(*intel_lpmd_dbus_exit_callback)(void);

// Dbus object initialization
static void pref_object_init(PrefObject *obj)
{
	g_assert (obj != NULL);
}

// Dbus object class initialization
static void pref_object_class_init(PrefObjectClass *_class)
{
	g_assert (_class != NULL);
}

static gboolean dbus_interface_terminate(PrefObject *obj, GError **error)
{
	lpmd_log_debug ("intel_lpmd_dbus_interface_terminate\n");
	lpmd_terminate ();
	if (intel_lpmd_dbus_exit_callback)
		intel_lpmd_dbus_exit_callback ();

	return TRUE;
}

static gboolean dbus_interface_l_pm__fo_rc_e__on(PrefObject *obj, GError **error)
{
	lpmd_log_debug ("intel_lpmd_dbus_interface_lpm_enter\n");
	lpmd_force_on ();

	return TRUE;
}

static gboolean dbus_interface_l_pm__fo_rc_e__of_f(PrefObject *obj, GError **error)
{
	lpmd_log_debug ("intel_lpmd_dbus_interface_lpm_exit\n");
	lpmd_force_off ();

	return TRUE;
}

static gboolean dbus_interface_l_pm__au_to(PrefObject *obj, GError **error)
{
	lpmd_set_auto ();
	return TRUE;
}

#pragma GCC diagnostic push

static GDBusInterfaceVTable interface_vtable;
extern gint watcher_id;

static GDBusNodeInfo *
lpmd_dbus_load_introspection(const gchar *filename, GError **error)
{
	g_autoptr(GBytes) data = NULL;
	g_autofree gchar *path = NULL;

	path = g_build_filename("/org/freedesktop/intel_lpmd", filename, NULL);
	data = g_resources_lookup_data(path, G_RESOURCE_LOOKUP_FLAGS_NONE, error);
	if (data == NULL)
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
			     gpointer          user_data) {
	return TRUE;
}


static void
lpmd_dbus_on_bus_acquired(GDBusConnection *connection,
			 const gchar     *name,
			 gpointer         user_data) {
	guint registration_id;
	GDBusProxy *proxy_id = NULL;
	GError *error = NULL;
	GDBusNodeInfo *introspection_data = NULL;

	if (user_data == NULL) {
		lpmd_log_error("user_data is NULL\n");
		return;
	}

	introspection_data = lpmd_dbus_load_introspection("src/intel_lpmd_dbus_interface.xml",
							 &error);
	if (introspection_data == NULL || error != NULL) {
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
	g_assert(proxy_id != NULL);
}

static void
lpmd_dbus_on_name_acquired(GDBusConnection *connection,
			  const gchar     *name,
			  gpointer         user_data) {
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
int intel_dbus_server_init(gboolean (*exit_handler)(void)) {
	PrefObject *value_obj;

	intel_lpmd_dbus_exit_callback = exit_handler;

	value_obj = PREF_OBJECT(g_object_new(PREF_TYPE_OBJECT, NULL));
	if (value_obj == NULL) {
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
