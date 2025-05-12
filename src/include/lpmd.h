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
#include <stdint.h>
#include <signal.h>
#include <pthread.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <string.h>
#include <signal.h>
#include <poll.h>
#include <cpuid.h>

#include "config.h"
#include "thermal.h"

#define LOG_DEBUG_INFO	1

#define LOCKF_SUPPORT

#ifdef GLIB_SUPPORT
#include <glib.h>
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
	TERMINATE, LPM_FORCE_ON, LPM_FORCE_OFF, LPM_AUTO, HFI_EVENT,
} message_name_t;

#define MAX_MSG_SIZE		512

typedef struct {
	message_name_t msg_id;
	int msg_size;
	unsigned long msg[MAX_MSG_SIZE];
} message_capsul_t;

#define MAX_STR_LENGTH		256
#define MAX_CONFIG_STATES	10
#define MAX_STATE_NAME		32
#define MAX_CONFIG_LEN		64

enum lpmd_states {
	LPMD_OFF,
	LPMD_ON,
	LPMD_AUTO,
	LPMD_FREEZE,
	LPMD_RESTORE,
	LPMD_TERMINATE,
};

typedef struct {
	int util_cpu;	/* From Util monitor */
	int util_sys;	/* From Util monitor */
	int util_gfx;	/* From Util monitor */
	int wlt_hint;	/* From WLT monitor */
	int has_hfi_update;	/* From HFI monitor */
	int polling_interval;
	/* active_cpus from HFI monitor but we can leverage CPUMASK_HFI for now */
}lpmd_data_t;

enum default_config_state {
	DEFAULT_OFF,	/* lpmd force off: state with all default power settings */
	DEFAULT_ON,	/* lpmd force on: state with global CPU/IRQ/ITMT/EPP configurations */
	DEFAULT_HFI,	/* LPM state with CPU isolation based on HFI hints only */
	CONFIG_STATE_BASE,
	MAX_STATES = CONFIG_STATE_BASE + MAX_CONFIG_STATES,
	STATE_NONE = MAX_STATES,
};

typedef struct {
	int id;
	int valid;
	char name[MAX_STATE_NAME];
	int wlt_type;
	int entry_system_load_thres;
	int exit_system_load_thres;
	int exit_system_load_hyst;
	int enter_cpu_load_thres;
	int exit_cpu_load_thres;
	int enter_gfx_load_thres;
	int exit_gfx_load_thres;
	int min_poll_interval;
	int max_poll_interval;
	int poll_interval_increment;
	int epp;
	int epb;
	char active_cpus[MAX_STR_LENGTH];
	// If active CPUs are specified then
	// the below counts don't matter
	int island_0_number_p_cores;
	int island_0_number_e_cores;
	int island_1_number_p_cores;
	int island_1_number_e_cores;
	int island_2_number_p_cores;
	int island_2_number_e_cores;

	int itmt_state;
	int irq_migrate;

	// Private state variables, not configurable
	int entry_load_sys;
	int entry_load_cpu;
	int cpumask_idx;
	int steady;
}lpmd_config_state_t;

// lpmd config data
typedef struct {
	int mode;
	int performance_def;
	int balanced_def;
	int powersaver_def;
	int hfi_lpm_enable;
	int wlt_hint_enable;
	int wlt_hint_poll_enable;
	int wlt_proxy_enable;
	union {
		struct {
			uint32_t util_sys_enable:1;
			uint32_t util_cpu_enable:1;
			uint32_t util_gfx_enable:1;
		};
		uint32_t util_enable;
	};
	int util_entry_threshold;
	int util_exit_threshold;
	int util_entry_delay;
	int util_exit_delay;
	int util_entry_hyst;
	int util_exit_hyst;
	int ignore_itmt;
	int lp_mode_epp;
	char lp_mode_cpus[MAX_STR_LENGTH];
	int cpu_family;
	int cpu_model;
	char cpu_config[MAX_CONFIG_LEN];
	int config_state_count;
	int tdp;
	lpmd_config_state_t config_states[MAX_STATES];
	lpmd_data_t data;
} lpmd_config_t;

enum lpm_cpu_process_mode {
	LPM_CPU_CGROUPV2,
	LPM_CPU_ISOLATE,
	LPM_CPU_POWERCLAMP,
	LPM_CPU_OFFLINE,
	LPM_CPU_MODE_MAX = LPM_CPU_POWERCLAMP,
};

#define NUM_USER_CPUMASKS	10
enum cpumask_idx {
	CPUMASK_LPM_DEFAULT,
	CPUMASK_ONLINE,
	CPUMASK_HFI,
	CPUMASK_HFI_BANNED,
	CPUMASK_HFI_LAST,
	CPUMASK_UTIL,
	CPUMASK_USER,
	CPUMASK_MAX = CPUMASK_USER + NUM_USER_CPUMASKS,
	CPUMASK_NONE = CPUMASK_MAX,
};

#define UTIL_DELAY_MAX		5000
#define UTIL_HYST_MAX		10000

