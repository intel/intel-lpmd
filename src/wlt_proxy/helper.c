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
 * some functions are from intel_lpmd project
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <err.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <sys/types.h>
#include <dirent.h>
#include <errno.h>

#include <sys/file.h>
#include <sys/mount.h>
#include <sys/time.h>
#include <sys/un.h>

#include "wlt_proxy_common.h"

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

struct _fd_cache {
	int pkg0_rapl_fd;
	int cgroup_partition_fd;
	int cgroup_isolate_fd;
	int inject_cpumask;
	int inject_duration;
	int inject_idle;
};

static struct _fd_cache fd_cache;

static int _write_int_fd(int fd, int i)
{
	int ret;

	if (lseek(fd, 0, SEEK_SET))
		perror("seek1");
	ret = dprintf(fd, "%d", i);
	if (ret <= 0) {
		perror("write0");
		return -1;
	}
	return ret;
}

static int _write_str_fd(int fd, const char *str)
{
	int ret;

	if (lseek(fd, 0, SEEK_SET))
		perror("lseek: _write_str_fd");
	ret = write(fd, str, strlen(str));
	if (ret <= 0) {
		perror("write: _write_str_fd");
		return -1;
	}
	return ret;
}

static int _read_str_fd(int fd, char *str)
{
	int ret;

	if (lseek(fd, 0, SEEK_SET))
		perror("lseek: _read_str_fd");
	ret = read(fd, str, strlen(str));
	if (ret <= 0) {
		perror("read: _read_str_fd");
		return -1;
	}
	return ret;
}

int write_str_fd(int fd, const char *str)
{
	return _write_str_fd(fd, str);
}

int read_str_fd(int fd, char *str)
{
	return _read_str_fd(fd, str);
}


FILE *open_fs(const char *name, char *mode)
{
	FILE *filep;
	filep = fopen(name, mode);
	if (!filep) {
		return NULL;
	}
	return filep;
}
int close_fs(FILE * filep)
{
	return fclose(filep);
}

int write_str_fs(FILE * filep, const char *str)
{
	int ret;
	if (!filep)
		return -1;

	fseek(filep, 0, SEEK_SET);
	ret = fwrite((const void *)str, 1, strlen(str), filep);
	if (ret <= 0) {
		perror("fwrite: write_str_fs");
		fclose(filep);
		return ret;
	}
	fflush(filep);
	return 0;
}

static int _write_str(const char *name, char *str, const char *mode)
{
	FILE *filep;
	int ret;

	filep = fopen(name, mode);
	if (!filep) {
		perror("fopen: _write_str");
		return 1;
	}

	ret = fprintf(filep, "%s", str);
	if (ret <= 0) {
		perror("fprintf: _write_str");
		fclose(filep);
		return 1;
	}

	fclose(filep);
	return 0;
}

int fs_write_str(const char *name, char *str)
{
	return _write_str(name, str, "r+");
}

int fs_write_str_append(const char *name, char *str)
{
	return _write_str(name, str, "a+");
}

int fs_write_int(const char *name, int val)
{
	FILE *filep;
	int ret;

	filep = fopen(name, "r+");
	if (!filep) {
		perror("fopen: fs_write_int");
		return 1;
	}

	fseek(filep, 0, SEEK_SET);
	ret = fprintf(filep, "%d", val);
	if (ret <= 0) {
		perror("fwrite: fs_write_int");
		fclose(filep);
		return 1;
	}
	fflush(filep);

	fclose(filep);
	return 0;

}

int fs_read_str(const char *name, char *val)
{
	FILE *filep;
    int ret;

	if (!val)
		return -1;

	filep = fopen(name, "r");
	if (!filep) {
		log_err("fs_read_str: Open %s failed\n", name);
		return -1;
	}
	fseek(filep, 0, SEEK_SET);
	ret = fread(val, 256, 256, filep);
	/* XXX check feof and ret */
    if (ret != 1) {
		log_err("fs_read_str: Read %s failed, ret %d\n", name, ret);		
	}
	fclose(filep);

	return 0;
}

int fs_read_int(const char *name, int *val)
{
	FILE *filep;
	int t, ret;

	filep = fopen(name, "r");
	if (!filep) {
		log_err("fs_read_int: Open %s failed\n", name);
		return 1;
	}

	ret = fscanf(filep, "%d", &t);
	if (ret != 1) {
		log_err("fs_read_int: Read %s failed, ret %d\n", name, ret);
		fclose(filep);
		return 1;
	}

	fclose(filep);

	*val = t;

	return 0;

}

