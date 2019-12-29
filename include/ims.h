/*
 *
 *  oFono - Open Source Telephony
 *
 *  Copyright (C) 2017  Intel Corporation. All rights reserved.
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License version 2 as
 *  published by the Free Software Foundation.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
 *
 */

#ifndef __OFONO_IMS_H
#define __OFONO_IMS_H

#ifdef __cplusplus
extern "C" {
#endif

#include <ofono/types.h>

struct ofono_ims;

typedef void (*ofono_ims_register_cb_t)(const struct ofono_error *error,
						void *data);
typedef void (*ofono_ims_status_cb_t)(const struct ofono_error *error,
						int reg_info, int ext_info,
						void *data);

struct ofono_ims_driver {
	const char *name;
	int (*probe)(struct ofono_ims *ims, void *data);
	void (*remove)(struct ofono_ims *ims);
	void (*ims_register)(struct ofono_ims *ims,
				ofono_ims_register_cb_t cb, void *data);
	void (*ims_unregister)(struct ofono_ims *ims,
				ofono_ims_register_cb_t cb, void *data);
	void (*registration_status)(struct ofono_ims *ims,
				ofono_ims_status_cb_t cb, void *data);
};

void ofono_ims_status_notify(struct ofono_ims *ims, int reg_info,
							int ext_info);

int ofono_ims_driver_register(const struct ofono_ims_driver *d);
void ofono_ims_driver_unregister(const struct ofono_ims_driver *d);

struct ofono_ims *ofono_ims_create(struct ofono_modem *modem,
					const char *driver, void *data);

void ofono_ims_register(struct ofono_ims *ims);
void ofono_ims_remove(struct ofono_ims *ims);

void ofono_ims_set_data(struct ofono_ims *ims, void *data);
void *ofono_ims_get_data(const struct ofono_ims *ims);

#ifdef __cplusplus
}
#endif

#endif
