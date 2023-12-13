/*
 * lpmd_proc.c: Intel Low Power Daemon core processing
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
 * This file contains the main LPMD thread and poll loop. Call correct
 * processing function on receiving user or system command.
 */

#include "lpmd.h"

static lpmd_config_t lpmd_config;

char *lpm_cmd_str[LPM_CMD_MAX] = {
		[USER_ENTER] = "usr enter",
		[USER_EXIT] = "usr exit",
		[USER_AUTO] = "usr auto",
		[HFI_ENTER] = "hfi enter",
		[HFI_EXIT] = "hfi exit",
		[UTIL_ENTER] = "utl enter",
		[UTIL_EXIT] = "utl exit",
};

static int in_low_power_mode = 0;

static pthread_mutex_t lpmd_mutex;

int lpmd_lock(void)
{
	return pthread_mutex_lock (&lpmd_mutex);
}

int lpmd_unlock(void)
{
	return pthread_mutex_unlock (&lpmd_mutex);
}

/*
 * It may take a relatively long time to enter/exit low power mode.
 * Hold lpmd_lock to make sure there is no state change ongoing.
 */
int in_lpm(void)
{
	int ret;

	lpmd_lock ();
	ret = in_low_power_mode;
	lpmd_unlock ();
	return !!ret;
}

/* Can be configurable */
int get_idle_percentage(void)
{
	return 90;
}

/* Can be configurable */
int get_idle_duration(void)
{
	return -1;
}

int get_cpu_mode(void)
{
	return lpmd_config.mode;
}

int has_hfi_lpm_monitor(void)
{
	return !!lpmd_config.hfi_lpm_enable;
}

int has_hfi_suv_monitor(void)
{
	return !!lpmd_config.hfi_suv_enable;
}

int has_util_monitor(void)
{
	return !!lpmd_config.util_enable;
}

int get_util_entry_interval(void)
{
	return lpmd_config.util_entry_delay;
}

int get_util_exit_interval(void)
{
	return lpmd_config.util_exit_delay;
}

int get_util_entry_threshold(void)
{
	return lpmd_config.util_entry_threshold;
}

int get_util_exit_threshold(void)
{
	return lpmd_config.util_exit_threshold;
}

int get_util_entry_hyst(void)
{
	return lpmd_config.util_entry_hyst;
}

int get_util_exit_hyst(void)
{
	return lpmd_config.util_exit_hyst;
}

/* ITMT Management */
#define PATH_ITMT_CONTROL "/proc/sys/kernel/sched_itmt_enabled"

static int process_itmt(int enter)
{
	if (lpmd_config.ignore_itmt)
		return 0;

	lpmd_log_info ("Process ITMT ...\n");

	return lpmd_write_int (PATH_ITMT_CONTROL, enter > 0 ? 0 : 1, LPMD_LOG_INFO);
}

/* Main functions */

enum lpm_state {
	LPM_USER_ON = 1 << 0,
	LPM_USER_OFF = 1 << 1,
	LPM_SUV_ON = 1 << 2,
	LPM_HFI_ON = 1 << 3,
	LPM_UTIL_ON = 1 << 4,
};

/* Force off by default */
int lpm_state = LPM_USER_OFF;

/*
 * 1: request valid and already satisfied. 0: respond valid and need to continue to process. -1: request invalid
 */
static int lpm_can_process(enum lpm_command cmd)
{
	switch (cmd) {
		case USER_ENTER:
			lpm_state &= ~LPM_USER_OFF;
			lpm_state |= LPM_USER_ON;

			/* Set the flag but do not proceed when in SUV mode */
			if (lpm_state & LPM_SUV_ON)
				return 0;
			return 1;
		case USER_EXIT:
			lpm_state &= ~LPM_USER_ON;
			lpm_state |= LPM_USER_OFF;

			/* Set the flag but do not proceed when in SUV mode */
			if (lpm_state & LPM_SUV_ON)
				return 0;

			return 1;
		case USER_AUTO:
			lpm_state &= ~LPM_USER_ON;
			lpm_state &= ~LPM_USER_OFF;
			/* Do nothing but just clear the flag */
			return 0;
		case HFI_ENTER:
			if (lpm_state & LPM_USER_OFF)
				return 0;

			/* Ignore HFI LPM hints when in SUV mode */
			if (lpm_state & LPM_SUV_ON)
				return 0;

			lpm_state |= LPM_HFI_ON;
			return 1;
		case HFI_EXIT:
			lpm_state &= ~LPM_HFI_ON;

			if (lpm_state & LPM_USER_ON)
				return 0;

			/* Do not proceed when in SUV mode */
			if (lpm_state & LPM_SUV_ON)
				return 0;

			return 1;
		case UTIL_ENTER:
			if (lpm_state & (LPM_USER_OFF))
				return 0;

			/* Do not proceed when in SUV mode */
			if (lpm_state & LPM_SUV_ON)
				return 0;

			return 1;
		case UTIL_EXIT:
			if (lpm_state & LPM_USER_ON)
				return 0;

			/* Do not proceed when in SUV mode */
			if (lpm_state & LPM_SUV_ON)
				return 0;

			/* Trust HFI LPM hints over utilization monitor */
			if (lpm_state & LPM_HFI_ON)
				return 0;
			return 1;

			/* Quit LPM because of SUV mode */
		case HFI_SUV_ENTER:
			lpm_state &= ~LPM_HFI_ON; /* HFI SUV hints means LPM hints is invalid */
			/* Fallthrough */
		case DBUS_SUV_ENTER:
			lpm_state |= LPM_SUV_ON;
			return 1;
			/* Re-enter LPM when quitting SUV mode */
		case HFI_SUV_EXIT:
		case DBUS_SUV_EXIT:
			lpm_state &= ~LPM_SUV_ON;
			/* Re-enter LPM because it is forced by user */
			if (lpm_state & LPM_USER_ON)
				return 1;
			/* Do oppoturnistic LPM based on util/hfi requests */
			return 0;
		default:
			return 1;
	}
}

