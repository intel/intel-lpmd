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
 * Some functions are repurposed from intel_lpmd project
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

#include "common.h"
#include "cpu_group.h"

/* 
 * XXX: MAX_IRQS may need to be bumped up
 */
#define MAX_IRQS_PROXY	256

struct each_irq {
	int irq_num;
	FILE *filep;
	char affinity[MAX_CPUMASK_SIZE];
};

struct info_irqs_proxy {
	/* Cached IRQ smp_affinity info */
	int nr_irqs;
	struct each_irq irq[MAX_IRQS_PROXY];
};

struct info_irqs_proxy info_irqs_proxy;
/*defined in lpmd_irq*/
struct info_irqs_proxy *p_info = &info_irqs_proxy;

/* Interrupt Management */

int restore_irq_mask(void)
{
	int i;

	log_debug("\tRestore IRQ affinity (native)\n");

	for (i = 0; i < MAX_IRQS_PROXY - 1; i++) {

		char *str = p_info->irq[i].affinity;

		if (!p_info->irq[i].filep)
			continue;
		write_str_fs(p_info->irq[i].filep, str);
		close_fs(p_info->irq[i].filep);
	}
	memset(p_info, 0, sizeof(*p_info));
	return 0;
}

#define MAX_IRQ_STR_LENGTH 127

static int save_irq_mask(void)
{
	int i;
	FILE *filep = NULL;
	size_t size = 0;
	char path[MAX_STR_LENGTH];
	char *str = NULL;

	log_debug("\tsave IRQ affinity (native)\n");

	for (i = 0; i < MAX_IRQS_PROXY - 1; i++) {
		p_info->nr_irqs++;
		p_info->irq[i].filep = NULL;

		snprintf(path, MAX_STR_LENGTH, "/proc/irq/%i/smp_affinity", i);

		filep = open_fs(path, "r+");
		if (filep)
			p_info->irq[i].filep = filep;
		else
			continue;

		if (getline(&str, &size, filep) <= 0) {
			log_err("Failed to get IRQ%d smp_affinity\n", i);
			continue;
		}

		strncpy(p_info->irq[i].affinity, str, MAX_IRQ_STR_LENGTH - 1);

		/* Remove the Newline */
		size = strlen(p_info->irq[i].affinity);
		p_info->irq[i].affinity[size - 1] = '\0';
	}
	free(str);
	return 0;
}

#define MASK_LEN (32)
int update_irqs()
{
	int i;
	char mask[32];

	strncpy(mask, get_cpus_hexstr(get_cur_state()), MASK_LEN - 1);

	for (i = 0; i < MAX_IRQS_PROXY - 1; i++) {
		if (!p_info->irq[i].filep)
			continue;
		write_str_fs(p_info->irq[i].filep, mask);
	}
	return 0;
}

int init_irq_proxy()
{
	log_debug("Saving original IRQs ...\n");
	save_irq_mask();

	return 0;
}
