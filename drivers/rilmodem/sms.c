/*
 *
 *  oFono - Open Source Telephony - RIL Modem Support
 *
 *  Copyright (C) 2008-2011  Intel Corporation. All rights reserved.
 *  Copyright (C) 2012-2013 Canonical Ltd.
 *  Copyright (C) 2013 Jolla Ltd.
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
#include <ctype.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

#include <glib.h>
#include <gril.h>
#include <parcel.h>

#include <ofono/log.h>
#include <ofono/modem.h>
#include <ofono/sms.h>

#include "smsutil.h"
#include "util.h"
#include "common.h"

#include "rilmodem.h"

struct sms_data {
	GRil *ril;
	unsigned int vendor;
};

static void ril_csca_set_cb(struct ril_msg *message, gpointer user_data)
{
	struct cb_data *cbd = user_data;
	ofono_sms_sca_set_cb_t cb = cbd->cb;
	struct sms_data *sd = cbd->user;

	if (message->error == RIL_E_SUCCESS) {
		CALLBACK_WITH_SUCCESS(cb, cbd->data);
	} else {
		ofono_error("%s RILD reply failure: %s",
			g_ril_request_id_to_string(sd->ril, message->req),
			ril_error_to_string(message->error));
		CALLBACK_WITH_FAILURE(cb, cbd->data);
	}
}

static void ril_csca_set(struct ofono_sms *sms,
			const struct ofono_phone_number *sca,
			ofono_sms_sca_set_cb_t cb, void *user_data)
{
	struct sms_data *sd = ofono_sms_get_data(sms);
	struct cb_data *cbd = cb_data_new(cb, user_data, sd);
	struct parcel rilp;
	char number[OFONO_MAX_PHONE_NUMBER_LENGTH + 4];

	snprintf(number, sizeof(number), "\"%s\"", phone_number_to_string(sca));

	parcel_init(&rilp);
	parcel_w_string(&rilp, number);

	g_ril_append_print_buf(sd->ril, "(%s)", number);

	if (g_ril_send(sd->ril, RIL_REQUEST_SET_SMSC_ADDRESS, &rilp,
			ril_csca_set_cb, cbd, g_free) > 0)
		return;

	g_free(cbd);
	CALLBACK_WITH_FAILURE(cb, user_data);
}

static void ril_csca_query_cb(struct ril_msg *message, gpointer user_data)
{
	struct cb_data *cbd = user_data;
	ofono_sms_sca_query_cb_t cb = cbd->cb;
	struct sms_data *sd = cbd->user;
	struct ofono_phone_number sca;
	struct parcel rilp;
	char *temp_buf;
	char *number;

	if (message->error != RIL_E_SUCCESS)
		goto error;

	g_ril_init_parcel(message, &rilp);

	temp_buf = parcel_r_string(&rilp);
	if (temp_buf == NULL)
		goto error;

	/* RIL gives address in quotes */
	number = strtok(temp_buf, "\"");
	if (number == NULL || *number == '\0') {
		g_free(temp_buf);
		goto error;
	}

	if (number[0] == '+') {
		number = number + 1;
		sca.type = OFONO_NUMBER_TYPE_INTERNATIONAL;
	} else
		sca.type = OFONO_NUMBER_TYPE_UNKNOWN;

	strncpy(sca.number, number, OFONO_MAX_PHONE_NUMBER_LENGTH);
	sca.number[OFONO_MAX_PHONE_NUMBER_LENGTH] = '\0';

	g_ril_append_print_buf(sd->ril, "{type=%d,number=%s}",
				sca.type, sca.number);
	g_ril_print_response(sd->ril, message);

	g_free(temp_buf);

	CALLBACK_WITH_SUCCESS(cb, &sca, cbd->data);
	return;

error:
	CALLBACK_WITH_FAILURE(cb, NULL, cbd->data);
}

static void ril_csca_query(struct ofono_sms *sms, ofono_sms_sca_query_cb_t cb,
					void *user_data)
{
	struct sms_data *sd = ofono_sms_get_data(sms);
	struct cb_data *cbd = cb_data_new(cb, user_data, sd);

	DBG("Sending csca_query");

	if (g_ril_send(sd->ril, RIL_REQUEST_GET_SMSC_ADDRESS, NULL,
			ril_csca_query_cb, cbd, g_free) == 0) {
		g_free(cbd);
		CALLBACK_WITH_FAILURE(cb, NULL, user_data);
	}
}

