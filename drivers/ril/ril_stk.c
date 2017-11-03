/*
 *  oFono - Open Source Telephony - RIL-based devices
 *
 *  Copyright (C) 2015-2017 Jolla Ltd.
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License version 2 as
 *  published by the Free Software Foundation.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 */

#include "ril_plugin.h"
#include "ril_util.h"
#include "ril_log.h"

#include "util.h"

#ifndef UI_LANG
#  define UI_LANG "/var/lib/environment/nemo/locale.conf"
#endif

enum ril_stk_events {
	STK_EVENT_PROACTIVE_COMMAND,
	STK_EVENT_SESSION_END,
	STK_EVENT_NOTIFY,
	STK_EVENT_COUNT
};

struct ril_stk {
	struct ofono_stk *stk;
	GRilIoChannel *io;
	GRilIoQueue *q;
	gulong event_id[STK_EVENT_COUNT];
};

struct ril_stk_cbd {
	union _ofono_stk_cb {
		ofono_stk_envelope_cb_t envelope;
		ofono_stk_generic_cb_t generic;
		gpointer ptr;
	} cb;
	gpointer data;
};

#define ril_stk_cbd_free g_free

static inline struct ril_stk *ril_stk_get_data(struct ofono_stk *stk)
{
	return ofono_stk_get_data(stk);
}

struct ril_stk_cbd *ril_stk_cbd_new(void *cb, void *data)
{
	struct ril_stk_cbd *cbd = g_new0(struct ril_stk_cbd, 1);

	cbd->cb.ptr = cb;
	cbd->data = data;
	return cbd;
}

static void ril_stk_envelope_cb(GRilIoChannel *io, int status,
				const void *data, guint len, void *user_data)
{
	struct ofono_error error;
	struct ril_stk_cbd *cbd = user_data;
	ofono_stk_envelope_cb_t cb = cbd->cb.envelope;

	if (status == RIL_E_SUCCESS) {
		DBG("%u bytes(s)", len);
		cb(ril_error_ok(&error), NULL, 0, cbd->data);
	} else {
		DBG("Envelope reply failure: %s", ril_error_to_string(status));
		cb(ril_error_failure(&error), NULL, 0, cbd->data);
	}
}

static void ril_stk_envelope(struct ofono_stk *stk, int length,
	const unsigned char *cmd, ofono_stk_envelope_cb_t cb, void *data)
{
	struct ril_stk *sd = ril_stk_get_data(stk);
	GRilIoRequest *req = grilio_request_new();
	char *hex_envelope = encode_hex(cmd, length, 0);

	DBG("%s", hex_envelope);
	grilio_request_append_utf8(req, hex_envelope);
	g_free(hex_envelope);
	grilio_queue_send_request_full(sd->q, req,
			RIL_REQUEST_STK_SEND_ENVELOPE_COMMAND,
			ril_stk_envelope_cb, ril_stk_cbd_free,
			ril_stk_cbd_new(cb, data));
	grilio_request_unref(req);
}

static void ril_stk_terminal_response_cb(GRilIoChannel *io, int status,
				const void *data, guint len, void *user_data)
{
	struct ofono_error error;
	struct ril_stk_cbd *cbd = user_data;
	ofono_stk_generic_cb_t cb = cbd->cb.generic;

	DBG("");
	if (status == RIL_E_SUCCESS) {
		cb(ril_error_ok(&error), cbd->data);
	} else {
		ofono_error("Error in sending terminal response");
		cb(ril_error_failure(&error), cbd->data);
	}
}

static void ril_stk_terminal_response(struct ofono_stk *stk, int length,
					const unsigned char *resp,
					ofono_stk_generic_cb_t cb, void *data)
{
	struct ril_stk *sd = ril_stk_get_data(stk);
	GRilIoRequest *req = grilio_request_new();
	char *hex_tr = encode_hex(resp, length, 0);

	DBG("rilmodem terminal response: %s", hex_tr);
	grilio_request_append_utf8(req, hex_tr);
	g_free(hex_tr);
	grilio_queue_send_request_full(sd->q, req,
				RIL_REQUEST_STK_SEND_TERMINAL_RESPONSE,
				ril_stk_terminal_response_cb,
				ril_stk_cbd_free, ril_stk_cbd_new(cb, data));
	grilio_request_unref(req);
}

static void ril_stk_user_confirmation(struct ofono_stk *stk,
					ofono_bool_t confirm)
{
	struct ril_stk *sd = ril_stk_get_data(stk);
	GRilIoRequest *req = grilio_request_sized_new(8);

	DBG("%d", confirm);
	grilio_request_append_int32(req, 1);            /* size of array */
	grilio_request_append_int32(req, confirm);      /* yes/no */

	grilio_queue_send_request(sd->q, req,
		RIL_REQUEST_STK_HANDLE_CALL_SETUP_REQUESTED_FROM_SIM);
	grilio_request_unref(req);
}

