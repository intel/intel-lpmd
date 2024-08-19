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

#include "wlt_proxy_common.h"
#include "cpu_group.h"
#include "lpmd.h"

/* 
 * XXX: MAX_IRQS may need to be bumped up
 */
#define MAX_IRQS_PROXY      256
#define MAX_IRQ_STR_LENGTH  127
#define MASK_LEN            (32)

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

/* helper functions - todo: move to lpmd_helpers.c?*/
static FILE *open_fs(const char *name, char *mode)
{
    FILE *filep;
    filep = fopen(name, mode);
    if (!filep) {
        return NULL;
    }
    return filep;
}

static int close_fs(FILE * filep)
{
    return fclose(filep);
}

static int write_str_fs(FILE * filep, const char *str)
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

/* Interrupt Management */
int restore_irq_mask(void)
{
    int i;

    lpmd_log_debug("\tRestore IRQ affinity (native)\n");

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

static int save_irq_mask(void)
{
    int i;
    FILE *filep = NULL;
    size_t size = 0;
    char path[MAX_STR_LENGTH];
    char *str = NULL;

    lpmd_log_debug("\tsave IRQ affinity (native)\n");

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
            lpmd_log_error("Failed to get IRQ%d smp_affinity\n", i);
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
    lpmd_log_debug("Saving original IRQs ...\n");
    save_irq_mask();
    
    return 0;
}
