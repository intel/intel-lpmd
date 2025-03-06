/*
 * lpmd_hfi.c: intel_lpmd HFI monitor
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
 * This file processes HFI messages from the firmware. When the EE column for
 * a CPU is 255, that CPU will be in allowed list to run all thread.
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
#include <pthread.h>
#include <netlink/genl/genl.h>
#include <netlink/genl/family.h>
#include <netlink/genl/ctrl.h>

#include "thermal.h"
#include "lpmd.h"

struct hfi_event_data {
	struct nl_sock *nl_handle;
	struct nl_cb *nl_cb;
};

struct hfi_event_data drv;

static int ack_handler(struct nl_msg *msg, void *arg)
{
	int *err = arg;
	*err = 0;

	return NL_STOP;
}

static int finish_handler(struct nl_msg *msg, void *arg)
{
	int *ret = arg;
	*ret = 0;

	return NL_SKIP;
}

static int error_handler(struct sockaddr_nl *nla, struct nlmsgerr *err, void *arg)
{
	int *ret = arg;
	*ret = err->error;

	return NL_SKIP;
}

static int seq_check_handler(struct nl_msg *msg, void *arg)
{
	return NL_OK;
}

static int send_and_recv_msgs(struct hfi_event_data *drv, struct nl_msg *msg,
								int (*valid_handler)(struct nl_msg*, void*), void *valid_data)
{
	struct nl_cb *cb;
	int err = -ENOMEM;

	cb = nl_cb_clone (drv->nl_cb);
	if (!cb)
		goto out;

	err = nl_send_auto_complete (drv->nl_handle, msg);
	if (err < 0)
		goto out;

	err = 1;

	nl_cb_err (cb, NL_CB_CUSTOM, error_handler, &err);
	nl_cb_set (cb, NL_CB_FINISH, NL_CB_CUSTOM, finish_handler, &err);
	nl_cb_set (cb, NL_CB_ACK, NL_CB_CUSTOM, ack_handler, &err);

	if (valid_handler)
		nl_cb_set (cb, NL_CB_VALID, NL_CB_CUSTOM, valid_handler, valid_data);

	while (err > 0)
		err = nl_recvmsgs (drv->nl_handle, cb);
out: nl_cb_put (cb);
	nlmsg_free (msg);
	return err;
}

struct family_data {
	const char *group;
	int id;
};

static int family_handler(struct nl_msg *msg, void *arg)
{
	struct family_data *res = arg;
	struct nlattr *tb[CTRL_ATTR_MAX + 1];
	struct genlmsghdr *gnlh = nlmsg_data (nlmsg_hdr (msg));
	struct nlattr *mcgrp;
	int i;

	nla_parse (tb, CTRL_ATTR_MAX, genlmsg_attrdata (gnlh, 0), genlmsg_attrlen (gnlh, 0), NULL);
	if (!tb[CTRL_ATTR_MCAST_GROUPS])
		return NL_SKIP;

	nla_for_each_nested (mcgrp, tb[CTRL_ATTR_MCAST_GROUPS], i)
	{
		struct nlattr *tb2[CTRL_ATTR_MCAST_GRP_MAX + 1];
		nla_parse (tb2, CTRL_ATTR_MCAST_GRP_MAX, nla_data (mcgrp), nla_len (mcgrp), NULL);
		if (!tb2[CTRL_ATTR_MCAST_GRP_NAME] || !tb2[CTRL_ATTR_MCAST_GRP_ID]
				|| strncmp (nla_data (tb2[CTRL_ATTR_MCAST_GRP_NAME]), res->group,
							nla_len (tb2[CTRL_ATTR_MCAST_GRP_NAME])) != 0)
			continue;
		res->id = nla_get_u32 (tb2[CTRL_ATTR_MCAST_GRP_ID]);
		break;
	};

	return 0;
}

static int nl_get_multicast_id(struct hfi_event_data *drv, const char *family, const char *group)
{
	struct nl_msg *msg;
	int ret = -1;
	struct family_data res = { group, -ENOENT };

	msg = nlmsg_alloc ();
	if (!msg)
		return -ENOMEM;
	genlmsg_put (msg, 0, 0, genl_ctrl_resolve (drv->nl_handle, "nlctrl"), 0, 0, CTRL_CMD_GETFAMILY,
					0);
	NLA_PUT_STRING (msg, CTRL_ATTR_FAMILY_NAME, family);

	ret = send_and_recv_msgs (drv, msg, family_handler, &res);
	msg = NULL;
	if (ret == 0)
		ret = res.id;

nla_put_failure: nlmsg_free (msg);
	return ret;
}

/* Process HFI event */
struct perf_cap {
	int cpu;
	int perf;
	int eff;
};

