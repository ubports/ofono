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

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <stdint.h>
#include <stdbool.h>
#include <inttypes.h>

#include <ofono/log.h>
#include <ofono/modem.h>
#include <ofono/gprs.h>
#include "common.h"

#include "drivers/mbimmodem/mbim.h"
#include "drivers/mbimmodem/mbim-message.h"
#include "drivers/mbimmodem/mbimmodem.h"

struct gprs_data {
	struct mbim_device *device;
	struct l_idle *delayed_register;
};

static void mbim_packet_service_set_cb(struct mbim_message *message, void *user)
{
	struct cb_data *cbd = user;
	ofono_gprs_cb_t cb = cbd->cb;

	DBG("");

	if (mbim_message_get_error(message) != 0)
		CALLBACK_WITH_FAILURE(cb, cbd->data);
	else
		CALLBACK_WITH_SUCCESS(cb, cbd->data);
}

static void mbim_gprs_set_attached(struct ofono_gprs *gprs, int attached,
					ofono_gprs_cb_t cb, void *data)
{
	struct gprs_data *gd = ofono_gprs_get_data(gprs);
	struct cb_data *cbd = cb_data_new(cb, data);
	struct mbim_message *message;

	DBG("");

	message = mbim_message_new(mbim_uuid_basic_connect,
					MBIM_CID_PACKET_SERVICE,
					MBIM_COMMAND_TYPE_SET);
	/*
	 * MBIMPacketServiceActionAttach (0) or
	 * MBIMPacketServiceActionDetach (1)
	 */
	mbim_message_set_arguments(message, "u", attached ? 0 : 1);

	if (mbim_device_send(gd->device, GPRS_GROUP, message,
				mbim_packet_service_set_cb, cbd, l_free) > 0)
		return;

	l_free(cbd);
	mbim_message_unref(message);
	CALLBACK_WITH_FAILURE(cb, data);
}

static void mbim_packet_service_query_cb(struct mbim_message *message,
								void *user)
{
	struct cb_data *cbd = user;
	ofono_gprs_status_cb_t cb = cbd->cb;
	uint32_t dummy;
	uint32_t state;

	DBG("%u", mbim_message_get_error(message));

	if (mbim_message_get_error(message) != 0)
		goto error;

	if (!mbim_message_get_arguments(message, "uu", &dummy, &state))
		goto error;

	if (state == 2)
		CALLBACK_WITH_SUCCESS(cb,
					NETWORK_REGISTRATION_STATUS_REGISTERED,
					cbd->data);
	else
		CALLBACK_WITH_SUCCESS(cb, NETWORK_REGISTRATION_STATUS_UNKNOWN,
					cbd->data);

	return;

error:
	CALLBACK_WITH_FAILURE(cb, -1, cbd->data);
}

static void mbim_gprs_registration_status(struct ofono_gprs *gprs,
					ofono_gprs_status_cb_t cb,
					void *data)
{
	struct gprs_data *gd = ofono_gprs_get_data(gprs);
	struct cb_data *cbd = cb_data_new(cb, data);
	struct mbim_message *message;

	DBG("");

	message = mbim_message_new(mbim_uuid_basic_connect,
					MBIM_CID_PACKET_SERVICE,
					MBIM_COMMAND_TYPE_QUERY);
	mbim_message_set_arguments(message, "");

	if (mbim_device_send(gd->device, GPRS_GROUP, message,
				mbim_packet_service_query_cb, cbd, l_free) > 0)
		return;

	l_free(cbd);
	mbim_message_unref(message);
	CALLBACK_WITH_FAILURE(cb, -1, data);
}

static void mbim_packet_service_changed(struct mbim_message *message,
								void *user)
{
	struct ofono_gprs *gprs = user;
	uint32_t nw_error;
	uint32_t packet_service_state;
	uint32_t highest_avail_data_class;
	uint64_t uplink_speed;
	uint64_t downlink_speed;

	DBG("");

	if (!mbim_message_get_arguments(message, "uuutt",
						&nw_error,
						&packet_service_state,
						&highest_avail_data_class,
						&uplink_speed,
						&downlink_speed))
		return;

	DBG("uplink: %"PRIu64", downlink: %"PRIu64,
					uplink_speed, downlink_speed);
	DBG("nw_error: %u", nw_error);

	if (packet_service_state == 2) {
		uint32_t bearer =
			mbim_data_class_to_tech(highest_avail_data_class);

		ofono_gprs_status_notify(gprs,
			NETWORK_REGISTRATION_STATUS_REGISTERED);
		ofono_gprs_bearer_notify(gprs, bearer);
	} else
		ofono_gprs_status_notify(gprs,
				NETWORK_REGISTRATION_STATUS_UNKNOWN);
}

