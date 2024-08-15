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
 * Author: Noor ul Mubeen <noor.u.mubeen@intel.com>
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <err.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <cpuid.h>
#include <dirent.h>
#include <ctype.h>
#include <limits.h>
#include <time.h>

#include "wlt_proxy_common.h"
#include "cpu_group.h"
#include "lpmd.h"

#ifdef __USE_LPMD_IRQ__
#include "lpmd_irq.h"
#endif

#define BITMASK_SIZE 32
#define MAX_FREQ_MAPS (6)
/* Support for CGROUPV2 */
#define PATH_CGROUP                    "/sys/fs/cgroup"
#define PATH_CG2_SUBTREE_CONTROL	PATH_CGROUP "/cgroup.subtree_control"

#define PATH_CPUMASK "/sys/module/intel_powerclamp/parameters/cpumask"
#define PATH_MAXIDLE "/sys/module/intel_powerclamp/parameters/max_idle"
#define PATH_DURATION "/sys/module/intel_powerclamp/parameters/duration"
#define PATH_THERMAL "/sys/class/thermal"

#define ENABLE_FREQ_CLAMPING 1
#define ENABLE_IRQ_REBALANCE 1

extern int cpumask_to_hexstr(cpu_set_t *mask, char *str, int size);
extern int cpumask_to_str(cpu_set_t *mask, char *buf, int length);
extern int irq_rebalance;
extern int inject_update;
extern struct group_util grp;

static int topo_max_cpus;
/* starting 6.5 kernel, cpu0 is assumed to be always online */
static int max_online_cpu;
size_t size_cpumask;

struct _lp_state {
	bool disabled;
	cpu_set_t *mask;
	cpu_set_t *inj_mask;
	char *inj_hexstr;
	char *name;
	char *str;
	char *str_reverse;
	char *hexstr;
	char *hexstr_reverse;
	int poll;
	enum elastic_poll poll_order;
	int stay_count;
	int stay_count_last;
	int stay_count_update_sec;
	int stay_count_update_sec_prev;
	int spike_type;
	float stay_scalar;
	int epp;
	int epb;
	int ppw_enabled;
	int last_max_util;
	int last_poll;
	int freq_ctl; //enable freq clamping
};

static int common_min_freq;
static int freq_map_count;
static struct _freq_map freq_map[MAX_FREQ_MAPS];

/*
 * ppw is not tested. disabled for now.
 */
/*static struct _lp_state lp_state[MAX_MODE] = {
	[INIT_MODE] = {.name = "Avail cpu: P/E/L",.poll = BASE_POLL_MT,.epp =
		       PERFORMANCE_EPP,.epb = EPB_AC,.stay_scalar = 1,
		       .poll_order = ZEROTH,.freq_ctl = 1},
	[PERF_MODE] = {.name = "Perf:non-soc cpu",.poll = BASE_POLL_PERF,.epp =
		       PERFORMANCE_EPP,.epb = EPB_AC,.stay_scalar = 1,
		       .poll_order = ZEROTH,.freq_ctl = 1},
	[BYPS_MODE] = {.name = "bypass mode     ",.poll = BASE_POLL_PERF,.epp =
		       POWERSAVE_EPP,.epb = EPB_DC,.stay_scalar = 1,
		       .poll_order = ZEROTH},
	[MDRT2E_MODE] = {.name = "Moderate 2E     ",.poll =
			BASE_POLL_MDRT2E,.epp = POWERSAVE_EPP,.epb =
			EPB_DC,.stay_scalar = 1,
			.poll_order = LINEAR},
	[MDRT3E_MODE] = {.name = "Moderate 3E     ",.poll = BASE_POLL_MDRT3E,.epp =
		       POWERSAVE_EPP,.epb = EPB_DC,.stay_scalar = 1,
		       .poll_order = LINEAR},
	[MDRT4E_MODE] = {.name = "Moderate 4E     ",.poll =
			BASE_POLL_MDRT4E,.epp = POWERSAVE_EPP,.epb =
			EPB_DC,.stay_scalar = 1,
			.poll_order = LINEAR},
	[RESP_MODE] = {.name = "Responsive 2L   ",.poll = BASE_POLL_RESP,.epp =
		       PERFORMANCE_EPP,.epb = EPB_AC,.stay_scalar = 1,
		       .poll_order = CUBIC,.freq_ctl = 1},
	[NORM_MODE] = {.name = "Normal LP 2L    ",.poll = BASE_POLL_NORM,.epp =
		       POWERSAVE_EPP,.epb = EPB_DC,.stay_scalar = 1,
		       .poll_order = QUADRATIC},
	[DEEP_MODE] = {.name = "Deep LP 1L      ",.poll = BASE_POLL_DEEP,.epp =
		       POWERSAVE_EPP,.epb = EPB_DC,.stay_scalar = 1,
		       .poll_order = CUBIC},
};*/

