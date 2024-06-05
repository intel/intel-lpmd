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
 
#ifndef _ADDITIONAL_COMMON_H_
#define _ADDITIONAL_COMMON_H_

 //privilege interface

#include <stdio.h> 
#include <string.h> 
 
 /** set capability. returns 0 on sucess; -1 on error. */
int _set_capability(int capability);

/** clear capability. returns 0 on sucess; -1 on error. */
int _clear_capability(int capability);

/** drop privilege. returns 0 on sucess; -1 on error. */
int _drop_privilege();

/** raise privilege. returns 0 on sucess; -1 on error. */
int _raise_privilege();

#endif /*_ADDITIONAL_COMMON_H_*/