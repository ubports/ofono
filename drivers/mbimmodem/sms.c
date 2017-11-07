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

#define _GNU_SOURCE
#include <string.h>
#include <errno.h>
#include <stdint.h>
#include <stdbool.h>

#include <ofono/log.h>
#include <ofono/modem.h>
#include <ofono/sms.h>
#include "common.h"

#include "drivers/mbimmodem/mbim.h"
#include "drivers/mbimmodem/mbim-message.h"
#include "drivers/mbimmodem/mbimmodem.h"

struct sms_data {
	struct mbim_device *device;
	uint32_t configuration_notify_id;
};

static void mbim_sca_set_cb(struct mbim_message *message, void *user)
{
	struct cb_data *cbd = user;
	ofono_sms_sca_set_cb_t cb = cbd->cb;

	DBG("");

	if (mbim_message_get_error(message) != 0)
		CALLBACK_WITH_FAILURE(cb, cbd->data);
	else
		CALLBACK_WITH_SUCCESS(cb, cbd->data);
}

static void mbim_sca_set(struct ofono_sms *sms,
			const struct ofono_phone_number *sca,
			ofono_sms_sca_set_cb_t cb, void *data)
{
	struct sms_data *sd = ofono_sms_get_data(sms);
	struct cb_data *cbd = cb_data_new(cb, data);
	struct mbim_message *message;
	const char *numberstr = phone_number_to_string(sca);

	message = mbim_message_new(mbim_uuid_sms,
					MBIM_CID_SMS_CONFIGURATION,
					MBIM_COMMAND_TYPE_SET);
	mbim_message_set_arguments(message, "us", 0, numberstr);

	if (mbim_device_send(sd->device, SMS_GROUP, message,
				mbim_sca_set_cb, cbd, l_free) > 0)
		return;

	l_free(cbd);
	mbim_message_unref(message);
	CALLBACK_WITH_FAILURE(cb, data);
}

static void mbim_sca_query_cb(struct mbim_message *message, void *user)
{
	struct cb_data *cbd = user;
	ofono_sms_sca_query_cb_t cb = cbd->cb;
	struct ofono_phone_number sca;
	uint32_t dummy;
	L_AUTO_FREE_VAR(char *, number) = NULL;
	const char *p;

	if (mbim_message_get_error(message) != 0)
		goto error;

	if (!mbim_message_get_arguments(message, "uuuus",
					&dummy, &dummy, &dummy, &dummy,
					&number))
		goto error;

	if (number[0] == '+') {
		p = number + 1;
		sca.type = 145;
	} else {
		p = number;
		sca.type = 129;
	}

	strncpy(sca.number, p, OFONO_MAX_PHONE_NUMBER_LENGTH);
	sca.number[OFONO_MAX_PHONE_NUMBER_LENGTH] = '\0';
	CALLBACK_WITH_SUCCESS(cb, &sca, cbd->data);
	return;

error:
	CALLBACK_WITH_FAILURE(cb, NULL, cbd->data);
}

static void mbim_sca_query(struct ofono_sms *sms, ofono_sms_sca_query_cb_t cb,
					void *data)
{
	struct sms_data *sd = ofono_sms_get_data(sms);
	struct cb_data *cbd = cb_data_new(cb, data);
	struct mbim_message *message;

	message = mbim_message_new(mbim_uuid_sms,
					MBIM_CID_SMS_CONFIGURATION,
					MBIM_COMMAND_TYPE_QUERY);
	mbim_message_set_arguments(message, "");

	if (mbim_device_send(sd->device, SMS_GROUP, message,
				mbim_sca_query_cb, cbd, l_free) > 0)
		return;

	l_free(cbd);
	mbim_message_unref(message);
	CALLBACK_WITH_FAILURE(cb, NULL, data);
}

static void mbim_delete_cb(struct mbim_message *message, void *user)
{
	DBG("%u", mbim_message_get_error(message));
}

static void mbim_sms_send_cb(struct mbim_message *message, void *user)
{
	struct cb_data *cbd = user;
	struct sms_data *sd = cbd->user;
	ofono_sms_submit_cb_t cb = cbd->cb;
	uint32_t mr;
	struct mbim_message *delete;

	DBG("%u", mbim_message_get_error(message));

	if (mbim_message_get_error(message) != 0)
		goto error;

	if (!mbim_message_get_arguments(message, "u", &mr))
		goto error;

	/* Just in case, send an SMS DELETE command for Sent messages */
	delete = mbim_message_new(mbim_uuid_sms,
					MBIM_CID_SMS_DELETE,
					MBIM_COMMAND_TYPE_SET);
	mbim_message_set_arguments(delete, "uu", 4, 0);

	if (!mbim_device_send(sd->device, SMS_GROUP, delete,
				mbim_delete_cb, NULL, NULL))
		mbim_message_unref(delete);

	CALLBACK_WITH_SUCCESS(cb, mr, cbd->data);
	return;

error:
	CALLBACK_WITH_FAILURE(cb, -1, cbd->data);
}

