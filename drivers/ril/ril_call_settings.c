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

#include "common.h"

struct ril_call_settings {
	GRilIoQueue *q;
	guint timer_id;
};

struct ril_call_settings_cbd {
	union _ofono_call_settings_cb {
		ofono_call_settings_status_cb_t status;
		ofono_call_settings_set_cb_t set;
		ofono_call_settings_clir_cb_t clir;
		gpointer ptr;
	} cb;
	gpointer data;
};

#define ril_call_settings_cbd_free g_free

static inline struct ril_call_settings *ril_call_settings_get_data(
					struct ofono_call_settings *b)
{
	return ofono_call_settings_get_data(b);
}

static struct ril_call_settings_cbd *ril_call_settings_cbd_new(void *cb,
								void *data)
{
	struct ril_call_settings_cbd *cbd;

	cbd = g_new0(struct ril_call_settings_cbd, 1);
	cbd->cb.ptr = cb;
	cbd->data = data;
	return cbd;
}

static inline void ril_call_settings_submit_req(struct ril_call_settings *sd,
	GRilIoRequest* req, guint code, GRilIoChannelResponseFunc response,
							void *cb, void *data)
{
	grilio_queue_send_request_full(sd->q, req, code, response,
				ril_call_settings_cbd_free,
				ril_call_settings_cbd_new(cb, data));
}

static void ril_call_settings_clip_query_cb(GRilIoChannel *io, int status,
				const void *data, guint len, void *user_data)
{
	struct ofono_error error;
	struct ril_call_settings_cbd *cbd = user_data;
	ofono_call_settings_status_cb_t cb = cbd->cb.status;

	if (status == RIL_E_SUCCESS) {
		GRilIoParser rilp;
		int res = 0;

		/* data length of the response */
		grilio_parser_init(&rilp, data, len);
		if (grilio_parser_get_int32(&rilp, &res) && res > 0) {
			grilio_parser_get_int32(&rilp, &res);
		}

		cb(ril_error_ok(&error), res, cbd->data);
	} else {
		cb(ril_error_failure(&error), -1, cbd->data);
	}
}

static void ril_call_settings_set_cb(GRilIoChannel *io, int status,
				const void *data, guint len, void *user_data)
{
	struct ofono_error error;
	struct ril_call_settings_cbd *cbd = user_data;
	ofono_call_settings_set_cb_t cb = cbd->cb.set;

	if (status == RIL_E_SUCCESS) {
		cb(ril_error_ok(&error), cbd->data);
	} else {
		cb(ril_error_failure(&error), cbd->data);
	}
}

static void ril_call_settings_cw_set(struct ofono_call_settings *cs, int mode,
			int cls, ofono_call_settings_set_cb_t cb, void *data)
{
	struct ril_call_settings *sd = ril_call_settings_get_data(cs);
	GRilIoRequest *req = grilio_request_sized_new(12);

	grilio_request_append_int32(req, 2);    /* Number of params */
	grilio_request_append_int32(req, mode); /* on/off */

	/* Modem seems to respond with error to all queries
	 * or settings made with bearer class
	 * BEARER_CLASS_DEFAULT. Design decision: If given
	 * class is BEARER_CLASS_DEFAULT let's map it to
	 * SERVICE_CLASS_VOICE effectively making it the
	 * default bearer. This in line with API which is
	 * contains only voice anyways.
	 */
	if (cls == BEARER_CLASS_DEFAULT) {
		cls = BEARER_CLASS_VOICE;
	}

	grilio_request_append_int32(req, cls);  /* Service class */

	ril_call_settings_submit_req(sd, req, RIL_REQUEST_SET_CALL_WAITING,
					ril_call_settings_set_cb, cb, data);
	grilio_request_unref(req);
}

static void ril_call_settings_cw_query_cb(GRilIoChannel *io, int status,
				const void *data, guint len, void *user_data)
{
	struct ofono_error error;
	struct ril_call_settings_cbd *cbd = user_data;
	ofono_call_settings_status_cb_t cb = cbd->cb.status;

	if (status == RIL_E_SUCCESS) {
		GRilIoParser rilp;
		int res = 0;
		int sv = 0;

		grilio_parser_init(&rilp, data, len);

		/* first value in int[] is len so let's skip that */
		grilio_parser_get_int32(&rilp, NULL);

		/* status of call waiting service, disabled is returned only if
		 * service is not active for any service class */
		grilio_parser_get_int32(&rilp, &res);
		DBG("CW enabled/disabled: %d", res);

		if (res > 0) {
			/* services for which call waiting is enabled,
			   27.007 7.12 */
			grilio_parser_get_int32(&rilp, &sv);
			DBG("CW enabled for: %d", sv);
		}

		cb(ril_error_ok(&error), sv, cbd->data);
	} else {
		cb(ril_error_failure(&error), -1, cbd->data);
	}
}

