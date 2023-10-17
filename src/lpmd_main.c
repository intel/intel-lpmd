/*
 * lpmd_main.c: Intel Low Power Daemon main entry point
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
 * This source file contains main() function, which parses command line
 * option. Call lpmd init function. Provide logging support.
 * Also allow to daemonize.
 */

#include <glib-unix.h>
#include <syslog.h>

#include "lpmd.h"

#if !defined(INTEL_LPMD_DIST_VERSION)
#define INTEL_LPMD_DIST_VERSION PACKAGE_VERSION
#endif

#define EXIT_UNSUPPORTED 2

extern int intel_lpmd_dbus_server_init(gboolean (*exit_handler)(void));

// Lock file
static int lock_file_handle = -1;
static const char *lock_file = TDRUNDIR "/intel_lpmd.pid";

// Default log level
static int lpmd_log_level = G_LOG_LEVEL_ERROR | G_LOG_LEVEL_CRITICAL | G_LOG_LEVEL_WARNING
		| G_LOG_LEVEL_MESSAGE;

int in_debug_mode(void)
{
	return !!(lpmd_log_level & G_LOG_LEVEL_DEBUG);
}

// Daemonize or not
static gboolean intel_lpmd_daemonize;
static gboolean use_syslog;

// Disable dbus
static gboolean dbus_enable;

static GMainLoop *g_main_loop;

// g_log handler. All logs will be directed here
static void intel_lpmd_logger(const gchar *log_domain, GLogLevelFlags log_level,
								const gchar *message, gpointer user_data)
{
	if (!(lpmd_log_level & log_level))
		return;

	int syslog_priority;
	const char *prefix;
	time_t seconds;

	switch (log_level) {
		case G_LOG_LEVEL_ERROR:
			prefix = "[CRIT]";
			syslog_priority = LOG_CRIT;
			break;
		case G_LOG_LEVEL_CRITICAL:
			prefix = "[ERR]";
			syslog_priority = LOG_ERR;
			break;
		case G_LOG_LEVEL_WARNING:
			prefix = "[WARN]";
			syslog_priority = LOG_WARNING;
			break;
		case G_LOG_LEVEL_MESSAGE:
			prefix = "[MSG]";
			syslog_priority = LOG_NOTICE;
			break;
		case G_LOG_LEVEL_DEBUG:
			prefix = "[DEBUG]";
			syslog_priority = LOG_DEBUG;
			break;
		case G_LOG_LEVEL_INFO:
		default:
			prefix = "[INFO]";
			syslog_priority = LOG_INFO;
			break;
	}

	seconds = time (NULL);

	if (use_syslog)
		syslog (syslog_priority, "%s", message);
	else
		g_print ("[%lld]%s%s", (long long) seconds, prefix, message);
}

static void clean_up_lockfile(void)
{
	if (lock_file_handle != -1) {
		(void) close (lock_file_handle);
		(void) unlink (lock_file);
	}
}

static bool check_intel_lpmd_running(void)
{

	lock_file_handle = open (lock_file, O_RDWR | O_CREAT, 0600);
	if (lock_file_handle == -1) {
//		 Couldn't open lock file
		lpmd_log_error ("Could not open PID lock file %s, exiting\n", lock_file);
		return false;
	}
//	 Try to lock file
	if (lockf (lock_file_handle, F_TLOCK, 0) == -1) {
//		 Couldn't get lock on lock file
		lpmd_log_error ("Couldn't get lock file %d\n", getpid ());
		close (lock_file_handle);
		return true;
	}

	return false;
}

// SIGTERM & SIGINT handler
static gboolean sig_int_handler(void)
{
//	 Call terminate function
	lpmd_terminate ();

	sleep (1);

	if (g_main_loop)
		g_main_loop_quit (g_main_loop);

//	 Clean up if any
	clean_up_lockfile ();

	exit (EXIT_SUCCESS);

	return FALSE;
}

