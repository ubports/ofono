/*
 *
 *  oFono - Open Source Telephony
 *
 *  Copyright (C) 2008-2016  Intel Corporation. All rights reserved.
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
#include <stdio.h>

#include <glib.h>

#include <ofono/log.h>
#include <ofono/modem.h>
#include <ofono/stk.h>
#include "util.h"

#include <gril.h>
#include <parcel.h>

#include "rilmodem.h"
#include "vendor.h"

struct stk_data {
	GRil *ril;
	unsigned int vendor;
};

static void ril_stk_terminal_response_cb(struct ril_msg *message,
				gpointer user_data)
{
	struct cb_data *cbd = user_data;
	ofono_stk_generic_cb_t cb = cbd->cb;
	struct stk_data *sd = cbd->user;

	g_ril_print_response(sd->ril, message);

	if (message->error == RIL_E_SUCCESS) {
		CALLBACK_WITH_SUCCESS(cb, cbd->data);
	} else {
		ofono_error("%s RILD reply failure: %s",
			g_ril_request_id_to_string(sd->ril, message->req),
			ril_error_to_string(message->error));
		CALLBACK_WITH_FAILURE(cb, cbd->data);
	}
}

static void ril_stk_terminal_response(struct ofono_stk *stk, int len,
				const unsigned char *data,
				ofono_stk_generic_cb_t cb, void *user_data)
{
	struct stk_data *sd = ofono_stk_get_data(stk);
	struct cb_data *cbd = cb_data_new(cb, user_data, sd);
	struct parcel rilp;
	char *buf = alloca(len * 2 + 1);
	int size = 0;

	for (; len; len--)
		size += sprintf(buf + size, "%02hhX", *data++);

	parcel_init(&rilp);
	parcel_w_string(&rilp, buf);

	if (g_ril_send(sd->ril, RIL_REQUEST_STK_SEND_TERMINAL_RESPONSE, &rilp,
			ril_stk_terminal_response_cb, cbd, g_free) > 0)
		return;

	g_free(cbd);
	CALLBACK_WITH_FAILURE(cb, user_data);
}

static void ril_stk_envelope_cb(struct ril_msg *message, gpointer user_data)
{
	struct cb_data *cbd = user_data;
	ofono_stk_envelope_cb_t cb = cbd->cb;
	struct stk_data *sd = cbd->user;
	struct parcel rilp;
	unsigned char *response = NULL;
	long len = 0;
	char *pdu;

	g_ril_print_response(sd->ril, message);

	if (message->error == RIL_E_SUCCESS) {
		g_ril_init_parcel(message, &rilp);
		pdu = parcel_r_string(&rilp);

		if (pdu)
			response = decode_hex(pdu, -1, &len, -1);

		CALLBACK_WITH_SUCCESS(cb, response, len, cbd->data);
		g_free(response);
	} else {
		ofono_error("%s RILD reply failure: %s",
			g_ril_request_id_to_string(sd->ril, message->req),
			ril_error_to_string(message->error));
		CALLBACK_WITH_FAILURE(cb, NULL, 0, cbd->data);
	}
}

static void ril_stk_envelope(struct ofono_stk *stk, int len,
				const unsigned char *cmd,
				ofono_stk_envelope_cb_t cb, void *user_data)
{
	struct stk_data *sd = ofono_stk_get_data(stk);
	struct cb_data *cbd = cb_data_new(cb, user_data, sd);
	struct parcel rilp;
	char *buf = alloca(len * 2 + 1);
	int size = 0;

	for (; len; len--)
		size += sprintf(buf + size, "%02hhX", *cmd++);

	parcel_init(&rilp);
	parcel_w_string(&rilp, buf);

	if (g_ril_send(sd->ril, RIL_REQUEST_STK_SEND_ENVELOPE_COMMAND, &rilp,
			ril_stk_envelope_cb, cbd, g_free) > 0)
		return;

	g_free(cbd);
	CALLBACK_WITH_FAILURE(cb, NULL, 0, user_data);
}

static void ril_stk_proactive_cmd_notify(struct ril_msg *message,
				gpointer user_data)
{
	struct ofono_stk *stk = user_data;
	struct parcel rilp;
	long pdulen;
	unsigned char *pdu;

	DBG("");

	g_ril_init_parcel(message, &rilp);
	pdu = decode_hex(parcel_r_string(&rilp), -1, &pdulen, -1);

	ofono_stk_proactive_command_notify(stk, pdulen, pdu);
	g_free(pdu);
}

static void ril_stk_event_notify(struct ril_msg *message, gpointer user_data)
{
	struct ofono_stk *stk = user_data;
	struct parcel rilp;
	long pdulen;
	unsigned char *pdu;

	DBG("");

	g_ril_init_parcel(message, &rilp);
	pdu = decode_hex(parcel_r_string(&rilp), -1, &pdulen, -1);

	ofono_stk_proactive_command_handled_notify(stk, pdulen, pdu);
	g_free(pdu);
}

static void ril_stk_session_end_notify(struct ril_msg *message,
				gpointer user_data)
{
	struct ofono_stk *stk = user_data;

	DBG("");
	ofono_stk_proactive_session_end_notify(stk);
}

static void ril_stk_initialize_cb(struct ril_msg *message,
				gpointer user_data)
{
	struct ofono_stk *stk = user_data;
	struct stk_data *sd = ofono_stk_get_data(stk);

	if (message->error != RIL_E_SUCCESS) {
		ofono_error("%s RILD reply failure: %s",
			g_ril_request_id_to_string(sd->ril, message->req),
			ril_error_to_string(message->error));
		ofono_stk_remove(stk);

		return;
	}

	ofono_stk_register(stk);
}

static int ril_stk_probe(struct ofono_stk *stk, unsigned int vendor,
				void *user)
{
	GRil *ril = user;
	struct stk_data *data;

	data = g_new0(struct stk_data, 1);
	data->ril = g_ril_clone(ril);
	data->vendor = vendor;

	ofono_stk_set_data(stk, data);

	g_ril_register(ril, RIL_UNSOL_STK_PROACTIVE_COMMAND,
					ril_stk_proactive_cmd_notify, stk);

	g_ril_register(ril, RIL_UNSOL_STK_SESSION_END,
					ril_stk_session_end_notify, stk);

	g_ril_register(ril, RIL_UNSOL_STK_EVENT_NOTIFY,
					ril_stk_event_notify, stk);

	g_ril_send(data->ril, RIL_REQUEST_REPORT_STK_SERVICE_IS_RUNNING, NULL,
					ril_stk_initialize_cb, stk, NULL);

	return 0;
}

static void ril_stk_remove(struct ofono_stk *stk)
{
	struct stk_data *data = ofono_stk_get_data(stk);

	ofono_stk_set_data(stk, NULL);

	g_ril_unref(data->ril);
	g_free(data);
}

static struct ofono_stk_driver driver = {
	.name = RILMODEM,
	.probe = ril_stk_probe,
	.remove = ril_stk_remove,
	.envelope = ril_stk_envelope,
	.terminal_response = ril_stk_terminal_response,
};

void ril_stk_init(void)
{
	ofono_stk_driver_register(&driver);
}

void ril_stk_exit(void)
{
	ofono_stk_driver_unregister(&driver);
}
