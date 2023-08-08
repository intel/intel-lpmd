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

#include "lpmd.h"

typedef struct {
	GObject parent;
} PrefObject;

typedef struct {
	GObjectClass parent;
} PrefObjectClass;

GType
pref_object_get_type(void);
#define MAX_DBUS_REPLY_STR_LEN	100
#define PREF_TYPE_OBJECT (pref_object_get_type())
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
dbus_interface_s_uv__mo_de__en_te_r(PrefObject *obj, GError **error);

static gboolean
dbus_interface_s_uv__mo_de__ex_it(PrefObject *obj, GError **error);

#include "intel_lpmd_dbus_interface.h"

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

	dbus_g_object_type_install_info ( PREF_TYPE_OBJECT, &dbus_glib_dbus_interface_object_info);
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

static gboolean dbus_interface_s_uv__mo_de__en_te_r(PrefObject *obj, GError **error)
{
	lpmd_log_debug ("intel_lpmd_dbus_interface_suv_enter\n");

	if (!has_suv_support ())
		return FALSE;

	lpmd_suv_enter ();

	return TRUE;
}

static gboolean dbus_interface_s_uv__mo_de__ex_it(PrefObject *obj, GError **error)
{
	if (!has_suv_support ())
		return FALSE;

	lpmd_log_debug ("intel_lpmd_dbus_interface_suv_exit\n");
	lpmd_suv_exit ();

	return TRUE;
}

int intel_dbus_server_init(gboolean (*exit_handler)(void))
{
	DBusGConnection *bus;
	DBusGProxy *bus_proxy;
	GError *error = NULL;
	guint result;
	PrefObject *value_obj;

	intel_lpmd_dbus_exit_callback = exit_handler;

	bus = dbus_g_bus_get (DBUS_BUS_SYSTEM, &error);
	if (error != NULL) {
		lpmd_log_error ("Couldn't connect to session bus: %s:\n", error->message);
		return LPMD_FATAL_ERROR;
	}

	// Get a bus proxy instance
	bus_proxy = dbus_g_proxy_new_for_name (bus, DBUS_SERVICE_DBUS, DBUS_PATH_DBUS,
											DBUS_INTERFACE_DBUS);
	if (bus_proxy == NULL) {
		lpmd_log_error ("Failed to get a proxy for D-Bus:\n");
		return LPMD_FATAL_ERROR;
	}

	lpmd_log_debug ("Registering the well-known name (%s)\n", INTEL_LPMD_SERVICE_NAME);
	// register the well-known name
	if (!dbus_g_proxy_call (bus_proxy, "RequestName", &error, G_TYPE_STRING,
							INTEL_LPMD_SERVICE_NAME, G_TYPE_UINT, 0, G_TYPE_INVALID, G_TYPE_UINT,
							&result, G_TYPE_INVALID)) {
		lpmd_log_error ("D-Bus.RequestName RPC failed: %s\n", error->message);
		return LPMD_FATAL_ERROR;
	}
	lpmd_log_debug ("RequestName returned %d.\n", result);
	if (result != DBUS_REQUEST_NAME_REPLY_PRIMARY_OWNER) {
		lpmd_log_error ("Failed to get the primary well-known name:\n");
		return LPMD_FATAL_ERROR;
	}
	value_obj = (PrefObject*) g_object_new (PREF_TYPE_OBJECT, NULL);
	if (value_obj == NULL) {
		lpmd_log_error ("Failed to create one Value instance:\n");
		return LPMD_FATAL_ERROR;
	}

	lpmd_log_debug ("Registering it on the D-Bus.\n");
	dbus_g_connection_register_g_object (bus, INTEL_LPMD_SERVICE_OBJECT_PATH, G_OBJECT (value_obj));

	return LPMD_SUCCESS;
}
