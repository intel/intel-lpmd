/*
 * lpmd_cpu.c: CPU related processing
 *
 * Copyright (C) 2025 Intel Corporation. All rights reserved.
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
#include <err.h>
#include <netlink/genl/ctrl.h>

#include "lpmd.h"

static int uevent_fd = -1;

static int has_cpu_uevent(void)
{
	ssize_t i = 0;
	ssize_t len;
	const char *dev_path = "DEVPATH=";
	unsigned int dev_path_len = strlen(dev_path);
	const char *cpu_path = "/devices/system/cpu/cpu";
	char buffer[MAX_STR_LENGTH];

	len = recv (uevent_fd, buffer, sizeof(buffer) - 1, MSG_DONTWAIT);
	if (len <= 0)
		return 0;
	buffer[len] = '\0';

	lpmd_log_debug ("Receive uevent: %s\n", buffer);

	while (i < len) {
		if (strlen (buffer + i) > dev_path_len
				&& !strncmp (buffer + i, dev_path, dev_path_len)) {
			if (!strncmp (buffer + i + dev_path_len, cpu_path,
					strlen (cpu_path))) {
				lpmd_log_debug ("\tMatches: %s\n", buffer + i + dev_path_len);
				return 1;
			}
		}
		i += strlen (buffer + i) + 1;
	}

	return 0;
}

#define PATH_PROC_STAT "/proc/stat"

int check_cpu_hotplug(void)
{
	FILE *filep;
	int curr;
	int ret;

	if (!has_cpu_uevent ())
		return 0;

	filep = fopen (PATH_PROC_STAT, "r");
	if (!filep)
		return 0;

	curr = cpumask_alloc();
	if (curr == CPUMASK_NONE)
		err(3, "ALLOC_CPUMASK");
		
	while (!feof (filep)) {
		char *tmpline = NULL;
		size_t size = 0;
		char *line;
		int cpu;
		char *p;
		int ret;

		tmpline = NULL;
		size = 0;

		if (getline (&tmpline, &size, filep) <= 0) {
			free (tmpline);
			break;
		}

		line = strdup (tmpline);

		p = strtok (line, " ");

		ret = sscanf (p, "cpu%d", &cpu);
		if (ret != 1)
			goto free;

		cpumask_add_cpu(cpu, curr);

free:
		free (tmpline);
		free (line);
	}

	fclose (filep);

	ret = cpumask_equal(curr, CPUMASK_ONLINE);
	cpumask_free(curr);

	if (ret)
		return update_lpmd_state(LPMD_RESTORE);

	return update_lpmd_state(LPMD_FREEZE);
}

int uevent_init(void)
{
	struct sockaddr_nl nls;

	memset (&nls, 0, sizeof(struct sockaddr_nl));

	nls.nl_family = AF_NETLINK;
	nls.nl_pid = getpid();
	nls.nl_groups = -1;

	uevent_fd = socket (PF_NETLINK, SOCK_DGRAM, NETLINK_KOBJECT_UEVENT);
	if (uevent_fd < 0)
		return uevent_fd;

	if (bind (uevent_fd, (struct sockaddr*) &nls, sizeof(struct sockaddr_nl))) {
		lpmd_log_warn ("kob_uevent bind failed\n");
		close (uevent_fd);
		return -1;
	}

	lpmd_log_debug ("Uevent binded\n");
	return uevent_fd;
}
