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
#include <sched.h>
#include <dirent.h>
#include <ctype.h>
#include <signal.h>
#include <netlink/genl/genl.h>
#include <netlink/genl/family.h>
#include <netlink/genl/ctrl.h>

#include "lpmd.h"

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


/* Interrupt Management */
#define SOCKET_PATH "irqbalance"
#define SOCKET_TMPFS "/run/irqbalance"

static int irqbalance_ban_cpus(char *irq_str)
{
	char socket_cmd[MAX_STR_LENGTH];
	int offset;

	lpmd_log_debug ("\tUpdate IRQ affinity (irqbalance)\n");
	offset = snprintf (socket_cmd, MAX_STR_LENGTH, "settings cpus %s", irq_str);
	if (offset >= MAX_STR_LENGTH)
		offset = MAX_STR_LENGTH - 1;

	socket_cmd[offset] = '\0';
	socket_send_cmd (irq_socket_name, socket_cmd);

	lpmd_log_debug ("\tSend socket command %s\n", socket_cmd);
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

static int update_one_irq(int irq, char *irq_str)
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

static int native_update_irqs(char *irq_str)
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

		update_one_irq (number, irq_str);
		free (line);
	}

	fclose (filep);

	irq_updated = 1;
	return 0;
}

int process_irq(lpmd_config_state_t *state)
{
	switch (state->irq_migrate) {
		case SETTING_IGNORE:
			lpmd_log_info ("Ignore IRQ migration\n");
			return 0;
		case SETTING_RESTORE:
			if (irqbalance_pid == -1)
				native_restore_irqs();
			else
				irqbalance_ban_cpus("NULL");
			return 0;
		default:
			if (state->cpumask_idx == CPUMASK_NONE)
				return 0;
			if (irqbalance_pid == -1)
				native_update_irqs(get_proc_irq_str(state->cpumask_idx));
			else
				irqbalance_ban_cpus(get_irqbalance_str(state->cpumask_idx));
			return 0;
	}	
	return 0;
}

int irq_init (void)
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
