/*
 * lpmd_helper.c: helper functions
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
#include <time.h>

#include "lpmd.h"

int copy_user_string(char *src, char *dst, int size)
{
	int offset_src, offset_dst;

	for (offset_src = 0, offset_dst = 0; src[offset_src] != '\0' && offset_src < size; offset_src++) {
		/* Ignore heading spaces */
		if (src[offset_src] == ' ' && !offset_dst)
			continue;
		dst[offset_dst] = src[offset_src];
		offset_dst++;
	}
	dst[offset_dst] = '\0';

	/* Remove tailing spaces */
	while (dst[--offset_dst] == ' ')
		dst[offset_dst] = '\0';

	return 0;
}

static int _write_str(const char *name, char *str, int print_level, int log_level, const char *mode)
{
	FILE *filep;
	char prefix[16];
	int i, ret;

	if (print_level >= 15)
		return 1;

	if (print_level <= 0) {
		prefix[0] = '\0';
	}
	else {
		for (i = 0; i < print_level; i++)
			prefix[i] = '\t';
		prefix[i] = '\0';
	}

	filep = fopen (name, mode);
	if (!filep) {
		lpmd_log_error ("%sOpen %s failed\n", prefix, name);
		return 1;
	}

	ret = fprintf (filep, "%s", str);
	if (ret <= 0) {
		lpmd_log_error ("%sWrite \"%s\" to %s failed, strlen %zu, ret %d\n", prefix, str, name,
						strlen (str), ret);
		fclose (filep);
		return 1;
	}

	switch (print_level) {
		case LPMD_LOG_INFO:
			lpmd_log_info ("%sWrite \"%s\" to %s\n", prefix, str, name);
			break;
		case LPMD_LOG_DEBUG:
			lpmd_log_debug ("%sWrite \"%s\" to %s\n", prefix, str, name);
			break;
		case LPMD_LOG_MSG:
			lpmd_log_msg ("%sWrite \"%s\" to %s\n", prefix, str, name);
			break;
		default:
			break;
	}

	fclose (filep);
	return 0;
}

int lpmd_write_str(const char *name, char *str, int print_level)
{
	if (!name || !str)
		return 0;

	return _write_str (name, str, print_level, 2, "r+");
}

int lpmd_write_str_append(const char *name, char *str, int print_level)
{
	if (!name || !str)
		return 0;

	return _write_str (name, str, print_level, 2, "a+");
}

int lpmd_write_str_verbose(const char *name, char *str, int print_level)
{
	if (!name || !str)
		return 0;

	return _write_str (name, str, print_level, 3, "r+");
}

int lpmd_write_int(const char *name, int val, int print_level)
{
	FILE *filep;
	char prefix[16];
	int i, ret;
	struct timespec tp1 = { }, tp2 = { };

	if (!name)
		return 1;

	clock_gettime (CLOCK_MONOTONIC, &tp1);

	if (print_level >= 15)
		return 1;

	if (print_level < 0) {
		prefix[0] = '\0';
	}
	else {
		for (i = 0; i < print_level; i++)
			prefix[i] = '\t';
		prefix[i] = '\0';
	}

	filep = fopen (name, "r+");
	if (!filep) {
		lpmd_log_error ("%sOpen %s failed\n", prefix, name);
		return 1;
	}

	ret = fprintf (filep, "%d", val);
	if (ret <= 0) {
		lpmd_log_error ("%sWrite \"%d\" to %s failed, ret %d\n", prefix, val, name, ret);
		fclose (filep);
		return 1;
	}

	clock_gettime (CLOCK_MONOTONIC, &tp2);

	switch (print_level) {
		case LPMD_LOG_INFO:
			lpmd_log_info ("%sWrite \"%d\" to %s (%lu ns)\n", prefix, val, name,
							1000000000 * (tp2.tv_sec - tp1.tv_sec) + tp2.tv_nsec - tp1.tv_nsec);
			break;
		case LPMD_LOG_DEBUG:
			lpmd_log_debug ("%sWrite \"%d\" to %s (%lu ns)\n", prefix, val, name,
							1000000000 * (tp2.tv_sec - tp1.tv_sec) + tp2.tv_nsec - tp1.tv_nsec);
			break;
		case LPMD_LOG_MSG:
			lpmd_log_msg ("%sWrite \"%d\" to %s (%lu ns)\n", prefix, val, name,
							1000000000 * (tp2.tv_sec - tp1.tv_sec) + tp2.tv_nsec - tp1.tv_nsec);
			break;
		default:
			break;
	}

	fclose (filep);
	return 0;

}

int lpmd_read_int(const char *name, int *val, int print_level)
{
	FILE *filep;
	char prefix[16];
	int i, t, ret;

	if (!name || !val)
		return 1;

	if (print_level >= 15)
		return 1;

	if (print_level < 0) {
		prefix[0] = '\0';
	}
	else {
		for (i = 0; i < print_level; i++)
			prefix[i] = '\t';
		prefix[i] = '\0';
	}

	filep = fopen (name, "r");
	if (!filep) {
		lpmd_log_error ("%sOpen %s failed\n", prefix, name);
		return 1;
	}

	ret = fscanf (filep, "%d", &t);
	if (ret != 1) {
		lpmd_log_error ("%sRead %s failed, ret %d\n", prefix, name, ret);
		fclose (filep);
		return 1;
	}

	fclose (filep);

	*val = t;

	if (print_level >= 0)
		lpmd_log_debug ("%sRead \"%d\" from %s\n", prefix, *val, name);

	return 0;

}

/*
 * lpmd_open does not require print on success
 * print_level: -1: don't print on error
 */
int lpmd_open(const char *name, int print_level)
{
	FILE *filep;
	char prefix[16];
	int i;

	if (!name)
		return 1;

	if (print_level >= 15)
		return 1;

	if (print_level < 0) {
		prefix[0] = '\0';
	}
	else {
		for (i = 0; i < print_level; i++)
			prefix[i] = '\t';
		prefix[i] = '\0';
	}

	filep = fopen (name, "r");
	if (!filep) {
		if (print_level >= 0)
			lpmd_log_error ("%sOpen %s failed\n", prefix, name);
		return 1;
	}

	fclose (filep);
	return 0;
}

char* get_time(void)
{
	static time_t time_cur;

	time_cur = time (NULL);
	return ctime (&time_cur);
}

static struct timespec timespec;
static char time_buf[MAX_STR_LENGTH];
void time_start(void)
{
	clock_gettime (CLOCK_MONOTONIC, &timespec);
}

char* time_delta(void)
{
	static struct timespec tp1;
	clock_gettime (CLOCK_MONOTONIC, &tp1);
	snprintf (time_buf, MAX_STR_LENGTH, "%ld ns",
				1000000000 * (tp1.tv_sec - timespec.tv_sec) + tp1.tv_nsec - timespec.tv_nsec);
	memset (&timespec, 0, sizeof(timespec));
	return time_buf;
}

uint64_t read_msr(int cpu, uint32_t msr)
{
	char msr_file_name[64];
	int fd;
	uint64_t value;

	snprintf(msr_file_name, sizeof(msr_file_name), "/dev/cpu/%d/msr", cpu);

	fd = open(msr_file_name, O_RDONLY);
	if (fd < 0)
		return UINT64_MAX;

	if (pread(fd, &value, sizeof(value), msr) != sizeof(value)) {
		close(fd);
		return UINT64_MAX;
	}

	close(fd);

	return value;
}
