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
#include <err.h>

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

/*
 * Use one Ecore Module as LPM CPUs.
 * Applies on Hybrid platforms like AlderLake/RaptorLake.
 */
int is_cpu_atom(int cpu)
{
	unsigned int eax, ebx, ecx, edx;
	unsigned int type;

	if (cpu_migrate(cpu) < 0) {
		lpmd_log_error("Failed to migrated to cpu%d\n", cpu);
		return -1;
	}

	cpuid(0x1a, eax, ebx, ecx, edx);

	type = (eax >> 24) & 0xFF;

	cpu_clear_affinity();
	return type == 0x20;
}

static int is_cpu_in_l3(int cpu)
{
	unsigned int eax, ebx, ecx, edx, subleaf;

	if (cpu_migrate(cpu) < 0) {
		lpmd_log_error("Failed to migrated to cpu%d\n", cpu);
		err (1, "cpu migrate");
	}

	for(subleaf = 0;; subleaf++) {
		unsigned int type, level;

		cpuid_count(4, subleaf, eax, ebx, ecx, edx);

		type = eax & 0x1f;
		level = (eax >> 5) & 0x7;

		/* No more caches */
		if (!type)
			break;
		/* Unified Cache */
		if (type !=3 )
			continue;
		/* L3 */
		if (level != 3)
			continue;

		cpu_clear_affinity();
		return 1;
	}

	cpu_clear_affinity();
	return 0;
}

int is_cpu_pcore(int cpu)
{
	return !is_cpu_atom(cpu);
}

int is_cpu_ecore(int cpu)
{
	if (!is_cpu_atom(cpu))
		return 0;
	return is_cpu_in_l3(cpu);
}

int is_cpu_lcore(int cpu)
{
	if (!is_cpu_atom(cpu))
		return 0;
	return !is_cpu_in_l3(cpu);
}

#define PATH_RAPL	"/sys/class/powercap"
static int get_tdp(void)
{
	FILE *filep;
	DIR *dir;
	struct dirent *entry;
	int ret;
	char path[MAX_STR_LENGTH * 2];
	char str[MAX_STR_LENGTH];
	char *pos;
	int tdp = 0;

	if ((dir = opendir (PATH_RAPL)) == NULL) {
		perror ("opendir() error");
		return 1;
	}

	while ((entry = readdir (dir)) != NULL) {
		if (strlen (entry->d_name) > 100)
			continue;

		if (strncmp(entry->d_name, "intel-rapl", strlen("intel-rapl")))
			continue;

		snprintf (path, MAX_STR_LENGTH * 2, "%s/%s/name", PATH_RAPL, entry->d_name);
		filep = fopen (path, "r");
		if (!filep)
			continue;

		ret = fread (str, 1, MAX_STR_LENGTH, filep);
		fclose (filep);

		if (ret <= 0)
			continue;

		if (strncmp(str, "package", strlen("package")))
			continue;

		snprintf (path, MAX_STR_LENGTH * 2, "%s/%s/constraint_0_max_power_uw", PATH_RAPL, entry->d_name);
		filep = fopen (path, "r");
		if (!filep)
			continue;

		ret = fread (str, 1, MAX_STR_LENGTH, filep);
		fclose (filep);

		if (ret <= 0)
			continue;

		if (ret >= MAX_STR_LENGTH)
			ret = MAX_STR_LENGTH - 1;

		str[ret] = '\0';
		tdp = strtol(str, &pos, 10);
		break;
	}
	closedir (dir);

	return tdp / 1000000;
}


#define BITMASK_SIZE 32
int detect_max_cpus(void)
{
	FILE *filep;
	unsigned long dummy;
	int max_cpus;
	int i;

	max_cpus = 0;
	for (i = 0; i < 256; ++i) {
		char path[MAX_STR_LENGTH];

		snprintf (path, sizeof(path), "/sys/devices/system/cpu/cpu%d/topology/thread_siblings", i);

		filep = fopen (path, "r");
		if (filep)
			break;
	}

	if (!filep) {
		lpmd_log_error ("Can't get max cpu number\n");
		return -1;
	}

	while (fscanf (filep, "%lx,", &dummy) == 1)
		max_cpus += BITMASK_SIZE;
	fclose (filep);

	lpmd_log_debug ("\t%d CPUs supported in maximum\n", max_cpus);
	
	set_max_cpus(max_cpus);
	return 0;
}

