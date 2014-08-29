/*
 *
 *  oFono - Open Source Telephony
 *
 *  Copyright (C) 2008-2011  Intel Corporation. All rights reserved.
 *  Copyright (C) 2014 Jolla Ltd
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
#include <stdlib.h>
#include <stdio.h>
#include <errno.h>

#include <glib.h>

#include <ofono/log.h>
#include <ofono/modem.h>
#include <ofono/stk.h>

#include "gril.h"
#include "util.h"

#include "rilmodem.h"
#include "ril_constants.h"

struct stk_data {
	GRil *ril;
};

gboolean subscribed;

static void ril_envelope_cb(struct ril_msg *message, gpointer user_data)
{
	struct cb_data *cbd = user_data;
	ofono_stk_envelope_cb_t cb = cbd->cb;
	struct ofono_error error;

	DBG("");

	if (message->error == RIL_E_SUCCESS) {
		decode_ril_error(&error, "OK");
	} else {
		DBG("Envelope reply failure: %s",
				ril_error_to_string(message->error));
		decode_ril_error(&error, "FAIL");
	}

	cb(&error, NULL, 0, cbd->data);
}

static void ril_stk_envelope(struct ofono_stk *stk, int length,
				const unsigned char *command,
				ofono_stk_envelope_cb_t cb, void *data)
{
	struct stk_data *sd = ofono_stk_get_data(stk);
	struct cb_data *cbd = cb_data_new(cb, data);
	struct parcel rilp;
	char *hex_envelope = NULL;
	int request = RIL_REQUEST_STK_SEND_ENVELOPE_COMMAND;
	guint ret;

	DBG("");

	hex_envelope = encode_hex(command, length, 0);
	DBG("rilmodem envelope: %s", hex_envelope);

	parcel_init(&rilp);
	parcel_w_string(&rilp, hex_envelope);
	g_free(hex_envelope);
	hex_envelope = NULL;

	ret = g_ril_send(sd->ril, request,
				rilp.data, rilp.size, ril_envelope_cb,
				cbd, g_free);

	parcel_free(&rilp);

	if (ret <= 0) {
			g_free(cbd);
			CALLBACK_WITH_FAILURE(cb, NULL, -1, data);
		}
}

static void ril_tr_cb(struct ril_msg *message, gpointer user_data)
{
	struct cb_data *cbd = user_data;
	ofono_stk_generic_cb_t cb = cbd->cb;
	struct ofono_error error;

	DBG("");

	if (message->error == RIL_E_SUCCESS) {
		decode_ril_error(&error, "OK");
	} else {
		DBG("Error in sending terminal response");
		ofono_error("Error in sending terminal response");
		decode_ril_error(&error, "FAIL");
	}

	cb(&error, cbd->data);
}

static void ril_stk_terminal_response(struct ofono_stk *stk, int length,
					const unsigned char *resp,
					ofono_stk_generic_cb_t cb, void *data)
{
	struct stk_data *sd = ofono_stk_get_data(stk);
	struct cb_data *cbd = cb_data_new(cb, data);
	struct parcel rilp;
	char *hex_tr = NULL;
	int request = RIL_REQUEST_STK_SEND_TERMINAL_RESPONSE;
	guint ret;

	DBG("");

	hex_tr = encode_hex(resp, length, 0);
	DBG("rilmodem terminal response: %s", hex_tr);

	parcel_init(&rilp);
	parcel_w_string(&rilp, hex_tr);
	g_free(hex_tr);
	hex_tr = NULL;

	ret = g_ril_send(sd->ril, request,
				rilp.data, rilp.size, ril_tr_cb,
				cbd, g_free);

	parcel_free(&rilp);

	if (ret <= 0) {
			g_free(cbd);
			CALLBACK_WITH_FAILURE(cb, data);
		}
}

static void ril_stk_user_confirmation(struct ofono_stk *stk,
					ofono_bool_t confirm)
{
	struct stk_data *sd = ofono_stk_get_data(stk);
	struct parcel rilp;
	int request = RIL_REQUEST_STK_HANDLE_CALL_SETUP_REQUESTED_FROM_SIM;
	int ret;

	DBG("");

	/* Only pcmd needing user confirmation is call set up
	 * RIL_REQUEST_STK_HANDLE_CALL_SETUP_REQUESTED_FROM_SIM
	 */
	parcel_init(&rilp);
	parcel_w_int32(&rilp, 1);		/* size of array */
	parcel_w_int32(&rilp, confirm);	/* yes/no */

	/* fire and forget i.e. not waiting for the callback*/
	ret = g_ril_send(sd->ril, request, rilp.data,
		 rilp.size, NULL, NULL, NULL);

	g_ril_print_request_no_args(sd->ril, ret, request);

	parcel_free(&rilp);
}

