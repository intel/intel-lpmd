/*
 * Copyright (c) 2024, Intel Corporation.
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms and conditions of the GNU General Public License,
 * version 2, as published by the Free Software Foundation.
 *
 * This program is distributed in the hope it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
 * more details.
 *
 * Author: Noor ul Mubeen <noor.u.mubeen@intel.com>
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <err.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <getopt.h>
#include <signal.h>
#include <pthread.h>

#include "common.h"

static char output_file[MAX_STR_LENGTH];

enum log_level {
	LOG_ERR,
	LOG_INFO,
	LOG_DEBUG,
	LOG_VERBOSE,
};

static int loglevel;

void eco_printf(int level, const char *format, ...)
{
	va_list args;

	if (level > loglevel)
		return;

	va_start(args, format);

	if (output_file[0]) {
		char buffer[MAX_STR_LENGTH];

		vsprintf(buffer, format, args);
		if (fs_write_str_append(output_file, buffer))
			exit(-1);
	}

	if (level == LOG_ERR)
		vfprintf(stderr, format, args);
	else if (!output_file[0])
		vprintf(format, args);

	va_end(args);
}