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

struct ril_call_volume {
	struct ofono_call_volume *v;
	GRilIoQueue *q;
	guint timer_id;
};

struct ril_call_volume_req {
	ofono_call_volume_cb_t cb;
	gpointer data;
};

static inline struct ril_call_volume *ril_call_volume_get_data(
					struct ofono_call_volume *v)
{
	return ofono_call_volume_get_data(v);
}

static void ril_call_volume_mute_cb(GRilIoChannel *io, int status,
				const void *data, guint len, void *user_data)
{
	struct ofono_error error;
	struct ril_call_volume_req *cbd = user_data;
	ofono_call_volume_cb_t cb = cbd->cb;

	if (status == RIL_E_SUCCESS) {
		cb(ril_error_ok(&error), cbd->data);
	} else {
		ofono_error("Could not set the ril mute state");
		cb(ril_error_failure(&error), cbd->data);
	}
}

static void ril_call_volume_mute(struct ofono_call_volume *v, int muted,
				ofono_call_volume_cb_t cb, void *data)
{
	struct ril_call_volume *vd = ril_call_volume_get_data(v);
	struct ril_call_volume_req *cbd;
	GRilIoRequest *req = grilio_request_sized_new(8);

	cbd = g_new(struct ril_call_volume_req, 1);
	cbd->cb = cb;
	cbd->data = data;

	DBG("%d", muted);
	grilio_request_append_int32(req, 1);
	grilio_request_append_int32(req, muted);
	grilio_queue_send_request_full(vd->q, req, RIL_REQUEST_SET_MUTE,
				 ril_call_volume_mute_cb, g_free, cbd);
	grilio_request_unref(req);
}

static void ril_call_volume_query_mute_cb(GRilIoChannel *io, int status,
				const void *data, guint len, void *user_data)
{
	struct ril_call_volume *vd = user_data;

	if (status == RIL_E_SUCCESS) {
		int muted = 0;
		GRilIoParser rilp;

		grilio_parser_init(&rilp, data, len);
		grilio_parser_get_int32(&rilp, NULL); /* Array length */
		grilio_parser_get_int32(&rilp, &muted);
		DBG("{%d}", muted);
		ofono_call_volume_set_muted(vd->v, muted);
	} else {
		ofono_error("Could not retrive the ril mute state");
	}
}

static gboolean ril_call_volume_register(gpointer user_data)
{
	struct ril_call_volume *vd = user_data;

	DBG("");
	GASSERT(vd->timer_id);
	vd->timer_id = 0;
	ofono_call_volume_register(vd->v);

	/* Probe the mute state */
	grilio_queue_send_request_full(vd->q, NULL,
		RIL_REQUEST_GET_MUTE, ril_call_volume_query_mute_cb, NULL, vd);

	/* This makes the timeout a single-shot */
	return FALSE;
}

static int ril_call_volume_probe(struct ofono_call_volume *v,
				unsigned int vendor, void *data)
{
	struct ril_modem *modem = data;
	struct ril_call_volume *vd = g_new0(struct ril_call_volume, 1);

	DBG("");
	vd->v = v;
	vd->q = grilio_queue_new(ril_modem_io(modem));
	vd->timer_id = g_idle_add(ril_call_volume_register, vd);
	ofono_call_volume_set_data(v, vd);
	return 0;
}

static void ril_call_volume_remove(struct ofono_call_volume *v)
{
	struct ril_call_volume *vd = ril_call_volume_get_data(v);

	DBG("");
	ofono_call_volume_set_data(v, NULL);

	if (vd->timer_id) {
		g_source_remove(vd->timer_id);
	}

	grilio_queue_cancel_all(vd->q, FALSE);
	grilio_queue_unref(vd->q);
	g_free(vd);
}

const struct ofono_call_volume_driver ril_call_volume_driver = {
	.name       = RILMODEM_DRIVER,
	.probe      = ril_call_volume_probe,
	.remove     = ril_call_volume_remove,
	.mute       = ril_call_volume_mute,
};

/*
 * Local Variables:
 * mode: C
 * c-basic-offset: 8
 * indent-tabs-mode: t
 * End:
 */
