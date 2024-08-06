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
  
/* This file reads cpu freq scaling governor information.*/

/*
 * pseudo code:
 *  get sysfs mount path as mnt path may vary
 *  [default path: /sys/devices/system/cpu/cpu0/cpufreq/scaling_available_governors]
 *  [possible values : performance, powersaver]
 *  get supported governers
 *  if present,continue; if not present, then return error;
 * [default path: /sys/devices/system/cpu/cpu0/cpufreq/scaling_governor]
 *  get governer
 *  if set to performance; pause dyn epp setting and algorithm; if set to powersave, then resume/continue dynamic EPP.

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

#define PPS_SCALING_GOVERNOR "/sys/devices/system/cpu/cpu0/cpufreq/scaling_governor"

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


int PPS_is_performance_governer() {
	
	char value[PATH_MAX];
	int val;
	int ret = get_value_str(PPS_SCALING_GOVERNOR, &val, value, PATH_MAX);
	if (ret == 0) {
		if(strcmp (value, "performance") == 0 ){
			return 1;
		} else {
			return 0;
		}
	}
	
	return -1;//unknown
}

