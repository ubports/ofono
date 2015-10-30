/*
 *  oFono - Open Source Telephony - RIL-based devices
 *
 *  Copyright (C) 2015 Jolla Ltd.
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
#include "ril_constants.h"

struct ril_oem_raw {
	GRilIoQueue *q;
	guint timer_id;
};

struct ril_oem_raw_cbd {
	ofono_oem_raw_query_cb_t cb;
	gpointer data;
};

#define ril_oem_raw_cbd_free g_free

static inline struct ril_oem_raw *ril_oem_raw_get_data(
						struct ofono_oem_raw *raw)
{
	return ofono_oem_raw_get_data(raw);
}

static struct ril_oem_raw_cbd *ril_oem_raw_cbd_new(ofono_oem_raw_query_cb_t cb,
								void *data)
{
	struct ril_oem_raw_cbd *cbd = g_new0(struct ril_oem_raw_cbd, 1);

	cbd->cb = cb;
	cbd->data = data;
	return cbd;
}

static void ril_oem_raw_request_cb(GRilIoChannel *io, int status,
			const void *data, guint len, void *user_data)
{
	struct ofono_error error;
	struct ril_oem_raw_cbd *cbd = user_data;

	if (status == RIL_E_SUCCESS) {
		struct ofono_oem_raw_results result;

		result.data = (void *)data;
		result.length = len;
		cbd->cb(ril_error_ok(&error), &result, cbd->data);
	} else {
		DBG("error:%d len:%d ", status, len);
		cbd->cb(ril_error_failure(&error), NULL, cbd->data);
	}
}

static void ril_oem_raw_request(struct ofono_oem_raw *raw,
			       const struct ofono_oem_raw_request *request,
			       ofono_oem_raw_query_cb_t cb, void *data)
{
	struct ril_oem_raw *od = ril_oem_raw_get_data(raw);
	GRilIoRequest *req = grilio_request_sized_new(request->length);

	grilio_request_append_bytes(req, request->data, request->length);
	grilio_queue_send_request_full(od->q, req, RIL_REQUEST_OEM_HOOK_RAW,
				ril_oem_raw_request_cb, ril_oem_raw_cbd_free,
				ril_oem_raw_cbd_new(cb, data));
	grilio_request_unref(req);
}

static gboolean ril_oem_raw_register(gpointer user_data)
{
	struct ofono_oem_raw *raw = user_data;
	struct ril_oem_raw *od = ril_oem_raw_get_data(raw);

	DBG("");
	GASSERT(od->timer_id);
	od->timer_id = 0;
	ofono_oem_raw_dbus_register(raw);

	/* Single-shot */
	return FALSE;
}

static int ril_oem_raw_probe(struct ofono_oem_raw *raw, unsigned int vendor,
								void *data)
{
	struct ril_modem *modem = data;
	struct ril_oem_raw *od = g_new0(struct ril_oem_raw, 1);

	DBG("");
	od->q = grilio_queue_new(ril_modem_io(modem));
	od->timer_id = g_idle_add(ril_oem_raw_register, raw);
	ofono_oem_raw_set_data(raw, od);
	return 0;
}

static void ril_oem_raw_remove(struct ofono_oem_raw *raw)
{
	struct ril_oem_raw *od = ril_oem_raw_get_data(raw);

	DBG("");
	grilio_queue_cancel_all(od->q, TRUE);
	ofono_oem_raw_set_data(raw, NULL);

	if (od->timer_id) {
		g_source_remove(od->timer_id);
	}

	grilio_queue_unref(od->q);
	g_free(od);
}

/* const */ struct ofono_oem_raw_driver ril_oem_raw_driver = {
	.name           = RILMODEM_DRIVER,
	.probe          = ril_oem_raw_probe,
	.remove         = ril_oem_raw_remove,
	.request        = ril_oem_raw_request,
};

/*
 * Local Variables:
 * mode: C
 * c-basic-offset: 8
 * indent-tabs-mode: t
 * End:
 */
