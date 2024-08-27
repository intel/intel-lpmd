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
//#define _GNU_SOURCE

#include "state_manager.h"
#include "lpmd.h"

/*
 * If polling is too fast some of the stats (such as util)
 * could be momentarily high owing to state change disturbances.
 * avoid unexpected decision due to this as it may not be tied to workload per-se.
 * any setting below, say 15ms, needs careful assessment.
 */
#define MIN_POLL_PERIOD 15

#define MAX_MDRT4E_LP_CPU    (4)
#define MAX_MDRT3E_LP_CPU    (3)
#define MAX_MDRT2E_LP_CPU    (2)
#define MAX_RESP_LP_CPU        (2)
#define MAX_NORM_LP_CPU        (2)
#define MAX_DEEP_LP_CPU        (1)

#define BITMASK_SIZE 32
#define MAX_FREQ_MAPS (6)

/* Support for CGROUPV2 */
#define PATH_CGROUP                    "/sys/fs/cgroup"
#define PATH_CG2_SUBTREE_CONTROL    PATH_CGROUP "/cgroup.subtree_control"

extern int cpumask_to_hexstr(cpu_set_t *mask, char *str, int size);
extern int cpumask_to_str(cpu_set_t *mask, char *buf, int length);
extern int irq_rebalance;
extern struct group_util grp;

//static int topo_max_cpus;
/* starting 6.5 kernel, cpu0 is assumed to be always online */
//extern int max_online_cpu;
size_t size_cpumask;

struct _fd_cache {
    int cgroup_partition_fd;
    int cgroup_isolate_fd;
};

static struct _fd_cache fd_cache;

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

struct _freq_map {
    int start_cpu;
    int end_cpu;
    int turbo_freq_khz;
    int perf_order;
};
static struct _freq_map freq_map[MAX_FREQ_MAPS];

//[BYPS_MODE] = {.name =   "bypass mode",.poll = BASE_POLL_PERF,.poll_order = ZEROTH},

static struct _lp_state lp_state[MAX_MODE] = {
    [INIT_MODE] = {.name =   "Avail cpu: P/E/L",.poll = BASE_POLL_MT,.poll_order = ZEROTH},
    [PERF_MODE] = {.name =   "Perf:non-soc cpu",.poll = BASE_POLL_PERF,.poll_order = ZEROTH},
    [MDRT2E_MODE] = {.name = "Moderate 2E",.poll =    BASE_POLL_MDRT2E,.poll_order = LINEAR},
    [MDRT3E_MODE] = {.name = "Moderate 3E",.poll = BASE_POLL_MDRT3E,.poll_order = LINEAR},
    [MDRT4E_MODE] = {.name = "Moderate 4E",.poll =    BASE_POLL_MDRT4E, .poll_order = LINEAR},
    [RESP_MODE] = {.name =   "Responsive 2L",.poll = BASE_POLL_RESP, .poll_order = CUBIC},
    [NORM_MODE] = {.name =   "Normal LP 2L",.poll = BASE_POLL_NORM, .poll_order = QUADRATIC},
    [DEEP_MODE] = {.name =   "Deep LP 1L",.poll = BASE_POLL_DEEP, .poll_order = CUBIC},
};

static enum lp_state_idx cur_state = INIT_MODE;
static int needs_state_reset = 1;

int process_cpu_isolate_enter(void);
int process_cpu_isolate_exit(void);

static int zero_isol_cpu(enum lp_state_idx);
static char *get_inj_hexstr(enum lp_state_idx idx);

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

    process_cpu_isolate_exit();

#ifdef __USE_LPMD_IRQ__
    native_restore_irqs();
#endif
}

