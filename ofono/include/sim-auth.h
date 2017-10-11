/*
 *
 *  oFono - Open Source Telephony
 *
 *  Copyright (C) 2008-2011  Intel Corporation. All rights reserved.
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

#ifndef __OFONO_SIM_AUTH_H
#define __OFONO_SIM_AUTH_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

#include <ofono/types.h>

struct ofono_sim_auth;

typedef void (*ofono_sim_list_apps_cb_t)(const struct ofono_error *error,
					const unsigned char *dataobj,
					int len, void *data);

typedef void (*ofono_sim_open_channel_cb_t)(const struct ofono_error *error,
		int session_id, void *data);

typedef void (*ofono_sim_close_channel_cb_t)(const struct ofono_error *error,
		void *data);

typedef void (*ofono_logical_access_cb_t)(const struct ofono_error *error,
		const uint8_t *resp, uint16_t len, void *data);

struct ofono_sim_auth_driver {
	const char *name;
	int (*probe)(struct ofono_sim_auth *sa, unsigned int vendor,
			void *data);
	void (*remove)(struct ofono_sim_auth *sa);

	void (*list_apps)(struct ofono_sim_auth *sa,
			ofono_sim_list_apps_cb_t cb, void *data);
	void (*open_channel)(struct ofono_sim_auth *sa, const uint8_t *aid,
			ofono_sim_open_channel_cb_t cb, void *data);
	void (*close_channel)(struct ofono_sim_auth *sa, int session_id,
			ofono_sim_close_channel_cb_t cb, void *data);
	void (*logical_access)(struct ofono_sim_auth *sa,
			int session_id, const uint8_t *pdu, uint16_t len,
			ofono_logical_access_cb_t cb, void *data);
};

int ofono_sim_auth_driver_register(const struct ofono_sim_auth_driver *d);
void ofono_sim_auth_driver_unregister(const struct ofono_sim_auth_driver *d);

struct ofono_sim_auth *ofono_sim_auth_create(struct ofono_modem *modem,
						unsigned int vendor,
						const char *driver, void *data);

void ofono_sim_auth_register(struct ofono_sim_auth *sa);
void ofono_sim_auth_remove(struct ofono_sim_auth *sa);

void ofono_sim_auth_set_data(struct ofono_sim_auth *sa, void *data);
void *ofono_sim_auth_get_data(struct ofono_sim_auth *sa);

#ifdef __cplusplus
}
#endif

#endif /* __OFONO_SIM_AUTH_H */