int detect_cpu_topo(lpmd_config_t *lpmd_config)
{
	FILE *filep;
	int i;
	char path[MAX_STR_LENGTH];
	int ret;
	int pcores, ecores, lcores;
	int tdp;

	ret = detect_max_cpus();
	if (ret)
		return ret;

	cpumask_reset(CPUMASK_ONLINE);
	pcores = ecores = lcores = 0;

	for (i = 0; i < get_max_cpus(); i++) {
		unsigned int online = 0;

		snprintf (path, sizeof(path), "/sys/devices/system/cpu/cpu%d/online", i);
		filep = fopen (path, "r");
		if (filep) {
			if (fscanf (filep, "%u", &online) != 1)
				lpmd_log_warn ("fread failed for %s\n", path);
			fclose (filep);
		}
		else if (!i)
			online = 1;
		else
			break;

		if (!online)
			continue;

		cpumask_add_cpu(i, CPUMASK_ONLINE);
		if (is_cpu_pcore(i))
			pcores++;
		else if (is_cpu_ecore(i))
			ecores++;
		else if (is_cpu_lcore(i))
			lcores++;
	}
	set_max_online_cpu(i);

	tdp = get_tdp();
	lpmd_log_info("Detected %d Pcores, %d Ecores, %d Lcores, TDP %dW\n", pcores, ecores, lcores, tdp);
	ret = snprintf(lpmd_config->cpu_config, MAX_CONFIG_LEN - 1, " %dP%dE%dL-%dW ", pcores, ecores, lcores, tdp);

	lpmd_config->tdp = tdp;

	return 0;
}

static int detect_lpm_cpus_cmd(char *cmd)
{
	int ret;

	ret = cpumask_init_cpus (cmd, CPUMASK_LPM_DEFAULT);
	if (ret <= 0)
		cpumask_reset(CPUMASK_LPM_DEFAULT);

	return ret;
}

static int detect_lpm_cpus_cluster(void)
{
	FILE *filep;
	char path[MAX_STR_LENGTH];
	char str[MAX_STR_LENGTH];
	int i, ret;

	for (i = get_max_cpus(); i >= 0; i--) {
		if (!is_cpu_online (i))
			continue;

		snprintf (path, sizeof(path), "/sys/devices/system/cpu/cpu%d/topology/cluster_cpus_list",
					i);
		path[MAX_STR_LENGTH - 1] = '\0';

		filep = fopen (path, "r");
		if (!filep)
			continue;

		ret = fread (str, 1, MAX_STR_LENGTH - 1, filep);
		fclose (filep);

		if (ret <= 0)
			continue;

		str[ret] = '\0';

		if (cpumask_init_cpus (str, CPUMASK_LPM_DEFAULT) <= 0)
			continue;

		/* An Ecore module contains 4 Atom cores */
		if (cpumask_nr_cpus(CPUMASK_LPM_DEFAULT) == 4 && is_cpu_atom(i))
			break;

		cpumask_reset(CPUMASK_LPM_DEFAULT);
	}

	if (!cpumask_has_cpu(CPUMASK_LPM_DEFAULT))
		return 0;

	return cpumask_nr_cpus(CPUMASK_LPM_DEFAULT);
}

static int detect_cpu_lcore(int cpu)
{
	if (is_cpu_lcore(cpu))
		cpumask_add_cpu(cpu, CPUMASK_LPM_DEFAULT);
	return 0;
}

/*
 * Use Lcore CPUs as LPM CPUs.
 * Applies on platforms like MeteorLake.
 */
static int detect_lpm_cpus_lcore(void)
{
	int i;

	for (i = 0; i < get_max_cpus(); i++) {
		if (!is_cpu_online (i))
			continue;
		if (detect_cpu_lcore(i) < 0)
			return -1;
	}

	/* All cpus has L3 */
	if (!cpumask_has_cpu(CPUMASK_LPM_DEFAULT))
		return 0;

	/* All online cpus don't have L3 */
	if (cpumask_equal(CPUMASK_LPM_DEFAULT, CPUMASK_ONLINE))
		goto err;

	return cpumask_nr_cpus(CPUMASK_LPM_DEFAULT);

err:
	cpumask_reset(CPUMASK_LPM_DEFAULT);
	return 0;
}

int detect_lpm_cpus(char *cmd_cpus)
{
	int ret;
	char *str;

	if (cmd_cpus && cmd_cpus[0] != '\0') {
		ret = detect_lpm_cpus_cmd (cmd_cpus);
		if (ret <= 0) {
			lpmd_log_error ("\tInvalid -c parameter: %s\n", cmd_cpus);
			exit (-1);
		}
		str = "CommandLine";
		goto end;
	}

	ret = detect_lpm_cpus_lcore ();
	if (ret < 0)
		return ret;

	if (ret > 0) {
		str = "Lcores";
		goto end;
	}

	if (detect_lpm_cpus_cluster ()) {
		str = "Ecores";
		goto end;
	}

end: if (cpumask_has_cpu(CPUMASK_LPM_DEFAULT))
		lpmd_log_info ("\tUse CPU %s as Default Low Power CPUs (%s)\n",
						get_cpus_str (CPUMASK_LPM_DEFAULT), str);

	return 0;
}
