// SPDX-License-Identifier: GPL-2.0-or-later
/* Copyright (C) 2026 Intel Corporation */

#include "lpmd.h" //wlt_type
#include "state_common.h"

/* wlt_proxy polling interval - updated at every state change */
int next_proxy_poll = 1000;

/* wlt_proxy hint - updated at every state change */
int wlt_type = WLT_IDLE;

/* called at the configured interval to take action; return next interval and workload type*/
int read_wlt_proxy(int *interval)
{
	state_machine_auto();
	*interval = next_proxy_poll;

	return wlt_type;
}

/* Returns success if proxy supported on platform */
int wlt_proxy_init(void)
{
	return util_init_proxy();
}

/* make sure all resource are properly released and closed */
void wlt_proxy_uninit(void)
{
	util_uninit_proxy();
}