static int dry_run = 0;

/* Must be invoked with lpmd_lock held */
int enter_lpm(enum lpm_command cmd)
{
	lpmd_log_debug ("Request %d (%10s). lpm_state 0x%x\n", cmd, lpm_cmd_str[cmd], lpm_state);

	if (!lpm_can_process (cmd)) {
		lpmd_log_debug ("Request stopped. lpm_state 0x%x\n", lpm_state);
		return 1;
	}

	if (in_low_power_mode) {
		lpmd_log_debug ("Request skipped because the system is already in Low Power Mode ---\n");
		return 0;
	}

	time_start ();

	switch (cmd) {
		case USER_ENTER:
		case UTIL_ENTER:
			set_lpm_cpus (CPUMASK_LPM_DEFAULT);
			break;
		case HFI_ENTER:
			set_lpm_cpus (CPUMASK_HFI);
			break;
		default:
			lpmd_log_info ("Unsupported LPM reason %d\n", cmd);
			return 1;
	}
	if (!has_lpm_cpus ()) {
		lpmd_log_error ("No LPM CPUs available\n");
		return 1;
	}

	lpmd_log_msg ("------ Enter Low Power Mode (%10s) --- %s", lpm_cmd_str[cmd], get_time ());

	if (dry_run) {
		lpmd_log_debug ("----- Dry Run -----\n");
		goto end;
	}

	process_itmt (1);
	process_irqs (1, get_cpu_mode ());
	process_cpus (1, get_cpu_mode ());

end:
	lpmd_log_info ("----- Done (%s) ---\n", time_delta ());
	in_low_power_mode = 1;

	return 0;
}

/* Must be invoked with lpmd_lock held */
int exit_lpm(enum lpm_command cmd)
{
	lpmd_log_debug ("Request %d (%10s). lpm_state 0x%x\n", cmd, lpm_cmd_str[cmd], lpm_state);

	if (!lpm_can_process (cmd)) {
		lpmd_log_debug ("Request stopped. lpm_state 0x%x\n", lpm_state);
		return 1;
	}

	if (!in_low_power_mode) {
		lpmd_log_debug (
				"Request skipped because the system is already out of Low Power Mode ---\n");
		return 0;
	}

	time_start ();

	lpmd_log_msg ("------ Exit Low Power Mode (%10s) --- %s", lpm_cmd_str[cmd], get_time ());

	if (dry_run) {
		lpmd_log_debug ("----- Dry Run -----\n");
		goto end;
	}

	process_cpus (0, get_cpu_mode ());
	process_irqs (0, get_cpu_mode ());
	process_itmt (0);

end:
	lpmd_log_info ("----- Done (%s) ---\n", time_delta ());
	in_low_power_mode = 0;

	return 0;
}

/* should be invoked without lock held */
int process_lpm(enum lpm_command cmd)
{
	int ret;

	lpmd_lock ();
	switch (cmd) {
		case USER_ENTER:
		case HFI_ENTER:
		case UTIL_ENTER:
		case USER_AUTO:
			ret = enter_lpm (cmd);
			break;
		case USER_EXIT:
		case HFI_EXIT:
		case UTIL_EXIT:
			ret = exit_lpm (cmd);
			break;
		default:
			ret = -1;
			break;
	}
	lpmd_unlock ();
	return ret;
}

static int
proc_message(message_capsul_t *msg);
static int write_pipe_fd;

