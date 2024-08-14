/*
 * intel_lpmd_irq.h: Intel Low Power Daemon IRQ header file
 *
 * Copyright (C) 2024 Intel Corporation. All rights reserved.
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
#ifndef LPMD_INTEL_LPMD_IRQ_H
#define LPMD_INTEL_LPMD_IRQ_H

#include <stdio.h>

/* irq.c */
int init_irq(void);
int process_irqs(int enter, enum lpm_cpu_process_mode mode);
int update_lpm_irq(cpu_set_t *cpumask, int action);
int native_restore_irqs();
int native_update_irqs();

#endif
