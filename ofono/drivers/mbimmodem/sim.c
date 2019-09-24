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

#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <errno.h>

#include <ofono/log.h>
#include <ofono/modem.h>
#include <ofono/sim.h>

#include "drivers/mbimmodem/mbim.h"
#include "drivers/mbimmodem/mbim-message.h"
#include "drivers/mbimmodem/mbimmodem.h"

struct sim_data {
	struct mbim_device *device;
	char *iccid;
	char *imsi;
	uint32_t last_pin_type;
	bool present : 1;
};

static void mbim_sim_state_changed(struct ofono_sim *sim, uint32_t ready_state)
{
	struct sim_data *sd = ofono_sim_get_data(sim);

	DBG("ready_state: %u", ready_state);

	switch (ready_state) {
	case 0: /* Not Initialized */
		break;
	case 1: /* Initialized */
		if (!sd->present)
			ofono_sim_inserted_notify(sim, true);

		sd->present = true;
		ofono_sim_initialized_notify(sim);
		break;
	case 6: /* Device Locked */
		if (!sd->present)
			ofono_sim_inserted_notify(sim, true);

		sd->present = true;
		break;
	case 2: /* Not inserted */
	case 3: /* Bad SIM */
	case 4: /* Failure */
	case 5: /* Not activated */
		if (sd->present)
			ofono_sim_inserted_notify(sim, false);

		sd->present = false;
		break;
	default:
		break;
	};
}

static void mbim_read_imsi(struct ofono_sim *sim,
				ofono_sim_imsi_cb_t cb, void *user_data)
{
	struct sim_data *sd = ofono_sim_get_data(sim);

	DBG("");

	CALLBACK_WITH_SUCCESS(cb, sd->imsi, user_data);
}

static enum ofono_sim_password_type mbim_pin_type_to_sim_password(
							uint32_t pin_type)
{
	switch (pin_type) {
	case 0:  /* No Pin */
		return OFONO_SIM_PASSWORD_NONE;
	case 2: /* PIN1 key */
		return OFONO_SIM_PASSWORD_SIM_PIN;
	case 3: /* PIN2 key */
		return OFONO_SIM_PASSWORD_SIM_PIN2;
	case 4: /* device to SIM key */
		return OFONO_SIM_PASSWORD_PHSIM_PIN;
	case 5: /* device to very first SIM key */
		return OFONO_SIM_PASSWORD_PHFSIM_PIN;
	case 6: /* network personalization key */
		return OFONO_SIM_PASSWORD_PHNET_PIN;
	case 7: /* network subset personalization key */
		return OFONO_SIM_PASSWORD_PHNETSUB_PIN;
	case 8: /* service provider (SP) personalization key */
		return OFONO_SIM_PASSWORD_PHSP_PIN;
	case 9: /* corporate personalization key */
		return OFONO_SIM_PASSWORD_PHCORP_PIN;
	case 11: /* PUK1 */
		return OFONO_SIM_PASSWORD_SIM_PUK;
	case 12: /* PUK2 */
		return OFONO_SIM_PASSWORD_SIM_PUK2;
	case 13: /* device to very first SIM PIN unlock key */
		return OFONO_SIM_PASSWORD_PHFSIM_PUK;
	case 14: /* network personalization unlock key */
		return OFONO_SIM_PASSWORD_PHNET_PUK;
	case 15: /* network subset personaliation unlock key */
		return OFONO_SIM_PASSWORD_PHNETSUB_PUK;
	case 16: /* service provider (SP) personalization unlock key */
		return OFONO_SIM_PASSWORD_PHSP_PUK;
	case 17: /* corporate personalization unlock key */
		return OFONO_SIM_PASSWORD_PHCORP_PUK;
	}

	return OFONO_SIM_PASSWORD_INVALID;
}

