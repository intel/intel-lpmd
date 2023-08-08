/*
 * intel_lpmd_control.c: Intel Low Power Daemon control utility
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
 * This program can be used to control modes of Low power mode daemon
 */

#include <stdio.h>
#include <unistd.h>
#include <sys/types.h>
#include <dbus/dbus-glib.h>

#define INTEL_LPMD_SERVICE_NAME         "org.freedesktop.intel_lpmd"
#define INTEL_LPMD_SERVICE_OBJECT_PATH  "/org/freedesktop/intel_lpmd"
#define INTEL_LPMD_SERVICE_INTERFACE    "org.freedesktop.intel_lpmd"

int main(int argc, char **argv)
{
	GError *error = NULL;
	DBusGConnection *bus;
	DBusGProxy *proxy;
	char command[20];

	if (geteuid () != 0) {
		fprintf (stderr, "Must run as root\n");
		exit (0);
	}

	if (argc < 2) {
		fprintf (stderr, "intel_lpmd_control: missing control command\n");
		fprintf (stderr, "syntax:\n");
		fprintf (stderr, "intel_lpmd_control ON|OFF|AUTO\n");
		exit (0);
	}

	if (!strncmp (argv[1], "ON", 2))
		strcpy (command, "LPM_FORCE_ON");
	else if (!strncmp (argv[1], "OFF", 3))
		strcpy (command, "LPM_FORCE_OFF");
	else if (!strncmp (argv[1], "AUTO", 4))
		strcpy (command, "LPM_AUTO");
	else {
		fprintf (stderr, "intel_lpmd_control: Invalid command\n");
		exit (0);
	}

	bus = dbus_g_bus_get (DBUS_BUS_SYSTEM, &error);
	if (!bus) {
		g_warning ("Unable to connect to system bus: %s", error->message);
		g_error_free (error);
	}

	proxy = dbus_g_proxy_new_for_name (bus, INTEL_LPMD_SERVICE_NAME,
									   INTEL_LPMD_SERVICE_OBJECT_PATH,
									   INTEL_LPMD_SERVICE_INTERFACE);

	if (!dbus_g_proxy_call (proxy, command, &error, G_TYPE_INVALID, G_TYPE_INVALID)) {
		g_warning ("Failed to send message: %s", error->message);
		g_error_free (error);
	}
}
