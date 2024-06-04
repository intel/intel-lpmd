/*
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
 * This source file contains main() function, which parses command line
 * option. Call lpmd init function. Provide logging support.
 * Also allow to daemonize.
 */
 
#define _GNU_SOURCE         /* See feature_test_macros(7) */
#include <unistd.h>
#include <unistd.h>
#include <sys/capability.h>
#include <sys/prctl.h>

#include "additional_common.h"

#include <sys/capability.h>
cap_t cap_get_proc(void);
int cap_set_proc(cap_t cap_p);

#include <sys/types.h>
cap_t cap_get_pid(pid_t pid);
  
#define MAP_MAX_SIZE 100

static int initialized  = -1;
static int map_size = 0;
static char keys[MAP_MAX_SIZE][MAP_MAX_SIZE]; // Keys  
static int values[MAP_MAX_SIZE]; // values 

/** modify capability. returns 0 on sucess; -1 on error. */
//int _modify_capability(int capability, cap_flag_value_t setting);
  
static int getIndex(char key[]) {
    for (int i = 0; i < map_size; i++) {
        if (strcmp(keys[i], key) == 0) { 
            return i;
        }
    }
    return -1;//unknown
} 

static int init() {

	if (initialized == -1) {
		
	    int i = 0;	
		
		values[i] = CAP_AUDIT_CONTROL;
		strncpy(keys[i++], "CAP_AUDIT_CONTROL", sizeof("CAP_AUDIT_CONTROL"));
		
		values[i] = CAP_AUDIT_READ;
		strncpy(keys[i++], "CAP_AUDIT_READ", sizeof("CAP_AUDIT_READ"));
		
		values[i] = CAP_SETUID;
		strncpy(keys[i++], "CAP_SETUID", sizeof("CAP_SETUID"));
		
		initialized = 1;
	}

	return 0;	
}

//char priv_cap_list [][]

/**
static std::map<std::string, int> priv_cap_list {
    {("CAP_AUDIT_CONTROL"), CAP_AUDIT_CONTROL},
    {("CAP_AUDIT_READ"), CAP_AUDIT_READ},
    {("CAP_AUDIT_WRITE"), CAP_AUDIT_WRITE},
    {("CAP_BLOCK_SUSPEND"), CAP_BLOCK_SUSPEND},
    //{("CAP_BPF"), CAP_BPF}, //for 5.8/5.9 kernel
    //{("CAP_CHECKPOINT_RESTORE"), CAP_CHECKPOINT_RESTORE}, //for 5.8/5.9 kernel
    {("CAP_CHOWN"), CAP_CHOWN},
    {("CAP_DAC_OVERRIDE"), CAP_DAC_OVERRIDE},
    {("CAP_DAC_READ_SEARCH"), CAP_DAC_READ_SEARCH},
    {("CAP_FOWNER"), CAP_FOWNER},
    {("CAP_IPC_LOCK"), CAP_IPC_LOCK},
    {("CAP_IPC_OWNER"), CAP_IPC_OWNER},
    {("CAP_KILL"), CAP_KILL},
    {("CAP_LEASE"), CAP_LEASE},
    {("CAP_LINUX_IMMUTABLE"), CAP_LINUX_IMMUTABLE},
    {("CAP_MAC_ADMIN"), CAP_MAC_ADMIN},
    {("CAP_MAC_OVERRIDE"), CAP_MAC_OVERRIDE},
    {("CAP_MKNOD"), CAP_MKNOD},
    {("CAP_NET_ADMIN"), CAP_NET_ADMIN},
    {("CAP_NET_BIND_SERVICE"), CAP_NET_BIND_SERVICE},
    {("CAP_NET_BROADCAST"), CAP_NET_BROADCAST},
    {("CAP_NET_RAW"), CAP_NET_RAW},
    //{("CAP_PERFMON"), CAP_PERFMON}, //for 5.8/5.9 kernel
    {("CAP_SETGID"), CAP_SETGID},
    {("CAP_SETFCAP"), CAP_SETFCAP},
    {("CAP_SETPCAP"), CAP_SETPCAP},
    {("CAP_SETUID"), CAP_SETUID},
    {("CAP_SYS_ADMIN"), CAP_SYS_ADMIN},
    {("CAP_SYS_BOOT"), CAP_SYS_BOOT},
    {("CAP_SYS_CHROOT"), CAP_SYS_CHROOT},
    {("CAP_SYS_MODULE"), CAP_SYS_MODULE},
    {("CAP_SYS_NICE"), CAP_SYS_NICE},
    {("CAP_SYS_PACCT"), CAP_SYS_PACCT},
    {("CAP_SYS_NICE"), CAP_SYS_NICE},
    {("CAP_SYS_PTRACE"), CAP_SYS_PTRACE},
    {("CAP_SYS_RAWIO"), CAP_SYS_RAWIO},
    {("CAP_SYS_RESOURCE"), CAP_SYS_RESOURCE},
    {("CAP_SYS_TIME"), CAP_SYS_TIME},
    {("CAP_SYS_TTY_CONFIG"), CAP_SYS_TTY_CONFIG},
    {("CAP_SYSLOG"), CAP_SYSLOG},
    {("CAP_WAKE_ALARM"), CAP_WAKE_ALARM}};
*/


