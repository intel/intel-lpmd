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
#include <upower.h>
#include "wlt_proxy.h"

static lpmd_config_t lpmd_config;

lpmd_config_t *get_lpmd_config(void)
{
	return &lpmd_config;
}

static UpClient *upower_client;

static pthread_mutex_t lpmd_mutex;

int lpmd_lock(void)
{
	return pthread_mutex_lock (&lpmd_mutex);
}

int lpmd_unlock(void)
{
	return pthread_mutex_unlock (&lpmd_mutex);
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

/* Main functions */

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
		lpmd_log_warn ("Write to pipe failed\n");
}

void lpmd_terminate(void)
{
	lpmd_send_message (TERMINATE, 0, NULL);
	sleep (1);
	if (upower_client)
		g_clear_object(&upower_client);
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

#define LPMD_NUM_OF_POLL_FDS	5

static pthread_t lpmd_core_main;
static pthread_attr_t lpmd_attr;

static struct pollfd poll_fds[LPMD_NUM_OF_POLL_FDS];
static int poll_fd_cnt;

static int idx_pipe_fd = -1;
static int idx_uevent_fd = -1;
static int idx_hfi_fd = -1;
static int idx_wlt_fd = -1;

#include <gio/gio.h>

static GDBusProxy *power_profiles_daemon;

static enum power_profile_daemon_mode ppd_mode = PPD_INVALID;

int get_ppd_mode(void)
{
	return ppd_mode;
}

static void power_profiles_changed_cb(void)
{
	g_autoptr (GVariant)
	active_profile_v = NULL;

	active_profile_v = g_dbus_proxy_get_cached_property (power_profiles_daemon, "ActiveProfile");

	if (active_profile_v && g_variant_is_of_type (active_profile_v, G_VARIANT_TYPE_STRING)) {
		const char *active_profile = g_variant_get_string (active_profile_v, NULL);

		lpmd_log_debug ("power_profiles_changed_cb: %s\n", active_profile);

		if (strcmp (active_profile, "power-saver") == 0) {
			ppd_mode = PPD_POWERSAVER;
			lpmd_send_message (lpmd_config.powersaver_def, 0, NULL);
		} else if (strcmp (active_profile, "performance") == 0) {
			ppd_mode = PPD_PERFORMANCE;
			lpmd_send_message (lpmd_config.performance_def, 0, NULL);
		} else if (strcmp (active_profile, "balanced") == 0) {
			ppd_mode = PPD_BALANCED;
			lpmd_send_message (lpmd_config.balanced_def, 0, NULL);
		} else {
			lpmd_log_warn("Ignore unsupported power profile: %s\n", active_profile);
		}
	}
}

static int connect_to_power_profile_daemon(void)
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
			return 0;
		}
		else {
			lpmd_log_info ("Could not setup DBus watch for power-profiles-daemon");
		}
	}
	return 1;
}

static int battery_mode;

int is_on_battery(void)
{
	return battery_mode;
}

static void upower_daemon_cb (UpClient *client, GParamSpec *pspec, gpointer user_data)
{
	battery_mode = up_client_get_on_battery(upower_client);
	lpmd_log_info("upower event: on-battery: %d\n", battery_mode);
}

static void connect_to_upower_daemon(void)
{
	GError *error = NULL;
	GPtrArray *devices;
	int i;

	upower_client = up_client_new_full (NULL, &error);
	if (upower_client == NULL) {
		g_warning ("Cannot connect to upowerd: %s", error->message);
		g_error_free (error);
		return;
	}

	lpmd_log_info("connected to upower daemon\n");
	g_signal_connect (upower_client, "notify", G_CALLBACK (upower_daemon_cb), NULL);

	devices = up_client_get_devices2 (upower_client);
	for (i=0; i < devices->len; i++) {
		UpDevice *device;
		device = g_ptr_array_index (devices, i);
		g_signal_connect (device, "notify", G_CALLBACK (upower_daemon_cb), NULL);
	}
}

/* Poll time out default */
#define POLL_TIMEOUT_DEFAULT_SECONDS	1

// called from LPMD main thread to process user and system messages
static int proc_message(message_capsul_t *msg)
{
	lpmd_log_debug ("Received message %d\n", msg->msg_id);
	switch (msg->msg_id) {
		case TERMINATE:
			lpmd_log_msg ("Terminating ...\n");
			update_lpmd_state(LPMD_TERMINATE);
			break;
		case LPM_FORCE_ON:
			// Always stay in LPM mode
			update_lpmd_state(LPMD_ON);
			break;
		case LPM_FORCE_OFF:
			// Never enter LPM mode
			update_lpmd_state(LPMD_OFF);
			break;
		case LPM_AUTO:
			// Enable oppotunistic LPM
			update_lpmd_state(LPMD_AUTO);
			break;
		default:
			break;
	}

	return 0;
}

static void dump_poll_results(int ret)
{
	int i = 0;


//	if (!in_debug_mode())
	if (1)
		return;

	if (idx_pipe_fd != -1) {
		lpmd_log_debug("poll_fds[%s]: event %d, revent %d\n", "  Pipe", poll_fds[i].events, poll_fds[i].revents);
		i++;
	}

	if (idx_uevent_fd != -1) {
		lpmd_log_debug("poll_fds[%s]: event %d, revent %d\n", "Uevent", poll_fds[i].events, poll_fds[i].revents);
		i++;
	}

	if (idx_hfi_fd != -1) {
		lpmd_log_debug("poll_fds[%s]: event %d, revent %d\n", "   HFI", poll_fds[i].events, poll_fds[i].revents);
		i++;
	}

	if (idx_wlt_fd != -1) {
		lpmd_log_debug("poll_fds[%s]: event %d, revent %d\n", "   WLT", poll_fds[i].events, poll_fds[i].revents);
		i++;
	}
}