static uint32_t mbim_pin_type_from_sim_password(
					enum ofono_sim_password_type type)
{
	switch (type) {
	case OFONO_SIM_PASSWORD_SIM_PIN:
		return 2; /* PIN1 key */
	case OFONO_SIM_PASSWORD_SIM_PIN2:
		return 3; /* PIN2 key */
	case OFONO_SIM_PASSWORD_PHSIM_PIN:
		return 4; /* device to SIM key */
	case OFONO_SIM_PASSWORD_PHFSIM_PIN:
		return 5; /* device to very first SIM key */
	case OFONO_SIM_PASSWORD_PHNET_PIN:
		return 6; /* network personalization key */
	case OFONO_SIM_PASSWORD_PHNETSUB_PIN:
		return 7; /* network subset personalization key */
	case OFONO_SIM_PASSWORD_PHSP_PIN:
		return 8; /* service provider (SP) personalization key */
	case OFONO_SIM_PASSWORD_PHCORP_PIN:
		return 9; /* corporate personalization key */
	case OFONO_SIM_PASSWORD_SIM_PUK:
		return 11; /* PUK1 */
	case OFONO_SIM_PASSWORD_SIM_PUK2:
		return 12; /* PUK2 */
	case OFONO_SIM_PASSWORD_PHFSIM_PUK:
		return 13; /* device to very first SIM PIN unlock key */
	case OFONO_SIM_PASSWORD_PHNET_PUK:
		return 14; /* network personalization unlock key */
	case OFONO_SIM_PASSWORD_PHNETSUB_PUK:
		return 15; /* network subset personaliation unlock key */
	case OFONO_SIM_PASSWORD_PHSP_PUK:
		return 16; /* service provider (SP) personalization unlock key */
	case OFONO_SIM_PASSWORD_PHCORP_PUK:
		return 17; /* corporate personalization unlock key */
	case OFONO_SIM_PASSWORD_NONE:
	case OFONO_SIM_PASSWORD_INVALID:
		break;
	}

	return 0;
}

static void mbim_pin_query_cb(struct mbim_message *message, void *user)
{
	struct cb_data *cbd = user;
	struct sim_data *sd = cbd->user;
	ofono_sim_passwd_cb_t cb = cbd->cb;
	uint32_t pin_type;
	uint32_t pin_state;
	enum ofono_sim_password_type sim_password;
	bool r;

	DBG("");

	if (mbim_message_get_error(message) != 0)
		goto error;

	r = mbim_message_get_arguments(message, "uu",
					&pin_type, &pin_state);
	if (!r)
		goto error;

	sim_password = mbim_pin_type_to_sim_password(pin_type);
	if (sim_password == OFONO_SIM_PASSWORD_INVALID)
		goto error;

	if (pin_state == 0)
		sim_password = OFONO_SIM_PASSWORD_NONE;

	sd->last_pin_type = pin_type;

	CALLBACK_WITH_SUCCESS(cb, sim_password, cbd->data);
	return;

error:
	CALLBACK_WITH_FAILURE(cb, -1, cbd->data);
}

static void mbim_pin_query(struct ofono_sim *sim,
				ofono_sim_passwd_cb_t cb, void *user_data)
{
	struct sim_data *sd = ofono_sim_get_data(sim);
	struct cb_data *cbd = cb_data_new(cb, user_data);
	struct mbim_message *message;

	DBG("");

	cbd->user = sd;

	message = mbim_message_new(mbim_uuid_basic_connect,
					MBIM_CID_PIN,
					MBIM_COMMAND_TYPE_QUERY);
	mbim_message_set_arguments(message, "");

	if (mbim_device_send(sd->device, SIM_GROUP, message,
				mbim_pin_query_cb, cbd, l_free) > 0)
		return;

	l_free(cbd);
	mbim_message_unref(message);
	CALLBACK_WITH_FAILURE(cb, -1, user_data);
}

static void mbim_pin_retries_cb(struct mbim_message *message, void *user)
{
	struct cb_data *cbd = user;
	ofono_sim_pin_retries_cb_t cb = cbd->cb;
	int retries[OFONO_SIM_PASSWORD_INVALID];
	size_t i;
	uint32_t pin_type;
	uint32_t pin_state;
	uint32_t remaining;
	enum ofono_sim_password_type sim_password;
	bool r;

	DBG("");

	if (mbim_message_get_error(message) != 0)
		goto error;

	r = mbim_message_get_arguments(message, "uuu",
					&pin_type, &pin_state, &remaining);
	if (!r)
		goto error;

	sim_password = mbim_pin_type_to_sim_password(pin_type);
	if (sim_password == OFONO_SIM_PASSWORD_INVALID)
		goto error;

	for (i = 0; i < OFONO_SIM_PASSWORD_INVALID; i++)
		retries[i] = -1;

	if (pin_state == 0 || sim_password == OFONO_SIM_PASSWORD_NONE) {
		CALLBACK_WITH_SUCCESS(cb, retries, cbd->data);
		return;
	}

	if (remaining == 0xffffffff)
		retries[sim_password] = -1;
	else
		retries[sim_password] = remaining;

	CALLBACK_WITH_SUCCESS(cb, retries, cbd->data);
	return;

error:
	CALLBACK_WITH_FAILURE(cb, NULL, cbd->data);
}

