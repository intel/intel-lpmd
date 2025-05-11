/*
 * lpmd_cpu.c: CPU related processing
 *
 * Copyright (C) 2025 Intel Corporation. All rights reserved.
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
#include "lpmd.h"

/* Bit 15 of CPUID.7 EDX stands for Hybrid support */
#define CPUFEATURE_HYBRID	(1 << 15)
#define PATH_PM_PROFILE "/sys/firmware/acpi/pm_profile"

struct cpu_model_entry {
	unsigned int family;
	unsigned int model;
};

static struct cpu_model_entry id_table[] = {
		{ 6, 0x97 }, // Alderlake
		{ 6, 0x9a }, // Alderlake
		{ 6, 0xb7 }, // Raptorlake
		{ 6, 0xba }, // Raptorlake
		{ 6, 0xbf }, // Raptorlake S
		{ 6, 0xaa }, // Meteorlake
		{ 6, 0xac }, // Meteorlake
		{ 6, 0xbd }, // Lunarlake
		{ 6, 0xcc }, // Pantherlake
		{ 0, 0 } // Last Invalid entry
};

int detect_supported_platform(lpmd_config_t *lpmd_config)
{
	unsigned int eax, ebx, ecx, edx;
	unsigned int max_level, family, model, stepping;
	int val;

	cpuid(0, eax, ebx, ecx, edx);

	/* Unsupported vendor */
        if (ebx != 0x756e6547 || edx != 0x49656e69 || ecx != 0x6c65746e) {
		lpmd_log_info("Unsupported vendor\n");
		return -1;
	}

	max_level = eax;
	cpuid(1, eax, ebx, ecx, edx);
	family = (eax >> 8) & 0xf;
	model = (eax >> 4) & 0xf;
	stepping = eax & 0xf;

	if (family == 6)
		model += ((eax >> 16) & 0xf) << 4;

	lpmd_log_info("%u CPUID levels; family:model:stepping 0x%x:%x:%x (%u:%u:%u)\n",
			max_level, family, model, stepping, family, model, stepping);

	if (!do_platform_check()) {
		lpmd_log_info("Ignore platform check\n");
		goto end;
	}

	/* Need CPUID.1A to detect CPU core type */
        if (max_level < 0x1a) {
		lpmd_log_info("CPUID leaf 0x1a not supported, unable to detect CPU type\n");
		return -1;
        }

	cpuid_count(7, 0, eax, ebx, ecx, edx);

	/* Run on Hybrid platforms only */
	if (!(edx & CPUFEATURE_HYBRID)) {
		lpmd_log_info("Non-Hybrid platform detected\n");
		return -1;
	}

	/* /sys/firmware/acpi/pm_profile is mandatory */
	if (lpmd_read_int(PATH_PM_PROFILE, &val, -1)) {
		lpmd_log_info("Failed to read PM profile %s\n", PATH_PM_PROFILE);
		return -1;
	}

	if (val != 2) {
		lpmd_log_info("Non-Mobile PM profile detected. %s returns %d\n", PATH_PM_PROFILE, val);
		return -1;
	}

	/* Platform meets all the criteria for lpmd to run, check the allow list */
	val = 0;
	while (id_table[val].family) {
		if (id_table[val].family == family && id_table[val].model == model)
			break;
		val++;
        }

	/* Unsupported model */
	if (!id_table[val].family) {
		lpmd_log_info("Platform not supported yet.\n");
		lpmd_log_debug("Supported platforms:\n");
		val = 0;
		while (id_table[val].family) {
			lpmd_log_debug("\tfamily %d model %d\n", id_table[val].family, id_table[val].model);
			val++;
		}
		return -1;
	}

end:
	lpmd_config->cpu_family = family;
	lpmd_config->cpu_model = model;

	return 0;
}