static struct _lp_state lp_state[MAX_MODE] = {
	[INIT_MODE] = {.name =   "Avail cpu: P/E/L",.poll = BASE_POLL_MT,.poll_order = ZEROTH,.freq_ctl = 1},
	[PERF_MODE] = {.name =   "Perf:non-soc cpu",.poll = BASE_POLL_PERF,.poll_order = ZEROTH,.freq_ctl = 1},
	[BYPS_MODE] = {.name =   "bypass mode",.poll = BASE_POLL_PERF,.poll_order = ZEROTH},
	[MDRT2E_MODE] = {.name = "Moderate 2E",.poll =	BASE_POLL_MDRT2E,.poll_order = LINEAR},
	[MDRT3E_MODE] = {.name = "Moderate 3E",.poll = BASE_POLL_MDRT3E,.poll_order = LINEAR},
	[MDRT4E_MODE] = {.name = "Moderate 4E",.poll =	BASE_POLL_MDRT4E, .poll_order = LINEAR},
	[RESP_MODE] = {.name =   "Responsive 2L",.poll = BASE_POLL_RESP, .poll_order = CUBIC,.freq_ctl = 1},
	[NORM_MODE] = {.name =   "Normal LP 2L",.poll = BASE_POLL_NORM, .poll_order = QUADRATIC},
	[DEEP_MODE] = {.name =   "Deep LP 1L",.poll = BASE_POLL_DEEP, .poll_order = CUBIC},
};

static enum lp_state_idx cur_state = INIT_MODE;
static int needs_state_reset = 1;

int process_cpu_isolate_enter(void);
int process_cpu_isolate_exit(void);
int process_cpu_powerclamp_exit(void);
static int zero_isol_cpu(enum lp_state_idx);
static char *get_inj_hexstr(enum lp_state_idx idx);
static char path_powerclamp[MAX_STR_LENGTH * 2];
static int default_dur = -1;

int get_freq_map_count()
{
	return freq_map_count;
}

int get_freq_map(int j, struct _freq_map *fmap)
{
	*fmap = freq_map[j];
	return 1;
}

int get_state_reset(void)
{
	return needs_state_reset;
}

void set_state_reset(void)
{
	needs_state_reset = 1;
}

/* General helpers */
char *get_mode_name(enum lp_state_idx state)
{
	return lp_state[state].name;
}

int get_mode_cpu_count(enum lp_state_idx state)
{
	return CPU_COUNT_S(size_cpumask, lp_state[state].mask);
}

int get_mode_max(void)
{
	return MAX_MODE;
}

//util
bool is_state_disabled(enum lp_state_idx state)
{
	return lp_state[state].disabled;
}

//util
enum lp_state_idx get_cur_state(void)
{
	return cur_state;
}

void set_cur_state(enum lp_state_idx state)
{
	cur_state = state;
}

//util
int is_state_valid(enum lp_state_idx state)
{
	return ((state >= INIT_MODE) && (state < MAX_MODE)
		&& !lp_state[state].disabled);
}

int get_state_epp(enum lp_state_idx state)
{
	return lp_state[state].epp;
}

int get_state_epb(enum lp_state_idx state)
{
	return lp_state[state].epb;
}

//util
int state_support_freq_ctl(enum lp_state_idx state)
{
	return lp_state[state].freq_ctl;
}

int state_has_ppw(enum lp_state_idx state)
{
	return lp_state[state].ppw_enabled;
}

//util
int get_poll_ms(enum lp_state_idx state)
{
	return lp_state[state].poll;
}

int set_state_poll(int poll, enum lp_state_idx state)
{
	return (lp_state[state].poll = poll);
}

int set_state_poll_order(int poll_order, enum lp_state_idx state)
{
	return (lp_state[state].poll_order = poll_order);
}

int get_state_poll_order(enum lp_state_idx state)
{
	return (lp_state[state].poll_order);
}

int get_stay_count(enum lp_state_idx state)
{
	return (lp_state[state].stay_count);
}

int set_stay_count(enum lp_state_idx state, int count)
{
	return (lp_state[state].stay_count = count);
}

int do_countdown(enum lp_state_idx state)
{
	lp_state[state].stay_count -= 1;

	if (lp_state[state].stay_count <= 0) {
		lp_state[state].stay_count = 0;
		return 1;
	}

	lp_state[state].stay_count_last = lp_state[state].stay_count;

	return 0;
}

