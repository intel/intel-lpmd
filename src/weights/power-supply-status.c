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
* 5. type - 
*/

/*
 * pseudo code:
 *  get sysfs mount path as mnt path may vary
 *  search for power_supply folder as different system may have different path
 *  enumerate supply names - 
 *   get "type" if present, if "type" == Battery check "status" == charging or full. return 1
 *            if not present, then get "online" if present check for 1.

 * interfaces: read only
 * privelege needed:  none
 */

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <dirent.h>
#include <sys/stat.h>
#include "lpmd.h"

#include "weights_common.h"

static int is_initialized = -1;//not initialized
static int is_supported = -1; //unknown
static int is_power_connected = -1; //unknown
static int is_folder_found = 0;
static char interface_path[PATH_MAX] = "";//todo: can be more than 1 power supply - wall charger /usb...
static char base_path[PATH_MAX] = "";
typedef char contents_array[100][PATH_MAX];

/*
#define POWER_SUPPLY_BASE_PATH "/sys/class/power_supply"
#define AC_POWER_SUPPLY_ONLINE_PATH "/sys/class/power_supply/AC1/online"
#define AC_POWER_SUPPLY_ONLINE_UEVENT sys/class/power_supply/AC1/uevent

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

/**
* returns -1 on error, 0 if not found; 1 if found
*/
static int find_dir(char *start_dir, int depth, char *dir_to_find)
{
    DIR *dp;
    struct dirent *entry;
    struct stat statbuf;	
	
	//printf("searching depth = %d ; %s \n", depth, start_dir );
	
    if((dp = opendir(start_dir)) == NULL) {
        lpmd_log_debug("cannot open directory: %s\n", start_dir);
        return -1;
    }
    
    int ret = chdir(start_dir);
    if(ret ==0 ) {
        while((entry = readdir(dp)) != NULL) {
            int res = lstat(entry->d_name, &statbuf);
            if (res != 0)
                continue;
            if(S_ISDIR(statbuf.st_mode)) {
				
                /* ignore . and .. */
                if(strcmp(".",entry->d_name) == 0 ||
                    strcmp("..",entry->d_name) == 0)
                    continue;

                if(strcmp(dir_to_find, entry->d_name) == 0) {
					
					if (getcwd(base_path, sizeof(base_path)) != NULL) {
						//printf("Current working dir: %s\n", base_path);
						char ch = '/';
						strncat(base_path, &ch, sizeof(ch));
						strncat(base_path, entry->d_name, sizeof(entry->d_name));

						//printf("power supply base path - %s/\n", base_path);
						is_folder_found = 1;
					
					} else {
					   perror("getcwd() error");
					}
					
                    break;
                } //else printf("%*s%s\n",depth,"",entry->d_name);
				
				if(is_folder_found == 0 && depth >= 0) {
					/* Recurse at a new indent level */
					find_dir(entry->d_name, depth+4, dir_to_find);
				}
            }
            //else printf("%*s%s\n",depth,"",entry->d_name);
        }
        ret = chdir("..");
    }
    closedir(dp);
    return is_folder_found;
}

