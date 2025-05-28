/*
 * lpmd_wlt.c: intel_lpmd WLT (WorkLoadType) monitor
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

#include "lpmd.h"


// Workload type classification
#define WORKLOAD_NOTIFICATION_DELAY_ATTRIBUTE "/sys/bus/pci/devices/0000:00:04.0/workload_hint/notification_delay_ms"
#define WORKLOAD_ENABLE_ATTRIBUTE "/sys/bus/pci/devices/0000:00:04.0/workload_hint/workload_hint_enable"
#define WORKLOAD_TYPE_INDEX_ATTRIBUTE  "/sys/bus/pci/devices/0000:00:04.0/workload_hint/workload_type_index"

#define NOTIFICATION_DELAY	100

// Read current Workload type
int wlt_update(int fd)
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

// Clear workload type notifications
int wlt_exit(void)
{
	int fd;

	/* Disable feature via sysfs knob */
	fd = open(WORKLOAD_ENABLE_ATTRIBUTE, O_RDWR);
	if (fd < 0)
		return 0;

	// Disable WLT notification
	if (write(fd, "0\n", 2) < 0) {
		close (fd);
		return 0;
	}

	close(fd);

	return 0;
}

// Initialize Workload type notifications
int wlt_init (void)
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
		wlt_exit();
		return fd;
	}

	lpmd_log_debug ("init_wlt end wlt fd:%d\n", fd);

	return fd;
}