static void ril_submit_sms_cb(struct ril_msg *message, gpointer user_data)
{
	struct cb_data *cbd = user_data;
	ofono_sms_submit_cb_t cb = cbd->cb;
	struct sms_data *sd = cbd->user;
	struct parcel rilp;
	int mr;
	char *ack_pdu;
	int error;

	if (message->error != RIL_E_SUCCESS) {
		CALLBACK_WITH_FAILURE(cb, 0, cbd->data);
		return;
	}

	g_ril_init_parcel(message, &rilp);

	/*
	 * TP-Message-Reference for GSM/
	 * BearerData MessageId for CDMA
	 */
	mr = parcel_r_int32(&rilp);
	ack_pdu = parcel_r_string(&rilp);
	error = parcel_r_int32(&rilp);

	g_ril_append_print_buf(sd->ril, "{%d,%s,%d}", mr, ack_pdu, error);
	g_ril_print_response(sd->ril, message);
	g_free(ack_pdu);

	CALLBACK_WITH_SUCCESS(cb, mr, cbd->data);
}

static void imc_sms_bearer_query_cb(struct ril_msg *message,
					gpointer user_data)
{
	struct cb_data *cbd = user_data;
	ofono_sms_bearer_query_cb_t cb = cbd->cb;
	struct parcel rilp;
	int bearer;
	char **strv = NULL;
	char *endptr;

	DBG("");

	if (message->error != RIL_E_SUCCESS) {
		ofono_error("Reply failure: %s",
				ril_error_to_string(message->error));
		goto error;
	}

	/*
	 * OEM_HOOK_STRINGS response is a char**, representing
	 * an array of null-terminated UTF-8 strings.
	 */
	g_ril_init_parcel(message, &rilp);
	strv = parcel_r_strv(&rilp);

	if (strv == NULL) {
		ofono_error("%s: malformed parcel", __func__);
		goto error;
	}

	bearer = strtoul(strv[0], &endptr, 10);	/* convert to int */

	if (endptr == strv[0] || *endptr != '\0') {
		ofono_error("Convert to Int failed");
		goto error;
	}

	g_strfreev(strv);

	CALLBACK_WITH_SUCCESS(cb, bearer, cbd->data);
	return;
error:

	if(strv != NULL)
		g_strfreev(strv);

	CALLBACK_WITH_FAILURE(cb, -1, cbd->data);
}

static void ril_sms_bearer_query(struct ofono_sms *sms,
				ofono_sms_bearer_query_cb_t cb, void *user_data)
{
	struct sms_data *sd = ofono_sms_get_data(sms);
	struct cb_data *cbd = cb_data_new(cb, user_data, sd);
	struct parcel rilp;
	int cmd_id;
	char buf[4];

	DBG("");

	if (sd->vendor == OFONO_RIL_VENDOR_IMC_SOFIA3GR) {
		/*
		 * OEM_HOOK_STRINGS request is a char **, representing an array
		 * of null-terminated UTF-8 strings. Here just cmd_id as string.
		 */
		parcel_init(&rilp);
		parcel_w_int32(&rilp, 1);	/* No. of strings */

		/* RIL_OEM_HOOK_STRING_GET_SMS_TRANSPORT_MODE = 0x000000A9 */
		cmd_id = 0x000000A9;
		sprintf(buf, "%d", cmd_id);
		parcel_w_string(&rilp, buf);

		if (g_ril_send(sd->ril, RIL_REQUEST_OEM_HOOK_STRINGS, &rilp,
					imc_sms_bearer_query_cb,
					cbd, g_free) > 0)
			return;
	}

	g_free(cbd);
	CALLBACK_WITH_FAILURE(cb, -1, user_data);
}

static void imc_set_domain_pref_cb(struct ril_msg *message, void *user_data)
{
	struct cb_data *cbd = user_data;
	ofono_sms_bearer_set_cb_t cb = cbd->cb;
	struct sms_data *sd = cbd->user;

	DBG("");

	if (message->error != RIL_E_SUCCESS) {
		ofono_error("%s RILD reply failure: %s",
				g_ril_request_id_to_string(sd->ril, message->req),
				ril_error_to_string(message->error));
		CALLBACK_WITH_FAILURE(cb, cbd->data);
		return;
	}

	CALLBACK_WITH_SUCCESS(cb, cbd->data);
}

