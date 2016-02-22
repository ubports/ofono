/*
 *  oFono - Open Source Telephony - RIL-based devices
 *
 *  Copyright (C) 2015-2016 Jolla Ltd.
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License version 2 as
 *  published by the Free Software Foundation.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 *  GNU General Public License for more details.
 */

#include "ril_plugin.h"
#include "ril_util.h"
#include "ril_log.h"
#include "ril_constants.h"

#include "smsutil.h"
#include "util.h"

struct ril_ussd {
	struct ofono_ussd *ussd;
	GRilIoChannel *io;
	GRilIoQueue *q;
	guint timer_id;
	gulong event_id;
};

struct ril_ussd_cbd {
	ofono_ussd_cb_t cb;
	gpointer data;
};

#define ril_ussd_cbd_free g_free

static inline struct ril_ussd *ril_ussd_get_data(struct ofono_ussd *ussd)
{
	return ofono_ussd_get_data(ussd);
}

static struct ril_ussd_cbd *ril_ussd_cbd_new(ofono_ussd_cb_t cb, void *data)
{
	struct ril_ussd_cbd *cbd = g_new0(struct ril_ussd_cbd, 1);

	cbd->cb = cb;
	cbd->data = data;
	return cbd;
}

static void ril_ussd_cb(GRilIoChannel *io, int status,
			const void *data, guint len, void *user_data)
{
	struct ofono_error error;
	struct ril_ussd_cbd *cbd = user_data;

	if (status == RIL_E_SUCCESS) {
		cbd->cb(ril_error_ok(&error), cbd->data);
	} else {
		cbd->cb(ril_error_failure(&error), cbd->data);
	}
}

static void ril_ussd_request(struct ofono_ussd *ussd, int dcs,
	const unsigned char *pdu, int len, ofono_ussd_cb_t cb, void *data)
{
	struct ofono_error error;
	enum sms_charset charset;
	struct ril_ussd *ud = ril_ussd_get_data(ussd);

	ofono_info("send ussd, len:%d", len);
	if (cbs_dcs_decode(dcs, NULL, NULL, &charset, NULL, NULL, NULL)) {
		if (charset == SMS_CHARSET_7BIT) {
			unsigned char unpacked_buf[182];
			long written = 0;

			unpack_7bit_own_buf(pdu, len, 0, TRUE,
					sizeof(unpacked_buf)-1, &written, 0,
					unpacked_buf);

			unpacked_buf[written] = 0;
			if (written >= 1) {
				/*
				 * When USSD was packed, additional CR
				 * might have been added (according to
				 * 23.038 6.1.2.3.1). So if the last
				 * character is CR, it should be removed
				 * here.
				 *
				 * Over 2 characters long USSD string must
				 * end with # (checked in valid_ussd_string),
				 * so it should be safe to remove extra CR.
				 */
				GRilIoRequest *req = grilio_request_new();
				int length = strlen((char *)unpacked_buf);
				while (length > 2 &&
					unpacked_buf[length-1] == '\r') {
					unpacked_buf[--length] = 0;
				}
				grilio_request_append_utf8_chars(req, (char*)
						unpacked_buf, length);
				grilio_queue_send_request(ud->q, req,
					RIL_REQUEST_SEND_USSD);
				grilio_request_unref(req);
				cb(ril_error_ok(&error), data);
				return;
			}
		}
	}

	cb(ril_error_failure(&error), data);
}

static void ril_ussd_cancel(struct ofono_ussd *ussd,
				ofono_ussd_cb_t cb, void *data)
{
	struct ril_ussd *ud = ril_ussd_get_data(ussd);

	ofono_info("send ussd cancel");
	grilio_queue_send_request_full(ud->q, NULL, RIL_REQUEST_CANCEL_USSD,
		ril_ussd_cb, ril_ussd_cbd_free, ril_ussd_cbd_new(cb, data));
}

static void ril_ussd_notify(GRilIoChannel *io, guint code,
				const void *data, guint len, void *user_data)
{
	struct ril_ussd *ud = user_data;
	GRilIoParser rilp;
	char *ussd_from_network = NULL;
	char *type = NULL;
	int ussdtype = 0;

	ofono_info("ussd_received");

	GASSERT(code == RIL_UNSOL_ON_USSD);
	grilio_parser_init(&rilp, data, len);
	grilio_parser_get_uint32(&rilp, NULL);
	type = grilio_parser_get_utf8(&rilp);
	ussd_from_network = grilio_parser_get_utf8(&rilp);

	ussdtype = g_ascii_xdigit_value(*type);

	if (ussd_from_network) {
		const int data_len = strlen(ussd_from_network);
		DBG("ussd_received, length %d", data_len);
		ofono_ussd_notify(ud->ussd, ussdtype, 0xFF,
			(const unsigned char *) ussd_from_network, data_len);
	} else {
		ofono_ussd_notify(ud->ussd, ussdtype, 0, NULL, 0);
	}

	/* ussd_from_network not freed because core does that if dcs is 0xFF */
	g_free(type);
	return;
}

static gboolean ril_ussd_register(gpointer user_data)
{
	struct ril_ussd *ud = user_data;

	DBG("");
	GASSERT(ud->timer_id);
	ud->timer_id = 0;
	ofono_ussd_register(ud->ussd);

	/* Register for USSD events */
	ud->event_id = grilio_channel_add_unsol_event_handler(ud->io,
				ril_ussd_notify, RIL_UNSOL_ON_USSD, ud);

	/* Single-shot */
	return FALSE;
}

static int ril_ussd_probe(struct ofono_ussd *ussd, unsigned int vendor,
								void *data)
{
	struct ril_modem *modem = data;
	struct ril_ussd *ud = g_try_new0(struct ril_ussd, 1);

	DBG("");
	ud->ussd = ussd;
	ud->io = grilio_channel_ref(ril_modem_io(modem));
	ud->q = grilio_queue_new(ud->io);
	ud->timer_id = g_idle_add(ril_ussd_register, ud);
	ofono_ussd_set_data(ussd, ud);
	return 0;
}

static void ril_ussd_remove(struct ofono_ussd *ussd)
{
	struct ril_ussd *ud = ril_ussd_get_data(ussd);

	DBG("");
	ofono_ussd_set_data(ussd, NULL);

	if (ud->timer_id > 0) {
		g_source_remove(ud->timer_id);
	}

	grilio_channel_remove_handler(ud->io, ud->event_id);
	grilio_channel_unref(ud->io);
	grilio_queue_cancel_all(ud->q, FALSE);
	grilio_queue_unref(ud->q);
	g_free(ud);
}

const struct ofono_ussd_driver ril_ussd_driver = {
	.name           = RILMODEM_DRIVER,
	.probe          = ril_ussd_probe,
	.remove         = ril_ussd_remove,
	.request        = ril_ussd_request,
	.cancel         = ril_ussd_cancel
};

/*
 * Local Variables:
 * mode: C
 * c-basic-offset: 8
 * indent-tabs-mode: t
 * End:
 */