static void ril_stk_pcmd_notify(GRilIoChannel *io, guint code,
			const void *data, guint data_len, void *user_data)
{
	struct ril_stk *sd = user_data;
	GRilIoParser rilp;
	char *pcmd;
	guchar *pdu;
	long len = 0;

	GASSERT(code == RIL_UNSOL_STK_PROACTIVE_COMMAND);
	grilio_parser_init(&rilp, data, data_len);
	pcmd = grilio_parser_get_utf8(&rilp);
	DBG("pcmd: %s", pcmd);

	pdu = decode_hex(pcmd, strlen(pcmd), &len, -1);
	g_free(pcmd);

	ofono_stk_proactive_command_notify(sd->stk, len, pdu);
	g_free(pdu);
}

static void ril_stk_event_notify(GRilIoChannel *io, guint code,
			const void *data, guint data_len, void *user_data)
{
	struct ril_stk *sd = user_data;
	GRilIoParser rilp;
	char *pcmd = NULL;
	guchar *pdu = NULL;
	long len;

	/* Proactive command has been handled by the modem. */
	GASSERT(code == RIL_UNSOL_STK_EVENT_NOTIFY);
	grilio_parser_init(&rilp, data, data_len);
	pcmd = grilio_parser_get_utf8(&rilp);
	DBG("pcmd: %s", pcmd);
	pdu = decode_hex(pcmd, strlen(pcmd), &len, -1);
	g_free(pcmd);

	ofono_stk_proactive_command_handled_notify(sd->stk, len, pdu);
	g_free(pdu);
}

static void ril_stk_session_end_notify(GRilIoChannel *io, guint code,
				const void *data, guint len, void *user_data)
{
	struct ril_stk *sd = user_data;

	DBG("");
	GASSERT(code == RIL_UNSOL_STK_SESSION_END);
	ofono_stk_proactive_session_end_notify(sd->stk);
}

static void ril_stk_agent_ready(struct ofono_stk *stk)
{
	struct ril_stk *sd = ril_stk_get_data(stk);

	DBG("");
	if (!sd->event_id[STK_EVENT_PROACTIVE_COMMAND]) {
		DBG("Subscribing notifications");
		sd->event_id[STK_EVENT_PROACTIVE_COMMAND] =
			grilio_channel_add_unsol_event_handler(sd->io,
				ril_stk_pcmd_notify,
				RIL_UNSOL_STK_PROACTIVE_COMMAND, sd);

		GASSERT(!sd->event_id[STK_EVENT_SESSION_END]);
		sd->event_id[STK_EVENT_SESSION_END] =
			grilio_channel_add_unsol_event_handler(sd->io,
				ril_stk_session_end_notify,
				RIL_UNSOL_STK_SESSION_END, sd);

		GASSERT(!sd->event_id[STK_EVENT_NOTIFY]);
		sd->event_id[STK_EVENT_NOTIFY] =
			grilio_channel_add_unsol_event_handler(sd->io,
				ril_stk_event_notify, 
				RIL_UNSOL_STK_EVENT_NOTIFY, sd);

		grilio_queue_send_request(sd->q, NULL,
				RIL_REQUEST_REPORT_STK_SERVICE_IS_RUNNING);
	}
}

static void ril_stk_set_lang()
{
	GError *error = NULL;
	GIOChannel* chan = g_io_channel_new_file(UI_LANG, "r", &error);
	if (chan) {
		GString* buf = g_string_new(NULL);
		gsize term;
		while (g_io_channel_read_line_string(chan, buf, &term, NULL) ==
							G_IO_STATUS_NORMAL) {
			char* lang;
			g_string_set_size(buf, term);
			lang = strstr(buf->str, "LANG=");
			if (lang) {
				setenv("LANG", lang + 5, TRUE);
			}
		}
		g_string_free(buf, TRUE);
		g_io_channel_unref(chan);
	} else {
		DBG("%s: %s", UI_LANG, error->message);
		g_error_free(error);
	}
}

static int ril_stk_probe(struct ofono_stk *stk, unsigned int vendor, void *data)
{
	struct ril_modem *modem = data;
	struct ril_stk *sd = g_new0(struct ril_stk, 1);

	DBG("");
	sd->stk = stk;
	sd->io = grilio_channel_ref(ril_modem_io(modem));
	sd->q = grilio_queue_new(sd->io);

	ofono_stk_set_data(stk, sd);
	ofono_stk_register(stk);
	ril_stk_set_lang();
	return 0;
}

static void ril_stk_remove(struct ofono_stk *stk)
{
	struct ril_stk *sd = ril_stk_get_data(stk);
	unsigned int i;

	DBG("");
	ofono_stk_set_data(stk, NULL);

	for (i=0; i<G_N_ELEMENTS(sd->event_id); i++) {
		grilio_channel_remove_handler(sd->io, sd->event_id[i]);
	}

	grilio_channel_unref(sd->io);
	grilio_queue_cancel_all(sd->q, FALSE);
	grilio_queue_unref(sd->q);
	g_free(sd);
}

const struct ofono_stk_driver ril_stk_driver = {
	.name                   = RILMODEM_DRIVER,
	.probe                  = ril_stk_probe,
	.remove                 = ril_stk_remove,
	.envelope               = ril_stk_envelope,
	.terminal_response      = ril_stk_terminal_response,
	.user_confirmation      = ril_stk_user_confirmation,
	.ready                  = ril_stk_agent_ready
};

/*
 * Local Variables:
 * mode: C
 * c-basic-offset: 8
 * indent-tabs-mode: t
 * End:
 */
