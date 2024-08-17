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
* 3. /status - unknown(AC connected), charging(AC connected), discharging, not charging, full(AC connected)
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
#include <ctype.h>

#ifdef _ENABLE_UEVENT_
//#include <glib-object.h>
#include <gudev/gudev.h> //gudev-1.0; upower-glib
#endif

#include "lpmd.h"
#include "knobs_common.h"

static int is_initialized = -1;//not initialized
static int is_supported = -1; //unknown
static int is_power_connected = -1; //unknown
static int is_folder_found = 0;
static char interface_path[PATH_MAX] = "";//todo: can be more than 1 power supply - wall charger /usb...
static char base_path[PATH_MAX] = "";
typedef char contents_array[100][PATH_MAX];
static bool bueventSubscription = false;
static int bFoundBatt = 0;

void status_init();
void status_deinit();

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

static int get_value_str(char *path, char *val)
{
	FILE *filep;
	int ret=0;
	
    if (!val)
		return -1;
    
	filep = fopen (path, "r");
	if (!filep)
		return -1;

	ret = fread(val, 1, 256, filep);
    if (ret <= 0) {
		lpmd_log_error("get_value_str from %s failed\n", path);		
	}
	fclose(filep);

	return ret;
}


int PSS_init() {

    if(is_initialized == 1) {
        return is_supported;
    }
	
	bueventSubscription = false;
	is_supported = -1;
	is_folder_found = 0;
	memset(base_path, '\0', sizeof(base_path));
	memset(interface_path, '\0', sizeof(interface_path));
		
    is_supported = find_dir("/sys/", 0, "power_supply");
	
	status_init();//uevent subscribe
	
    is_initialized = 1;
    return is_supported;
}

void PSS_deinit() {
	status_deinit();//uevent unsubscribe
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
	
    //-1 means BAT* folder does not exist from previous searching; if BAT* folder does not exist, no need to check any more
    if (bFoundBatt == -1){
        is_power_connected = true; 
        lpmd_log_info("There is no battery on this device\n");
        return is_power_connected; 
    }
	//if uevent subscription is successful then
	if(bueventSubscription == true && is_power_connected != -1) {
		return is_power_connected;
	}
	
	if(is_supported == 1 ) {
		//printf("interface_path : %s \n", interface_path);
		//printf("base_path : %s \n", base_path);
		if(strcmp (interface_path, "") == 0) {
			int value = 0;
			int supply_names_count = 0;
			int folder_names_only = 1, files_only = 0;
			
			//char out_supplies[PATH_MAX][PATH_MAX];
			contents_array out_supplies;
			int res = get_contents(base_path, &supply_names_count, folder_names_only, out_supplies);
			if (res == -1){
				lpmd_log_debug ("unable to get power_supply base directory information %s\n", base_path);
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
				lpmd_log_info("power_supply_base_path: %s \n", power_supply_base_path);
                
                //checking whether the folder name is a BAT* 
                if (strstr(power_supply_base_path, "BAT")!= NULL)
                    bFoundBatt = 1; 
                else {
                    bFoundBatt = 0; 
                    continue; 
                }
                
				contents_array out_contents;
				res = get_contents(power_supply_base_path, &content_count, files_only, out_contents);
				if (res == -1){
					lpmd_log_debug ("unable to get power_supply content directory information, continue %s\n", power_supply_base_path);
					continue;
				}		
 				for (int j = 0; j < content_count; j++) {
					char* p_content = strdup(out_contents[j]);

                    //in battery folder, we only care about the status file 
                    if (p_content && strcmp(p_content, "status") == 0){
                        char str_value[256];
                        strncat(power_supply_base_path, "/", sizeof("/"));
                        strncat(power_supply_base_path, "status", sizeof("status"));                        
                        int ret = get_value_str(power_supply_base_path, str_value);
                        //to all lower case 
                        if (strlen(str_value) != 0){
                            for(int i = 0; str_value[i]; i++){
                                str_value[i] = tolower(str_value[i]);
                            }
                        }
                        
                        if (strcmp(str_value, "discharging") == 0 || strcmp(str_value, "not charging") == 0){
                            lpmd_log_info("battery powered, value of status is %s\n", str_value);
                            is_power_connected = false; 
                        } else {
                            is_power_connected = true; 
                            lpmd_log_info("power connected, value of status is %s\n", str_value);
                        }
                        //saving the interface_path for next time checking
                        strncpy(interface_path, power_supply_base_path, sizeof(power_supply_base_path));
                        
                        if (p_content)
                            free(p_content);                        
                        return is_power_connected; 
                    } else {
                        if (p_content)
                            free(p_content);
                        continue; 
                    }
                }        
			}
            // no battery folder found, assume it's AC/USB connected    
            if (bFoundBatt == 0){
                bFoundBatt = -1; 
                is_power_connected = true; 
                return is_power_connected; 
            }            
		}
    
        //interface path saved, get the value directly 
		if(strcmp(interface_path, "") != 0) {
			char str_value[256];
			int ret = get_value_str(interface_path, str_value);
            //to all lower case 
            if (strlen(str_value) != 0){
                for(int i = 0; str_value[i]; i++){
                    str_value[i] = tolower(str_value[i]);
                }
            }
            
            if (strcmp(str_value, "discharging") == 0 || strcmp(str_value, "not charging") == 0){
                lpmd_log_info("interface exists, value of status is %s, battery powered\n", str_value);
                is_power_connected = false; 
            } else {
                is_power_connected = true; 
                lpmd_log_info("interface exists, value of status is %s, power connected\n", str_value);
            }            
            
			return is_power_connected;
		}
	}
	
	return -1;//unknown if not available.
}

#ifdef _ENABLE_UEVENT_
//uevent based power supply status detection

GUdevClient *gdev_client;
gboolean gb_active;

void get_battery_status ()
{
  GList *devices, *l;
  devices = g_udev_client_query_by_subsystem (gdev_client, "power_supply");
  if (devices == NULL)
    return;

  for (l = devices; l != NULL; l = l->next) {
    GUdevDevice *dev = l->data;
    const char *value;

    if (g_strcmp0 (g_udev_device_get_sysfs_attr (dev, "scope"), "Device") != 0)
      continue;

    value = g_udev_device_get_sysfs_attr_uncached (dev, "status");
    if (!value)
      continue;

    if (g_strcmp0 ("full", value) == 0) {
		is_power_connected = true;
	} else if (g_strcmp0 ("charging", value) == 0) {
		is_power_connected = true;
	} else if (g_strcmp0 ("discharging", value) == 0) {
		is_power_connected = false;
	} else {
		is_power_connected = false;
	}
	
    break;
  }

  g_list_free_full (devices, g_object_unref);
}

void uevent_cb (GUdevClient *client,
           gchar       *action,
           GUdevDevice *device,
           gpointer     user_data)
{
  if (g_strcmp0 (action, "add") != 0)
    return;

  if (!g_udev_device_has_sysfs_attr (device, "status"))
    return;

	//read power supply status.
	get_battery_status();
}
#endif

//sudo udevadm trigger --subsystem-match=power_supply --action=add
void status_init() {
#ifdef _ENABLE_UEVENT_
	const gchar * const subsystem_match[] = { "power_supply", NULL };
	gdev_client = g_udev_client_new (subsystem_match);
	g_signal_connect (G_OBJECT (gdev_client), "uevent",
                    G_CALLBACK (uevent_cb), NULL);
	bueventSubscription = true;
#endif
}

void status_deinit() {
#ifdef _ENABLE_UEVENT_
	bueventSubscription = false;
	g_clear_object (gdev_client);
#endif
}