static void provisioned_contexts_query_cb(struct mbim_message *message,
								void *user)
{
	struct mbim_message_iter contexts;
	uint32_t n_contexts;
	uint32_t id;
	uint8_t type[16];
	char *apn;
	char *username;
	char *password;
	uint32_t compression;
	uint32_t auth_protocol;

	DBG("");

	if (mbim_message_get_error(message) != 0)
		return;

	if (!mbim_message_get_arguments(message, "a(u16ysssuu)",
						&n_contexts, &contexts))
		return;

	DBG("n_contexts: %u", n_contexts);

	while (mbim_message_iter_next_entry(&contexts, &id, type, &apn,
						&username, &password,
						&compression, &auth_protocol)) {
		char uuidstr[37];

		l_uuid_to_string(type, uuidstr, sizeof(uuidstr));
		DBG("id: %u, type: %s", id, uuidstr);
		DBG("apn: %s, username: %s, password: %s",
			apn, username, password);
		DBG("compression: %u, auth_protocol: %u",
			compression, auth_protocol);

		l_free(apn);
		l_free(username);
		l_free(password);
	}
}

static void delayed_register(struct l_idle *idle, void *user_data)
{
	struct ofono_gprs *gprs = user_data;
	struct gprs_data *gd = ofono_gprs_get_data(gprs);
	struct mbim_message *message;

	DBG("");

	l_idle_remove(idle);
	gd->delayed_register = NULL;

	/* Query provisioned contexts for debugging purposes only */
	message = mbim_message_new(mbim_uuid_basic_connect,
					MBIM_CID_PROVISIONED_CONTEXTS,
					MBIM_COMMAND_TYPE_QUERY);
	mbim_message_set_arguments(message, "");
	mbim_device_send(gd->device, 0, message,
				provisioned_contexts_query_cb, gprs, NULL);

	if (!mbim_device_register(gd->device, GPRS_GROUP,
					mbim_uuid_basic_connect,
					MBIM_CID_PACKET_SERVICE,
					mbim_packet_service_changed,
					gprs, NULL))
		goto error;

	ofono_gprs_register(gprs);
	return;

error:
	ofono_gprs_remove(gprs);
}

static int mbim_gprs_probe(struct ofono_gprs *gprs, unsigned int vendor,
								void *data)
{
	struct mbim_device *device = data;
	struct gprs_data *gd;

	DBG("");

	gd = l_new(struct gprs_data, 1);
	gd->device = mbim_device_ref(device);
	gd->delayed_register = l_idle_create(delayed_register, gprs, NULL);

	ofono_gprs_set_data(gprs, gd);

	return 0;
}

static void mbim_gprs_remove(struct ofono_gprs *gprs)
{
	struct gprs_data *gd = ofono_gprs_get_data(gprs);

	DBG("");

	ofono_gprs_set_data(gprs, NULL);

	l_idle_remove(gd->delayed_register);
	mbim_device_cancel_group(gd->device, GPRS_GROUP);
	mbim_device_unregister_group(gd->device, GPRS_GROUP);
	mbim_device_unref(gd->device);
	gd->device = NULL;
	l_free(gd);
}

static const struct ofono_gprs_driver driver = {
	.name			= "mbim",
	.probe			= mbim_gprs_probe,
	.remove			= mbim_gprs_remove,
	.set_attached		= mbim_gprs_set_attached,
	.attached_status	= mbim_gprs_registration_status,
};

void mbim_gprs_init(void)
{
	ofono_gprs_driver_register(&driver);
}

void mbim_gprs_exit(void)
{
	ofono_gprs_driver_unregister(&driver);
}
