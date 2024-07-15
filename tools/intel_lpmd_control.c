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
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/types.h>

#include <glib.h>
#include <glib/gprintf.h>

#include <gio/gio.h>
#include <gio/gdbusmessage.h>

#define INTEL_LPMD_SERVICE_NAME         "org.freedesktop.intel_lpmd"
#define INTEL_LPMD_SERVICE_OBJECT_PATH  "/org/freedesktop/intel_lpmd"
#define INTEL_LPMD_SERVICE_INTERFACE    "org.freedesktop.intel_lpmd"

int
main(int argc, char **argv)
{
	g_autoptr(GDBusConnection) connection = NULL;
	g_autoptr (GString) command = NULL;
	GError *error = NULL;

	if (geteuid () != 0) {
		g_warning ("Must run as root");
		exit (1);
	}

	if (argc < 2) {
		fprintf (stderr, "intel_lpmd_control: missing control command\n");
		fprintf (stderr, "syntax:\n");
		fprintf (stderr, "intel_lpmd_control ON|OFF|AUTO\n");
		exit (0);
	}

	if (!strncmp (argv[1], "ON", 2))
		command = g_string_new ("LPM_FORCE_ON");
	else if (!strncmp (argv[1], "OFF", 3))
		command = g_string_new ("LPM_FORCE_OFF");
	else if (!strncmp (argv[1], "AUTO", 4))
		command = g_string_new ("LPM_AUTO");
	else {
		g_warning ("intel_lpmd_control: Invalid command");
		exit (1);
	}

	connection = g_bus_get_sync(G_BUS_TYPE_SYSTEM, NULL, &error);
	if (connection == NULL)
		return FALSE;

	g_dbus_connection_call_sync (connection,
				     INTEL_LPMD_SERVICE_NAME,
				     INTEL_LPMD_SERVICE_OBJECT_PATH,
				     INTEL_LPMD_SERVICE_INTERFACE,
				     command->str,
				     NULL,
				     NULL,
				     G_DBUS_CALL_FLAGS_NONE,
				     -1,
				     NULL,
				     &error);

	if (error != NULL) {
		g_warning ("Fail on connecting lpmd: %s", error->message);
		exit (1);
	}

	return 0;
}