static void ril_sms_bearer_set(struct ofono_sms *sms, int bearer,
				ofono_sms_bearer_set_cb_t cb, void *user_data)
{
	struct sms_data *sd = ofono_sms_get_data(sms);
	struct cb_data *cbd = cb_data_new(cb, user_data, sd);
	struct parcel rilp;
	int cmd_id;
	char buf1[4];
	char buf2[4];

	DBG("Bearer: %d", bearer);

	if (sd->vendor == OFONO_RIL_VENDOR_IMC_SOFIA3GR) {
		/*
		 * OEM_HOOK_STRINGS request is a char **, representing an array
		 * of null-terminated UTF-8 strings. Here cmd_id and domain
		 * to be sent as strings.
		 */
		parcel_init(&rilp);
		parcel_w_int32(&rilp, 2);	/* no. of strings */

		/* RIL_OEM_HOOK_STRING_SET_SMS_TRANSPORT_MODE = 0x000000AA */
		cmd_id = 0x000000AA;
		sprintf(buf1, "%d", cmd_id);
		parcel_w_string(&rilp, buf1);
		sprintf(buf2, "%d", bearer);
		parcel_w_string(&rilp, buf2);

		if (g_ril_send(sd->ril, RIL_REQUEST_OEM_HOOK_STRINGS, &rilp,
					imc_set_domain_pref_cb,
					cbd, g_free) > 0)
			return;
	}

	g_free(cbd);
	CALLBACK_WITH_FAILURE(cb, user_data);
}

static void ril_cmgs(struct ofono_sms *sms, const unsigned char *pdu,
			int pdu_len, int tpdu_len, int mms,
			ofono_sms_submit_cb_t cb, void *user_data)
{
	struct sms_data *sd = ofono_sms_get_data(sms);
	struct cb_data *cbd = cb_data_new(cb, user_data, sd);
	struct parcel rilp;
	int smsc_len;
	char hexbuf[tpdu_len * 2 + 1];

	DBG("pdu_len: %d, tpdu_len: %d mms: %d", pdu_len, tpdu_len, mms);

	/* TODO: if (mms) { ... } */

	parcel_init(&rilp);
	parcel_w_int32(&rilp, 2);	/* Number of strings */

	/*
	 * SMSC address:
	 *
	 * smsc_len == 1, then zero-length SMSC was spec'd
	 * RILD expects a NULL string in this case instead
	 * of a zero-length string.
	 */
	smsc_len = pdu_len - tpdu_len;
	/* TODO: encode SMSC & write to parcel */
	if (smsc_len > 1)
		ofono_error("SMSC address specified (smsc_len %d); "
				"NOT-IMPLEMENTED", smsc_len);

	parcel_w_string(&rilp, NULL); /* SMSC address; NULL == default */

	/*
	 * TPDU:
	 *
	 * 'pdu' is a raw hexadecimal string
	 *  encode_hex() turns it into an ASCII/hex UTF8 buffer
	 *  parcel_w_string() encodes utf8 -> utf16
	 */
	encode_hex_own_buf(pdu + smsc_len, tpdu_len, 0, hexbuf);
	parcel_w_string(&rilp, hexbuf);

	g_ril_append_print_buf(sd->ril, "(%s)", hexbuf);

	if (g_ril_send(sd->ril, RIL_REQUEST_SEND_SMS, &rilp,
			ril_submit_sms_cb, cbd, g_free) > 0)
		return;

	g_free(cbd);
	CALLBACK_WITH_FAILURE(cb, -1, user_data);
}

static void ril_ack_delivery_cb(struct ril_msg *message, gpointer user_data)
{
	if (message->error != RIL_E_SUCCESS)
		ofono_error("SMS acknowledgement failed: "
				"Further SMS reception is not guaranteed");
}

