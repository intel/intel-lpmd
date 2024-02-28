/*
 * intel_lpmd.h: Intel Low Power Daemon common header file
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
 */
#ifndef LPMD_INTEL_LPMD_H
#define LPMD_INTEL_LPMD_H

#include <stdio.h>
#include <getopt.h>
#include <locale.h>
#include <errno.h>
#include <stdbool.h>
#include <stdlib.h>
#include <signal.h>
#include <pthread.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <string.h>
#include <signal.h>
#include <poll.h>

#include "config.h"
#include "thermal.h"

#define LOG_DEBUG_INFO	1

#define LOCKF_SUPPORT

#ifdef GLIB_SUPPORT
#include <glib.h>
#include <dbus/dbus.h>
#include <dbus/dbus-glib-lowlevel.h>
#include <dbus/dbus-glib.h>
#include <glib/gi18n.h>
#include <gmodule.h>


// Log macros
enum log_level {
	LPMD_LOG_NONE,
	LPMD_LOG_INFO,
	LPMD_LOG_DEBUG,
	LPMD_LOG_MSG,
	LPMD_LOG_WARN,
	LPMD_LOG_ERROR,
	LPMD_LOG_FATAL,
};

#define lpmd_log_fatal		g_error		// Print error and terminate
#define lpmd_log_error		g_critical
#define lpmd_log_warn		g_warning
#define lpmd_log_msg		g_message
#define lpmd_log_debug		g_debug
#define lpmd_log_info(...)	g_log(NULL, G_LOG_LEVEL_INFO, __VA_ARGS__)
#else
static int dummy_printf(const char *__restrict __format, ...)
{
	return 0;
}
#define lpmd_log_fatal		printf
#define lpmd_log_error		printf
#define lpmd_log_warn		printf
#define lpmd_log_msg		printf
#define lpmd_log_debug		dummy_printf
#define lpmd_log_info		printf
#endif

// Common return value defines
#define LPMD_SUCCESS		0
#define LPMD_ERROR		-1
#define LPMD_FATAL_ERROR		-2

// Dbus related
/* Well-known name for this service. */
#define INTEL_LPMD_SERVICE_NAME        	"org.freedesktop.intel_lpmd"
#define INTEL_LPMD_SERVICE_OBJECT_PATH 	"/org/freedesktop/intel_lpmd"
#define INTEL_LPMD_SERVICE_INTERFACE	"org.freedesktop.intel_lpmd"

typedef enum {
	TERMINATE, LPM_FORCE_ON, LPM_FORCE_OFF, LPM_AUTO, SUV_MODE_ENTER, SUV_MODE_EXIT, HFI_EVENT,
} message_name_t;

#define MAX_MSG_SIZE		512

typedef struct {
	message_name_t msg_id;
	int msg_size;
	unsigned long msg[MAX_MSG_SIZE];
} message_capsul_t;

#define MAX_STR_LENGTH		256

// lpmd config data
typedef struct {
	int mode;
	int performance_def;
	int balanced_def;
	int powersaver_def;
	int hfi_lpm_enable;
	int hfi_suv_enable;
	int util_enable;
	int util_entry_threshold;
	int util_exit_threshold;
	int util_entry_delay;
	int util_exit_delay;
	int util_entry_hyst;
	int util_exit_hyst;
	int ignore_itmt;
	int lp_mode_epp;
	char lp_mode_cpus[MAX_STR_LENGTH];
} lpmd_config_t;

enum lpm_cpu_process_mode {
	LPM_CPU_CGROUPV2,
	LPM_CPU_ISOLATE,
	LPM_CPU_POWERCLAMP,
	LPM_CPU_OFFLINE,
	LPM_CPU_MODE_MAX = LPM_CPU_POWERCLAMP,
};

enum lpm_command {
	USER_ENTER, /* Force enter LPM and always stay in LPM */
	USER_AUTO, /* Allow oppotunistic LPM based on util/hfi request */
	USER_EXIT, /* Force exit LPM and never enter LPM */
	HFI_ENTER,
	HFI_EXIT,
	HFI_SUV_ENTER,
	HFI_SUV_EXIT,
	DBUS_SUV_ENTER,
	DBUS_SUV_EXIT,
	UTIL_ENTER,
	UTIL_EXIT,
	LPM_CMD_MAX,
};