/* get poll value in microsec */
int get_state_poll(int util, enum lp_state_idx state)
{
	int poll, scale = (100 - util);
	float scale2;

	int order = (int)lp_state[state].poll_order;
	/* avoiding fpow() overhead */
	switch (order) {
	case ZEROTH:
		scale2 = (float)1;
		break;
	case LINEAR:
		scale2 = (float)scale / 100;
		break;
	case QUADRATIC:
		scale2 = (float)scale *scale / 10000;
		break;
	case CUBIC:
		scale2 = (float)scale *scale * scale / 1000000;
		break;
	default:
		scale2 = (float)scale / 100;
		break;
	}

	poll = (int)(lp_state[cur_state].poll * scale2);

	/* idle inject driver min duration is 6ms.
	 * limiting min poll to 16ms */
	if (poll < MIN_POLL_PERIOD)
		return MIN_POLL_PERIOD;
	return poll;
}

int get_last_maxutil(void)
{
	return lp_state[cur_state].last_max_util;
}

int set_last_maxutil(int v)
{
	lp_state[cur_state].last_max_util = v;
	return 1;
}

int set_last_poll(int v)
{
	lp_state[cur_state].last_poll = v;
	return 1;
}

int get_last_poll(void)
{
	return lp_state[cur_state].last_poll;
}

//util
int get_min_freq(int cpu)
{
	if (!is_cpu_online(cpu))
		return 0;

	return common_min_freq;
}

//util
int get_turbo_freq(int cpu)
{
	if (!is_cpu_online(cpu))
		return 0;

	for (int j = 0; j < MAX_FREQ_MAPS - 1; j++) {
		if ((cpu >= freq_map[j].start_cpu)
		    && (cpu <= freq_map[j].end_cpu))
			return freq_map[j].turbo_freq_khz;
	}
	return 0;
}

int check_reset_status(void)
{
	return needs_state_reset;
}

void exit_state_change(void)
{
	set_cur_state(INIT_MODE);
	needs_state_reset = 0;
	unclamp_default_freq(INIT_MODE);
	process_cpu_powerclamp_exit();
	process_cpu_isolate_exit();
#ifdef __REMOVE__
	revert_orig_epp();
	revert_orig_epb();
#endif
#ifdef __USE_LPMD_IRQ__
    native_restore_irqs();
#else
    restore_irq_mask();//replace with lpmd_irq function.
#endif
}

int apply_state_change(void)
{
	float test;

	if (!needs_state_reset)
		return 0;
#ifdef __REMOVE__
	// reset idle inject to 0 every state change
	if (IDLE_INJECT_FEATURE
	    && ((inject_update == DEACTIVATED) || (inject_update == PAUSE))) {
		process_cpu_powerclamp_exit();
	}
#endif

	if (irq_rebalance) {
		lpmd_log_error("ECO irq active -- revisit");
#ifdef __USE_LPMD_IRQ__
        native_update_irqs();
#else
        update_irqs();//replace with lpmd_irq function
#endif
		irq_rebalance = 0;
	}

	if ((cur_state == INIT_MODE) || (zero_isol_cpu(get_cur_state())))
		process_cpu_isolate_exit();
	else
		process_cpu_isolate_enter();

#ifdef __REMOVE__
	if (IDLE_INJECT_FEATURE && (inject_update == ACTIVATED)
	    && state_has_ppw(get_cur_state())) {
		process_cpu_powerclamp_enter(DURATION_SPILL *
					     get_state_poll(grp.c0_max,
							    get_cur_state()),
					     (100 - INJ_BUF_PCT - grp.c0_max));
		inject_update = RUNNING;
	}
#endif

	update_perf_diffs(&test, 1);

	needs_state_reset = 0;
	return 1;
}

/*defined in lpmd_cpu*/
/*int is_cpu_online(int cpu)
{
	int tmp;
	char path[MAX_STR_LENGTH];
	if (cpu == 0)
		return 1;
	if (!lp_state[INIT_MODE].mask) {
		// try sysfs 
		snprintf(path, sizeof(path),
			 "/sys/devices/system/cpu/cpu%d/online", cpu);
		fs_read_int(path, &tmp);
		return tmp;
	} else {
		return CPU_ISSET_S(cpu, size_cpumask, lp_state[INIT_MODE].mask);
	}
}*/

//util
int cpu_applicable(int cpu, enum lp_state_idx state)
{
	if (!lp_state[state].mask)
		return 0;

	return !!CPU_ISSET_S(cpu, size_cpumask, lp_state[state].mask);
}

/*defined in lpmd_cpu*/
/*int get_max_cpus(void)
{
	return topo_max_cpus;
}

int get_max_online_cpu(void)
{
	return max_online_cpu;
}*/

size_t alloc_cpu_set(cpu_set_t ** cpu_set)
{
	cpu_set_t *_cpu_set;
	size_t size;

	_cpu_set = CPU_ALLOC((topo_max_cpus + 1));
	if (_cpu_set == NULL)
		err(3, "CPU_ALLOC");
	size = CPU_ALLOC_SIZE((topo_max_cpus + 1));
	CPU_ZERO_S(size, _cpu_set);

	*cpu_set = _cpu_set;

	if (!size_cpumask)
		size_cpumask = size;

	if (size_cpumask && size_cpumask != size) {
		lpmd_log_error("Conflict cpumask size %lu vs. %lu\n", size,
			size_cpumask);
		exit(-1);
	}
	return size;
}