int apply_state_change(void)
{
    float test;

    if (!needs_state_reset)
        return 0;
    
    if (irq_rebalance) {
        //lpmd_log_error("ECO irq active -- revisit\n");
#ifdef __USE_LPMD_IRQ__
        native_update_irqs();
#endif
        irq_rebalance = 0;
    }

    if ((cur_state == INIT_MODE) || (zero_isol_cpu(get_cur_state())))
        process_cpu_isolate_exit();
    else
        process_cpu_isolate_enter();

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

//get_max_cpus
/*
size_t alloc_cpu_set(cpu_set_t ** cpu_set)
{
    cpu_set_t *_cpu_set;
    size_t size = 0;

    _cpu_set = CPU_ALLOC((topo_max_cpus + 1));
    if (_cpu_set == NULL) {
        //err(3, "CPU_ALLOC");//exits application
        lpmd_log_error("3 CPU_ALLOC\n");
        return size;
    }
    size = CPU_ALLOC_SIZE((topo_max_cpus + 1));
    CPU_ZERO_S(size, _cpu_set);

    *cpu_set = _cpu_set;

    if (!size_cpumask)
        size_cpumask = size;

    if (size_cpumask && size_cpumask != size) {
        lpmd_log_error("Conflict cpumask size %lu vs. %lu\n", size,
            size_cpumask);
        //exit(-1);
    }
    return size;
}
*/

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
    //set_cur_state(INIT_MODE);
    set_cur_state(NORM_MODE);    
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
    if (!lp_state[idx].inj_hexstr) {
        //err(3, "STR_ALLOC");
        lpmd_log_error("3 STR_ALLOC\n");
        return NULL;
    }

    ret = cpumask_to_hexstr(lp_state[idx].inj_mask, lp_state[idx].inj_hexstr,
              MAX_STR_LENGTH);
    if(ret == -1) {
        //todo: handle error
        return NULL;
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
    if (!lp_state[idx].str) {
        //err(3, "STR_ALLOC");
        lpmd_log_error("3 STR_ALLOC\n");
        return NULL;
    }

    ret = cpumask_to_str(lp_state[idx].mask, lp_state[idx].str, MAX_STR_LENGTH);
    if(ret == -1) {
        //todo: handle error
        return NULL;
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
    if (!lp_state[idx].hexstr) {
        //err(3, "STR_ALLOC");
        lpmd_log_error("3 STR_ALLOC\n");
        return NULL;
    }

    ret = cpumask_to_hexstr(lp_state[idx].mask, lp_state[idx].hexstr,
              MAX_STR_LENGTH);
    if(ret == -1) {
        //todo: handle error
        return NULL;
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
    if (!lp_state[idx].str_reverse) {
        //err(3, "STR_ALLOC");
        lpmd_log_error("3 STR_ALLOC\n");
        return NULL;
    }

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

    if (!lp_state[idx].mask){
        alloc_cpu_set(&lp_state[idx].mask);
    }
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
        if (*next == '-')    // no negative cpu numbers 
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
            next += 1;    // start range 
        } else if (*next == '.') {
            next += 1;
            if (*next == '.')
                next += 1;    // start range 
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


/**********************Marked for REmoval - begin ************************/

static int open_fd(const char *name, int flags)
{
    int fd;

    fd = open(name, flags);
    if (fd == -1) {
        lpmd_log_debug("Open %s failed\n", name);
        return -1;
    }
    return fd;
}

static int _write_str_fd(int fd, const char *str)
{
    int ret;

    if (lseek(fd, 0, SEEK_SET))
        perror("lseek: _write_str_fd");
    ret = write(fd, str, strlen(str));
    if (ret <= 0) {
        perror("write: _write_str_fd");
        return -1;
    }
    return ret;
}

static int _read_str_fd(int fd, char *str)
{
    int ret;

    if (lseek(fd, 0, SEEK_SET))
        perror("lseek: _read_str_fd");
    ret = read(fd, str, strlen(str));
    if (ret <= 0) {
        perror("read: _read_str_fd");
        return -1;
    }
    return ret;
}

static int close_fd(int fd)
{
    if (fd < 0) {
        lpmd_log_debug("invalid fd:%d\n", fd);
        return -1;
    }
    close(fd);
    return 0;
}

static int init_cgroup_fd(void)
{
    int fd_part, fd_set;
    DIR *dir;
    int ret;

    dir = opendir("/sys/fs/cgroup/eco");
    if (!dir) {
        ret = mkdir("/sys/fs/cgroup/eco", 0744);
        if (ret) {
            lpmd_log_debug("Can't create dir:%s errno:%d\n",
                   "/sys/fs/cgroup/eco", errno);
            return ret;
        }
        lpmd_log_debug("\tCreate %s\n", "/sys/fs/cgroup/eco");
    } else
        closedir(dir);

    fd_part = open_fd("/sys/fs/cgroup/eco/cpuset.cpus.partition", O_RDWR);
    if (fd_part > 0)
        fd_cache.cgroup_partition_fd = fd_part;
    else 
        close_fd(fd_part);
        
    fd_set = open_fd("/sys/fs/cgroup/eco/cpuset.cpus", O_RDWR);
    if (fd_set > 0)
        fd_cache.cgroup_isolate_fd = fd_set;
    else 
        close_fd(fd_set);
    return 1;
}

static void uninit_cgroup_fd () {
    if (fd_cache.cgroup_isolate_fd > 0)
        close(fd_cache.cgroup_isolate_fd);
    if (fd_cache.cgroup_partition_fd > 0)
        close(fd_cache.cgroup_partition_fd);
}

static int write_cgroup_partition(const char *str)
{    
    if (fd_cache.cgroup_partition_fd > 0)
        return _write_str_fd(fd_cache.cgroup_partition_fd, str);
 
    lpmd_log_debug("write_cgroup_partition\n");
    return 0;
}

static int write_cgroup_isolate(const char *str)
{
    if (fd_cache.cgroup_isolate_fd > 0)
        return _write_str_fd(fd_cache.cgroup_isolate_fd, str);
    
    lpmd_log_debug("write_cgroup_isolate\n");
    return 0;
}

int process_cpu_isolate_enter(void)
{
    lpmd_log_debug("process_cpu_isolate_enter\n");
    
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
    lpmd_log_debug("process_cpu_isolate_exit\n");
    
    if (write_cgroup_partition("member") < 0)
        return -1;
    if (write_cgroup_isolate(get_cpus_str_proxy(INIT_MODE)) < 0)
        return -1;

    return 0;
}

static int init_cgroup_fs(void)
{
    int ret;

    ret = check_cpu_isolate_support();
    return ret;
}

/**********************Marked for REmoval - end ************************/

/*defined in lpmd_cpu -- rename fn.*/
int init_cpu_proxy(void)
{
    int ret;

    ret = set_max_cpu_num();
    if (ret)
        return ret;

    //init_cgroup_fs();
    ret = check_cpu_isolate_support();
    if (ret)
        return ret;

    perf_stat_init();
    
    //init_cgroup_fd();

    return 0;
}

void uninit_cpu_proxy() {
    
    for (int idx = INIT_MODE + 1; idx < MAX_MODE; idx++) {
        reset_cpus_proxy(idx);
    }
    
    //uninit_cgroup_fd();
}
