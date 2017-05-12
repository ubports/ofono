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

#include <gutil_idlequeue.h>

/*
 * TODO: No public RIL api to query manufacturer or model.
 * Check where to get, could /system/build.prop be updated to have good values?
 */

enum ril_devinfo_cb_tag {
	DEVINFO_QUERY_SERIAL = 1,
	DEVINFO_QUERY_SVN
};

struct ril_devinfo {
	struct ofono_devinfo *info;
	GRilIoQueue *q;
	GUtilIdleQueue *iq;
	char *log_prefix;
	char *imeisv;
	char *imei;
};

struct ril_devinfo_cbd {
	struct ril_devinfo *di;
	ofono_devinfo_query_cb_t cb;
	gpointer data;
};

#define DBG_(self,fmt,args...) DBG("%s" fmt, (self)->log_prefix, ##args)
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

static void ril_devinfo_query_revision_cb(GRilIoChannel *io, int status,
			const void *data, guint len, void *user_data)
{
	struct ofono_error error;
	struct ril_devinfo_cbd *cbd = user_data;

	if (status == RIL_E_SUCCESS) {
		char *res;
		GRilIoParser rilp;
		grilio_parser_init(&rilp, data, len);
		res = grilio_parser_get_utf8(&rilp);
		DBG_(cbd->di, "%s", res);
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

	DBG_(di, "");
	grilio_queue_send_request_full(di->q, NULL,
				RIL_REQUEST_BASEBAND_VERSION,
				ril_devinfo_query_revision_cb,
				ril_devinfo_cbd_free,
				ril_devinfo_cbd_new(di, cb, data));
}

static void ril_devinfo_query_serial_cb(gpointer user_data)
{
	struct ril_devinfo_cbd *cbd = user_data;
	struct ril_devinfo *di = cbd->di;
	struct ofono_error error;

	DBG_(di, "%s", di->imei);
	cbd->cb(ril_error_ok(&error), di->imei, cbd->data);
}

static void ril_devinfo_query_svn_cb(gpointer user_data)
{
	struct ril_devinfo_cbd *cbd = user_data;
	struct ril_devinfo *di = cbd->di;
	struct ofono_error error;

	DBG_(di, "%s", di->imeisv);
	if (di->imeisv && di->imeisv[0]) {
		cbd->cb(ril_error_ok(&error), di->imeisv, cbd->data);
	} else {
		cbd->cb(ril_error_failure(&error), "", cbd->data);
	}
}

static void ril_devinfo_query(struct ril_devinfo *di,
			enum ril_devinfo_cb_tag tag, GUtilIdleFunc fn,
			ofono_devinfo_query_cb_t cb, void *data)
{
	GVERIFY_FALSE(gutil_idle_queue_cancel_tag(di->iq, tag));
	gutil_idle_queue_add_tag_full(di->iq, tag, fn,
					ril_devinfo_cbd_new(di, cb, data),
					ril_devinfo_cbd_free);
}

static void ril_devinfo_query_serial(struct ofono_devinfo *info,
				ofono_devinfo_query_cb_t cb,
				void *data)
{
	struct ril_devinfo *di = ril_devinfo_get_data(info);

	DBG_(di, "");
	ril_devinfo_query(di, DEVINFO_QUERY_SERIAL,
				ril_devinfo_query_serial_cb, cb, data);
}

static void ril_devinfo_query_svn(struct ofono_devinfo *info,
				ofono_devinfo_query_cb_t cb,
				void *data)
{
	struct ril_devinfo *di = ril_devinfo_get_data(info);

	DBG_(di, "");
	ril_devinfo_query(di, DEVINFO_QUERY_SVN,
				ril_devinfo_query_svn_cb, cb, data);
}

static void ril_devinfo_register(gpointer user_data)
{
	struct ril_devinfo *di = user_data;

	DBG_(di, "");
	ofono_devinfo_register(di->info);
}

static int ril_devinfo_probe(struct ofono_devinfo *info, unsigned int vendor,
				void *data)
{
	struct ril_modem *modem = data;
	struct ril_devinfo *di = g_new0(struct ril_devinfo, 1);

	di->log_prefix = (modem->log_prefix && modem->log_prefix[0]) ?
		g_strconcat(modem->log_prefix, " ", NULL) : g_strdup("");

	DBG_(di, "%s", modem->imei);
	GASSERT(modem->imei);

	di->q = grilio_queue_new(ril_modem_io(modem));
	di->info = info;
	di->imeisv = g_strdup(modem->imeisv);
	di->imei = g_strdup(modem->imei);
	di->iq = gutil_idle_queue_new();
	gutil_idle_queue_add(di->iq, ril_devinfo_register, di);
	ofono_devinfo_set_data(info, di);
	return 0;
}

static void ril_devinfo_remove(struct ofono_devinfo *info)
{
	struct ril_devinfo *di = ril_devinfo_get_data(info);

	DBG_(di, "");
	ofono_devinfo_set_data(info, NULL);
	gutil_idle_queue_cancel_all(di->iq);
	gutil_idle_queue_unref(di->iq);
	grilio_queue_cancel_all(di->q, FALSE);
	grilio_queue_unref(di->q);
	g_free(di->log_prefix);
	g_free(di->imeisv);
	g_free(di->imei);
	g_free(di);
}

const struct ofono_devinfo_driver ril_devinfo_driver = {
	.name                   = RILMODEM_DRIVER,
	.probe                  = ril_devinfo_probe,
	.remove                 = ril_devinfo_remove,
	/* query_revision won't be called if query_model is missing */
	.query_model            = ril_devinfo_query_unsupported,
	.query_revision         = ril_devinfo_query_revision,
	.query_serial           = ril_devinfo_query_serial,
	.query_svn              = ril_devinfo_query_svn
};

/*
 * Local Variables:
 * mode: C
 * c-basic-offset: 8
 * indent-tabs-mode: t
 * End:
 */
