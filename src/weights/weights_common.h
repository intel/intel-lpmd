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
 
 /** acquire resources. returns 0 if supported. */
 extern int PSS_init();
 
 /** 0 if available. */
 extern int PSS_is_available();
 
 /** free up resources */
 extern void PSS_deinit();
 
 /** return 1 if ac powered; -1 unknown; 0 if not */
 extern int is_ac_powered_power_supply_status();
 