int main(int argc, char *argv[])
{
	gboolean show_version = FALSE;
	gboolean log_info = FALSE;
	gboolean log_debug = FALSE;
	gboolean no_daemon = FALSE;
	gboolean systemd = FALSE;
	gboolean success;
	GOptionContext *opt_ctx;
	int ret;

	intel_lpmd_daemonize = TRUE;
	use_syslog = TRUE;
	dbus_enable = FALSE;

	GOptionEntry options[] =
			{ { "version", 0, 0, G_OPTION_ARG_NONE, &show_version, N_ ("Print intel_lpmd version and exit"), NULL },
			  { "no-daemon", 0, 0, G_OPTION_ARG_NONE, &no_daemon, N_ ("Don't become a daemon: Default is daemon mode"), NULL },
			  { "systemd", 0, 0, G_OPTION_ARG_NONE, &systemd, N_ ("Assume daemon is started by systemd, always run in non-daemon mode when using this parameter"), NULL },
			  { "loglevel=info", 0, 0, G_OPTION_ARG_NONE, &log_info, N_ ("Log severity: info level and up"), NULL },
			  { "loglevel=debug", 0, 0, G_OPTION_ARG_NONE, &log_debug, N_ ("Log severity: debug level and up: Max logging"), NULL },
			  { "dbus-enable", 0, 0, G_OPTION_ARG_NONE, &dbus_enable, N_ ( "Enable Dbus"), NULL },
			  { NULL, 0, 0, G_OPTION_ARG_NONE, NULL, NULL, NULL } };

	if (!g_module_supported ()) {
		fprintf (stderr, "GModules are not supported on your platform!\n");
		exit (EXIT_FAILURE);
	}

//	Set locale to be able to use environment variables
	setlocale (LC_ALL, "");

	bindtextdomain (GETTEXT_PACKAGE, TDLOCALEDIR);
	bind_textdomain_codeset (GETTEXT_PACKAGE, "UTF-8");
	textdomain (GETTEXT_PACKAGE);

//	 Parse options
	opt_ctx = g_option_context_new (NULL);
	g_option_context_set_translation_domain (opt_ctx, GETTEXT_PACKAGE);
	g_option_context_set_ignore_unknown_options (opt_ctx, FALSE);
	g_option_context_set_help_enabled (opt_ctx, TRUE);
	g_option_context_add_main_entries (opt_ctx, options, NULL);

	g_option_context_set_summary (opt_ctx,
				"Intel Low Power Daemon based on system usage takes action "
				"to reduce active power of the system.\n\n"
				"Copyright (c) 2023, Intel Corporation\n"
				"This program comes with ABSOLUTELY NO WARRANTY.\n"
				"This work is licensed under GPL v2.\n\n"
				"Use \"man intel_lpmd\" to get more details.");

	success = g_option_context_parse (opt_ctx, &argc, &argv, NULL);
	g_option_context_free (opt_ctx);

	if (!success) {
		fprintf (stderr, "Invalid option.  Please use --help to see a list of valid options.\n");
		exit (EXIT_FAILURE);
	}

	if (show_version) {
		fprintf (stdout, INTEL_LPMD_DIST_VERSION "\n");
		exit (EXIT_SUCCESS);
	}

	if (getuid () != 0) {
		fprintf (stderr, "You must be root to run intel_lpmd!\n");
		exit (EXIT_FAILURE);
	}

	if (g_mkdir_with_parents (TDRUNDIR, 0755) != 0) {
		fprintf (stderr, "Cannot create '%s': %s", TDRUNDIR, strerror (errno));
		exit (EXIT_FAILURE);
	}

	if (g_mkdir_with_parents (TDCONFDIR, 0755) != 0) {
		fprintf (stderr, "Cannot create '%s': %s", TDCONFDIR, strerror (errno));
		exit (EXIT_FAILURE);
	}

	if (log_info) {
		lpmd_log_level |= G_LOG_LEVEL_INFO;
	}

	if (log_debug) {
		lpmd_log_level |= G_LOG_LEVEL_INFO | G_LOG_LEVEL_DEBUG;
	}

	openlog ("intel_lpmd", LOG_PID, LOG_USER | LOG_DAEMON | LOG_SYSLOG);
//	 Don't care return val

	intel_lpmd_daemonize = !no_daemon && !systemd;
	use_syslog = !no_daemon || systemd;
	g_log_set_handler (NULL, G_LOG_LEVEL_MASK, intel_lpmd_logger, NULL);

	if (check_intel_lpmd_running ()) {
		lpmd_log_error ("An instance of intel_lpmd is already running, exiting ...\n");
		exit (EXIT_FAILURE);
	}

	if (!intel_lpmd_daemonize) {
		g_unix_signal_add (SIGINT, G_SOURCE_FUNC (sig_int_handler), NULL);
		g_unix_signal_add (SIGTERM, G_SOURCE_FUNC (sig_int_handler), NULL);
	}

	/*
	 *  Initialize the GType/GObject system
	 *  Since GLib 2.36, the type system is initialised automatically and this function
	 *  does nothing. Deprecated since: 2.36
	 */

	g_type_init ();

	// Create a main loop that will dispatch callbacks
	g_main_loop = g_main_loop_new (NULL, FALSE);
	if (g_main_loop == NULL) {
		clean_up_lockfile ();
		lpmd_log_error ("Couldn't create GMainLoop:\n");
		return LPMD_FATAL_ERROR;
	}

	if (dbus_enable)
		intel_dbus_server_init (sig_int_handler);

	if (intel_lpmd_daemonize) {
		printf ("Ready to serve requests: Daemonizing..\n");
		lpmd_log_info ("intel_lpmd ver %s: Ready to serve requests: Daemonizing..\n",
		INTEL_LPMD_DIST_VERSION);

		if (daemon (0, 0) != 0) {
			clean_up_lockfile ();
			lpmd_log_error ("Failed to daemonize.\n");
			return LPMD_FATAL_ERROR;
		}
	}

	ret = lpmd_main ();

	if (ret != LPMD_SUCCESS) {
		clean_up_lockfile ();
		closelog ();

		if (ret == LPMD_ERROR)
			exit (EXIT_UNSUPPORTED);
		else
			exit (EXIT_FAILURE);
	}

//	Start service requests on the D-Bus
	lpmd_log_debug ("Start main loop\n");
	g_main_loop_run (g_main_loop);
	lpmd_log_warn ("Oops g main loop exit..\n");

	fprintf (stdout, "Exiting ..\n");
	clean_up_lockfile ();
	closelog ();
}