static void lpmd_send_message(message_name_t msg_id, int size, unsigned char *msg)
{
	message_capsul_t msg_cap;
	int result;

	memset (&msg_cap, 0, sizeof(message_capsul_t));

	msg_cap.msg_id = msg_id;
	msg_cap.msg_size = (size > MAX_MSG_SIZE) ? MAX_MSG_SIZE : size;
	if (msg)
		memcpy (msg_cap.msg, msg, msg_cap.msg_size);

	result = write (write_pipe_fd, &msg_cap, sizeof(message_capsul_t));
	if (result < 0)
		lpmd_log_warn ("Write to pipe failed \n");
}

void lpmd_terminate(void)
{
	lpmd_send_message (TERMINATE, 0, NULL);
	sleep (1);
}

void lpmd_force_on(void)
{
	lpmd_send_message (LPM_FORCE_ON, 0, NULL);
}

void lpmd_force_off(void)
{
	lpmd_send_message (LPM_FORCE_OFF, 0, NULL);
}

void lpmd_set_auto(void)
{
	lpmd_send_message (LPM_AUTO, 0, NULL);
}

void lpmd_suv_enter(void)
{
	lpmd_send_message (SUV_MODE_ENTER, 0, NULL);
}

void lpmd_suv_exit(void)
{
	lpmd_send_message (SUV_MODE_EXIT, 0, NULL);
}

void lpmd_notify_hfi_event(void)
{
	lpmd_send_message (HFI_EVENT, 0, NULL);
	sleep (1);
}

#define LPMD_NUM_OF_POLL_FDS	4

static pthread_t lpmd_core_main;
static pthread_attr_t lpmd_attr;

static struct pollfd poll_fds[LPMD_NUM_OF_POLL_FDS];
static int poll_fd_cnt;

static int wakeup_fd;

static int hfi_wakeup_fd;

#include <gio/gio.h>

static GDBusProxy *power_profiles_daemon;

static void power_profiles_changed_cb(void)
{
	g_autoptr (GVariant)
	active_profile_v = NULL;

	active_profile_v = g_dbus_proxy_get_cached_property (power_profiles_daemon, "ActiveProfile");

	if (active_profile_v && g_variant_is_of_type (active_profile_v, G_VARIANT_TYPE_STRING)) {
		const char *active_profile = g_variant_get_string (active_profile_v, NULL);

		lpmd_log_debug ("power_profiles_changed_cb: %s\n", active_profile);

		if (strcmp (active_profile, "power-saver") == 0)
			lpmd_send_message (lpmd_config.powersaver_def, 0, NULL);
		else if (strcmp (active_profile, "performance") == 0)
			lpmd_send_message (lpmd_config.performance_def, 0, NULL);
		else if (strcmp (active_profile, "balanced") == 0)
			lpmd_send_message (lpmd_config.balanced_def, 0, NULL);
		else
			lpmd_log_warn("Ignore unsupported power profile: %s\n", active_profile);
	}
}

static void connect_to_power_profile_daemon(void)
{
	g_autoptr (GDBusConnection)
	bus = NULL;

	bus = g_bus_get_sync (G_BUS_TYPE_SYSTEM, NULL, NULL);
	if (bus) {
		power_profiles_daemon = g_dbus_proxy_new_sync (bus, G_DBUS_PROXY_FLAGS_DO_NOT_AUTO_START,
													   NULL,
													   "net.hadess.PowerProfiles",
													   "/net/hadess/PowerProfiles",
													   "net.hadess.PowerProfiles",
													   NULL,
													   NULL);

		if (power_profiles_daemon) {
			g_signal_connect_swapped (power_profiles_daemon, "g-properties-changed",
										(GCallback) power_profiles_changed_cb,
										NULL);
			power_profiles_changed_cb ();
		}
		else {
			lpmd_log_info ("Could not setup DBus watch for power-profiles-daemon");
		}
	}
}

/* Poll time out default */
#define POLL_TIMEOUT_DEFAULT_SECONDS	1

static bool main_loop_terminate;

// called from LPMD main thread to process user and system messages
static int proc_message(message_capsul_t *msg)
{
	int ret = 0;

	lpmd_log_debug ("Received message %d\n", msg->msg_id);
	switch (msg->msg_id) {
		case TERMINATE:
			lpmd_log_msg ("Terminating ...\n");
			ret = -1;
			main_loop_terminate = true;
			hfi_kill ();
			exit_lpm (USER_EXIT);
			break;
		case LPM_FORCE_ON:
			// Always stay in LPM mode
			enter_lpm (USER_ENTER);
			break;
		case LPM_FORCE_OFF:
			// Never enter LPM mode
			exit_lpm (USER_EXIT);
			break;
		case LPM_AUTO:
			// Enable oppotunistic LPM
			process_lpm (USER_AUTO);
			break;
		case SUV_MODE_ENTER:
			// Call function to enter SUV mode
			process_suv_mode (DBUS_SUV_ENTER);
			break;
		case SUV_MODE_EXIT:
			// Call function to exit SUV mode
			process_suv_mode (DBUS_SUV_EXIT);
			break;
		case HFI_EVENT:
			// Call the HFI callback from here
			break;
		default:
			break;
	}

	return ret;
}