static int suv_bit_set(void)
{
//	Depends on kernel patch to export kernel knobs for this
	return 0;
}

/*
 * Detect different kinds of CPU HFI hint
 * "LPM". EFF == 255
 * "SUV". PERF == EFF == 0, suv bit set.
 * "BAN". PERF == EFF == 0, suv bit not set.
 * "NOR".
 */
static char *update_one_cpu(struct perf_cap *perf_cap)
{
	if (perf_cap->cpu < 0)
		return NULL;

	if (!perf_cap->cpu) {
		reset_cpus (CPUMASK_HFI);
		reset_cpus (CPUMASK_HFI_BANNED);
	}

	if (perf_cap->eff == 255 * 4 && has_hfi_lpm_monitor ()) {
		add_cpu (perf_cap->cpu, CPUMASK_HFI);
		return "LPM";
	}
	if (!perf_cap->perf && !perf_cap->eff && has_hfi_suv_monitor () && suv_bit_set ()) {
		add_cpu (perf_cap->cpu, CPUMASK_HFI_SUV);
		return "SUV";
	}
	if (!perf_cap->perf && !perf_cap->eff) {
		add_cpu (perf_cap->cpu, CPUMASK_HFI_BANNED);
		return "BAN";
	}
	return "NOR";
}

static void process_one_event(int first, int last, int nr)
{
	/* Need to update more CPUs */
	if (nr == 16 && last != get_max_online_cpu ())
		return;

	if (has_cpus (CPUMASK_HFI)) {
		/* Ignore duplicate event */
		if (is_equal (CPUMASK_HFI_LAST, CPUMASK_HFI )) {
			lpmd_log_debug ("\tDuplicated HFI LPM hints ignored\n\n");
			return;
		}
		if (in_hfi_lpm ()) {
			lpmd_log_debug ("\tUpdate HFI LPM event\n\n");
		}
		else {
			lpmd_log_debug ("\tDetect HFI LPM event\n");
		}
		process_lpm (HFI_ENTER);
		reset_cpus (CPUMASK_HFI_LAST);
		copy_cpu_mask(CPUMASK_HFI, CPUMASK_HFI_LAST);
	}
	else if (has_cpus (CPUMASK_HFI_SUV)) {
		if (in_suv_lpm ()) {
			lpmd_log_debug ("\tUpdate HFI SUV event\n\n");
		}
		else {
			lpmd_log_debug ("\tDetect HFI SUV event\n");
		}
//		 TODO: SUV re-enter is not supported for now
		process_suv_mode (HFI_SUV_ENTER);
	}
	else if (has_cpus (CPUMASK_HFI_BANNED)) {
		copy_cpu_mask_exclude(CPUMASK_ONLINE, CPUMASK_HFI, CPUMASK_HFI_BANNED);
		/* Ignore duplicate event */
		if (is_equal (CPUMASK_HFI_LAST, CPUMASK_HFI )) {
			lpmd_log_debug ("\tDuplicated HFI BANNED hints ignored\n\n");
			return;
		}
		if (in_hfi_lpm ()) {
			lpmd_log_debug ("\tUpdate HFI LPM event with banned CPUs\n\n");
		}
		else {
			lpmd_log_debug ("\tDetect HFI LPM event with banned CPUs\n");
		}
		process_lpm (HFI_ENTER);
		reset_cpus (CPUMASK_HFI_LAST);
		copy_cpu_mask(CPUMASK_HFI, CPUMASK_HFI_LAST);
	}
	else if (in_hfi_lpm ()) {
		lpmd_log_debug ("\tHFI LPM recover\n");
//		 Don't override the DETECT_LPM_CPU_DEFAULT so it is auto recovered
		process_lpm (HFI_EXIT);
		reset_cpus (CPUMASK_HFI_LAST);
	}
	else if (in_suv_lpm ()) {
		lpmd_log_debug ("\tHFI SUV recover\n");
//		 Don't override the DETECT_LPM_CPU_DEFAULT so it is auto recovered
		process_suv_mode (HFI_SUV_EXIT);
	}
	else {
		lpmd_log_info ("\t\t\tUnsupported HFI event ignored\n");
	}
}