enum cpumask_idx {
	CPUMASK_LPM_DEFAULT, CPUMASK_ONLINE, CPUMASK_HFI, CPUMASK_HFI_BANNED, CPUMASK_HFI_SUV, /* HFI Survivability mode */
	CPUMASK_HFI_LAST, CPUMASK_MAX,
};

#define MAX_LPM_CPUS		32

#define UTIL_DELAY_MAX		5000
#define UTIL_HYST_MAX		10000

/* lpmd_main.c */
int in_debug_mode(void);

/* lpmd_proc.c: interfaces */
int lpmd_lock(void);
int lpmd_unlock(void);
int in_lpm(void);
int in_hfi_lpm(void);
int in_suv_lpm(void);
int get_idle_percentage(void);
int get_idle_duration(void);
int get_cpu_mode(void);
int has_hfi_lpm_monitor(void);
int has_hfi_suv_monitor(void);
int has_util_monitor(void);
int get_util_entry_interval(void);
int get_util_exit_interval(void);
int get_util_entry_threshold(void);
int get_util_exit_threshold(void);
int get_util_entry_hyst(void);
int get_util_exit_hyst(void);
void set_ignore_itmt(void);

int process_lpm(enum lpm_command cmd);
int process_lpm_unlock(enum lpm_command cmd);
int freeze_lpm(void);
int restore_lpm(void);

void lpmd_terminate(void);
void lpmd_force_on(void);
void lpmd_force_off(void);
void lpmd_set_auto(void);
void lpmd_suv_enter(void);
void lpmd_suv_exit(void);
void lpmd_notify_hfi_event(void);

/* lpmd_proc.c: init func */
int lpmd_main(void);

/* lpmd_dbus_server.c */
int intel_dbus_server_init(gboolean (*exit_handler)(void));

/* lpmd_config.c */
int lpmd_get_config(lpmd_config_t *lpmd_config);

/* util.c */
int periodic_util_update(void);

/* cpu.c */
int init_cpu(char *cmd_cpus, enum lpm_cpu_process_mode mode, int lp_mode_epp);
int process_cpus(int enter, enum lpm_cpu_process_mode mode);

/* cpu.c: helpers */
int is_cpu_online(int cpu);
int is_cpu_for_lpm(int cpu);
int get_max_cpus(void);
int get_max_online_cpu(void);

char* get_lpm_cpus_hexstr(void);
int has_lpm_cpus(void);
int has_cpus(enum cpumask_idx idx);

void copy_cpu_mask_exclude(enum cpumask_idx source, enum cpumask_idx dest, enum cpumask_idx exlude);
void copy_cpu_mask(enum cpumask_idx source, enum cpumask_idx dest);
void copy_cpu_mask_exclude(enum cpumask_idx source, enum cpumask_idx dest, enum cpumask_idx exlude);

int is_equal(enum cpumask_idx idx1, enum cpumask_idx idx2);

int add_cpu(int cpu, enum cpumask_idx idx);
void reset_cpus(enum cpumask_idx idx);
int set_lpm_cpus(enum cpumask_idx new);
int uevent_init(void);
int check_cpu_hotplug(void);

/* cpu.c : APIs for SUV mode support */
int process_suv_mode(enum lpm_command cmd);
int has_suv_support(void);

/* irq.c */
int init_irq(void);
int process_irqs(int enter, enum lpm_cpu_process_mode mode);

/* hfi.c */
int hfi_init(void);
int hfi_kill(void);
void hfi_receive(void);

/* socket.c */
int socket_init_connection(char *name);
int socket_send_cmd(char *name, char *data);

/* helper */
int lpmd_write_str(const char *name, char *str, int print_level);
int lpmd_write_str_verbose(const char *name, char *str, int print_level);
int lpmd_write_str_append(const char *name, char *str, int print_level);
int lpmd_write_int(const char *name, int val, int print_level);
int lpmd_open(const char *name, int print_level);
int lpmd_read_int(const char *name, int *val, int print_level);
char* get_time(void);
void time_start(void);
char* time_delta(void);

#endif