/*defined in lpmd_cpu - todo: check line: if (offset > length - 3) {*/
/* For CPU management */
/*static int cpumask_to_str(cpu_set_t * mask, char *buf, int length)
{
	int i;
	int offset = 0;

	for (i = 0; i < topo_max_cpus; i++) {
		if (!CPU_ISSET_S(i, size_cpumask, mask))
			continue;
		if (offset > length - 3) {
			lpmd_log_debug("cpumask_to_str: Too many cpus\n");
			return 1;
		}
		offset += snprintf(buf + offset, length - 1 - offset, "%d,", i);
	}
	buf[offset - 1] = '\0';
	return 0;
}

static char to_hexchar(int val)
{
	if (val <= 9)
		return val + '0';
	if (val >= 16)
		return -1;
	return val - 10 + 'a';
}


int cpumask_to_hexstr(cpu_set_t * mask, char *str, int size)
{
	int cpu;
	int i;
	int pos = 0;
	char c;

	for (cpu = 0; cpu < topo_max_cpus; cpu++) {
		i = cpu % 4;

		if (!i)
			c = 0;

		if (CPU_ISSET_S(cpu, size_cpumask, mask)) {
			c += (1 << i);
		}

		if (i == 3) {
			str[pos] = to_hexchar(c);
			pos++;
			if (pos >= size)
				return -1;
		}
	}
	str[pos] = '\0';

	pos--;
	for (i = 0; i <= pos / 2; i++) {
		c = str[i];
		str[i] = str[pos - i];
		str[pos - i] = c;
	}

	return 0;
}*/

void initialize_state_mask(void)
{
	set_cur_state(INIT_MODE);
	set_state_reset();
}

void and_into_injmask(enum lp_state_idx idx1, enum lp_state_idx idx2,
		      enum lp_state_idx inj_idx)
{
	if (!lp_state[inj_idx].inj_mask)
		return;

	CPU_AND_S(size_cpumask, lp_state[inj_idx].inj_mask,
		  lp_state[idx1].mask, lp_state[idx2].mask);
}

static char *get_inj_hexstr(enum lp_state_idx idx)
{
	int ret;

	if (!lp_state[idx].inj_mask)
		return NULL;

	if (!CPU_COUNT_S(size_cpumask, lp_state[idx].inj_mask))
		return NULL;

	if (lp_state[idx].inj_hexstr)
		return lp_state[idx].inj_hexstr;

	lp_state[idx].inj_hexstr = calloc(1, MAX_STR_LENGTH);
	if (!lp_state[idx].inj_hexstr)
		err(3, "STR_ALLOC");

	ret = cpumask_to_hexstr(lp_state[idx].inj_mask, lp_state[idx].inj_hexstr,
			  MAX_STR_LENGTH);
	if(ret == -1) {
		//todo: handle error
	}

	return lp_state[idx].inj_hexstr;
}

static char *get_cpus_str_proxy(enum lp_state_idx idx)
{
	int ret;
	if (!lp_state[idx].mask)
		return NULL;

	if (!CPU_COUNT_S(size_cpumask, lp_state[idx].mask))
		return NULL;

	if (lp_state[idx].str)
		return lp_state[idx].str;

	lp_state[idx].str = calloc(1, MAX_STR_LENGTH);
	if (!lp_state[idx].str)
		err(3, "STR_ALLOC");

	ret = cpumask_to_str(lp_state[idx].mask, lp_state[idx].str, MAX_STR_LENGTH);
	if(ret == -1) {
		//todo: handle error
	}
	return lp_state[idx].str;
}

char *get_cpus_hexstr(enum lp_state_idx idx)
{
	int ret;
	if (!lp_state[idx].mask)
		return NULL;

	if (!CPU_COUNT_S(size_cpumask, lp_state[idx].mask))
		return NULL;

	if (lp_state[idx].hexstr)
		return lp_state[idx].hexstr;

	lp_state[idx].hexstr = calloc(1, MAX_STR_LENGTH);
	if (!lp_state[idx].hexstr)
		err(3, "STR_ALLOC");

	ret = cpumask_to_hexstr(lp_state[idx].mask, lp_state[idx].hexstr,
			  MAX_STR_LENGTH);
	if(ret == -1) {
		//todo: handle error
	}
	return lp_state[idx].hexstr;
}

cpu_set_t *get_cpu_mask(enum lp_state_idx idx)
{
	return lp_state[idx].mask;
}