static void ril_ack_delivery(struct ofono_sms *sms)
{
	struct sms_data *sd = ofono_sms_get_data(sms);
	struct parcel rilp;

	parcel_init(&rilp);
	parcel_w_int32(&rilp, 2); /* Number of int32 values in array */
	parcel_w_int32(&rilp, 1); /* Successful receipt */
	parcel_w_int32(&rilp, 0); /* error code */

	g_ril_append_print_buf(sd->ril, "(1,0)");

	/* TODO: should ACK be sent for either of the error cases? */

	/* ACK the incoming NEW_SMS */
	g_ril_send(sd->ril, RIL_REQUEST_SMS_ACKNOWLEDGE, &rilp,
			ril_ack_delivery_cb, NULL, NULL);
}

static void ril_sms_notify(struct ril_msg *message, gpointer user_data)
{
	struct ofono_sms *sms = user_data;
	struct sms_data *sd = ofono_sms_get_data(sms);
	unsigned int smsc_len;
	long ril_buf_len;
	struct parcel rilp;
	char *ril_pdu;
	size_t ril_pdu_len;
	unsigned char pdu[176];

	DBG("req: %d; data_len: %d", message->req, (int) message->buf_len);

	g_ril_init_parcel(message, &rilp);

	ril_pdu = parcel_r_string(&rilp);
	if (ril_pdu == NULL)
		return;

	g_ril_append_print_buf(sd->ril, "{%s}", ril_pdu);
	g_ril_print_unsol(sd->ril, message);

	ril_pdu_len = strlen(ril_pdu);

	if (ril_pdu_len > sizeof(pdu) * 2)
		goto fail;

	if (decode_hex_own_buf(ril_pdu, ril_pdu_len,
					&ril_buf_len, -1, pdu) == NULL)
		goto fail;

	/*
	 * The first octect in the pdu contains the SMSC address length
	 * which is the X following octects it reads. We add 1 octet to
	 * the read length to take into account this read octet in order
	 * to calculate the proper tpdu length.
	 */
	smsc_len = pdu[0] + 1;
	DBG("smsc_len is %d", smsc_len);

	if (message->req == RIL_UNSOL_RESPONSE_NEW_SMS)
		/* Last parameter is 'tpdu_len' ( substract SMSC length ) */
		ofono_sms_deliver_notify(sms, pdu, ril_buf_len,
						ril_buf_len - smsc_len);
	else if (message->req == RIL_UNSOL_RESPONSE_NEW_SMS_STATUS_REPORT)
		ofono_sms_status_notify(sms, pdu, ril_buf_len,
						ril_buf_len - smsc_len);

	/* ACK the incoming NEW_SMS */
	ril_ack_delivery(sms);

fail:
	g_free(ril_pdu);
}

static gboolean ril_delayed_register(gpointer user_data)
{
	struct ofono_sms *sms = user_data;
	struct sms_data *data = ofono_sms_get_data(sms);

	DBG("");
	ofono_sms_register(sms);

	g_ril_register(data->ril, RIL_UNSOL_RESPONSE_NEW_SMS,
			ril_sms_notify,	sms);
	g_ril_register(data->ril, RIL_UNSOL_RESPONSE_NEW_SMS_STATUS_REPORT,
			ril_sms_notify, sms);

	return FALSE;
}

static int ril_sms_probe(struct ofono_sms *sms, unsigned int vendor,
				void *user)
{
	GRil *ril = user;
	struct sms_data *data;

	data = g_new0(struct sms_data, 1);
	data->ril = g_ril_clone(ril);
	data->vendor = vendor;

	ofono_sms_set_data(sms, data);

	g_idle_add(ril_delayed_register, sms);

	return 0;
}

static void ril_sms_remove(struct ofono_sms *sms)
{
	struct sms_data *data = ofono_sms_get_data(sms);

	DBG("");

	g_ril_unref(data->ril);
	g_free(data);

	ofono_sms_set_data(sms, NULL);
}

static struct ofono_sms_driver driver = {
	.name		= RILMODEM,
	.probe		= ril_sms_probe,
	.sca_query	= ril_csca_query,
	.sca_set	= ril_csca_set,
	.remove		= ril_sms_remove,
	.submit		= ril_cmgs,
	.bearer_query   = ril_sms_bearer_query,
	.bearer_set	= ril_sms_bearer_set
};

void ril_sms_init(void)
{
	ofono_sms_driver_register(&driver);
}

void ril_sms_exit(void)
{
	ofono_sms_driver_unregister(&driver);
}
