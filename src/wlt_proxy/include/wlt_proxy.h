/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Copyright (C) 2026 Intel Corporation */

#ifndef _WLT_PROXY_H_
#define _WLT_PROXY_H_

int read_wlt_proxy(int *interval);
int wlt_proxy_init(void);
void wlt_proxy_uninit(void);

#endif/* _WLT_PROXY_H_ */