int _drop_privilege() {

    uid_t ruid = -1, euid = -1, suid = -1;
    gid_t rgid = -1, egid = -1, sgid = -1;
    cap_t caps;
    int ret = -1;

    if (!(caps = cap_get_proc())) {
        printf("drop privilege: couldn't get process caps");
        return -1;
    } else { printf("drop privilege: get process caps"); }

    // keeps caps upon user switch
    if (prctl(PR_SET_KEEPCAPS, 1L)) {
        printf("drop privilege: error keeping caps \n");
        goto error;
    }

    if (getresuid(&ruid, &euid, &suid) == -1 || getresgid(&rgid, &egid, &sgid) == -1) {
        printf("drop privilege: couldn't get User/Group IDs \n");
        goto error;
    }

    // switch users (root --> user)
    if (setresgid(-1, rgid, -1) < 0 || setresuid(-1, ruid, -1) < 0) {
        printf("drop privilege: couldn't switch user \n");
        goto error;
    }

    // We should always check if changes are made
    if (getresuid(&ruid, &euid, &suid) == -1 || getresgid(&rgid, &egid, &sgid) == -1) {
        printf("drop privilege: couldn't get User/Group IDs!\n");
        goto error;
    } else {
        if (euid != ruid || egid != rgid) {
            printf("couldn't drop privilege\n");
            goto error;
        }
    }

    // clear root caps passed to user
    if (cap_clear_flag(caps, CAP_EFFECTIVE) == -1) {
        printf("drop privilege: couldn't clear caps \n");
        goto error;
    }

    // pass root caps to user
    if (cap_set_proc(caps) == -1) {
        printf("drop privilege: couldn't set process caps \n");
        goto error;
    }
    ret = 0;


error:
    if (cap_free(caps) == -1) {
        printf("drop privilege: couldn't free caps\n");
    } else {
        ret = 0;
    }

    return ret;
}

int _raise_privilege() {
	
    uid_t ruid = -1, euid = -1, suid = -1;
    gid_t rgid = -1, egid = -1, sgid = -1;

    if (getresuid(&ruid, &euid, &suid) == -1 || getresgid(&rgid, &egid, &sgid) == -1) {
        printf("raise privilege: couldn't get User/Group IDs");
        return -1;
    }

    if (setresuid(-1, suid, -1) < 0 || setresgid(-1, sgid, -1) < 0) {
        printf("raise privilege: couldn't switch user");
        return -1;
    }
	
    // We should always check if changes are made
    if (getresuid(&ruid, &euid, &suid) == -1 || getresgid(&rgid, &egid, &sgid) == -1) {
        printf("raise privilege: couldn't get User/Group IDs!");
        return -1;
    } else {
        if (euid != suid || egid != sgid) {
            printf("couldn't raise privilege");
            return -1;
        }
    }

    return 0;
}

int _modify_capability(int capability, cap_flag_value_t setting) {
    cap_t caps;
    cap_value_t capList[1];

    if (!(caps = cap_get_proc())) {
        printf("couldn't get process capabilities");
        return -1;
    }

    capList[0] = capability;
    if (cap_set_flag(caps, CAP_EFFECTIVE, 1, capList, setting) == -1) {
        printf("couldn't set capability");
        cap_free(caps);
        return -1;
    }

    if (cap_set_flag(caps, CAP_INHERITABLE, 1, capList, setting) == -1) {
        printf("couldn't set capability!");
        cap_free(caps);
        return -1;
    }

    if (cap_set_proc(caps) == -1) {
        printf("couldn't set process capabilities");
        cap_free(caps);
        return -1;
    }

    if (cap_set_ambient(capList[0], setting) < 0) {
       printf("couldn't set Capabilities");
    }

    if (cap_free(caps) == -1) {
        printf("couldn't free capabilities");
        return -1;
    }

    return 0;
}


int _set_capability(int capability) {
    return _modify_capability(capability, CAP_SET);
}

int _clear_capability(int capability) {
    return _modify_capability(capability, CAP_CLEAR);
}