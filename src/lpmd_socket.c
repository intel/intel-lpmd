/*
 * lpmd_socket.c: Intel Low Power Daemon socket helpers
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
 *
 * This file is used to send messages to IRQ daemon via sockets.
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
#include <pthread.h>

#include <sys/un.h>
#include <sys/socket.h>
#include <malloc.h>
#include <syslog.h>
#include <signal.h>
#include <time.h>
#include <inttypes.h>

#include "lpmd.h"

/* socket helpers */
int socket_init_connection(char *name)
{
	struct sockaddr_un addr;
	static int socket_fd;

	if (!name)
		return 0;

	memset (&addr, 0, sizeof(struct sockaddr_un));
	socket_fd = socket (AF_LOCAL, SOCK_STREAM, 0);
	if (socket_fd < 0) {
		perror ("Error opening socket");
		return 0;
	}
	addr.sun_family = AF_UNIX;

	snprintf (addr.sun_path, sizeof(addr.sun_path), "%s", name);

	if (connect (socket_fd, (struct sockaddr*) &addr, sizeof(addr)) < 0) {
		/* Try connect to abstract */
		memset (&addr, 0, sizeof(struct sockaddr_un));
		addr.sun_family = AF_UNIX;
		if (connect (socket_fd, (struct sockaddr*) &addr, sizeof(addr)) < 0) {
			close (socket_fd);
			return 0;
		}
	}

	return socket_fd;
}

static struct msghdr* create_credentials_msg()
{
	struct ucred *credentials;
	struct msghdr *msg;
	struct cmsghdr *cmsg;

	credentials = malloc (sizeof(struct ucred));
	if (!credentials)
		return NULL;

	credentials->pid = getpid ();
	credentials->uid = geteuid ();
	credentials->gid = getegid ();

	msg = malloc (sizeof(struct msghdr));
	if (!msg) {
		free (credentials);
		return msg;
	}

	memset (msg, 0, sizeof(struct msghdr));
	msg->msg_iovlen = 1;
	msg->msg_control = malloc (CMSG_SPACE(sizeof(struct ucred)));
	if (!msg->msg_control) {
		free (credentials);
		free (msg);
		return NULL;
	}

	msg->msg_controllen = CMSG_SPACE(sizeof(struct ucred));

	cmsg = CMSG_FIRSTHDR(msg);
	cmsg->cmsg_level = SOL_SOCKET;
	cmsg->cmsg_type = SCM_CREDENTIALS;
	cmsg->cmsg_len = CMSG_LEN(sizeof(struct ucred));
	memcpy (CMSG_DATA(cmsg), credentials, sizeof(struct ucred));

	free (credentials);
	return msg;
}

int socket_send_cmd(char *name, char *data)
{
	int socket_fd;
	struct msghdr *msg;
	struct iovec iov;
	char buf[MAX_STR_LENGTH];
	int ret;

	if (!name || !data)
		return LPMD_ERROR;

	socket_fd = socket_init_connection (name);
	if (!socket_fd)
		return LPMD_ERROR;

	msg = create_credentials_msg ();
	if (!msg)
		return LPMD_ERROR;

	iov.iov_base = (void*) data;
	iov.iov_len = strlen (data);
	msg->msg_iov = &iov;

	if (sendmsg (socket_fd, msg, 0) < 0) {
		free (msg->msg_control);
		free (msg);
		return LPMD_ERROR;
	}

	ret = read (socket_fd, buf, MAX_STR_LENGTH);
	if (ret < 0)
		lpmd_log_debug ("read failed\n");

	close (socket_fd);
	free (msg->msg_control);
	free (msg);
	return LPMD_SUCCESS;
}