#define cpuid(leaf, eax, ebx, ecx, edx)		\
	__cpuid(leaf, eax, ebx, ecx, edx);	\
	lpmd_log_debug("CPUID 0x%08x: eax = 0x%08x ebx = 0x%08x ecx = 0x%08x edx = 0x%08x\n",	\
			leaf, eax, ebx, ecx, edx);

#define cpuid_count(leaf, subleaf, eax, ebx, ecx, edx)		\
	__cpuid_count(leaf, subleaf, eax, ebx, ecx, edx);	\
	lpmd_log_debug("CPUID 0x%08x subleaf 0x%08x: eax = 0x%08x ebx = 0x%08x ecx = 0x%08x"	\
			"edx = 0x%08x\n", leaf, subleaf, eax, ebx, ecx, edx);

#define SETTING_RESTORE	-2
#define SETTING_IGNORE	-1

/* WLT hints parsing */
typedef enum {
	WLT_IDLE = 0,
	WLT_BATTERY_LIFE = 1,
	WLT_SUSTAINED = 2,
	WLT_BURSTY = 3,
	WLT_INVALID = 4,
} wlt_type_t;

enum power_profile_daemon_mode {
	PPD_PERFORMANCE,
	PPD_BALANCED,
	PPD_POWERSAVER,
	PPD_INVALID
};

/* lpmd_main.c */
int in_debug_mode(void);
int do_platform_check(void);

/* lpmd_proc.c: interfaces */
lpmd_config_t *get_lpmd_config(void);

int lpmd_lock(void);
int lpmd_unlock(void);

void lpmd_terminate(void);
void lpmd_force_on(void);
void lpmd_force_off(void);
void lpmd_set_auto(void);

int is_on_battery(void);
int get_ppd_mode(void);

/* lpmd_proc.c: init func */
int lpmd_main(void);

/* lpmd_dbus_server.c */
int intel_dbus_server_init(gboolean (*exit_handler)(void));

/* lpmd_config.c */
int lpmd_get_config(lpmd_config_t *lpmd_config);

/* lpmd_state_machine.c */
int update_lpmd_state(int state);
int get_lpmd_state(void);
int lpmd_init_config_state(lpmd_config_state_t *state);
int lpmd_build_config_states(lpmd_config_t *config);
int lpmd_enter_next_state(void);

/* lpmd_util.c */
int util_update(lpmd_config_t *lpmd_config);

/* lpmd_hfi.c */
int hfi_init(void);
int hfi_kill(void);
int hfi_update(void);

/* lpmd_wlt.c */
int wlt_init(void);
int wlt_exit(void);
int wlt_update(int fd);

/* lpmd_misc.c */
int itmt_init(void);
int get_itmt(void);
int process_itmt(lpmd_config_state_t *state);

int epp_epb_init(void);
int get_epp_epb(int *epp, char *epp_str, int size, int *epb);
int process_epp_epb(lpmd_config_state_t *state);

/* lpmd_irq.c */
int irq_init(void);
int process_irq(lpmd_config_state_t *state);

/* lpmd_cgroup.c*/
int cgroup_init(lpmd_config_t *config);
int cgroup_cleanup(void);
int process_cgroup(lpmd_config_state_t *state, enum lpm_cpu_process_mode mode);

/* lpmd_uevent.c */
int uevent_init(void);
int check_cpu_hotplug(void);

/* lpmd_cpu.c */
int detect_supported_platform(lpmd_config_t *lpmd_config);
int detect_cpu_topo(lpmd_config_t *lpmd_config);
int detect_lpm_cpus(char *cmd_cpus);

int is_cpu_ecore(int cpu);
int is_cpu_pcore(int cpu);

/* lpmd_cpumask.c */
int is_cpu_online(int cpu);
int get_max_cpus(void);
void set_max_cpus(int num);
int get_max_online_cpu(void);
void set_max_online_cpu(int num);
int cpu_migrate(int cpu);
int cpu_clear_affinity(void);

int cpumask_alloc(void);
int cpumask_free(enum cpumask_idx idx);
int cpumask_reset(enum cpumask_idx idx);

int cpumask_add_cpu(int cpu, enum cpumask_idx idx);
int cpumask_init_cpus(char *buf, enum cpumask_idx idx);
int cpumask_nr_cpus(enum cpumask_idx idx);
int cpumask_has_cpu(enum cpumask_idx idx);

int cpumask_equal(enum cpumask_idx idx1, enum cpumask_idx idx2);
void cpumask_copy(enum cpumask_idx source, enum cpumask_idx dest);
void cpumask_exclude_copy(enum cpumask_idx source, enum cpumask_idx dest, enum cpumask_idx exlude);

char *get_cpus_str(enum cpumask_idx idx);
char *get_proc_irq_str(enum cpumask_idx idx);
char *get_irqbalance_str(enum cpumask_idx idx);
char *get_cpu_isolation_str(enum cpumask_idx idx);
uint8_t *get_cgroup_systemd_vals(enum cpumask_idx idx, int *size);

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
uint64_t read_msr(int cpu, uint32_t msr);
#endif