static void mbim_submit(struct ofono_sms *sms, const unsigned char *pdu,
			int pdu_len, int tpdu_len, int mms,
			ofono_sms_submit_cb_t cb, void *data)
{
	struct sms_data *sd = ofono_sms_get_data(sms);
	struct cb_data *cbd = cb_data_new(cb, data);
	struct mbim_message *message;

	DBG("pdu_len: %d tpdu_len: %d mms: %d", pdu_len, tpdu_len, mms);

	cbd->user = sd;

	message = mbim_message_new(mbim_uuid_sms,
					MBIM_CID_SMS_SEND,
					MBIM_COMMAND_TYPE_SET);
	mbim_message_set_arguments(message, "ud", 0, "ay", pdu_len, pdu);

	if (mbim_device_send(sd->device, SMS_GROUP, message,
				mbim_sms_send_cb, cbd, l_free) > 0)
		return;

	l_free(cbd);
	mbim_message_unref(message);
	CALLBACK_WITH_FAILURE(cb, -1, data);
}

static void mbim_sms_send_delete(struct sms_data *sd, uint32_t index)
{
	struct mbim_message *delete;

	DBG("%u", index);

	delete = mbim_message_new(mbim_uuid_sms,
					MBIM_CID_SMS_DELETE,
					MBIM_COMMAND_TYPE_SET);
	mbim_message_set_arguments(delete, "uu", 1, index);

	if (!mbim_device_send(sd->device, SMS_GROUP, delete,
				mbim_delete_cb, NULL, NULL))
		mbim_message_unref(delete);
}

static void mbim_parse_sms_read_info(struct mbim_message *message,
							struct ofono_sms *sms)
{
	struct sms_data *sd = ofono_sms_get_data(sms);
	uint32_t format;
	uint32_t n_sms;
	struct mbim_message_iter array;
	struct mbim_message_iter bytes;
	uint32_t index;
	uint32_t status;
	uint32_t pdu_len;

	if (!mbim_message_get_arguments(message, "ua(uuay)",
						&format, &n_sms, &array))
		return;

	if (format != 0)
		return;

	while (mbim_message_iter_next_entry(&array, &index, &status,
							&pdu_len, &bytes)) {
		int i = 0;

		/* Ignore Draft (2) and Sent (3) messages */
		if (status == 0 || status == 1) {
			uint8_t pdu[176];
			uint32_t tpdu_len;

			while (mbim_message_iter_next_entry(&bytes, pdu + i))
				i++;

			tpdu_len = pdu_len - pdu[0] - 1;
			ofono_sms_deliver_notify(sms, pdu, pdu_len, tpdu_len);
		}

		mbim_sms_send_delete(sd, index);
	}
}

static void mbim_sms_read_notify(struct mbim_message *message, void *user)
{
	struct ofono_sms *sms = user;

	DBG("");

	mbim_parse_sms_read_info(message, sms);
}

static void mbim_sms_read_new_query_cb(struct mbim_message *message, void *user)
{
	struct ofono_sms *sms = user;

	DBG("");

	mbim_parse_sms_read_info(message, sms);
}

static void mbim_sms_message_store_status_changed(struct mbim_message *message,
								void *user)
{
	struct ofono_sms *sms = user;
	struct sms_data *sd = ofono_sms_get_data(sms);
	uint32_t flag;
	uint32_t index;
	struct mbim_message *read_query;

	DBG("");

	if (!mbim_message_get_arguments(message, "uu", &flag, &index))
		return;

	DBG("%u %u", flag, index);

	/* MBIM_SMS_FLAG_NEW_MESSAGE not set */
	if ((flag & 2) == 0)
		return;

	read_query = mbim_message_new(mbim_uuid_sms,
					MBIM_CID_SMS_READ,
					MBIM_COMMAND_TYPE_QUERY);
	if (!read_query)
		return;

	/* Query using MBIMSmsFormatPdu(0) and MBIMSmsFlagNew (2) */
	mbim_message_set_arguments(read_query, "uuu", 0, 2, 0);

	if (!mbim_device_send(sd->device, SMS_GROUP, read_query,
				mbim_sms_read_new_query_cb, sms, NULL))
		mbim_message_unref(read_query);
}

static void mbim_sms_read_all_query_cb(struct mbim_message *message, void *user)
{
	struct ofono_sms *sms = user;
	struct sms_data *sd = ofono_sms_get_data(sms);

	DBG("");

	mbim_parse_sms_read_info(message, sms);

	mbim_device_register(sd->device, SMS_GROUP, mbim_uuid_sms,
				MBIM_CID_SMS_MESSAGE_STORE_STATUS,
				mbim_sms_message_store_status_changed,
				sms, NULL);
}

