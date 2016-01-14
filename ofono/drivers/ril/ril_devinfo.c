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
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 */

#include "ril_plugin.h"
#include "ril_util.h"
#include "ril_log.h"

/*
 * TODO: No public RIL api to query manufacturer or model.
 * Check where to get, could /system/build.prop be updated to have good values?
 */

struct ril_devinfo {
	struct ofono_devinfo *info;
	GRilIoQueue *q;
	guint register_id;
	guint imei_id;
	char *imei;
};

struct ril_devinfo_cbd {
	struct ril_devinfo *di;
	ofono_devinfo_query_cb_t cb;
	gpointer data;
};

#define ril_devinfo_cbd_free g_free

static inline struct ril_devinfo *ril_devinfo_get_data(
					struct ofono_devinfo *info)
{
	return ofono_devinfo_get_data(info);
}

struct ril_devinfo_cbd *ril_devinfo_cbd_new(struct ril_devinfo *di,
				ofono_devinfo_query_cb_t cb, void *data)
{
	struct ril_devinfo_cbd *cbd = g_new0(struct ril_devinfo_cbd, 1);

	cbd->di = di;
	cbd->cb = cb;
	cbd->data = data;
	return cbd;
}

static void ril_devinfo_query_unsupported(struct ofono_devinfo *info,
			ofono_devinfo_query_cb_t cb, void *data)
{
	struct ofono_error error;
	cb(ril_error_failure(&error), "", data);
}

static void ril_devinfo_query_cb(GRilIoChannel *io, int status,
			const void *data, guint len, void *user_data)
{
	struct ofono_error error;
	struct ril_devinfo_cbd *cbd = user_data;

	if (status == RIL_E_SUCCESS) {
		char *res;
		GRilIoParser rilp;
		grilio_parser_init(&rilp, data, len);
		res = grilio_parser_get_utf8(&rilp);
		DBG("%s", res);
		cbd->cb(ril_error_ok(&error), res ? res : "", cbd->data);
		g_free(res);
	} else {
		cbd->cb(ril_error_failure(&error), NULL, cbd->data);
	}
}

static void ril_devinfo_query_revision(struct ofono_devinfo *info,
				ofono_devinfo_query_cb_t cb, void *data)
{
	struct ril_devinfo *di = ril_devinfo_get_data(info);

	DBG("");
	grilio_queue_send_request_full(di->q, NULL, RIL_REQUEST_BASEBAND_VERSION,
				ril_devinfo_query_cb, ril_devinfo_cbd_free,
				ril_devinfo_cbd_new(di, cb, data));
}

static gboolean ril_devinfo_query_serial_cb(void *user_data)
{
	struct ril_devinfo_cbd *cbd = user_data;
	struct ril_devinfo *di = cbd->di;
	struct ofono_error error;

	GASSERT(di->imei_id);
	di->imei_id = 0;

	cbd->cb(ril_error_ok(&error), di->imei, cbd->data);
	return FALSE;
}

static void ril_devinfo_query_serial(struct ofono_devinfo *info,
				ofono_devinfo_query_cb_t cb,
				void *data)
{
	struct ril_devinfo *di = ril_devinfo_get_data(info);

	GASSERT(!di->imei_id);
	if (di->imei_id) {
		g_source_remove(di->imei_id);
		di->imei_id = 0;
	}

	DBG("%s", di->imei);
	di->imei_id = g_idle_add_full(G_PRIORITY_DEFAULT_IDLE,
					ril_devinfo_query_serial_cb,
					ril_devinfo_cbd_new(di, cb, data),
					ril_devinfo_cbd_free);
}

static gboolean ril_devinfo_register(gpointer user_data)
{
	struct ril_devinfo *di = user_data;

	DBG("");
	di->register_id = 0;
	ofono_devinfo_register(di->info);

	/* This makes the timeout a single-shot */
	return FALSE;
}

static int ril_devinfo_probe(struct ofono_devinfo *info, unsigned int vendor,
				void *data)
{
	struct ril_modem *modem = data;
	struct ril_devinfo *di = g_new0(struct ril_devinfo, 1);

	DBG("%s %s %p", ril_modem_get_path(modem), modem->imei, di);
	GASSERT(modem->imei);
	di->q = grilio_queue_new(ril_modem_io(modem));
	di->info = info;
	di->imei = g_strdup(modem->imei);

	di->register_id = g_idle_add(ril_devinfo_register, di);
	ofono_devinfo_set_data(info, di);
	return 0;
}

static void ril_devinfo_remove(struct ofono_devinfo *info)
{
	struct ril_devinfo *di = ril_devinfo_get_data(info);

	DBG("%p", di);
	ofono_devinfo_set_data(info, NULL);

	if (di->register_id > 0) {
		g_source_remove(di->register_id);
	}

	if (di->imei_id > 0) {
		g_source_remove(di->imei_id);
	}

	grilio_queue_cancel_all(di->q, FALSE);
	grilio_queue_unref(di->q);
	g_free(di->imei);
	g_free(di);
}

const struct ofono_devinfo_driver ril_devinfo_driver = {
	.name                   = RILMODEM_DRIVER,
	.probe                  = ril_devinfo_probe,
	.remove                 = ril_devinfo_remove,
	.query_manufacturer     = ril_devinfo_query_unsupported,
	.query_model            = ril_devinfo_query_unsupported,
	.query_revision         = ril_devinfo_query_revision,
	.query_serial           = ril_devinfo_query_serial
};

/*
 * Local Variables:
 * mode: C
 * c-basic-offset: 8
 * indent-tabs-mode: t
 * End:
 */