#define PATH_CGROUP                    "/sys/fs/cgroup"
#define PATH_CG2_SUBTREE_CONTROL        PATH_CGROUP "/cgroup.subtree_control"
#define RAPL_PKG0_PWR "/sys/devices/virtual/powercap/intel-rapl/intel-rapl:0/energy_uj"

int init_rapl_fd(void)
{
	int fd;
	fd = open(RAPL_PKG0_PWR, O_RDONLY);
	if (fd == -1) {
		log_debug("init_rapl_fd: Open %s failed\n", RAPL_PKG0_PWR);
		return -1;
	}
	fd_cache.pkg0_rapl_fd = fd;
	return fd;
}

long long read_rapl_pkg0(void)
{
	char buf[64];
	size_t sz;
	if (lseek(fd_cache.pkg0_rapl_fd, 0, SEEK_SET))
		perror("lseek: read_rapl_pkg0");
	sz = read(fd_cache.pkg0_rapl_fd, buf, 64);
	if (sz == -1)
		perror("read: read_rapl_pkg0");
	return atoll(buf);
}

int open_fd(const char *name, int flags)
{
	int fd;

	fd = open(name, flags);
	if (fd == -1) {
		log_debug("Open %s failed\n", name);
		return -1;
	}
	return fd;
}

int close_fd(int fd)
{
	if (fd < 0) {
		log_debug("invalid fd:%d\n", fd);
		return -1;
	}
	close(fd);
	return 0;
}

int fs_open_check(const char *name)
{
	FILE *filep;

	filep = fopen(name, "r");
	if (!filep) {
		log_debug("Open %s failed\n", name);
		return 1;
	}

	fclose(filep);
	return 0;
}

#define PATH_CPUMASK "/sys/module/intel_powerclamp/parameters/cpumask"
#define PATH_MAXIDLE "/sys/module/intel_powerclamp/parameters/max_idle"
#define PATH_DURATION "/sys/module/intel_powerclamp/parameters/duration"

int init_inject_fd(void)
{
	int fd;

	fd = open_fd(PATH_CPUMASK, O_RDWR);
	if (fd > 0)
		fd_cache.inject_cpumask = fd;
	fd = open_fd(PATH_MAXIDLE, O_RDWR);
	if (fd > 0)
		fd_cache.inject_idle = fd;
	fd = open_fd(PATH_DURATION, O_RDWR);
	if (fd > 0)
		fd_cache.inject_duration = fd;
	return 1;
}

int write_inject_duration(int d)
{
	return _write_int_fd(fd_cache.inject_duration, d);
}

int write_inject_idle(int i)
{
	return _write_int_fd(fd_cache.inject_idle, i);
}

int write_inject_cpumask(const char *str)
{
	return _write_str_fd(fd_cache.inject_cpumask, str);
}

int write_cgroup_partition(const char *str)
{
	return _write_str_fd(fd_cache.cgroup_partition_fd, str);
}

int write_cgroup_isolate(const char *str)
{
	return _write_str_fd(fd_cache.cgroup_isolate_fd, str);
}

int init_cgroup_fd(void)
{
	int fd;
	DIR *dir;
	int ret;

	dir = opendir("/sys/fs/cgroup/eco");
	if (!dir) {
		ret = mkdir("/sys/fs/cgroup/eco", 0744);
		if (ret) {
			printf("Can't create dir:%s errno:%d\n",
			       "/sys/fs/cgroup/eco", errno);
			return ret;
		}
		log_debug("\tCreate %s\n", "/sys/fs/cgroup/eco");
	} else
		closedir(dir);

	fd = open_fd("/sys/fs/cgroup/eco/cpuset.cpus.partition", O_RDWR);
	if (fd > 0)
		fd_cache.cgroup_partition_fd = fd;
	fd = open_fd("/sys/fs/cgroup/eco/cpuset.cpus", O_RDWR);
	if (fd > 0)
		fd_cache.cgroup_isolate_fd = fd;
	return 1;
}

void init_all_fd(void)
{
	init_rapl_fd();
	init_cgroup_fd();
	init_inject_fd();
}

void close_all_fd(void)
{
	if (fd_cache.pkg0_rapl_fd > 0)
		close(fd_cache.pkg0_rapl_fd);
	if (fd_cache.cgroup_isolate_fd > 0)
		close(fd_cache.cgroup_isolate_fd);
	if (fd_cache.cgroup_partition_fd > 0)
		close(fd_cache.cgroup_partition_fd);
	if (fd_cache.inject_cpumask > 0)
		close(fd_cache.inject_cpumask);
	if (fd_cache.inject_idle > 0)
		close(fd_cache.inject_idle);
	if (fd_cache.inject_duration > 0)
		close(fd_cache.inject_duration);

}