static char *get_cpus_str_reverse(enum lp_state_idx idx)
{
	int ret;
	cpu_set_t *mask;

	if (!lp_state[idx].mask)
		return NULL;

	if (!CPU_COUNT_S(size_cpumask, lp_state[idx].mask))
		return NULL;

	if (lp_state[idx].str_reverse)
		return lp_state[idx].str_reverse;

	lp_state[idx].str_reverse = calloc(1, MAX_STR_LENGTH);
	if (!lp_state[idx].str_reverse)
		err(3, "STR_ALLOC");

	alloc_cpu_set(&mask);
	CPU_XOR_S(size_cpumask, mask, lp_state[idx].mask,
		  lp_state[INIT_MODE].mask);
	ret = cpumask_to_str(mask, lp_state[idx].str_reverse, MAX_STR_LENGTH);
	if(ret == -1) {
		//todo: handle error
	}
	CPU_FREE(mask);

	return lp_state[idx].str_reverse;
}

static int zero_isol_cpu(enum lp_state_idx idx)
{
	return (CPU_COUNT_S(size_cpumask, lp_state[idx].mask) ==
		CPU_COUNT_S(size_cpumask, lp_state[INIT_MODE].mask));
}

static int has_cpus_proxy(enum lp_state_idx idx)
{
	if (!lp_state[idx].mask)
		return 0;
	return CPU_COUNT_S(size_cpumask, lp_state[idx].mask);
}

static int add_cpu_proxy(int cpu, enum lp_state_idx idx)
{
	if (idx != INIT_MODE && !is_cpu_online(cpu))
		return 0;

	if (!lp_state[idx].mask)
		alloc_cpu_set(&lp_state[idx].mask);

	CPU_SET_S(cpu, size_cpumask, lp_state[idx].mask);

	if (idx == INIT_MODE)
		lpmd_log_debug("\tDetected %s CPU%d\n", lp_state[idx].name, cpu);
	else
		lpmd_log_info("\tDetected %s CPU%d\n", lp_state[idx].name, cpu);

	return 0;
}

static void reset_cpus_proxy(enum lp_state_idx idx)
{
	if (lp_state[idx].mask)
		//CPU_ZERO_S(size_cpumask, lp_state[idx].mask);
		CPU_FREE(lp_state[idx].mask);
	free(lp_state[idx].str);
	free(lp_state[idx].str_reverse);
	free(lp_state[idx].hexstr);
	free(lp_state[idx].hexstr_reverse);
	lp_state[idx].str = NULL;
	lp_state[idx].str_reverse = NULL;
	lp_state[idx].hexstr = NULL;
	lp_state[idx].hexstr_reverse = NULL;
	cur_state = INIT_MODE;
}

/* Parse CPU topology */
static int set_max_cpu_num(void)
{
	FILE *filep=NULL;
	unsigned long dummy;
	int i;

	topo_max_cpus = 0;
	for (i = 0; i < 256; ++i) {
		char path[MAX_STR_LENGTH];

		snprintf(path, sizeof(path),
			 "/sys/devices/system/cpu/cpu%d/topology/thread_siblings",
			 i);

		filep = fopen(path, "r");
		if (filep)
			break;
	}

	if (!filep) {
		lpmd_log_error("Can't get max cpu number\n");
		return -1;
	}

	while (fscanf(filep, "%lx,", &dummy) == 1)
		topo_max_cpus += BITMASK_SIZE;
	fclose(filep);

	lpmd_log_debug("\t%d CPUs supported in maximum\n", topo_max_cpus);
	return 0;
}

int parse_cpu_topology(void)
{
	FILE *filep = NULL;
	int i,ret;
	char path[MAX_STR_LENGTH] = "";

	reset_cpus_proxy(INIT_MODE);
	/* kenrel 6.5 cpu0 is considered always online */
	add_cpu_proxy(0, INIT_MODE);
	max_online_cpu++;

	for (i = 1; i < topo_max_cpus; i++) {
		char online[8]= "";

		snprintf(path, sizeof(path),
			 "/sys/devices/system/cpu/cpu%d/online", i);
		filep = fopen(path, "r");
		if (filep) {
			ret = fread(&online, sizeof(online), 1, filep);
            if (!ret) {
                lpmd_log_debug("unable to read cpu %d online status\n", i);            
            }
            fclose(filep);
		} else
			break;

		online[sizeof(online) - 1] = '\0';
		if (!atoi(online))
			continue;

		max_online_cpu++;
		add_cpu_proxy(i, INIT_MODE);
	}
	lpmd_log_info("cpu topology\n\tonline mask: 0x%s\n\tonline count: %d\n",
	       get_cpus_hexstr(INIT_MODE), max_online_cpu);
	return 0;
}

/*defined in lpmd_cpu*/
/*
 * parse cpuset with following syntax
 * 1,2,4..6,8-10 and set bits in cpu_subset
 */