static void ril_call_settings_cw_query(struct ofono_call_settings *cs, int cls,
				ofono_call_settings_status_cb_t cb, void *data)
{
	struct ril_call_settings *sd = ril_call_settings_get_data(cs);
	GRilIoRequest *req = grilio_request_sized_new(8);

	grilio_request_append_int32(req, 1);		/* Number of params */

	/*
	 * RILD expects service class to be 0 as certain carriers can reject
	 * the query with specific service class
	 */
	grilio_request_append_int32(req, 0);

	ril_call_settings_submit_req(sd, req, RIL_REQUEST_QUERY_CALL_WAITING,
				ril_call_settings_cw_query_cb, cb, data);
	grilio_request_unref(req);
}

static void ril_call_settings_clip_query(struct ofono_call_settings *cs,
			ofono_call_settings_status_cb_t cb, void *data)
{
	struct ril_call_settings *sd = ril_call_settings_get_data(cs);

	ril_call_settings_submit_req(sd, NULL, RIL_REQUEST_QUERY_CLIP,
				ril_call_settings_clip_query_cb, cb, data);
}

static void ril_call_settings_clir_cb(GRilIoChannel *io, int status,
				const void *data, guint len, void *user_data)
{
	struct ofono_error error;
	struct ril_call_settings_cbd *cbd = user_data;
	ofono_call_settings_clir_cb_t cb = cbd->cb.clir;

	if (status == RIL_E_SUCCESS) {
		GRilIoParser rilp;
		int override = -1, network = -1;

		grilio_parser_init(&rilp, data, len);
		/*first value in int[] is len so let's skip that*/
		grilio_parser_get_int32(&rilp, NULL);
		/* Set HideCallerId property from network */
		grilio_parser_get_int32(&rilp, &override);
		/* CallingLineRestriction indicates the state of
		the CLIR supplementary service in the network */
		grilio_parser_get_int32(&rilp, &network);

		cb(ril_error_ok(&error), override, network, cbd->data);
	} else {
		cb(ril_error_failure(&error), -1, -1, cbd->data);
	}
}

static void ril_call_settings_clir_query(struct ofono_call_settings *cs,
			ofono_call_settings_clir_cb_t cb, void *data)
{
	struct ril_call_settings *sd = ril_call_settings_get_data(cs);

	ril_call_settings_submit_req(sd, NULL, RIL_REQUEST_GET_CLIR,
					ril_call_settings_clir_cb, cb, data);
}

static void ril_call_settings_clir_set(struct ofono_call_settings *cs,
			int mode, ofono_call_settings_set_cb_t cb, void *data)
{
	struct ril_call_settings *sd = ril_call_settings_get_data(cs);
	GRilIoRequest *req = grilio_request_sized_new(8);

	grilio_request_append_int32(req, 1); /* Number of params */
	grilio_request_append_int32(req, mode); /* for outgoing calls */

	ril_call_settings_submit_req(sd, req, RIL_REQUEST_SET_CLIR,
					ril_call_settings_set_cb, cb, data);
	grilio_request_unref(req);
}

static gboolean ril_call_settings_register(gpointer user_data)
{
	struct ofono_call_settings *cs = user_data;
	struct ril_call_settings *sd = ril_call_settings_get_data(cs);

	DBG("");
	GASSERT(sd->timer_id);
	sd->timer_id = 0;
	ofono_call_settings_register(cs);

	/* Single-shot */
	return FALSE;
}

static int ril_call_settings_probe(struct ofono_call_settings *cs,
					unsigned int vendor, void *data)
{
	struct ril_modem *modem = data;
	struct ril_call_settings *sd = g_try_new0(struct ril_call_settings, 1);

	DBG("");
	sd->q = grilio_queue_new(ril_modem_io(modem));
	sd->timer_id = g_idle_add(ril_call_settings_register, cs);
	ofono_call_settings_set_data(cs, sd);
	return 0;
}

static void ril_call_settings_remove(struct ofono_call_settings *cs)
{
	struct ril_call_settings *sd = ril_call_settings_get_data(cs);

	DBG("");
	ofono_call_settings_set_data(cs, NULL);

	if (sd->timer_id > 0) {
		g_source_remove(sd->timer_id);
        }

	grilio_queue_cancel_all(sd->q, FALSE);
	grilio_queue_unref(sd->q);
	g_free(sd);
}

const struct ofono_call_settings_driver ril_call_settings_driver = {
	.name           = RILMODEM_DRIVER,
	.probe          = ril_call_settings_probe,
	.remove         = ril_call_settings_remove,
	.clip_query	= ril_call_settings_clip_query,
	.cw_query       = ril_call_settings_cw_query,
	.cw_set         = ril_call_settings_cw_set,
	.clir_query     = ril_call_settings_clir_query,
	.clir_set       = ril_call_settings_clir_set

	/*
	 * Not supported in RIL API
	 * .colp_query     = ril_call_settings_colp_query,
	 * .colr_query     = ril_call_settings_colr_query
	 */
};

/*
 * Local Variables:
 * mode: C
 * c-basic-offset: 8
 * indent-tabs-mode: t
 * End:
 */
