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
#include "wlt_proxy/wlt_proxy.h"


extern int next_proxy_poll; 
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

static int has_hfi_capability(void)
{
	unsigned int eax = 0, ebx = 0, ecx = 0, edx = 0;

	cpuid(6, eax, ebx, ecx, edx);
	if (eax & (1 << 19)) {
		lpmd_log_info("HFI capability detected\n");
		return 1;
	}
	return 0;
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

static int saved_itmt = SETTING_IGNORE;
static int lp_mode_itmt = SETTING_IGNORE;

int get_lpm_itmt(void)
{
	return lp_mode_itmt;
}

void set_lpm_itmt(int val)
{
	lp_mode_itmt = val;
}

int get_itmt(void)
{
	int val;

	lpmd_read_int(PATH_ITMT_CONTROL, &val, -1);
	return val;
}

static int init_itmt(void)
{
	return lpmd_read_int(PATH_ITMT_CONTROL, &saved_itmt, -1);
}

static int process_itmt(void)
{
	if (lp_mode_itmt == SETTING_RESTORE)
		lp_mode_itmt = saved_itmt;

	if (lp_mode_itmt == SETTING_IGNORE) {
		lpmd_log_debug("Ignore ITMT\n");
		return 0;
	}

	lpmd_log_debug ("%s ITMT\n", lp_mode_itmt ? "Enable" : "Disable");

	return lpmd_write_int(PATH_ITMT_CONTROL, lp_mode_itmt, -1);
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

int in_hfi_lpm(void)
{
	return lpm_state & LPM_HFI_ON;
}

int in_suv_lpm(void)
{
	return lpm_state & LPM_SUV_ON;
}

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
			/* Assume the system is already in HFI_LPM so that we can handle next HFI update whatever it is */
			if (has_hfi_lpm_monitor()) {
				lpmd_log_info("Use HFI \n");
				lpm_state |= LPM_HFI_ON;
			}
			return 0;
		case HFI_ENTER:
			if (lpm_state & (LPM_USER_OFF | LPM_USER_ON))
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
			if (lpm_state & (LPM_USER_OFF) && !lpmd_config.wlt_proxy_enable){
				lpmd_log_info("USER_OFF, return 0\n");
				return 0;
			}

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

	if (in_low_power_mode && cmd != HFI_ENTER && cmd != UTIL_ENTER) {
		lpmd_log_debug ("Request skipped because the system is already in Low Power Mode ---\n");
		return 0;
	}

	time_start ();

	if (cmd != USER_ENTER && cmd != UTIL_ENTER && cmd != HFI_ENTER) {
			lpmd_log_info ("Unsupported LPM reason %d\n", cmd);
			return 1;
	}

	lpmd_log_msg ("------ Enter Low Power Mode (%10s) --- %s", lpm_cmd_str[cmd], get_time ());

	if (dry_run) {
		lpmd_log_debug ("----- Dry Run -----\n");
		goto end;
	}

	process_itmt ();
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
	process_itmt ();

end:
	lpmd_log_info ("----- Done (%s) ---\n", time_delta ());
	in_low_power_mode = 0;

	return 0;
}

static int lpmd_freezed = 0;

/* should be invoked without lock held */
int process_lpm_unlock(enum lpm_command cmd)
{
	int ret;

	if (lpmd_freezed) {
		lpmd_log_error("lpmd freezed, command (%s) ignored\n", lpm_cmd_str[cmd]);
		return 0;
	}

	switch (cmd) {
		case UTIL_ENTER:
			if (!use_config_states()) {
				set_lpm_epp (lpmd_config.lp_mode_epp);
				set_lpm_epb (SETTING_IGNORE);
				set_lpm_itmt (lpmd_config.ignore_itmt ? SETTING_IGNORE : 0); /* Disable ITMT */
				set_lpm_irq(get_cpumask(CPUMASK_LPM_DEFAULT), 1);
				set_lpm_cpus (CPUMASK_LPM_DEFAULT);
			}
			ret = enter_lpm (cmd);
			break;
		case USER_ENTER:
		case USER_AUTO:
			reset_config_state();
			set_lpm_epp (lpmd_config.lp_mode_epp);
			set_lpm_epb (SETTING_IGNORE);
			set_lpm_itmt (lpmd_config.ignore_itmt ? SETTING_IGNORE : 0); /* Disable ITMT */
			set_lpm_irq(get_cpumask(CPUMASK_LPM_DEFAULT), 1);
			set_lpm_cpus (CPUMASK_LPM_DEFAULT);
			ret = enter_lpm (cmd);
			break;
		case HFI_SUV_EXIT:
		case DBUS_SUV_EXIT:
			set_lpm_epp (SETTING_IGNORE);
			set_lpm_epb (SETTING_IGNORE);
			set_lpm_itmt (SETTING_IGNORE);
			set_lpm_irq(NULL, SETTING_IGNORE);	/* SUV ignores IRQ */
			set_lpm_cpus (CPUMASK_HFI_SUV);
			ret = enter_lpm (cmd);
			break;
		case HFI_ENTER:
			set_lpm_epp (lpmd_config.lp_mode_epp);
			set_lpm_epb (SETTING_IGNORE);
			set_lpm_itmt (0);	/* HFI always disables ITMT */
			set_lpm_irq(NULL, SETTING_IGNORE);	/* HFI ignores IRQ */
			set_lpm_cpus (CPUMASK_HFI);
			ret = enter_lpm (cmd);
			break;
		/* exit_lpm does not require to invoke set_lpm_cpus() */
		case USER_EXIT:
		case UTIL_EXIT:
			reset_config_state();
			set_lpm_epp (SETTING_RESTORE);
			set_lpm_epb (SETTING_RESTORE);
			set_lpm_itmt (SETTING_RESTORE);
			set_lpm_irq(NULL, SETTING_RESTORE);
			ret = exit_lpm (cmd);
			break;
		case HFI_SUV_ENTER:
		case DBUS_SUV_ENTER:
			set_lpm_epp (SETTING_IGNORE);
			set_lpm_epb (SETTING_IGNORE);
			set_lpm_itmt (SETTING_IGNORE);
			set_lpm_irq(NULL, SETTING_IGNORE);
			ret = exit_lpm (cmd);
			break;
		case HFI_EXIT:
			set_lpm_epp (lpmd_config.lp_mode_epp == SETTING_IGNORE ? SETTING_IGNORE : SETTING_RESTORE);
			set_lpm_epb (SETTING_IGNORE);
			set_lpm_itmt (SETTING_RESTORE); /* Restore ITMT */
			set_lpm_irq(NULL, SETTING_IGNORE);	/* HFI ignores IRQ */
			ret = exit_lpm (cmd);
			break;
		default:
			ret = -1;
			break;
	}

	return ret;
}

int process_lpm(enum lpm_command cmd)
{
	int ret;

	lpmd_lock ();
	ret = process_lpm_unlock (cmd);
	lpmd_unlock ();
	return ret;
}

static int saved_lpm_state = -1;

int freeze_lpm(void)
{
	lpmd_lock ();

	if (lpmd_freezed)
		goto end;

	if (saved_lpm_state < 0)
		saved_lpm_state = lpm_state & (LPM_USER_ON | LPM_USER_OFF);

	process_lpm_unlock (USER_EXIT);

	/* Set lpmd_freezed later to allow process_lpm () */
	lpmd_freezed = 1;
end:
	lpmd_unlock ();
	return 0;
}

int restore_lpm(void)
{
	lpmd_lock ();

	if (!lpmd_freezed)
		goto end;

	if (saved_lpm_state >= 0) {
		lpm_state = saved_lpm_state;
		saved_lpm_state = -1;
	}

	/* Clear lpmd_freezed to allow process_lpm () */
	lpmd_freezed = 0;

	/* Restore previous USER_* cmd */
	if (lpm_state & LPM_USER_ON) {
		process_lpm_unlock (USER_ENTER);
		goto end;
	}

	if (lpm_state & LPM_USER_OFF) {
		process_lpm_unlock (USER_EXIT);
		goto end;
	}

	process_lpm_unlock (USER_AUTO);

end:
	lpmd_unlock ();
	return 0;
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

#define LPMD_NUM_OF_POLL_FDS	5

static pthread_t lpmd_core_main;
static pthread_attr_t lpmd_attr;

static struct pollfd poll_fds[LPMD_NUM_OF_POLL_FDS];
static int poll_fd_cnt;

static int idx_pipe_fd = -1;
static int idx_uevent_fd = -1;
static int idx_hfi_fd = -1;

static int wlt_fd;
static int idx_wlt_fd = -1;

// Workload type classification
#define WORKLOAD_NOTIFICATION_DELAY_ATTRIBUTE "/sys/bus/pci/devices/0000:00:04.0/workload_hint/notification_delay_ms"
#define WORKLOAD_ENABLE_ATTRIBUTE "/sys/bus/pci/devices/0000:00:04.0/workload_hint/workload_hint_enable"
#define WORKLOAD_TYPE_INDEX_ATTRIBUTE  "/sys/bus/pci/devices/0000:00:04.0/workload_hint/workload_type_index"

#define NOTIFICATION_DELAY	100

// Clear workload type notifications
static void exit_wlt()
{
	int fd;

	/* Disable feature via sysfs knob */
	fd = open(WORKLOAD_ENABLE_ATTRIBUTE, O_RDWR);
	if (fd < 0)
		return;

	// Disable WLT notification
	if (write(fd, "0\n", 2) < 0) {
		close (fd);
		return;
	}

	close(fd);
}

// Initialize Workload type notifications
static int init_wlt()
{
	char delay_str[64];
	int fd;

	lpmd_log_debug ("init_wlt begin\n");

	// Set notification delay
	fd = open(WORKLOAD_NOTIFICATION_DELAY_ATTRIBUTE, O_RDWR);
	if (fd < 0)
		return fd;

	sprintf(delay_str, "%d\n", NOTIFICATION_DELAY);

	if (write(fd, delay_str, strlen(delay_str)) < 0) {
		close(fd);
		return -1;
	}

	close(fd);

	// Enable WLT notification
	fd = open(WORKLOAD_ENABLE_ATTRIBUTE, O_RDWR);
	if (fd < 0)
		return fd;

	if (write(fd, "1\n", 2) < 0) {
		close(fd);
		return -1;
	}

	close(fd);

	// Open FD for workload type attribute
	fd = open(WORKLOAD_TYPE_INDEX_ATTRIBUTE, O_RDONLY);
	if (fd < 0) {
		exit_wlt();
		return fd;
	}

	lpmd_log_debug ("init_wlt end wlt fd:%d\n", fd);

	return fd;
}

// Read current Workload type
static int read_wlt(int fd)
{
	char index_str[4];
	int index, ret;

	if (fd < 0)
		return WLT_INVALID;

	if ((lseek(fd, 0L, SEEK_SET)) < 0)
		return WLT_INVALID;

	ret = read(fd, index_str, sizeof(index_str));
	if (ret <= 0)
		return WLT_INVALID;

	 ret = sscanf(index_str, "%d", &index);
	 if (ret < 0)
		return WLT_INVALID;

	lpmd_log_debug("wlt:%d\n", index);

	return index;
}

static void poll_for_wlt(int enable)
{
	static int wlt_enabled_once = 0;

	if (wlt_fd <= 0) {
		if (enable) {
			wlt_fd = init_wlt();
			if (wlt_fd < 0)
				return;
		} else {
			return;
		}
	}

	if (enable) {
		idx_wlt_fd = poll_fd_cnt;
		poll_fds[idx_wlt_fd].fd = wlt_fd;
		poll_fds[idx_wlt_fd].events = POLLPRI;
		poll_fds[idx_wlt_fd].revents = 0;
		if (!wlt_enabled_once)
			poll_fd_cnt++;
		wlt_enabled_once = 1;
	} else if (idx_wlt_fd >= 0) {
		poll_fds[idx_wlt_fd].fd = -1;
		idx_wlt_fd = -1;
	}
}

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
		g_object_unref(bus);
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
			process_lpm (USER_EXIT);
			break;
		case LPM_FORCE_ON:
			// Always stay in LPM mode
			process_lpm (USER_ENTER);
			break;
		case LPM_FORCE_OFF:
			// Never enter LPM mode
			process_lpm (USER_EXIT);
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
	int interval = -1, n;

	for (;;) {

		if (main_loop_terminate)
			break;

//		 Opportunistic LPM is disabled in below cases
		if (lpmd_config.wlt_proxy_enable){
			interval = lpmd_config.wlt_proxy_interval;
			//gets interval of different states 
			if (interval != next_proxy_poll && next_proxy_poll > 0)
				interval = next_proxy_poll;
		}
		else if (lpm_state & (LPM_USER_ON | LPM_USER_OFF | LPM_SUV_ON) | has_hfi_lpm_monitor ())
			interval = -1;
		else if (interval == -1)
			interval = 100;

		lpmd_log_debug("main loop polling interval is %d\n", interval);
		n = poll (poll_fds, poll_fd_cnt, interval);
		if (n < 0) {
			lpmd_log_warn ("Write to pipe failed \n");
			continue;
		}

		/* Time out, need to choose next util state and interval */
		if (n == 0 && interval > 0) {
			if (lpmd_config.wlt_proxy_enable){
				wlt_proxy_action_loop ();
			}
			else 
				interval = periodic_util_update (&lpmd_config, -1);
		}

		if (idx_pipe_fd >= 0 && (poll_fds[idx_pipe_fd].revents & POLLIN)) {
//			 process message written on pipe here

			message_capsul_t msg;

			int result = read (poll_fds[idx_pipe_fd].fd, &msg, sizeof(message_capsul_t));
			if (result < 0) {
				lpmd_log_warn ("read on wakeup fd failed\n");
				poll_fds[idx_pipe_fd].revents = 0;
				continue;
			}
			if (proc_message (&msg) < 0) {
				lpmd_log_debug ("Terminating thread..\n");
			}
		}

		if (idx_uevent_fd >= 0 && (poll_fds[idx_uevent_fd].revents & POLLIN)) {
			check_cpu_hotplug ();
		}

		if (idx_hfi_fd >= 0 && (poll_fds[idx_hfi_fd].revents & POLLIN)) {
			hfi_receive ();
		}

		if (idx_wlt_fd >= 0 && (poll_fds[idx_wlt_fd].revents & POLLPRI)) {
			int wlt_index;

			wlt_index = read_wlt(poll_fds[idx_wlt_fd].fd);
			interval = periodic_util_update (&lpmd_config, wlt_index);
		}


	}

	return NULL;
}

static void build_default_config_state(void)
{
	lpmd_config_state_t *state;

	if (lpmd_config.config_state_count)
		return;

	state = &lpmd_config.config_states[0];
	state->id = 1;
	snprintf(state->name, MAX_STATE_NAME, "LPM_DEEP");
	state->entry_system_load_thres = lpmd_config.util_entry_threshold;
	state->enter_cpu_load_thres = lpmd_config.util_exit_threshold;
	state->itmt_state = lpmd_config.ignore_itmt ? SETTING_IGNORE : 0;
	state->irq_migrate = 1;
	state->min_poll_interval = 100;
	state->max_poll_interval = 1000;
	state->poll_interval_increment = -1;
	state->epp = lpmd_config.lp_mode_epp;
	state->epb = SETTING_IGNORE;
	state->valid = 1;
	state->wlt_type = -1;
	snprintf(state->active_cpus, MAX_STR_LENGTH, "%s", get_cpus_str(CPUMASK_LPM_DEFAULT));

	state = &lpmd_config.config_states[1];
	state->id = 2;
	snprintf(state->name, MAX_STATE_NAME, "FULL_POWER");
	state->entry_system_load_thres = 100;
	state->enter_cpu_load_thres = 100;
	state->itmt_state = lpmd_config.ignore_itmt ? SETTING_IGNORE : SETTING_RESTORE;
	state->irq_migrate = 1;
	state->min_poll_interval = 1000;
	state->max_poll_interval = 1000;
	state->epp = lpmd_config.lp_mode_epp == SETTING_IGNORE ? SETTING_IGNORE : SETTING_RESTORE;
	state->epb = SETTING_IGNORE;
	snprintf(state->active_cpus, MAX_STR_LENGTH, "%s", get_cpus_str(CPUMASK_ONLINE));

	lpmd_config.config_state_count = 2;
}

int lpmd_main(void)
{
	int wake_fds[2];
	int ret;

	lpmd_log_debug ("lpmd_main begin\n");

	ret = check_cpu_capability(&lpmd_config);
	if (ret)
		return ret;

//	 Call all lpmd related functions here
	ret = lpmd_get_config (&lpmd_config);
	if (ret)
		return ret;

	pthread_mutex_init (&lpmd_mutex, NULL);

	ret = init_cpu (lpmd_config.lp_mode_cpus, lpmd_config.mode, lpmd_config.lp_mode_epp);
	if (ret)
		return ret;

	init_itmt();

	if (!has_suv_support () && lpmd_config.hfi_suv_enable)
		lpmd_config.hfi_suv_enable = 0;

	if (!has_hfi_capability ())
		lpmd_config.hfi_lpm_enable = 0;

	/* Must done after init_cpu() */
	build_default_config_state();

	util_init(&lpmd_config);

	ret = init_irq ();
	if (ret)
		return ret;

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

	idx_pipe_fd = poll_fd_cnt;
	poll_fds[idx_pipe_fd].fd = wake_fds[0];
	poll_fds[idx_pipe_fd].events = POLLIN;
	poll_fds[idx_pipe_fd].revents = 0;
	poll_fd_cnt++;

	poll_fds[poll_fd_cnt].fd = uevent_init ();
	if (poll_fds[poll_fd_cnt].fd > 0) {
		idx_uevent_fd = poll_fd_cnt;
		poll_fds[idx_uevent_fd].events = POLLIN;
		poll_fds[idx_uevent_fd].revents = 0;
		poll_fd_cnt++;
	}

	if (lpmd_config.hfi_lpm_enable || lpmd_config.hfi_suv_enable) {
		poll_fds[poll_fd_cnt].fd = hfi_init ();
		if (poll_fds[poll_fd_cnt].fd > 0) {
			idx_hfi_fd = poll_fd_cnt;
			poll_fds[idx_hfi_fd].events = POLLIN;
			poll_fds[idx_hfi_fd].revents = 0;
			poll_fd_cnt++;
		}
	}

	if (lpmd_config.wlt_hint_enable) {
		lpmd_config.util_enable = 0;
		if (lpmd_config.wlt_proxy_enable) {
			if (wlt_proxy_init(&lpmd_config) != LPMD_SUCCESS || !lpmd_config.wlt_proxy_interval) {
				lpmd_config.wlt_proxy_enable = 0;
				lpmd_log_error ("Invalid WLT Proxy setup\n");
			}
		} else {
			if (!lpmd_config.hfi_lpm_enable && !lpmd_config.hfi_suv_enable) {
				poll_for_wlt(1);
			}
		}
	}

	pthread_attr_init (&lpmd_attr);
	pthread_attr_setdetachstate (&lpmd_attr, PTHREAD_CREATE_DETACHED);

	connect_to_power_profile_daemon ();
	/*
	 * lpmd_core_main_loop: is the thread where all LPMD actions take place.
	 * All other thread send message via pipe to trigger processing
	 */
	ret = pthread_create (&lpmd_core_main, &lpmd_attr, lpmd_core_main_loop, NULL);
	if (ret)
		return LPMD_FATAL_ERROR;


	lpmd_log_debug ("lpmd_init succeeds\n");

	return LPMD_SUCCESS;
}