static void ril_stk_pcmd_notify(struct ril_msg *message, gpointer user_data)
{
	struct ofono_stk *stk = user_data;
	struct parcel rilp;
	char *pcmd = NULL;
	guchar *pdu = NULL;
	long len;

	DBG("");

	ril_util_init_parcel(message, &rilp);
	pcmd = parcel_r_string(&rilp);
	DBG("pcmd: %s", pcmd);

	pdu = decode_hex((const char *) pcmd,
			strlen(pcmd),
			&len, -1);

	g_free(pcmd);
	ofono_stk_proactive_command_notify(stk, len, (const guchar *)pdu);
	g_free(pdu);
}

static void ril_stk_event_notify(struct ril_msg *message, gpointer user_data)
{
	struct ofono_stk *stk = user_data;
	struct parcel rilp;
	char *pcmd = NULL;
	guchar *pdu = NULL;
	long len;

	DBG("");

	/* Proactive command has been handled by the modem. */
	ril_util_init_parcel(message, &rilp);
	pcmd = parcel_r_string(&rilp);
	DBG("pcmd: %s", pcmd);
	pdu = decode_hex((const char *) pcmd,
			strlen(pcmd),
			&len, -1);
	g_free(pcmd);
	pcmd = NULL;
	ofono_stk_proactive_command_handled_notify(stk, len,
			(const guchar *)pdu);
	g_free(pdu);
}

static void ril_stk_session_end_notify(struct ril_msg *message,
					gpointer user_data)
{
	struct ofono_stk *stk = user_data;

	DBG("");

	ofono_stk_proactive_session_end_notify(stk);
}

static void ril_stk_agent_ready(struct ofono_stk *stk)
{
	struct stk_data *sd = ofono_stk_get_data(stk);
	int request = RIL_REQUEST_REPORT_STK_SERVICE_IS_RUNNING;
	int ret;

	DBG("");

	if (!subscribed) {
		DBG("Subscribing notifications");
		g_ril_register(sd->ril, RIL_UNSOL_STK_PROACTIVE_COMMAND,
				ril_stk_pcmd_notify, stk);

		g_ril_register(sd->ril, RIL_UNSOL_STK_SESSION_END,
				ril_stk_session_end_notify, stk);

		g_ril_register(sd->ril, RIL_UNSOL_STK_EVENT_NOTIFY,
				ril_stk_event_notify, stk);
		subscribed = TRUE;
	}

	/* fire and forget i.e. not waiting for the callback*/
	ret = g_ril_send(sd->ril, request, NULL, 0,
		 NULL, NULL, NULL);

	g_ril_print_request_no_args(sd->ril, ret, request);
}

void ril_stk_set_lang()
{
	gchar *contents;
	GError *err = NULL;

	if (!g_file_get_contents(UI_LANG, &contents, NULL, &err)) {
		if (err)
			ofono_error("cannot open %s error: %d: message: %s",
					UI_LANG, err->code, err->message);
		g_error_free(err);
	} else {
		gchar *pch = g_strrstr(contents, CFG_LANG);
		/* Set System UI lang to env LANG */
		if (pch) {
			setenv("LANG", pch + strlen(CFG_LANG), 1);
			DBG("LANG %s", getenv("LANG"));
		}
		g_free(contents);
	}
}

static int ril_stk_probe(struct ofono_stk *stk, unsigned int vendor, void *data)
{
	GRil *ril = data;
	struct stk_data *sd;

	DBG("");

	sd = g_try_new0(struct stk_data, 1);
	if (sd == NULL)
		return -ENOMEM;

	sd->ril = g_ril_clone(ril);
	ofono_stk_set_data(stk, sd);

	/* Register interface in this phase for stk agent */
	ofono_stk_register(stk);

	subscribed = FALSE;

	/* UI language for local info */
	ril_stk_set_lang();

	return 0;
}

static void ril_stk_remove(struct ofono_stk *stk)
{
	struct stk_data *sd = ofono_stk_get_data(stk);

	DBG("");

	ofono_stk_set_data(stk, NULL);

	g_ril_unref(sd->ril);
	g_free(sd);
}

static struct ofono_stk_driver driver = {
	.name			= "rilmodem",
	.probe			= ril_stk_probe,
	.remove			= ril_stk_remove,
	.envelope		= ril_stk_envelope,
	.terminal_response	= ril_stk_terminal_response,
	.user_confirmation	= ril_stk_user_confirmation,
	.ready			= ril_stk_agent_ready
};

void ril_stk_init(void)
{
	ofono_stk_driver_register(&driver);
}

void ril_stk_exit(void)
{
	ofono_stk_driver_unregister(&driver);
}