int parse_cpu_str_proxy(char *buf, enum lp_state_idx idx)
{
	unsigned int start, end;
	char *next;
	int nr_cpus = 0;

	if (buf[0] == '\0')
		return 0;

	next = buf;

	while (next && *next) {
		if (*next == '-')	// no negative cpu numbers 
			goto error;

		start = strtoul(next, &next, 10);

		add_cpu_proxy(start, idx);
		nr_cpus++;

		if (*next == '\0')
			break;

		if (*next == ',') {
			next += 1;
			continue;
		}

		if (*next == '-') {
			next += 1;	// start range 
		} else if (*next == '.') {
			next += 1;
			if (*next == '.')
				next += 1;	// start range 
			else
				goto error;
		}

		end = strtoul(next, &next, 10);
		if (end <= start)
			goto error;

		while (++start <= end) {
			add_cpu_proxy(start, idx);
			nr_cpus++;
		}

		if (*next == ',')
			next += 1;
		else if (*next != '\0')
			goto error;
	}

	return nr_cpus;
 error:
	lpmd_log_error("CPU string malformed: %s\n", buf);
	return -1;
}

static int detect_lp_state_actual(void)
{
	char path[MAX_STR_LENGTH];
	int i;
	int tmp, tmp_min = INT_MAX, tmp_max = 0;
	enum lp_state_idx idx;
	int j = 0, actual_freq_buckets;
	int prev_turbo = 0;

	for (idx = INIT_MODE + 1; idx < MAX_MODE; idx++) {
		if (!alloc_cpu_set(&lp_state[idx].mask))
			lpmd_log_error("aloc fail");
	}

	/* 
	 * based on cpu advertized max turbo frequncies
	 * bucket the cpu into groups (MAX_FREQ_MAPS)
	 * there after map them to state's mask etc.
	 * XXX: need to handle corner cases in this logic.
	 */

	for (i = 0; i < max_online_cpu; i++) {

		if (!is_cpu_online(i)) {
			continue;
		}

		snprintf(path, sizeof(path),
			 "/sys/devices/system/cpu/cpu%d/cpufreq/cpuinfo_max_freq",
			 i);
		lpmd_read_int(path, &tmp, -1);

		if (!prev_turbo) {
			/* max turbo freq */
			freq_map[j].start_cpu = i;
			freq_map[j].turbo_freq_khz = tmp;
		} else if (prev_turbo != tmp) {
			freq_map[j].end_cpu = i - 1;	// fix me for i-1 not online
			j++;
			freq_map[j].start_cpu = i;
			freq_map[j].turbo_freq_khz = tmp;
		}
		prev_turbo = tmp;

		if (tmp < tmp_min)
			tmp_min = tmp;
		else if (tmp > tmp_max)
			tmp_max = tmp;

		/* min freq */
		snprintf(path, sizeof(path),
			 "/sys/devices/system/cpu/cpu%d/cpufreq/cpuinfo_min_freq",
			 i);
		lpmd_read_int(path, &tmp, -1);

		if ((common_min_freq == 0) || (common_min_freq == tmp))
			common_min_freq = tmp;
		else if (common_min_freq == -1)
			continue;
		else
			common_min_freq = -1;
	}
	freq_map[j].end_cpu = i - 1;
	actual_freq_buckets = j + 1;

	lpmd_log_debug("Freq buckets [%d]\n\tbucket turbo cpu-range", actual_freq_buckets);
	for (j = 0; j <= actual_freq_buckets - 1; j++) {
		freq_map_count++;
		lpmd_log_info("\n\t [%d]  %dMHz  %d-%d", j,
		       freq_map[j].turbo_freq_khz / 1000, freq_map[j].start_cpu,
		       freq_map[j].end_cpu);
	}

	for (i = topo_max_cpus - 1; i >= 0; i--) {
		if (!is_cpu_online(i))
			continue;
		snprintf(path, sizeof(path),
			 "/sys/devices/system/cpu/cpu%d/cpufreq/cpuinfo_max_freq",
			 i);
		lpmd_read_int(path, &tmp, -1);
		/* TBD: address the favoured core system with 1 or 2 top bin cpu */
		if (tmp == tmp_max) {
			/* for PERF mode need all cpu other than LP */
			if (actual_freq_buckets > 1)
				CPU_SET_S(i, size_cpumask,
					  lp_state[PERF_MODE].mask);
			CPU_SET_S(i, size_cpumask, lp_state[BYPS_MODE].mask);
		}
		/* fix me for case of favourd cores */
		if ((tmp > tmp_min) || ((freq_map_count == 1) && (tmp == tmp_min))) {
			/* for moderate2 mode 4 cpu are sufficient */
			if (CPU_COUNT_S(size_cpumask, lp_state[MDRT4E_MODE].mask)
			    >= MAX_MDRT4E_LP_CPU)
				continue;
			CPU_SET_S(i, size_cpumask, lp_state[MDRT4E_MODE].mask);

			/* for moderate mode 3 cpu are sufficient */
			if (CPU_COUNT_S(size_cpumask, lp_state[MDRT3E_MODE].mask)
			    >= MAX_MDRT3E_LP_CPU)
				continue;
			CPU_SET_S(i, size_cpumask, lp_state[MDRT3E_MODE].mask);

			/* for moderate0 mode  cpu are sufficient */
			if (CPU_COUNT_S(size_cpumask, lp_state[MDRT2E_MODE].mask)
			    >= MAX_MDRT2E_LP_CPU)
				continue;
			CPU_SET_S(i, size_cpumask, lp_state[MDRT2E_MODE].mask);
		}
		if (tmp == tmp_min) {
			if (CPU_COUNT_S(size_cpumask, lp_state[RESP_MODE].mask)
			    >= MAX_RESP_LP_CPU)
				continue;
			CPU_SET_S(i, size_cpumask, lp_state[RESP_MODE].mask);

			/* for LP mode 2 cpu are sufficient */
			/* fixme: club the "continue" statement correctly */

			if (CPU_COUNT_S(size_cpumask, lp_state[NORM_MODE].mask)
			    >= MAX_NORM_LP_CPU)
				continue;
			CPU_SET_S(i, size_cpumask, lp_state[NORM_MODE].mask);

			if (CPU_COUNT_S(size_cpumask, lp_state[DEEP_MODE].mask)
			    >= MAX_DEEP_LP_CPU)
				continue;
			CPU_SET_S(i, size_cpumask, lp_state[DEEP_MODE].mask);
		}
	}

	for (i = 0; i < topo_max_cpus; i++) {
		if (!is_cpu_online(i))
			continue;
		snprintf(path, sizeof(path),
			 "/sys/devices/system/cpu/cpu%d/cache/index3/level", i);
		if (!lpmd_open(path, -1)) {
			CPU_SET_S(i, size_cpumask, lp_state[PERF_MODE].mask);
			CPU_SET_S(i, size_cpumask, lp_state[BYPS_MODE].mask);
		}
	}

	/* bypass mode TBD. any strange workloads can be let to run bypass */
	lp_state[BYPS_MODE].disabled = true;
	/* MDRT with 2 cores is not know to be beneficial comapred. simplyfy */
	lp_state[MDRT2E_MODE].disabled = true;
	if (max_online_cpu <= 4) {
		lpmd_log_info("too few CPU: %d", max_online_cpu);
		exit(1);
	} else if (max_online_cpu <= 8) {
		lp_state[MDRT2E_MODE].disabled = true;
		lp_state[MDRT4E_MODE].disabled = true;
	}

	int cpu_count;
	for (idx = INIT_MODE; idx < MAX_MODE; idx++) {
		cpu_count = CPU_COUNT_S(size_cpumask, lp_state[idx].mask);
		if (!lp_state[idx].disabled && cpu_count) {
			if (state_has_ppw(idx)) {
				if (!lp_state[idx].inj_mask)
					alloc_cpu_set(&lp_state[idx].inj_mask);
				and_into_injmask(INIT_MODE, idx, idx);
			}
			lpmd_log_info("\t[%d] %s [0x%s] cpu count: %2d\n", idx,
			       lp_state[idx].name, get_cpus_hexstr(idx),
			       cpu_count);
		}
	}
	for (idx = INIT_MODE; idx < MAX_MODE; idx++) {
		cpu_count = CPU_COUNT_S(size_cpumask, lp_state[idx].mask);
		if (lp_state[idx].disabled || !cpu_count) {
			lpmd_log_info("\t[%d] %s [0x%s] cpu count: %2d\n", idx,
			       lp_state[idx].name, get_cpus_hexstr(idx),
			       cpu_count);
		}
	}

	return 1;
}

