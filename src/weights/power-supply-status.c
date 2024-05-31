/*
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
 */
 
/* This file reads power supply online status information [AC or battery powered?]
* battery presence, count and battery charging status.
* 1. /power_supply - find power supply folder in /sys
* 2. /supply_name - enumerate all supply_names [AC, BAT, USB] avialable under power supply
* 3. /status - unknown, charging, discharging, not charging, full
* 4. /online - 
*/

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <dirent.h>
#include <sys/stat.h>


#include "weights_common.h"

#define POWER_SUPPLY_BASE_PATH "/sys/class/power_supply"
#define AC_POWER_SUPPLY_ONLINE_PATH "/sys/class/power_supply/AC1/online"

static int is_initialized = 0;//not initialized
static int is_supported = -1; //unknown
static int is_power_connected = -1; //unknown

/*
"ls -l /sys/class/power_supply/"

on stock Ubuntu, 
AC -> /sys/class/power_supply/AC1/online
BAT -> AC -> /sys/class/power_supply/BAT0/online

On DELL [Inspiron-13-5330],
AC -> ../../devices/LNXSYSTM:00/LNXSYBUS:00/ACPI0003:00/power_supply/AC
BAT0 -> ../../devices/LNXSYSTM:00/LNXSYBUS:00/PNP0C0A:00/power_supply/BAT0
ucsi-source-psy-USBC000:001 -> ../../devices/platform/USBC000:00/power_supply/ucsi-source-psy-USBC000:001
ucsi-source-psy-USBC000:002 -> ../../devices/platform/USBC000:00/power_supply/ucsi-source-psy-USBC000:002

On HP [Baymax],
AC -> ../../devices/LNXSYSTM:00/LNXSYBUS:00/ACPI0003:00/power_supply/AC
BAT0 -> ../../devices/LNXSYSTM:00/LNXSYBUS:00/PNP0C0A:00/power_supply/BAT0
*/

#define MAX_STR_LENGTH 256
char base_path[MAX_STR_LENGTH];
char ac_supply_name_path[MAX_STR_LENGTH];

/**
* returns -1 on error, 0 if not found; 1 if found
*/
int find_dir(char *start_dir, int depth, char *dir_to_find)
{
    int is_folder_found = 0;
    DIR *dp;
    struct dirent *entry;
    struct stat statbuf;
	
	//printf("searching depth = %d ; %s \n", depth, start_dir );
	
    if((dp = opendir(start_dir)) == NULL) {
        fprintf(stderr,"cannot open directory: %s\n", start_dir);
        return -1;
    }
    
    int ret = chdir(start_dir);
    if(ret ==0 ) {
        while((entry = readdir(dp)) != NULL) {
            lstat(entry->d_name, &statbuf);
            if(S_ISDIR(statbuf.st_mode)) {
                /* ignore . and .. */
                if(strcmp(".",entry->d_name) == 0 ||
                    strcmp("..",entry->d_name) == 0)
                    continue;

                //printf("%*s%s/\n",depth,"",entry->d_name);
				printf("%s/\n",entry->d_name);
                if(strcmp(dir_to_find, entry->d_name) == 0) {
					//printf("%*s%s/\n", depth,"", entry->d_name);
					printf("found : %s/\n", entry->d_name);
                    strncpy(base_path, entry->d_name, sizeof(entry->d_name));
                    printf("power supply base path - %s/\n", base_path);
                    is_folder_found = 1;
                    break;
                } //else {
					
				//}
				
				if(depth < 8) {
					/* Recurse at a new indent level */
					find_dir(entry->d_name, depth+4, dir_to_find);
				}
            }
            //else printf("%*s%s\n",depth,"",entry->d_name);
        }
        ret = chdir("..");
    }
    closedir(dp);
	printf("is_folder_found : %d\n", is_folder_found);
    //return is_folder_found;
	return 1;//forcing for debug.
}

static int get_value(char *path, int *val)
{
	FILE *filep;
	int data;
	int ret;

	filep = fopen (path, "r");
	if (!filep)
		return 1;

	ret = fscanf (filep, "%d", &data);
	if (ret == 1) {
		*val = data;
		ret = 0;
	}

	fclose (filep);
	return ret;
}

static int get_value_str(char *path, int *val, char *str, int size)
{
	FILE *filep;
	int data;
	int ret;

	filep = fopen (path, "r");
	if (!filep)
		return 1;

	ret = fscanf (filep, "%d", &data);
	if (ret == 1) {
		*val = data;
		ret = 0;
		goto end;
	}

	ret = fread (str, 1, size, filep);
	if (ret <= 0)
		ret = 1;
	else {
		if (ret >= size)
			ret = size - 1;
		str[ret - 1] = '\0';
		ret = 0;
	}
end:
	fclose (filep);
	return ret;
}

static int enumerate_supply_names () 
{
    int ret = find_dir(base_path, 0, "");
    /*if supply name contains BAT then return 1 if [staus == charging or full]*/
    /*if supply name contains AC then return 1 if [online == 1] */
    /*if supply name contains ucsi or USBC then return 1 if [online == 1] */
    return 1;
    
}

int get_power_supply_online_status(int *ret_val)
{
	
    //if ac_supply_name_path not empty then read online directly
    //if empty then call enumerate_supply_names
	char path[MAX_STR_LENGTH];
    *ret_val = -1;
    //ret_str[0] = '\0';
    snprintf (path, sizeof(path), AC_POWER_SUPPLY_ONLINE_PATH);
	
	printf ("get_power_supply_online_status ");
    return get_value (path, ret_val);
}

int init_power_supply_status() {
	//printf ("init_power_supply_status ");
    if(is_initialized == 1) {
        return is_supported;
    }
    //memset(base_path, '\0', sizeof(base_path));
    //strcpy(base_path, POWER_SUPPLY_BASE_PATH);
    //strncpy(base_path, entry->d_name, sizeof(entry->d_name));
    //printf("power supply base path - %s/\n", base_path);
    //printf ("finding power_supply dir in /sys folder ");
    is_supported = find_dir("/sys/", 0, "power_supply");
    printf("power supply base path - %s/\n", base_path);

    is_initialized = 1;
    return is_supported;
}

int is_available_power_supply_status() {
    if(is_initialized == 0) {
        init_power_supply_status();
    }
    return is_supported;
}

int deinit_power_supply_status() {
    //nothing to free.
    is_initialized = 0; //reset
    return 0;
}

int is_ac_powered_power_supply_status() {
    //printf ("is_ac_powered_power_supply_status ");
    if(is_initialized == 0) {
       init_power_supply_status();
       if(is_supported == 1) {
            int value = 0;
            if(get_power_supply_online_status(&value) == 0) {
				is_power_connected = value;
			}
       }
    }

	printf ("is_power_connected %d " , is_power_connected);
    return is_power_connected;//unknown
}