// LPMD processing thread. This is callback to pthread lpmd_core_main
static void* lpmd_core_main_loop(void *arg)
{
	int interval, n;
	static int first_try = 1;

	for (;;) {

		if (main_loop_terminate)
			break;

		if (first_try) {
			interval = 100;
			first_try = 0;
		} else {
//			 Opportunistic LPM is disabled in below cases
			if (lpm_state & (LPM_USER_ON | LPM_USER_OFF | LPM_SUV_ON))
				interval = -1;
			else
				interval = periodic_util_update ();
		}

		n = poll (poll_fds, poll_fd_cnt, interval);
		if (n < 0) {
			lpmd_log_warn ("Write to pipe failed \n");
			continue;
		}

		if (wakeup_fd >= 0 && (poll_fds[wakeup_fd].revents & POLLIN)) {
//			 process message written on pipe here

			message_capsul_t msg;

			int result = read (poll_fds[wakeup_fd].fd, &msg, sizeof(message_capsul_t));
			if (result < 0) {
				lpmd_log_warn ("read on wakeup fd failed\n");
				poll_fds[wakeup_fd].revents = 0;
				continue;
			}
			if (proc_message (&msg) < 0) {
				lpmd_log_debug ("Terminating thread..\n");
			}
		}

		if (hfi_wakeup_fd >= 0 && (poll_fds[hfi_wakeup_fd].revents & POLLIN)) {
			hfi_receive ();
		}

	}

	return NULL;
}

int lpmd_main(void)
{
	int wake_fds[2];
	int ret, hfi_fd;

	lpmd_log_debug ("lpmd_main begin\n");

//	 Call all lpmd related functions here
	ret = lpmd_get_config (&lpmd_config);
	if (ret)
		return ret;

	pthread_mutex_init (&lpmd_mutex, NULL);

	ret = init_cpu (lpmd_config.lp_mode_cpus, lpmd_config.mode);
	if (ret)
		return ret;

	if (!has_suv_support () && lpmd_config.hfi_suv_enable)
		lpmd_config.hfi_suv_enable = 0;

	ret = init_irq ();
	if (ret)
		return ret;

	hfi_fd = -1;
	if (lpmd_config.hfi_lpm_enable || lpmd_config.hfi_suv_enable)
		hfi_fd = hfi_init ();

//	 Pipe is used for communication between two processes
	ret = pipe (wake_fds);
	if (ret) {
		lpmd_log_error ("pipe creation failed %d:\n", ret);
		return LPMD_FATAL_ERROR;
	}
	if (fcntl (wake_fds[0], F_SETFL, O_NONBLOCK) < 0) {
		lpmd_log_error ("Cannot set non-blocking on pipe: %s\n", strerror (errno));
		return LPMD_FATAL_ERROR;
	}
	if (fcntl (wake_fds[1], F_SETFL, O_NONBLOCK) < 0) {
		lpmd_log_error ("Cannot set non-blocking on pipe: %s\n", strerror (errno));
		return LPMD_FATAL_ERROR;
	}
	write_pipe_fd = wake_fds[1];

	memset (poll_fds, 0, sizeof(poll_fds));

	wakeup_fd = poll_fd_cnt;
	poll_fds[wakeup_fd].fd = wake_fds[0];
	poll_fds[wakeup_fd].events = POLLIN;
	poll_fds[wakeup_fd].revents = 0;
	poll_fd_cnt++;

	hfi_wakeup_fd = -1;
	if (hfi_fd > 0) {
		hfi_wakeup_fd = poll_fd_cnt;
		poll_fds[hfi_wakeup_fd].fd = hfi_fd;
		poll_fds[hfi_wakeup_fd].events = POLLIN;
		poll_fds[hfi_wakeup_fd].revents = 0;
		poll_fd_cnt++;
	}

	pthread_attr_init (&lpmd_attr);
	pthread_attr_setdetachstate (&lpmd_attr, PTHREAD_CREATE_DETACHED);

	/*
	 * lpmd_core_main_loop: is the thread where all LPMD actions take place.
	 * All other thread send message via pipe to trigger processing
	 */
	ret = pthread_create (&lpmd_core_main, &lpmd_attr, lpmd_core_main_loop, NULL);

	connect_to_power_profile_daemon ();

	lpmd_log_debug ("lpmd_init succeeds\n");

	return LPMD_SUCCESS;
}