static int detect_lp_state(void)
{
	int ret;

	ret = detect_lp_state_actual();

	if (ret <= 0) {
		lpmd_log_info("\tNo valid Low Power CPUs detected, exit\n");
		exit(1);
	} else {

	}

	if (has_cpus_proxy(INIT_MODE))
		lpmd_log_debug("\tUse CPU %s as Default Low Power CPUs\n",
			  get_cpus_str_proxy(INIT_MODE));

	return 0;
}

static int update_cpusets(char *data, int update)
{
	DIR *dir;
	struct dirent *entry;
	char path[MAX_STR_LENGTH * 2];
	int processed = 0;
	int ret = 0;

	if ((dir = opendir(PATH_CGROUP)) == NULL) {
		perror("opendir() error");
		return 1;
	}

	while ((entry = readdir(dir)) != NULL) {
		if (entry->d_name[0] != '.') {
			char *str;

			str = strchr(entry->d_name, '.');
			if (str) {
				if (!strncmp(str, ".slice", strlen(".slice"))) {
					snprintf(path, MAX_STR_LENGTH * 2,
						 "%s/%s/cpuset.cpus",
						 PATH_CGROUP, entry->d_name);
					if (update)
						ret = lpmd_write_str(path, data, -1);
					else
						ret = lpmd_open(path, -1);
					if (ret)
						goto closedir;
					processed = 1;
				}
			}
		}
	}

 closedir:
	closedir(dir);

	if (!processed)
		ret = 1;
	return ret ? 1 : 0;
}