static void mbim_pin_retries_query(struct ofono_sim *sim,
				ofono_sim_pin_retries_cb_t cb, void *user_data)
{
	struct sim_data *sd = ofono_sim_get_data(sim);
	struct cb_data *cbd = cb_data_new(cb, user_data);
	struct mbim_message *message;

	DBG("");

	message = mbim_message_new(mbim_uuid_basic_connect,
					MBIM_CID_PIN,
					MBIM_COMMAND_TYPE_QUERY);
	mbim_message_set_arguments(message, "");

	if (mbim_device_send(sd->device, SIM_GROUP, message,
				mbim_pin_retries_cb, cbd, l_free) > 0)
		return;

	l_free(cbd);
	mbim_message_unref(message);
	CALLBACK_WITH_FAILURE(cb, NULL, user_data);
}

static void mbim_pin_set_cb(struct mbim_message *message, void *user)
{
	struct cb_data *cbd = user;
	ofono_sim_lock_unlock_cb_t cb = cbd->cb;

	DBG("");

	if (mbim_message_get_error(message) != 0)
		CALLBACK_WITH_FAILURE(cb, cbd->data);
	else
		CALLBACK_WITH_SUCCESS(cb, cbd->data);
}

static void mbim_pin_set(struct ofono_sim *sim, uint32_t pin_type,
						uint32_t pin_operation,
						const char *old_passwd,
						const char *new_passwd,
						ofono_sim_lock_unlock_cb_t cb,
						void *data)
{
	struct sim_data *sd = ofono_sim_get_data(sim);
	struct cb_data *cbd = cb_data_new(cb, data);
	struct mbim_message *message;

	DBG("%u %u %s %s", pin_type, pin_operation, old_passwd, new_passwd);

	message = mbim_message_new(mbim_uuid_basic_connect,
					MBIM_CID_PIN,
					MBIM_COMMAND_TYPE_SET);
	mbim_message_set_arguments(message, "uuss", pin_type, pin_operation,
					old_passwd, new_passwd);

	if (mbim_device_send(sd->device, SIM_GROUP, message,
				mbim_pin_set_cb, cbd, l_free) > 0)
		return;

	l_free(cbd);
	mbim_message_unref(message);
	CALLBACK_WITH_FAILURE(cb, data);
}

static void mbim_pin_enter(struct ofono_sim *sim, const char *passwd,
				ofono_sim_lock_unlock_cb_t cb, void *data)
{
	struct sim_data *sd = ofono_sim_get_data(sim);

	/* Use MBIMPinOperationEnter (0) and NULL second PIN */
	mbim_pin_set(sim, sd->last_pin_type, 0, passwd, NULL, cb, data);
}

static void mbim_puk_enter(struct ofono_sim *sim, const char *puk,
				const char *passwd,
				ofono_sim_lock_unlock_cb_t cb, void *data)
{
	struct sim_data *sd = ofono_sim_get_data(sim);

	/* Use MBIMPinOperationEnter (0) and second PIN */
	mbim_pin_set(sim, sd->last_pin_type, 0, puk, passwd, cb, data);
}

static void mbim_pin_enable(struct ofono_sim *sim,
				enum ofono_sim_password_type passwd_type,
				int enable, const char *passwd,
				ofono_sim_lock_unlock_cb_t cb, void *data)
{
	uint32_t pin_type = mbim_pin_type_from_sim_password(passwd_type);

	if (pin_type == 0) {
		CALLBACK_WITH_FAILURE(cb, data);
		return;
	}

	/* Use MBIMPinOperationEnable (1) or MBIMPinOperationDisable (2) */
	mbim_pin_set(sim, pin_type, enable ? 1 : 2, passwd, NULL, cb, data);
}

