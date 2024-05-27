/*
 * irq.c: irq related processing
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
#define _GNU_SOURCE
#include <stdio.h>
#include <err.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/file.h>
#include <sys/types.h>
#include <sys/time.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <errno.h>
#include <getopt.h>
#include <cpuid.h>
#include <sched.h>
#include <dirent.h>
#include <ctype.h>
#include <signal.h>
#include <netlink/genl/genl.h>
#include <netlink/genl/family.h>
#include <netlink/genl/ctrl.h>

#include "lpmd.h"

char *lp_mode_irq_str;

static char irq_socket_name[64];

static int irqbalance_pid = -1;

#define MAX_IRQS		128

struct info_irq {
	int irq;
	char affinity[MAX_STR_LENGTH];
};

struct info_irqs {
	/* Cached IRQ smp_affinity info */
	int nr_irqs;
	struct info_irq irq[MAX_IRQS];
};

struct info_irqs info_irqs;
struct info_irqs *info = &info_irqs;

static char irq_str[MAX_STR_LENGTH];

static int lp_mode_irq;
int set_lpm_irq(cpu_set_t *cpumask, int action)
{
	lp_mode_irq = action;

	if (lp_mode_irq == SETTING_IGNORE)
		return 0;

	if (irqbalance_pid > 0) {
		if (lp_mode_irq == SETTING_RESTORE)
			snprintf(irq_str, sizeof("NULL"), "NULL");
		else
			cpumask_to_str(cpumask, irq_str, MAX_STR_LENGTH);
	} else {
		if (lp_mode_irq != SETTING_RESTORE)
			cpumask_to_hexstr(cpumask, irq_str, MAX_STR_LENGTH);
	}

	return 0;
}

static int dump_smp_affinity(void)
{
	FILE *filep;
	DIR *dir;
	struct dirent *d;
	char path[MAX_STR_LENGTH * 2];
	char str[MAX_STR_LENGTH];
	size_t ret;

	if (!in_debug_mode())
		return 0;

	dir = opendir("/proc/irq");
	if (dir) {
		while ((d = readdir(dir)) != NULL) {
			snprintf(path, MAX_STR_LENGTH * 2, "/proc/irq/%s/smp_affinity", d->d_name);
			filep = fopen(path, "r+");
			if (!filep)
				continue;

			str[0] = '\0';
			ret = fread(str, 1, MAX_STR_LENGTH, filep);
			if (ret)
				lpmd_log_debug("%s:%s", path, str);
			fclose(filep);
		}
		closedir(dir);
	}

	return 0;
}

/* Interrupt Management */
#define SOCKET_PATH "irqbalance"
#define SOCKET_TMPFS "/run/irqbalance"

static int irqbalance_ban_cpus(int enter)
{
	char socket_cmd[MAX_STR_LENGTH];
	struct timespec tp1, tp2;
	int cpu;
	int offset;
	int first = 1;

	dump_smp_affinity();

	if (lp_mode_irq == SETTING_RESTORE)
		lpmd_log_debug ("\tRestore IRQ affinity (irqbalance)\n");
	else
		lpmd_log_debug ("\tUpdate IRQ affinity (irqbalance)\n");

	offset = snprintf (socket_cmd, MAX_STR_LENGTH, "settings cpus %s", irq_str);
	if (offset >= MAX_STR_LENGTH)
		offset = MAX_STR_LENGTH - 1;
	socket_cmd[offset] = '\0';

	clock_gettime (CLOCK_MONOTONIC, &tp1);
	socket_send_cmd (irq_socket_name, socket_cmd);
	clock_gettime (CLOCK_MONOTONIC, &tp2);
	lpmd_log_debug ("\tSend socket command %s (%lu ns)\n", socket_cmd,
		1000000000UL * (tp2.tv_sec - tp1.tv_sec) + tp2.tv_nsec - tp1.tv_nsec);
	return 0;
}

static int native_restore_irqs(void)
{
	char path[MAX_STR_LENGTH];
	int i;

	lpmd_log_debug ("\tRestore IRQ affinity (native)\n");

	for (i = 0; i < info->nr_irqs; i++) {
		char *str = info->irq[i].affinity;

		snprintf (path, MAX_STR_LENGTH, "/proc/irq/%i/smp_affinity", info->irq[i].irq);

		lpmd_write_str (path, str, LPMD_LOG_DEBUG);
	}
	memset (info, 0, sizeof(*info));
	return 0;
}

static int irq_updated;