int check_cpu_powerclamp_support(void)
{
	FILE *filep;
	DIR *dir;
	struct dirent *entry;
	char *name = "intel_powerclamp";
	char str[20];
	int ret;

	if (lpmd_open(PATH_CPUMASK , -1))
		return 1;

	if ((dir = opendir(PATH_THERMAL)) == NULL) {
		perror("opendir() error");
		return 1;
	}

	while ((entry = readdir(dir)) != NULL) {
		if (strlen(entry->d_name) > 100)
			continue;
		snprintf(path_powerclamp, MAX_STR_LENGTH * 2, "%s/%s/type",
			 PATH_THERMAL, entry->d_name);
		filep = fopen(path_powerclamp, "r");
		if (!filep)
			continue;

		ret = fread(str, strlen(name), 1, filep);
		fclose(filep);

		if (ret != 1)
			continue;

		if (!strncmp(str, name, strlen(name))) {
			snprintf(path_powerclamp, MAX_STR_LENGTH * 2,
				 "%s/%s/cur_state", PATH_THERMAL,
				 entry->d_name);
			break;
		}
	}
	closedir(dir);

	if (path_powerclamp[0] == '\0')
		return 1;

	lpmd_log_debug("\tFound %s device at %s\n", name, path_powerclamp);
	return 0;
}

int process_cpu_powerclamp_update(int dur, int idl)
{
	if (lpmd_write_int(PATH_DURATION, dur, -1))
		return 1;

	if (lpmd_write_int(PATH_MAXIDLE, idl, -1))
		return 1;

	if (lpmd_write_int(path_powerclamp, idl, -1))
		return 1;

	return 0;
}

int process_cpu_powerclamp_enter(int dur, int idl)
{
	if (lpmd_write_str(PATH_CPUMASK, get_inj_hexstr(cur_state), -1))
		return 1;

	if (dur > 0) {
		if (lpmd_read_int(PATH_DURATION, &default_dur, -1))
			return 1;

		if (lpmd_write_int(PATH_DURATION, dur, -1))
			return 1;
	}

	if (lpmd_write_int(PATH_MAXIDLE, idl, -1))
		return 1;

	if (lpmd_write_int(path_powerclamp, idl, -1))
		return 1;

	return 0;
}

int process_cpu_powerclamp_exit()
{
	if (lpmd_write_int(PATH_DURATION, default_dur, -1))
		return 1;
	return lpmd_write_int(path_powerclamp, 0, -1);
}

static int check_cpu_isolate_support(void)
{
	lpmd_write_str(PATH_CG2_SUBTREE_CONTROL, "+cpuset", -1);
	return update_cpusets(NULL, 0);
}

int process_cpu_isolate_enter(void)
{
	if (write_cgroup_partition("member") < 0)
		return -1;
	/* check the case of 0 count in reverse str */
	if (write_cgroup_isolate(get_cpus_str_reverse(get_cur_state())) < 0)
		return -1;
	if (write_cgroup_partition("isolated") < 0)
		return -1;
	return 1;
}

int process_cpu_isolate_exit(void)
{
	if (write_cgroup_partition("member") < 0)
		return -1;
	if (write_cgroup_isolate(get_cpus_str_proxy(INIT_MODE)) < 0)
		return -1;

	return 0;
}

static int init_cgroup_fs(void)
{
	int ret;

	ret = check_cpu_powerclamp_support();
	ret = check_cpu_isolate_support();
	return ret;
}

/*defined in lpmd_cpu -- rename fn.*/
int init_cpu_proxy(void)
{
	int ret;

	ret = set_max_cpu_num();
	if (ret)
		return ret;

	lpmd_log_debug("Detecting CPUs ...\n");
	ret = parse_cpu_topology();
	if (ret)
		return ret;

	init_cgroup_fs();

	perf_stat_init();

	ret = detect_lp_state();

	if (ret)
		return ret;

	return 0;
}

void uninit_cpu_proxy(){
	for (int idx = INIT_MODE + 1; idx < MAX_MODE; idx++) {
        reset_cpus_proxy(idx);
	}
}
