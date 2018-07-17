/*
 *  oFono - Open Source Telephony - RIL-based devices
 *
 *  Copyright (C) 2015-2018 Jolla Ltd.
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
#include "ril_sim_card.h"
#include "ril_util.h"
#include "ril_log.h"

#include "common.h"

/* See 3GPP 27.007 7.4 for possible values */
#define RIL_MAX_SERVICE_LENGTH 3

/*
 * ril.h does not state that string count must be given, but that is
 * still expected by the modem
 */
#define RIL_SET_STRING_COUNT 5
#define RIL_SET_PW_STRING_COUNT 3

struct ril_call_barring {
	struct ril_sim_card *card;
	GRilIoQueue *q;
	guint timer_id;
};

struct ril_call_barring_cbd {
	struct ril_call_barring *bd;
	union _ofono_call_barring_cb {
		ofono_call_barring_query_cb_t query;
		ofono_call_barring_set_cb_t set;
		gpointer ptr;
	} cb;
	gpointer data;
};

#define ril_call_barring_cbd_free g_free

static inline struct ril_call_barring *ril_call_barring_get_data(
					struct ofono_call_barring *b)
{
	return ofono_call_barring_get_data(b);
}

static struct ril_call_barring_cbd *ril_call_barring_cbd_new(
			struct ril_call_barring *bd, void *cb, void *data)
{
	struct ril_call_barring_cbd *cbd;

	cbd = g_new0(struct ril_call_barring_cbd, 1);
	cbd->bd = bd;
	cbd->cb.ptr = cb;
	cbd->data = data;
	return cbd;
}

static inline void ril_call_barring_submit_request(struct ril_call_barring *bd,
	GRilIoRequest* req, guint code, GRilIoChannelResponseFunc response,
							void *cb, void *data)
{
	grilio_queue_send_request_full(bd->q, req, code, response,
				ril_call_barring_cbd_free,
				ril_call_barring_cbd_new(bd, cb, data));
}

static void ril_call_barring_query_cb(GRilIoChannel *io, int status,
				const void *data, guint len, void *user_data)
{
	struct ofono_error error;
	struct ril_call_barring_cbd *cbd = user_data;
	ofono_call_barring_query_cb_t cb = cbd->cb.query;

	if (status == RIL_E_SUCCESS) {
		int bearer_class = 0;
		GRilIoParser rilp;

		/*
		 * Services for which the specified barring facility is active.
		 * "0" means "disabled for all, -1 if unknown"
		 */
		grilio_parser_init(&rilp, data, len);
		grilio_parser_get_int32(&rilp, NULL); /* count */
		grilio_parser_get_int32(&rilp, &bearer_class);
		DBG("Active services: %d", bearer_class);
		cb(ril_error_ok(&error), bearer_class, cbd->data);
	} else {
		ofono_error("Call Barring query error %d", status);
		cb(ril_error_failure(&error), 0, cbd->data);
	}
}

static void ril_call_barring_query(struct ofono_call_barring *b,
			const char *lock, int cls,
			ofono_call_barring_query_cb_t cb, void *data)
{
	struct ril_call_barring *bd = ofono_call_barring_get_data(b);
	char cls_textual[RIL_MAX_SERVICE_LENGTH];
	GRilIoRequest *req;

	DBG("lock: %s, services to query: %d", lock, cls);

	/*
	 * RIL modems do not support 7 as default bearer class. According to
	 * the 22.030 Annex C: When service code is not given it corresponds to
	 * "All tele and bearer services"
	 */
	if (cls == BEARER_CLASS_DEFAULT) {
		cls = SERVICE_CLASS_NONE;
	}

	sprintf(cls_textual, "%d", cls);

	/*
	 * See 3GPP 27.007 7.4 for parameter descriptions.
	 */
	req = grilio_request_array_utf8_new(4, lock, "", cls_textual,
					ril_sim_card_app_aid(bd->card));
	ril_call_barring_submit_request(bd, req,
				RIL_REQUEST_QUERY_FACILITY_LOCK,
				ril_call_barring_query_cb, cb, data);
	grilio_request_unref(req);
}

static void ril_call_barring_set_cb(GRilIoChannel *io, int status,
				const void *data, guint len, void *user_data)
{
	struct ofono_error error;
	struct ril_call_barring_cbd *cbd = user_data;
	ofono_call_barring_set_cb_t cb = cbd->cb.set;

	if (status == RIL_E_SUCCESS) {
		cb(ril_error_ok(&error), cbd->data);
	} else {
		ofono_error("Call Barring Set error %d", status);
		cb(ril_error_failure(&error), cbd->data);
	}
}