static int update_one_irq(int irq)
{
	FILE *filep;
	size_t size = 0;
	char path[MAX_STR_LENGTH];
	char *str = NULL;

	if (info->nr_irqs >= (MAX_IRQS - 1)) {
		lpmd_log_error ("Too many IRQs\n");
		return -1;
	}


	snprintf (path, MAX_STR_LENGTH, "/proc/irq/%i/smp_affinity", irq);

	if (!irq_updated) {
		info->irq[info->nr_irqs].irq = irq;
		filep = fopen (path, "r");
		if (!filep)
			return -1;

		if (getline (&str, &size, filep) <= 0) {
			lpmd_log_error ("Failed to get IRQ%d smp_affinity\n", irq);
			free (str);
			fclose (filep);
			return -1;
		}

		fclose (filep);

		snprintf (info->irq[info->nr_irqs].affinity, MAX_STR_LENGTH, "%s", str);

		free (str);

		/* Remove the Newline */
		size = strnlen (info->irq[info->nr_irqs].affinity, MAX_STR_LENGTH);
		info->irq[info->nr_irqs].affinity[size - 1] = '\0';

		info->nr_irqs++;
	}

	return lpmd_write_str (path, irq_str, LPMD_LOG_DEBUG);
}

static int native_update_irqs(void)
{
	FILE *filep;
	char *line = NULL;
	size_t size = 0;

	lpmd_log_debug ("\tUpdate IRQ affinity (native)\n");

	filep = fopen ("/proc/interrupts", "r");
	if (!filep) {
		perror ("Error open /proc/interrupts\n");
		return -1;
	}

	/* first line is the header we don't need; nuke it */
	if (getline (&line, &size, filep) <= 0) {
		perror ("Error getline\n");
		free (line);
		fclose (filep);
		return -1;
	}
	free (line);

	while (!feof (filep)) {
		int number;
		char *c;

		line = NULL;
		size = 0;

		if (getline (&line, &size, filep) <= 0) {
			free (line);
			break;
		}

		/* lines with letters in front are special, like NMI count. Ignore */
		c = line;
		while (isblank(*(c)))
			c++;

		if (!isdigit(*c)) {
			free (line);
			break;
		}
		c = strchr (line, ':');
		if (!c) {
			free (line);
			continue;
		}

		*c = 0;
		number = strtoul (line, NULL, 10);

		update_one_irq (number);
		free (line);
	}

	fclose (filep);

	irq_updated = 1;
	return 0;
}

static int native_process_irqs(int enter)
{
	dump_smp_affinity();

	if (lp_mode_irq == SETTING_RESTORE)
		return native_restore_irqs ();
	else
		return native_update_irqs ();
}

int process_irqs(int enter, enum lpm_cpu_process_mode mode)
{
	/* No need to handle IRQs in offline mode */
	if (mode == LPM_CPU_OFFLINE)
		return 0;

	if (lp_mode_irq == SETTING_IGNORE) {
		lpmd_log_info ("Ignore IRQ migration\n");
		return 0;
	}

	lpmd_log_info ("Process IRQs ...\n");
	if (irqbalance_pid == -1)
		return native_process_irqs (enter);
	else
		return irqbalance_ban_cpus (enter);
}

int init_irq(void)
{
	DIR *dir;
	int socket_fd;
	int ret;

	lpmd_log_info ("Detecting IRQs ...\n");

	dir = opendir ("/run/irqbalance");
	if (dir) {
		struct dirent *entry;

		do {
			entry = readdir (dir);
			if (entry) {
				if (!strncmp (entry->d_name, "irqbalance", 10)) {
					ret = sscanf (entry->d_name, "irqbalance%d.sock", &irqbalance_pid);
					if (!ret)
						irqbalance_pid = -1;
				}
			}
		}
		while ((entry) && (irqbalance_pid == -1));
	
		closedir (dir);
	}

	if (irqbalance_pid == -1) {
		lpmd_log_info ("\tirqbalance not running, run in native mode\n");
		return LPMD_SUCCESS;
	}

	snprintf (irq_socket_name, 64, "%s/%s%d.sock", SOCKET_TMPFS, SOCKET_PATH, irqbalance_pid);
	socket_fd = socket_init_connection (irq_socket_name);
	if (socket_fd < 0) {
		lpmd_log_error ("Can not connect to irqbalance socket /run/irqbalance/irqbalance%d.sock\n",
						irqbalance_pid);
		return LPMD_ERROR;
	}
	close (socket_fd);
	lpmd_log_info ("\tFind irqbalance socket %s\n", irq_socket_name);
	return LPMD_SUCCESS;
}