static int get_contents(char *start_dir, int* count, int only_folders, contents_array content_array)
{
    DIR *dp;
    struct dirent *entry;
    struct stat statbuf;
	int n_count = 0;
	
    if((dp = opendir(start_dir)) == NULL) {
        lpmd_log_debug("cannot open directory: %s\n", start_dir);
        return -1;
    }
    
    int ret = chdir(start_dir);
    if(ret == 0 ) {
        while((entry = readdir(dp)) != NULL) {
            if (lstat(entry->d_name, &statbuf) == -1)
				continue; 
			
            if(S_ISDIR(statbuf.st_mode)) {
                /* ignore . and .. */
                if(strcmp(".",entry->d_name) == 0 ||
                    strcmp("..",entry->d_name) == 0)
                    continue;
            }
			//printf ("count = %d, content = %s\n", n_count, entry->d_name);
			strcpy(&((*content_array[n_count])), entry->d_name);
			//printf ("content = %s\n", content_array[n_count]);
			n_count = n_count + 1;
        }
        ret = chdir("..");
    }
    closedir(dp);
	
	*count = n_count;
	return 0;
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

int PSS_init() {

    if(is_initialized == 1) {
        return is_supported;
    }
	
	is_supported = -1;
	is_folder_found = 0;
	memset(base_path, '\0', sizeof(base_path));
	memset(interface_path, '\0', sizeof(interface_path));
		
    is_supported = find_dir("/sys/", 0, "power_supply");
		
    is_initialized = 1;
    return is_supported;
}

void PSS_deinit() {
    is_initialized = -1; //reset
    return;
}

int PSS_is_available() {
	
    if(is_initialized == 0) {
		PSS_init();
    }
    return is_supported;
}

int is_ac_powered_power_supply_status() {
    
    if(is_initialized != 1) {
       PSS_init();
    }// else printf ("is_initialized = true \n");
	
	if(is_supported == 1 ) {
		//printf("interface_path : %s \n", interface_path);
		//printf("base_path : %s \n", base_path);
		if(strcmp (interface_path, "") == 0) {
			int value = 0;
			int supply_names_count = 0;
			int folder_names_only = 1, files_only = 0;
			int is_powered = -1;
			
			//char out_supplies[PATH_MAX][PATH_MAX];
			contents_array out_supplies;
			int res = get_contents(base_path, &supply_names_count, folder_names_only, out_supplies);
			if (res == -1){
				printf ("unable to power_supply directory information %s\n", base_path);
				return -1;
			}
				
			for (int i = 0; i < supply_names_count; i++) {
				int content_count = 0;
				char power_supply_base_path[PATH_MAX] = {0};
				char* p_supply = out_supplies[i];				
				
				strncpy(power_supply_base_path, base_path, sizeof(base_path));
				strncat(power_supply_base_path, "/", sizeof("/"));	
 
				int size = strlen(p_supply);
				p_supply[size] = '\0'; 
				if (PATH_MAX > strlen(power_supply_base_path) + size + 1){
					char s_supply[PATH_MAX] = "";				
					strcpy(s_supply, p_supply);
					strncat(power_supply_base_path, s_supply, size);
				}
				//printf("power_supply_base_path 1 = %s \n", power_supply_base_path);

				contents_array out_contents;
				res = get_contents(power_supply_base_path, &content_count, files_only, out_contents);
				if (res == -1){
					printf ("unable to power_supply directory information, continue %s\n", power_supply_base_path);
					continue;
				}		
 
				for (int j = 0; j < content_count; j++) {
					char* p_content = strdup(out_contents[j]);
					if(p_content && strcmp(p_content, "online") == 0) {
						strncat(power_supply_base_path, "/", sizeof("/"));
						strncat(power_supply_base_path, "online", sizeof("online"));
							
						//printf("power_supply_base_path = %s \n", power_supply_base_path);
						int value = -1;
						int ret = get_value(power_supply_base_path, &value);
						if(ret == 0 ) {
							strncpy(interface_path, power_supply_base_path, sizeof(power_supply_base_path));
							//printf("interface : %s \n", interface_path);
							is_power_connected = value;
							is_powered = 0;
							if (p_content)
								free(p_content);							
							break;
						}
					}
					if (p_content)
						free(p_content);					
				}
 				
				if(is_powered == 0) {
					break;
				}
			}
		}// else printf ("is_supported = true \n");
		
		if(strcmp(interface_path, "") != 0) {
			int value = -1;
			int ret = get_value(interface_path, &value);
			if(ret == 0 ) {
				is_power_connected = value;
				lpmd_log_debug ("is_power_connected %d \n" , is_power_connected);
				return is_power_connected;
			}
		}
	}
	
	return -1;//unknown if not available.
}
