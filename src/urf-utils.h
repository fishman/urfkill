/* -*- Mode: C; tab-width: 8; indent-tabs-mode: t; c-basic-offset: 8 -*-
 *
 * Copyright (C) 2010 Gary Ching-Pang Lin <glin@suse.com>
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

#ifndef __URF_UTILS_H__
#define __URF_UTILS_H__

#include <glib.h>
#include <libudev.h>

typedef struct {
	char *sys_vendor;
	char *bios_date;
	char *bios_vendor;
	char *bios_version;
	char *product_name;
	char *product_version;
} DmiInfo;

DmiInfo			*get_dmi_info			(void);
void			 dmi_info_free			(DmiInfo	*info);
struct udev_device 	*get_rfkill_device_by_index	(struct udev	*udev,
							 guint		 index);

#endif /* __URF_UTILS_H__ */