static int handle_event(struct nl_msg *n, void *arg)
{
	struct nlmsghdr *nlh = nlmsg_hdr (n);
	struct genlmsghdr *genlhdr = genlmsg_hdr (nlh);
	struct nlattr *attrs[THERMAL_GENL_ATTR_MAX + 1];
	struct nlattr *cap;
	struct perf_cap perf_cap;
	int first_cpu = -1, last_cpu = -1, nr_cpus = 0;
	int j, index = 0, offset = 0;
	char buf[MAX_STR_LENGTH];

	if (!in_auto_mode())
		return 0;

	if (genlhdr->cmd != THERMAL_GENL_EVENT_CAPACITY_CHANGE)
		return 0;

	if (genlmsg_parse (nlh, 0, attrs, THERMAL_GENL_ATTR_MAX, NULL))
		return -1;

	perf_cap.cpu = perf_cap.perf = perf_cap.eff = -1;

	nla_for_each_nested (cap, attrs[THERMAL_GENL_ATTR_CAPACITY], j)
	{

		switch (index) {
			case 0:
				offset += snprintf (buf + offset, MAX_STR_LENGTH - offset, "\tCPU %3d: ",
									nla_get_u32 (cap));
				perf_cap.cpu = nla_get_u32 (cap);
				break;
			case 1:
				offset += snprintf (buf + offset, MAX_STR_LENGTH - offset, " PERF [%4d] ",
									nla_get_u32 (cap));
				perf_cap.perf = nla_get_u32 (cap);
				break;
			case 2:
				offset += snprintf (buf + offset, MAX_STR_LENGTH - offset, " EFF [%4d] ",
									nla_get_u32 (cap));
				perf_cap.eff = nla_get_u32 (cap);
				break;
			default:
				break;
		}
		index++;

		if (index == 3) {
			char *str;

			str = update_one_cpu (&perf_cap);
			offset += snprintf (buf + offset, MAX_STR_LENGTH - offset, " TYPE [%s]", str);
			buf[MAX_STR_LENGTH - 1] = '\0';
			lpmd_log_debug ("\t\t\t%s\n", buf);

			index = 0;
			offset = 0;

			if (first_cpu == -1)
				first_cpu = perf_cap.cpu;
			last_cpu = perf_cap.cpu;
			nr_cpus++;
		}
	}
	process_one_event (first_cpu, last_cpu, nr_cpus);

	return 0;
}

static int done = 0;

int hfi_kill(void)
{
	nl_socket_free (drv.nl_handle);
	done = 1;
	return 0;
}

void hfi_receive(void)
{
	int err = 0;

	while (!err)
		err = nl_recvmsgs (drv.nl_handle, drv.nl_cb);
}

int hfi_init(void)
{
	struct nl_sock *sock;
	struct nl_cb *cb;
	int mcast_id;

	reset_cpus (CPUMASK_HFI_LAST);

	signal (SIGPIPE, SIG_IGN);

	sock = nl_socket_alloc ();
	if (!sock) {
		lpmd_log_error ("nl_socket_alloc failed\n");
		goto err_proc;
	}

	if (genl_connect (sock)) {
		lpmd_log_error ("genl_connect(sk_event) failed\n");
		goto err_proc;
	}

	drv.nl_handle = sock;
	drv.nl_cb = cb = nl_cb_alloc (NL_CB_DEFAULT);
	if (drv.nl_cb == NULL) {
		lpmd_log_error ("Failed to allocate netlink callbacks");
		goto err_proc;
	}

	mcast_id = nl_get_multicast_id (&drv, THERMAL_GENL_FAMILY_NAME,
	THERMAL_GENL_EVENT_GROUP_NAME);
	if (mcast_id < 0) {
		lpmd_log_error ("nl_get_multicast_id failed\n");
		goto err_proc;
	}

	if (nl_socket_add_membership (sock, mcast_id)) {
		lpmd_log_error ("nl_socket_add_membership failed");
		goto err_proc;
	}

	nl_cb_set (cb, NL_CB_SEQ_CHECK, NL_CB_CUSTOM, seq_check_handler, &done);
	nl_cb_set (cb, NL_CB_VALID, NL_CB_CUSTOM, handle_event, NULL);

	nl_socket_set_nonblocking (sock);

	if (drv.nl_handle)
		return nl_socket_get_fd (drv.nl_handle);

err_proc: return -1;
}
