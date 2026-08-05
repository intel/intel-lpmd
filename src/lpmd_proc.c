// SPDX-License-Identifier: GPL-2.0-or-later
/* Copyright (C) 2026 Intel Corporation */

#include "lpmd.h"
#include <upower.h>
#include <time.h>
#include "wlt_proxy.h"

static struct lpmd_config_t lpmd_config;

struct lpmd_config_t *get_lpmd_config(void)
{
	return &lpmd_config;
}

static UpClient *upower_client;

static pthread_mutex_t lpmd_mutex;

int lpmd_lock(void)
{
	return pthread_mutex_lock(&lpmd_mutex);
}

int lpmd_unlock(void)
{
	return pthread_mutex_unlock(&lpmd_mutex);
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

static void lpmd_send_message(enum message_name_t msg_id, int size, unsigned char *msg)
{
	struct message_capsul_t msg_cap;
	int result;

	memset(&msg_cap, 0, sizeof(struct message_capsul_t));

	msg_cap.msg_id = msg_id;
	msg_cap.msg_size = (size > MAX_MSG_SIZE) ? MAX_MSG_SIZE : size;
	if (msg)
		memcpy(msg_cap.msg, msg, msg_cap.msg_size);

	result = write(write_pipe_fd, &msg_cap, sizeof(struct message_capsul_t));
	if (result < 0)
		lpmd_log_warn("Write to pipe failed\n");
}

void lpmd_terminate(void)
{
	restore_intel_pstate_mode();

	/*
	 * Stop any transient cpuset scopes synchronously here so that every
	 * termination path (SIGINT/SIGTERM handler, D-Bus Terminate from
	 * `systemctl stop intel_lpmd` or `intel_lpmd_control`) unbinds all
	 * managed PIDs before the process exits. process_cpuset_stop_all()
	 * issues a StopUnit D-Bus call per scope, which can exceed the 1s
	 * grace period below; doing it before signalling the worker
	 * guarantees cleanup runs to completion. The worker's later call
	 * is a safe no-op (g_pc_ctx is NULL after this).
	 */
	lpmd_process_cpuset_uninit ();

	lpmd_send_message (TERMINATE, 0, NULL);
	sleep (1);
	if (upower_client)
		g_clear_object(&upower_client);
}

void lpmd_force_on(void)
{
	lpmd_send_message(LPM_FORCE_ON, 0, NULL);
}

void lpmd_force_off(void)
{
	lpmd_send_message(LPM_FORCE_OFF, 0, NULL);
}

void lpmd_set_auto(void)
{
	lpmd_send_message(LPM_AUTO, 0, NULL);
}

void lpmd_set_process_preconfig(void)
{
	lpmd_send_message (LPM_PROCESS_PRECONFIG, 0, NULL);
}

#define LPMD_NUM_OF_POLL_FDS	6

static pthread_t lpmd_core_main;
static pthread_attr_t lpmd_attr;

static struct pollfd poll_fds[LPMD_NUM_OF_POLL_FDS];
static int poll_fd_cnt;

static int idx_pipe_fd = -1;
static int idx_uevent_fd = -1;
static int idx_hfi_fd = -1;
static int idx_wlt_fd = -1;
static int idx_pc_conn_fd = -1;

#include <gio/gio.h>

static GDBusProxy *power_profiles_daemon;

static enum power_profile_daemon_mode ppd_mode = PPD_INVALID;

int get_ppd_mode(void)
{
	return ppd_mode;
}

static void power_profiles_changed_cb(void)
{
	g_autoptr(GVariant)
	active_profile_v = NULL;

	active_profile_v = g_dbus_proxy_get_cached_property(power_profiles_daemon,
							    "ActiveProfile");

	if (active_profile_v && g_variant_is_of_type(active_profile_v, G_VARIANT_TYPE_STRING)) {
		const char *active_profile = g_variant_get_string(active_profile_v, NULL);

		lpmd_log_debug("%s: %s\n", __func__, active_profile);

		if (strcmp(active_profile, "power-saver") == 0) {
			ppd_mode = PPD_POWERSAVER;
			lpmd_send_message(lpmd_config.powersaver_def, 0, NULL);
		} else if (strcmp(active_profile, "performance") == 0) {
			ppd_mode = PPD_PERFORMANCE;
			lpmd_send_message(lpmd_config.performance_def, 0, NULL);
		} else if (strcmp(active_profile, "balanced") == 0) {
			ppd_mode = PPD_BALANCED;
			lpmd_send_message(lpmd_config.balanced_def, 0, NULL);
		} else {
			lpmd_log_warn("Ignore unsupported power profile: %s\n", active_profile);
		}

		if (lpmd_config.wlt_proxy_enable)
			lpmd_config.data.polling_interval = DEF_POLLING_INTERVAL;
	}
}

static int connect_to_power_profile_daemon(void)
{
	g_autoptr(GDBusConnection)
	bus = NULL;

	bus = g_bus_get_sync(G_BUS_TYPE_SYSTEM, NULL, NULL);
	if (bus) {
		power_profiles_daemon =
			g_dbus_proxy_new_sync(bus, G_DBUS_PROXY_FLAGS_DO_NOT_AUTO_START,
					      NULL, "net.hadess.PowerProfiles",
					      "/net/hadess/PowerProfiles",
					      "net.hadess.PowerProfiles", NULL, NULL);

		if (power_profiles_daemon) {
			g_signal_connect_swapped(power_profiles_daemon,
						 "g-properties-changed",
						 (GCallback)power_profiles_changed_cb,
						 NULL);
			power_profiles_changed_cb();
			return 0;
		}
		lpmd_log_info("Could not setup DBus watch for power-profiles-daemon");
	}
	return 1;
}

static int battery_mode = -1;

int is_on_battery(void)
{
	if (battery_mode < 0)
		battery_mode = up_client_get_on_battery(upower_client);

	return battery_mode;
}

static void upower_daemon_cb(UpClient *client, GParamSpec *pspec, gpointer user_data)
{
	static int mode = -1;

	battery_mode = up_client_get_on_battery(upower_client);
	if (mode != battery_mode) {
		process_balance_slider_default_update(&lpmd_config);
		process_slider_offset_default_update(&lpmd_config);
	}

	mode = battery_mode;
}

static void connect_to_upower_daemon(void)
{
	GError *error = NULL;
	GPtrArray *devices;
	UpDevice *device;
	int i;

	upower_client = up_client_new_full(NULL, &error);
	if (!upower_client) {
		g_warning("Cannot connect to upowerd: %s", error->message);
		g_error_free(error);
		return;
	}

	lpmd_log_info("connected to upower daemon\n");
	g_signal_connect(upower_client, "notify", G_CALLBACK(upower_daemon_cb), NULL);

	devices = up_client_get_devices2(upower_client);
	for (i = 0; i < devices->len; i++) {
		device = g_ptr_array_index(devices, i);
		g_signal_connect(device, "notify", G_CALLBACK(upower_daemon_cb), NULL);
	}
}

/* Poll time out default */
#define POLL_TIMEOUT_DEFAULT_SECONDS	1

// called from LPMD main thread to process user and system messages
static int proc_message(struct message_capsul_t *msg)
{
	int prev_state = get_lpmd_state();

	lpmd_log_debug("Received message %d\n", msg->msg_id);
	switch (msg->msg_id) {
	case TERMINATE:
		lpmd_log_msg("Terminating ...\n");
		update_lpmd_state(LPMD_TERMINATE);
		break;
	case LPM_FORCE_ON:
		(void)process_intel_pstate_mode(&lpmd_config);
		// Always stay in LPM mode
		update_lpmd_state(LPMD_ON);
		break;
	case LPM_FORCE_OFF:
		restore_intel_pstate_mode();
		if (lpmd_config.use_process_cpuset)
			lpmd_process_cpuset_unbind_all();
		// Never enter LPM mode
		update_lpmd_state(LPMD_OFF);
		break;
	case LPM_AUTO:
		(void)process_intel_pstate_mode(&lpmd_config);
		// Enable oppotunistic LPM
		update_lpmd_state(LPMD_AUTO);
		break;
	case LPM_PROCESS_PRECONFIG:
		(void)process_intel_pstate_mode(&lpmd_config);
		// PROCESS-PRECONFIG: only per-process cpuset is active; no LPM
		// transitions are driven by util/HFI/WLT.
		update_lpmd_state(LPMD_PROCESS_PRECONFIG);
		if (lpmd_config.use_process_cpuset && prev_state == LPMD_OFF)
			lpmd_process_cpuset_rescan();
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
		lpmd_log_debug("poll_fds[%s]: event %d, revent %d\n", "  Pipe",
			       poll_fds[i].events, poll_fds[i].revents);
		i++;
	}

	if (idx_uevent_fd != -1) {
		lpmd_log_debug("poll_fds[%s]: event %d, revent %d\n", "Uevent",
			       poll_fds[i].events, poll_fds[i].revents);
		i++;
	}

	if (idx_hfi_fd != -1) {
		lpmd_log_debug("poll_fds[%s]: event %d, revent %d\n", "   HFI",
			       poll_fds[i].events, poll_fds[i].revents);
		i++;
	}

	if (idx_wlt_fd != -1) {
		lpmd_log_debug("poll_fds[%s]: event %d, revent %d\n", "   WLT",
			       poll_fds[i].events, poll_fds[i].revents);
		i++;
	}
}

void update_reason(int reason)
{
	lpmd_config.data.need_update |= 1 << reason;
}

/*
 * This function should only work for user defined states - the ones located in
 * config files inside the <State> tag. Others like the default ones can share
 * their cpumasks with other states and therefore can't be uniquely identified
 * by their cpumask idx.
 */
char *user_cpumask_idx_to_state_name(enum cpumask_idx idx)
{
	for (int i = 0 ; i < MAX_STATES ; i++) {
		if (lpmd_config.config_states[i].cpumask_idx == idx)
			return lpmd_config.config_states[i].name;
	}
	return NULL;
}

static int hfi_timeout_cached_polling;
void set_polling(int ms)
{
	if (!hfi_timeout_cached_polling)
		hfi_timeout_cached_polling = lpmd_config.data.polling_interval;
	lpmd_config.data.polling_interval = ms;
}

void reset_polling(void)
{
	lpmd_config.data.polling_interval = hfi_timeout_cached_polling;
	hfi_timeout_cached_polling = 0;
}

// LPMD processing thread. This is callback to pthread lpmd_core_main
static void *lpmd_core_main_loop(void *arg)
{
	struct message_capsul_t msg;
	int wlt_hint, result, n;

	/* Rescan /proc every 60 seconds to bind any matching PIDs spawned
	 * after lpmd_process_cpuset_init() ran at startup. */
	const time_t process_cpuset_rescan_interval = 60;
	time_t process_cpuset_last_rescan = time(NULL);

	lpmd_config.data.polling_interval = DEF_POLLING_INTERVAL;

	for (;;) {
		if (get_lpmd_state() == LPMD_TERMINATE)
			break;

		if (lpmd_config.data.polling_interval <= 0)
			lpmd_config.data.polling_interval = process_cpuset_rescan_interval * 1000;

		n = poll (poll_fds, poll_fd_cnt, lpmd_config.data.polling_interval);
		if (n < 0) {
			lpmd_log_warn("Write to pipe failed\n");
			continue;
		}
		dump_poll_results(n);

		if (hfi_timeout == HFI_TIMEOUT_TIMER) {
			int delta = hfi_time_delta();
			lpmd_log_debug("hfi_timeout: Timer : %dms / %dms\n", delta / 1000000, DEF_HFI_TIMEOUT);

			/* If perf flag was set don't finish the timeout */
			if (going_back_to_perf) {
				lpmd_log_debug("hfi_timeout: staying in performance cpumask\n");
				hfi_timeout = HFI_TIMEOUT_FINAL;
				hfi_timeout_state_action(hfi_timeout);
			/* Else check if timeout finished before going to LPM */
			} else if (hfi_timeout_over(DEF_HFI_TIMEOUT) > 0) {
				lpmd_log_debug("hfi_timeout: timeout over - moving to LP cpumask\n");
				hfi_timeout = HFI_TIMEOUT_CACHED;
				hfi_timeout_state_action(hfi_timeout);
			}
		}

		/* Periodic process_cpuset rescan (skipped while OFF) */
		if (lpmd_config.use_process_cpuset && get_lpmd_state() != LPMD_OFF) {
			time_t now = time(NULL);
			if (now - process_cpuset_last_rescan >= process_cpuset_rescan_interval) {
				lpmd_process_cpuset_rescan();
				process_cpuset_last_rescan = now;
			}
		}

		/* Polling time out, update polling data */
		if (n == 0 && lpmd_config.util_monitor && lpmd_config.data.polling_interval > 0) {
			update_reason(UPDATE_UTIL);
			util_update(&lpmd_config);

			if (lpmd_config.wlt_proxy_enable)
				lpmd_config.data.wlt_hint =
					read_wlt_proxy(&lpmd_config.data.polling_interval);
		}

		/* Check CPU hotplug. Maybe need to freeze lpmd */
		if (idx_uevent_fd >= 0 && (poll_fds[idx_uevent_fd].revents & POLLIN))
			check_cpu_hotplug();

		/* Update CPUMASK_HFI */
		if (idx_hfi_fd >= 0 && (poll_fds[idx_hfi_fd].revents & POLLIN)) {
			/* Timeout cached updates hfi manually */
			if (hfi_timeout < HFI_TIMEOUT_FINAL)
				hfi_update();
			else if (hfi_timeout == HFI_TIMEOUT_FINAL)
				hfi_timeout = HFI_TIMEOUT_PERF;
			else
				hfi_timeout = HFI_TIMEOUT_LP;
		}

		/* Update WLT hint */
		if (idx_wlt_fd >= 0 && (poll_fds[idx_wlt_fd].revents & POLLPRI)) {
			wlt_hint = wlt_update(poll_fds[idx_wlt_fd].fd);
			if (wlt_hint != lpmd_config.data.wlt_hint) {
				lpmd_config.data.wlt_hint = wlt_hint;
				update_reason(UPDATE_WLT);
			}
		}

		/* Drain proc-connector events: classify newly-exec()ed PIDs. */
		if (idx_pc_conn_fd >= 0 && (poll_fds[idx_pc_conn_fd].revents & POLLIN)) {
			lpmd_process_cpuset_proc_connector_handle();
		}

		/* Respond Dbus commands */
		if (idx_pipe_fd >= 0 && (poll_fds[idx_pipe_fd].revents & POLLIN)) {
//			 process message written on pipe here

			result = read(poll_fds[idx_pipe_fd].fd, &msg,
				      sizeof(struct message_capsul_t));
			if (result < 0) {
				lpmd_log_warn("read on wakeup fd failed\n");
				poll_fds[idx_pipe_fd].revents = 0;
				continue;
			}
			if (proc_message(&msg) < 0)
				lpmd_log_debug("Terminating thread..\n");
			update_reason(UPDATE_USER);
		}

		if (lpmd_config.data.need_update) {
			/* Enter next state after collecting all system statistics */
			lpmd_enter_next_state();
			lpmd_config.data.need_update = 0;
		}
	}

	if (lpmd_config.wlt_proxy_enable)
		wlt_proxy_uninit();

	hfi_kill ();
	/* Stop any transient cpuset scopes we created before tearing down cgroups. */
	lpmd_process_cpuset_uninit();
	cgroup_cleanup();

	return NULL;
}

int lpmd_main(void)
{
	int wake_fds[2];
	int ret;

	lpmd_log_debug("%s begin\n", __func__);

	ret = detect_supported_platform(&lpmd_config);
	if (ret)
		return ret;

	ret = detect_cpu_topo(&lpmd_config);
	if (ret)
		goto cleanup;

//	 Call all lpmd related functions here
	ret = lpmd_get_config(&lpmd_config);
	if (ret)
		goto cleanup;

	(void)process_intel_pstate_mode(&lpmd_config);

	pthread_mutex_init(&lpmd_mutex, NULL);

	ret = detect_lpm_cpus(lpmd_config.lp_mode_cpus);
	if (ret)
		goto cleanup;

	ret = cgroup_init(&lpmd_config);
	if (ret)
		goto cleanup;

	itmt_init();

	ret = epp_epb_init();
	if (ret)
		goto cleanup;

	if (lpmd_config.hfi_lpm_enable && !has_hfi_capability()) {
		lpmd_log_error("System doesn't support HFI but it was used in the config file!\n");
		ret = LPMD_CONFIGURATION_ERROR;
		goto cleanup;
	}

	/* Must done after init_cpu() */
	lpmd_build_config_states(&lpmd_config);

	ret = exclude_incompatible_configs(&lpmd_config);
	if (ret)
		goto cleanup;

	/* If <UseProcessCPUSet> is set, seed the process_cpuset library with
	 * the active P/E/LP-E core sets (still alive in core_type_masks[])
	 * and attach matching processes to transient cpuset scopes. */
	lpmd_process_cpuset_init(&lpmd_config);
	ret = irq_init();
	if (ret)
		return ret;

	connect_to_upower_daemon();
//	 Pipe is used for communication between two processes
	ret = pipe(wake_fds);
	if (ret) {
		lpmd_log_error("pipe creation failed %d:\n", ret);
		return LPMD_FATAL_ERROR;
	}
	if (fcntl(wake_fds[0], F_SETFL, O_NONBLOCK) < 0) {
		lpmd_log_error("Cannot set non-blocking on pipe: %s\n", strerror(errno));
		(void)close(wake_fds[0]);
		(void)close(wake_fds[1]);
		return LPMD_FATAL_ERROR;
	}
	if (fcntl(wake_fds[1], F_SETFL, O_NONBLOCK) < 0) {
		lpmd_log_error("Cannot set non-blocking on pipe: %s\n", strerror(errno));
		(void)close(wake_fds[0]);
		(void)close(wake_fds[1]);
		return LPMD_FATAL_ERROR;
	}
	write_pipe_fd = wake_fds[1];

	memset(poll_fds, 0, sizeof(poll_fds));

	idx_pipe_fd = poll_fd_cnt;
	poll_fds[idx_pipe_fd].fd = wake_fds[0];
	poll_fds[idx_pipe_fd].events = POLLIN;
	poll_fds[idx_pipe_fd].revents = 0;
	poll_fd_cnt++;

	poll_fds[poll_fd_cnt].fd = uevent_init();
	if (poll_fds[poll_fd_cnt].fd > 0) {
		idx_uevent_fd = poll_fd_cnt;
		poll_fds[idx_uevent_fd].events = POLLIN;
		poll_fds[idx_uevent_fd].revents = 0;
		poll_fd_cnt++;
	}

	if (lpmd_config.hfi_lpm_enable) {
		poll_fds[poll_fd_cnt].fd = hfi_init();
		if (poll_fds[poll_fd_cnt].fd > 0) {
			idx_hfi_fd = poll_fd_cnt;
			poll_fds[idx_hfi_fd].events = POLLIN;
			poll_fds[idx_hfi_fd].revents = 0;
			poll_fd_cnt++;
		}
	}

	if (lpmd_config.wlt_hint_enable &&
	    lpmd_config.wlt_proxy_enable &&
	    wlt_proxy_init() != LPMD_SUCCESS) {
		lpmd_config.wlt_proxy_enable = 0;
		lpmd_log_error("Error setting up WLT Proxy. wlt_proxy_enable disabled\n");
		return LPMD_ERROR;
	}

	if (lpmd_config.wlt_hint_enable) {
		if (!lpmd_config.wlt_proxy_enable) {
			poll_fds[poll_fd_cnt].fd = wlt_init();
			if (poll_fds[poll_fd_cnt].fd > 0) {
				idx_wlt_fd = poll_fd_cnt;
				poll_fds[idx_wlt_fd].events = POLLPRI;
				poll_fds[idx_wlt_fd].revents = 0;
				poll_fd_cnt++;
			}
		}
		if (lpmd_config.wlt_notification_delay != -1) {
			if (wlt_set_notification_delay(lpmd_config.wlt_notification_delay) != LPMD_SUCCESS)
				lpmd_log_error("Error setting WLT notification delay. wlt_notification_delay set to default value\n");
		}
	}

	/*
	 * Subscribe to the kernel proc-connector multicast group so newly
	 * exec()ed PIDs can be classified immediately, instead of waiting
	 * for the periodic /proc rescan. Optional: the helper falls back
	 * gracefully (returns -1) when CAP_NET_ADMIN or the kernel
	 * feature is missing.
	 */
	if (lpmd_config.use_process_cpuset) {
		int conn_fd = lpmd_process_cpuset_proc_connector_init();
		if (conn_fd >= 0 && poll_fd_cnt < LPMD_NUM_OF_POLL_FDS) {
			poll_fds[poll_fd_cnt].fd = conn_fd;
			poll_fds[poll_fd_cnt].events = POLLIN;
			poll_fds[poll_fd_cnt].revents = 0;
			idx_pc_conn_fd = poll_fd_cnt;
			poll_fd_cnt++;
		}
	}

	pthread_attr_init (&lpmd_attr);
	pthread_attr_setdetachstate (&lpmd_attr, PTHREAD_CREATE_DETACHED);

	/* Enable lpmd auto run when power profile daemon is not connected */
	if (connect_to_power_profile_daemon())
		lpmd_set_auto();

	process_balance_slider_default_update(&lpmd_config);
	process_slider_offset_default_update(&lpmd_config);

	/*
	 * lpmd_core_main_loop: is the thread where all LPMD actions take place.
	 * All other thread send message via pipe to trigger processing
	 */
	ret = pthread_create(&lpmd_core_main, &lpmd_attr, lpmd_core_main_loop, NULL);
	if (ret)
		return LPMD_FATAL_ERROR;

	lpmd_log_debug("lpmd_init succeeds\n");

	return LPMD_SUCCESS;
cleanup:
	restore_intel_pstate_mode();
	free_cpu_type_masks(&lpmd_config);
	return ret;
}