// LPMD processing thread. This is callback to pthread lpmd_core_main
static void* lpmd_core_main_loop(void *arg)
{
	int n;

	lpmd_config.data.polling_interval = 100;

	for (;;) {

		if (get_lpmd_state() == LPMD_TERMINATE)
			break;

		n = poll (poll_fds, poll_fd_cnt, lpmd_config.data.polling_interval);
		if (n < 0) {
			lpmd_log_warn ("Write to pipe failed\n");
			continue;
		}
		dump_poll_results(n);

		/* Polling time out, update polling data */
		if (n == 0 && lpmd_config.data.polling_interval > 0) {
			util_update(&lpmd_config);

			if (lpmd_config.wlt_proxy_enable)
				lpmd_config.data.wlt_hint = read_wlt_proxy(&lpmd_config.data.polling_interval);
		}

		/* Check CPU hotplug. Maybe need to freeze lpmd */
		if (idx_uevent_fd >= 0 && (poll_fds[idx_uevent_fd].revents & POLLIN)) {
			check_cpu_hotplug ();
		}

		/* Update CPUMASK_HFI */
		if (idx_hfi_fd >= 0 && (poll_fds[idx_hfi_fd].revents & POLLIN)) {
			hfi_update();
		}

		/* Update WLT hint */
		if (idx_wlt_fd >= 0 && (poll_fds[idx_wlt_fd].revents & POLLPRI)) {
			lpmd_config.data.wlt_hint = wlt_update(poll_fds[idx_wlt_fd].fd);
		}

		/* Respond Dbus commands */
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

		/* Enter next state after collecting all system statistics */
		lpmd_enter_next_state();
	}

	if (lpmd_config.wlt_proxy_enable)
		wlt_proxy_uninit();
	hfi_kill ();
	cgroup_cleanup();

	return NULL;
}

int lpmd_main(void)
{
	int wake_fds[2];
	int ret;

	lpmd_log_debug ("lpmd_main begin\n");

	ret = detect_supported_platform(&lpmd_config);
	if (ret)
		return ret;

	ret = detect_cpu_topo(&lpmd_config);
	if (ret)
		return ret;

//	 Call all lpmd related functions here
	ret = lpmd_get_config (&lpmd_config);
	if (ret)
		return ret;

	pthread_mutex_init (&lpmd_mutex, NULL);

	ret = detect_lpm_cpus(lpmd_config.lp_mode_cpus);
	if (ret)
		return ret;

	ret = cgroup_init(&lpmd_config);
	if (ret)
		return ret;

	ret = itmt_init();
	if (ret)
		return ret;

	ret = epp_epb_init();
	if (ret)
		return ret;

	if (!has_hfi_capability ())
		lpmd_config.hfi_lpm_enable = 0;

	/* Must done after init_cpu() */
	lpmd_build_config_states(&lpmd_config);

	ret = irq_init();
	if (ret)
		return ret;

	connect_to_upower_daemon();
//	 Pipe is used for communication between two processes
	ret = pipe (wake_fds);
	if (ret) {
		lpmd_log_error ("pipe creation failed %d:\n", ret);
		return LPMD_FATAL_ERROR;
	}
	if (fcntl (wake_fds[0], F_SETFL, O_NONBLOCK) < 0) {
		lpmd_log_error ("Cannot set non-blocking on pipe: %s\n", strerror (errno));
		(void)close(wake_fds[0]);
		(void)close(wake_fds[1]);
		return LPMD_FATAL_ERROR;
	}
	if (fcntl (wake_fds[1], F_SETFL, O_NONBLOCK) < 0) {
		lpmd_log_error ("Cannot set non-blocking on pipe: %s\n", strerror (errno));
		(void)close(wake_fds[0]);
		(void)close(wake_fds[1]);
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

	if (lpmd_config.hfi_lpm_enable) {
		poll_fds[poll_fd_cnt].fd = hfi_init ();
		if (poll_fds[poll_fd_cnt].fd > 0) {
			idx_hfi_fd = poll_fd_cnt;
			poll_fds[idx_hfi_fd].events = POLLIN;
			poll_fds[idx_hfi_fd].revents = 0;
			poll_fd_cnt++;
		}
	}

	if (lpmd_config.wlt_hint_enable) {
		if (lpmd_config.wlt_proxy_enable) {
			if (wlt_proxy_init() != LPMD_SUCCESS) {
				lpmd_config.wlt_proxy_enable = 0;
				lpmd_log_error ("Error setting up WLT Proxy. wlt_proxy_enable disabled\n");
			}
		}
		if (!lpmd_config.hfi_lpm_enable) {
			lpmd_config.util_enable = 0;
			if (!lpmd_config.wlt_proxy_enable) {
				poll_fds[poll_fd_cnt].fd = wlt_init();
				if (poll_fds[poll_fd_cnt].fd > 0) {
					poll_fds[idx_wlt_fd].events = POLLIN;
					poll_fds[idx_wlt_fd].revents = 0;
					poll_fd_cnt++;
				}
			}
		}
	}

	pthread_attr_init (&lpmd_attr);
	pthread_attr_setdetachstate (&lpmd_attr, PTHREAD_CREATE_DETACHED);

	/* Enable lpmd auto run when power profile daemon is not connected */
	if (connect_to_power_profile_daemon ())
		lpmd_set_auto();
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