static bool mbim_sms_finish_init(struct ofono_sms *sms)
{
	struct sms_data *sd = ofono_sms_get_data(sms);
	struct mbim_message *message;

	/*
	 * Class 0 SMS comes via SMS_READ notification, so register for these
	 * here.  After that we send an SMS_READ request to retrieve any new
	 * SMS messages.  In the callback we will register to
	 * MESSAGE_STORE_STATUS to receive notification that new SMS messages
	 * have arrived
	 */
	if (!mbim_device_register(sd->device, SMS_GROUP,
					mbim_uuid_sms,
					MBIM_CID_SMS_READ,
					mbim_sms_read_notify, sms, NULL))
		return false;

	message = mbim_message_new(mbim_uuid_sms,
					MBIM_CID_SMS_READ,
					MBIM_COMMAND_TYPE_QUERY);
	if (!message)
		return false;

	/* Query using MBIMSmsFormatPdu(0) and MBIMSmsFlagAll (0) */
	mbim_message_set_arguments(message, "uuu", 0, 0, 0);

	if (!mbim_device_send(sd->device, SMS_GROUP, message,
				mbim_sms_read_all_query_cb, sms, NULL)) {
		mbim_message_unref(message);
		return false;
	}

	return true;
}

static void mbim_sms_configuration_changed(struct mbim_message *message,
								void *user)
{
	struct ofono_sms *sms = user;
	struct sms_data *sd = ofono_sms_get_data(sms);
	uint32_t storage_state;

	DBG("");

	if (!mbim_message_get_arguments(message, "u", &storage_state))
		goto error;

	if (storage_state != 1)
		return;

	mbim_device_unregister(sd->device, sd->configuration_notify_id);
	sd->configuration_notify_id = 0;

	if (!mbim_sms_finish_init(sms))
		goto error;

	ofono_sms_register(sms);
	return;

error:
	ofono_sms_remove(sms);
}

static void mbim_sms_configuration_query_cb(struct mbim_message *message,
								void *user)
{
	struct ofono_sms *sms = user;
	struct sms_data *sd = ofono_sms_get_data(sms);
	uint32_t error;
	uint32_t storage_state;
	uint32_t format;
	uint32_t max_messages;

	DBG("");

	error = mbim_message_get_error(message);

	/*
	 * SUBSCRIBER_READY_STATUS tells us that a SIM is in ReadyState,
	 * unfortunately that seems to be not enough to know that the SMS
	 * state is initialized.  Handle this here, if we get an error 14
	 * 'MBIM_STATUS_NOT_INITIALIZED', then listen for the
	 * SMS_CONFIGURATION notification.  Why some devices return an error
	 * here instead of responding with a 0 storage state is a mystery
	 */
	switch (error) {
	case 14: /* Seems SIM ReadyState is sometimes not enough */
		goto setup_notification;
	case 0:
		break;
	default:
		goto error;
	}

	/* We don't bother parsing CdmaShortMessageSize or ScAddress array */
	if (!mbim_message_get_arguments(message, "uuu",
					&storage_state, &format, &max_messages))
		goto error;

	DBG("storage_state: %u, format: %u, max_messages: %u",
			storage_state, format, max_messages);

	if (format != 0) {
		DBG("Unsupported SMS Format, expect 0 (PDU)");
		goto error;
	}

	if (storage_state == 1) {
		if (!mbim_sms_finish_init(sms))
			goto error;

		ofono_sms_register(sms);
		return;
	}

setup_notification:
	/* Wait for storage_state to go to Initialized before registering */
	sd->configuration_notify_id = mbim_device_register(sd->device,
						SMS_GROUP,
						mbim_uuid_sms,
						MBIM_CID_SMS_CONFIGURATION,
						mbim_sms_configuration_changed,
						sms, NULL);
	if (sd->configuration_notify_id > 0)
		return;

error:
	ofono_sms_remove(sms);
}

static int mbim_sms_probe(struct ofono_sms *sms, unsigned int vendor,
							void *data)
{
	struct mbim_device *device = data;
	struct sms_data *sd;
	struct mbim_message *message;

	DBG("");

	message = mbim_message_new(mbim_uuid_sms,
					MBIM_CID_SMS_CONFIGURATION,
					MBIM_COMMAND_TYPE_QUERY);
	if (!message)
		return -ENOMEM;

	mbim_message_set_arguments(message, "");

	if (!mbim_device_send(device, SMS_GROUP, message,
				mbim_sms_configuration_query_cb, sms, NULL)) {
		mbim_message_unref(message);
		return -EIO;
	}

	sd = l_new(struct sms_data, 1);
	sd->device = mbim_device_ref(device);
	ofono_sms_set_data(sms, sd);

	return 0;
}

static void mbim_sms_remove(struct ofono_sms *sms)
{
	struct sms_data *sd = ofono_sms_get_data(sms);

	DBG("");

	ofono_sms_set_data(sms, NULL);

	mbim_device_cancel_group(sd->device, SMS_GROUP);
	mbim_device_unregister_group(sd->device, SMS_GROUP);
	mbim_device_unref(sd->device);
	sd->device = NULL;
	l_free(sd);
}

static struct ofono_sms_driver driver = {
	.name		= "mbim",
	.probe		= mbim_sms_probe,
	.remove		= mbim_sms_remove,
	.sca_query	= mbim_sca_query,
	.sca_set	= mbim_sca_set,
	.submit		= mbim_submit,
};

void mbim_sms_init(void)
{
	ofono_sms_driver_register(&driver);
}

void mbim_sms_exit(void)
{
	ofono_sms_driver_unregister(&driver);
}
