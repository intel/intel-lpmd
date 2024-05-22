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
 
/* This file reads power supply online status information */

#define POWER_SUPPLY_BASE_PATH "/sys/class/power_supply"
#define AC_POWER_SUPPLY_ONLINE_PATH "/sys/class/power_supply/AC1/online"


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

int get_power_supply_online_status(int *ret_val)
{
	char path[MAX_STR_LENGTH];
    *ret_val = -1;
    ret_str[0] = '\0';
    snprintf (path, sizeof(path), POWER_SUPPLY_BASE_PATH, c);
    get_value (path, ret_val);
    return 0;
}