static void mbim_pin_change(struct ofono_sim *sim,
				enum ofono_sim_password_type passwd_type,
				const char *old_passwd, const char *new_passwd,
				ofono_sim_lock_unlock_cb_t cb, void *data)
{
	uint32_t pin_type = mbim_pin_type_from_sim_password(passwd_type);

	if (pin_type == 0) {
		CALLBACK_WITH_FAILURE(cb, data);
		return;
	}

	/* Use MBIMPinOperationChange (3) */
	mbim_pin_set(sim, pin_type, 3, old_passwd, new_passwd, cb, data);
}

static void mbim_subscriber_ready_status_changed(struct mbim_message *message,
								void *user)
{
	struct ofono_sim *sim = user;
	struct sim_data *sd = ofono_sim_get_data(sim);
	uint32_t ready_state;
	char *imsi;
	char *iccid;
	uint32_t ready_info;

	DBG("");

	if (!mbim_message_get_arguments(message, "ussu",
					&ready_state, &imsi,
					&iccid, &ready_info))
		return;

	l_free(sd->iccid);
	sd->iccid = iccid;

	l_free(sd->imsi);
	sd->imsi = imsi;

	DBG("%s %s", iccid, imsi);

	mbim_sim_state_changed(sim, ready_state);
}

static void mbim_subscriber_ready_status_cb(struct mbim_message *message,
								void *user)
{
	struct ofono_sim *sim = user;
	struct sim_data *sd = ofono_sim_get_data(sim);
	uint32_t ready_state;
	char *imsi;
	char *iccid;
	uint32_t ready_info;
	bool r;

	DBG("");

	if (mbim_message_get_error(message) != 0)
		goto error;

	/* We don't bother parsing MSISDN/MDN array */
	r = mbim_message_get_arguments(message, "ussu",
					&ready_state, &imsi,
					&iccid, &ready_info);
	if (!r)
		goto error;

	sd->iccid = iccid;
	sd->imsi = imsi;

	if (!mbim_device_register(sd->device, SIM_GROUP,
					mbim_uuid_basic_connect,
					MBIM_CID_SUBSCRIBER_READY_STATUS,
					mbim_subscriber_ready_status_changed,
					sim, NULL))
		goto error;

	ofono_sim_register(sim);
	DBG("%s %s", iccid, imsi);
	mbim_sim_state_changed(sim, ready_state);
	return;

error:
	ofono_sim_remove(sim);
}

static int mbim_sim_probe(struct ofono_sim *sim, unsigned int vendor,
				void *data)
{
	struct mbim_device *device = data;
	struct mbim_message *message;
	struct sim_data *sd;

	message = mbim_message_new(mbim_uuid_basic_connect,
					MBIM_CID_SUBSCRIBER_READY_STATUS,
					MBIM_COMMAND_TYPE_QUERY);
	if (!message)
		return -ENOMEM;

	mbim_message_set_arguments(message, "");

	if (!mbim_device_send(device, SIM_GROUP, message,
				mbim_subscriber_ready_status_cb, sim, NULL)) {
		mbim_message_unref(message);
		return -EIO;
	}

	sd = l_new(struct sim_data, 1);
	sd->device = mbim_device_ref(device);
	ofono_sim_set_data(sim, sd);

	return 0;
}

static void mbim_sim_remove(struct ofono_sim *sim)
{
	struct sim_data *sd = ofono_sim_get_data(sim);

	ofono_sim_set_data(sim, NULL);

	mbim_device_cancel_group(sd->device, SIM_GROUP);
	mbim_device_unregister_group(sd->device, SIM_GROUP);
	mbim_device_unref(sd->device);
	sd->device = NULL;

	l_free(sd->iccid);
	l_free(sd->imsi);
	l_free(sd);
}

static struct ofono_sim_driver driver = {
	.name			= "mbim",
	.probe			= mbim_sim_probe,
	.remove			= mbim_sim_remove,
	.read_imsi		= mbim_read_imsi,
	.query_passwd_state	= mbim_pin_query,
	.query_pin_retries	= mbim_pin_retries_query,
	.send_passwd		= mbim_pin_enter,
	.reset_passwd		= mbim_puk_enter,
	.change_passwd		= mbim_pin_change,
	.lock			= mbim_pin_enable,
};

void mbim_sim_init(void)
{
	ofono_sim_driver_register(&driver);
}

void mbim_sim_exit(void)
{
	ofono_sim_driver_unregister(&driver);
}
