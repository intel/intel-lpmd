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
  
/* This file reads hardware wlt support status information */

/*
 * pseudo code:
 *  get sysfs mount path as mnt path may vary
 *  [default path: /sys/bus/pci/devices/0000\:00\:04.0/workload_hint/]
 *  search for workload_hint folder as different system may have different path
 *  if present, return 1; if not present, then return 0;

 * interfaces: read only
 * privelege needed:  none
 */

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <dirent.h>
#include <sys/stat.h>

#include "weights_common.h"

static int is_initialized = -1;//not initialized
static int is_supported = -1; //unknown
static int is_folder_found = 0;
static char base_path[PATH_MAX] = "";

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

                if(strcmp(dir_to_find, entry->d_name) == 0) {
					
					if (getcwd(base_path, sizeof(base_path)) != NULL) {
						//printf("Current working dir: %s\n", base_path);
						char ch = '/';
						strncat(base_path, &ch, sizeof(ch));
						strncat(base_path, entry->d_name, sizeof(entry->d_name));

						printf("base path - %s/\n", base_path);
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

int HWS_init() {

    if(is_initialized == 1) {
        return is_supported;
    }
	
	is_supported = -1;
	is_folder_found = 0;
	memset(base_path, '\0', sizeof(base_path));
		
    is_supported = find_dir("/sys/", 0, "workload_hint");
		
    is_initialized = 1;
    return is_supported;
}

void HWS_deinit() {
    is_initialized = -1; //reset
    return;
}

int HWS_is_available() {
	
    if(is_initialized != 1) {
		HWS_init();
    }
    return is_supported;
}
