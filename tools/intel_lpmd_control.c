// SPDX-License-Identifier: GPL-2.0-or-later
/* Copyright (C) 2026 Intel Corporation */

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

int main(int argc, char **argv)
{
	g_autoptr(GDBusConnection) connection = NULL;
	g_autoptr(GString) command = NULL;
	g_autoptr(GVariant) result = NULL;
	GError *error = NULL;
	const gchar *state;

	if (geteuid()) {
		g_warning("Must run as root");
		exit(1);
	}

	if (argc < 2) {
		fprintf (stderr, "intel_lpmd_control: missing control command\n");
		fprintf (stderr, "syntax:\n");
		fprintf (stderr, "intel_lpmd_control ON|OFF|AUTO|PROCESS-PRECONFIG|STATUS|LIST-UNBOUND|LIST-UNBOUND-USER|LIST-BOUND|UNBIND-ALL\n");
		fprintf (stderr, "intel_lpmd_control ADD-PROCESS <name> <user_interactive|user_initiated|Unclassified|utility|background|realtime|GameProfileCPU|GameProfileGPU|GameProfileHybrid>\n");
		fprintf (stderr, "intel_lpmd_control GET-PROC-CLASSIFICATION <name>\n");
		fprintf (stderr, "intel_lpmd_control SET-FOCUS <pid>   (use 0 to clear)\n");
		exit (0);
	}

	connection = g_bus_get_sync(G_BUS_TYPE_SYSTEM, NULL, &error);
	if (!connection)
		return FALSE;

	if (!strncmp(argv[1], "STATUS", 6)) {
		result = g_dbus_connection_call_sync(connection,
						     INTEL_LPMD_SERVICE_NAME,
						     INTEL_LPMD_SERVICE_OBJECT_PATH,
						     INTEL_LPMD_SERVICE_INTERFACE,
						     "GetState",
						     NULL,
						     G_VARIANT_TYPE("(s)"),
						     G_DBUS_CALL_FLAGS_NONE,
						     -1,
						     NULL,
						     &error);

		if (error) {
			g_warning("Fail on connecting lpmd: %s", error->message);
			exit(1);
		}

		g_variant_get(result, "(&s)", &state);
		g_print("%s\n", state);

		return 0;
	}

	if (!strcmp (argv[1], "ADD-PROCESS")) {
		g_autoptr(GVariant) result = NULL;
		gint rc = -1;

		if (argc < 4) {
			fprintf (stderr,
				 "ADD-PROCESS requires <name> <classification>\n");
			exit (1);
		}

		result = g_dbus_connection_call_sync (connection,
						      INTEL_LPMD_SERVICE_NAME,
						      INTEL_LPMD_SERVICE_OBJECT_PATH,
						      INTEL_LPMD_SERVICE_INTERFACE,
						      "LPM_ADD_NEW_PROCESS",
						      g_variant_new ("(ss)", argv[2], argv[3]),
						      G_VARIANT_TYPE ("(i)"),
						      G_DBUS_CALL_FLAGS_NONE,
						      -1,
						      NULL,
						      &error);
		if (error != NULL) {
			g_warning ("Fail on connecting lpmd: %s", error->message);
			exit (1);
		}
		g_variant_get (result, "(i)", &rc);
		if (rc < 0) {
			fprintf (stderr,
				 "ADD-PROCESS rejected (invalid classification, "
				 "process_cpuset disabled, or persist failed)\n");
			exit (1);
		}
		return 0;
	}

	if (!strcmp (argv[1], "GET-PROC-CLASSIFICATION")) {
		g_autoptr(GVariant) result = NULL;
		const gchar *reply = NULL;
		gint rc = -1;

		if (argc < 3) {
			fprintf (stderr,
				 "GET-PROC-CLASSIFICATION requires <name>\n");
			exit (1);
		}

		result = g_dbus_connection_call_sync (connection,
						      INTEL_LPMD_SERVICE_NAME,
						      INTEL_LPMD_SERVICE_OBJECT_PATH,
						      INTEL_LPMD_SERVICE_INTERFACE,
						      "LPM_GET_PROC_CLASSIFICATION",
						      g_variant_new ("(s)", argv[2]),
						      G_VARIANT_TYPE ("(si)"),
						      G_DBUS_CALL_FLAGS_NONE,
						      -1,
						      NULL,
						      &error);
		if (error != NULL) {
			g_warning ("Fail on connecting lpmd: %s", error->message);
			exit (1);
		}
		g_variant_get (result, "(&si)", &reply, &rc);
		g_print ("%s\n", reply);
		if (rc < 0)
			exit (1);
		return 0;
	}

	if (!strcmp (argv[1], "SET-FOCUS")) {
		g_autoptr(GVariant) result = NULL;
		gint rc = -1;
		gint pid;

		if (argc < 3) {
			fprintf (stderr, "SET-FOCUS requires <pid> (0 to clear)\n");
			exit (1);
		}
		pid = (gint)strtol (argv[2], NULL, 10);

		result = g_dbus_connection_call_sync (connection,
						      INTEL_LPMD_SERVICE_NAME,
						      INTEL_LPMD_SERVICE_OBJECT_PATH,
						      INTEL_LPMD_SERVICE_INTERFACE,
						      "LPM_SET_FOCUS_PID",
						      g_variant_new ("(i)", pid),
						      G_VARIANT_TYPE ("(i)"),
						      G_DBUS_CALL_FLAGS_NONE,
						      -1,
						      NULL,
						      &error);
		if (error != NULL) {
			g_warning ("Fail on connecting lpmd: %s", error->message);
			exit (1);
		}
		g_variant_get (result, "(i)", &rc);
		if (rc < 0) {
			fprintf (stderr, "SET-FOCUS rejected (process_cpuset disabled or sd-bus error)\n");
			exit (1);
		}
		return 0;
	}

	if (!strncmp (argv[1], "ON", 2))
		command = g_string_new ("LPM_FORCE_ON");
	else if (!strncmp (argv[1], "OFF", 3))
		command = g_string_new ("LPM_FORCE_OFF");
	else if (!strncmp (argv[1], "AUTO", 4))
		command = g_string_new ("LPM_AUTO");
	else if (!strncmp (argv[1], "PROCESS-PRECONFIG", 17))
		command = g_string_new ("LPM_PROCESS_PRECONFIG");
	else if (!strncmp (argv[1], "LIST-UNBOUND-USER", 17))
		command = g_string_new ("LPM_LIST_UNBOUND_USER_PROCS");
	else if (!strncmp (argv[1], "LIST-UNBOUND", 12))
		command = g_string_new ("LPM_LIST_UNBOUND_PROCS");
	else if (!strncmp (argv[1], "LIST-BOUND", 10))
		command = g_string_new ("LPM_LIST_BOUND_PROCS");
	else if (!strncmp (argv[1], "UNBIND-ALL", 10))
		command = g_string_new ("LPM_UNBIND_ALL");
	else {
		g_warning ("intel_lpmd_control: Invalid command");
		exit (1);
	}

	g_dbus_connection_call_sync(connection,
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

	if (error) {
		g_warning("Fail on connecting lpmd: %s", error->message);
		exit(1);
	}

	return 0;
}
