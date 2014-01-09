/*
 *
 *  oFono - Open Source Telephony
 *
 *  Copyright (C) 2013 Jolla Ltd
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
#ifndef __OFONO_OEM_RAW_H
#define __OFONO_OEM_RAW_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>
#include <dbus/dbus.h>
#include <ofono/types.h>

struct ofono_oem_raw;

/* Request response from driver to core */
struct ofono_oem_raw_results {
	char *data;
	int length;
};

/* Request details from core to driver */
struct ofono_oem_raw_request {
	char *data;
	int length; /* Number of bytes in data */
	DBusMessage *pending;
};

typedef void (*ofono_oem_raw_query_cb_t)(const struct ofono_error *error,
		const struct ofono_oem_raw_results *results, void *data);

struct ofono_oem_raw_driver {
	const char *name;
	int (*probe)(struct ofono_oem_raw *raw,
			unsigned int vendor,
			void *data);
	void (*remove)(struct ofono_oem_raw *raw);
	void (*request)(struct ofono_oem_raw *raw,
			const struct ofono_oem_raw_request *request,
			ofono_oem_raw_query_cb_t cb,
			void *data);
};

struct ofono_oem_raw *ofono_oem_raw_create(struct ofono_modem *modem,
					unsigned int vendor,
					const char *driver,
					void *data);
void ofono_oem_raw_dbus_register(struct ofono_oem_raw *raw);
void ofono_oem_raw_remove(struct ofono_oem_raw *raw);
int ofono_oem_raw_driver_register(struct ofono_oem_raw_driver *driver);
void ofono_oem_raw_driver_unregister(struct ofono_oem_raw_driver *driver);
void *ofono_oem_raw_get_data(struct ofono_oem_raw *raw);
void ofono_oem_raw_set_data(struct ofono_oem_raw *raw, void *cid);

#ifdef __cplusplus
}
#endif

#endif /* __OFONO_OEM_RAW_H */