static void ril_call_barring_set(struct ofono_call_barring *b,
		const char *lock, int enable, const char *passwd, int cls,
		ofono_call_barring_set_cb_t cb, void *data)
{
	struct ril_call_barring *bd = ofono_call_barring_get_data(b);
	char cls_textual[RIL_MAX_SERVICE_LENGTH];
	GRilIoRequest *req = grilio_request_new();

	DBG("lock: %s, enable: %i, bearer class: %i", lock, enable, cls);

	/*
	 * RIL modem does not support 7 as default bearer class. According to
	 * the 22.030 Annex C: When service code is not given it corresponds to
	 * "All tele and bearer services"
	 */
	if (cls == BEARER_CLASS_DEFAULT) {
		cls = SERVICE_CLASS_NONE;
	}

	sprintf(cls_textual, "%d", cls);

	/* See 3GPP 27.007 7.4 for parameter descriptions */
	grilio_request_append_int32(req, RIL_SET_STRING_COUNT);
	grilio_request_append_utf8(req, lock);  /* Facility code */
	grilio_request_append_utf8(req, enable ?
					RIL_FACILITY_LOCK :
					RIL_FACILITY_UNLOCK);
	grilio_request_append_utf8(req, passwd);
	grilio_request_append_utf8(req, cls_textual);
	grilio_request_append_utf8(req, ril_sim_card_app_aid(bd->card));

	ril_call_barring_submit_request(bd, req,
				RIL_REQUEST_SET_FACILITY_LOCK,
				ril_call_barring_set_cb, cb, data);
	grilio_request_unref(req);
}

static void ril_call_barring_set_passwd_cb(GRilIoChannel *io, int status,
				const void *data, guint len, void *user_data)
{
	struct ofono_error error;
	struct ril_call_barring_cbd *cbd = user_data;
	ofono_call_barring_set_cb_t cb = cbd->cb.set;

	if (status == RIL_E_SUCCESS) {
		cb(ril_error_ok(&error), cbd->data);
	} else {
		ofono_error("Call Barring Set PW error %d", status);
		cb(ril_error_failure(&error), cbd->data);
	}
}

static void ril_call_barring_set_passwd(struct ofono_call_barring *b,
		const char *lock, const char *old_passwd,
		const char *new_passwd, ofono_call_barring_set_cb_t cb,
		void *data)
{
	struct ril_call_barring *bd = ofono_call_barring_get_data(b);
	GRilIoRequest *req = grilio_request_new();

	DBG("");
	grilio_request_append_int32(req, RIL_SET_PW_STRING_COUNT);
	grilio_request_append_utf8(req, lock);            /* Facility code */
	grilio_request_append_utf8(req, old_passwd);
	grilio_request_append_utf8(req, new_passwd);

	ril_call_barring_submit_request(bd, req,
			RIL_REQUEST_CHANGE_BARRING_PASSWORD,
			ril_call_barring_set_passwd_cb, cb, data);
	grilio_request_unref(req);
}

static gboolean ril_call_barring_register(gpointer user_data)
{
	struct ofono_call_barring *b = user_data;
	struct ril_call_barring *bd = ril_call_barring_get_data(b);

	GASSERT(bd->timer_id);
	bd->timer_id = 0;
	ofono_call_barring_register(b);
	return FALSE;
}

static int ril_call_barring_probe(struct ofono_call_barring *b,
					unsigned int vendor, void *data)
{
	struct ril_modem *modem = data;
	struct ril_call_barring *bd = g_new0(struct ril_call_barring, 1);

	DBG("");
	bd->card = ril_sim_card_ref(modem->sim_card);
	bd->q = grilio_queue_new(ril_modem_io(modem));
	bd->timer_id = g_idle_add(ril_call_barring_register, b);
	ofono_call_barring_set_data(b, bd);
	return 0;
}

static void ril_call_barring_remove(struct ofono_call_barring *b)
{
	struct ril_call_barring *bd = ril_call_barring_get_data(b);

	DBG("");
	ofono_call_barring_set_data(b, NULL);

	if (bd->timer_id > 0) {
		g_source_remove(bd->timer_id);
	}

	ril_sim_card_unref(bd->card);
	grilio_queue_cancel_all(bd->q, FALSE);
	grilio_queue_unref(bd->q);
	g_free(bd);
}

const struct ofono_call_barring_driver ril_call_barring_driver = {
	.name           = RILMODEM_DRIVER,
	.probe          = ril_call_barring_probe,
	.remove         = ril_call_barring_remove,
	.query          = ril_call_barring_query,
	.set            = ril_call_barring_set,
	.set_passwd	= ril_call_barring_set_passwd
};

/*
 * Local Variables:
 * mode: C
 * c-basic-offset: 8
 * indent-tabs-mode: t
 * End:
 */
