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

#include <gutil_strv.h>

struct ril_cbs {
	struct ofono_cbs *cbs;
	GRilIoChannel *io;
	GRilIoQueue *q;
	char *log_prefix;
	gulong event_id;
};

struct ril_cbs_cbd {
	struct ril_cbs *cd;
	ofono_cbs_set_cb_t cb;
	gpointer data;
};

#define RIL_CBS_CHECK_RETRY_MS    1000
#define RIL_CBS_CHECK_RETRY_COUNT 30

#define DBG_(cd,fmt,args...) DBG("%s" fmt, (cd)->log_prefix, ##args)

#define ril_cbs_cbd_free g_free

static struct ril_cbs_cbd *ril_cbs_cbd_new(struct ril_cbs *cd,
		ofono_cbs_set_cb_t cb, void *data)
{
	struct ril_cbs_cbd *cbd = g_new(struct ril_cbs_cbd, 1);

	cbd->cd = cd;
	cbd->cb = cb;
	cbd->data = data;
	return cbd;
}

static void ril_cbs_request_activation(struct ril_cbs *cd,
		gboolean activate, GRilIoChannelResponseFunc response,
		GDestroyNotify destroy, void* user_data)
{
	GRilIoRequest* req = grilio_request_sized_new(8);

	grilio_request_append_int32(req, 1);
	grilio_request_append_int32(req, activate ? 0 :1);

	DBG_(cd, "%sactivating CB", activate ? "" : "de");
	grilio_queue_send_request_full(cd->q, req,
				RIL_REQUEST_GSM_SMS_BROADCAST_ACTIVATION,
				response, destroy, user_data);
	grilio_request_unref(req);
}

static void ril_cbs_set_config(struct ril_cbs *cd, const char *topics,
			GRilIoChannelResponseFunc response,
			GDestroyNotify destroy, void* user_data)
{
	char **list = topics ? g_strsplit(topics, ",", 0) : NULL;
	int i, n = gutil_strv_length(list);
	GRilIoRequest* req = grilio_request_new();

	grilio_request_append_int32(req, n);
	for (i = 0; i < n; i++) {
		const char *entry = list[i];
		const char *delim = strchr(entry, '-');
		int from, to;
		if (delim) {
			char **range = g_strsplit(topics, "-", 0);
			from = atoi(range[0]);
			to = atoi(range[1]);
			g_strfreev(range);
		} else {
			from = to = atoi(entry);
		}

		grilio_request_append_int32(req, from);
		grilio_request_append_int32(req, to);
		grilio_request_append_int32(req, 0);
		grilio_request_append_int32(req, 0xff);
		grilio_request_append_int32(req, 1);
	}

	DBG_(cd, "configuring CB");
	grilio_queue_send_request_full(cd->q, req,
			RIL_REQUEST_GSM_SET_BROADCAST_SMS_CONFIG,
			response, destroy, user_data);
	grilio_request_unref(req);
	g_strfreev(list);
}

static void ril_cbs_cb(GRilIoChannel *io, int ril_status,
				const void *data, guint len, void *user_data)
{
	struct ril_cbs_cbd *cbd = user_data;

	if (cbd->cb) {
		struct ofono_error error;

		if (ril_status == RIL_E_SUCCESS) {
			cbd->cb(ril_error_ok(&error), cbd->data);
		} else {
			cbd->cb(ril_error_failure(&error), cbd->data);
		}
	}
}

static void ril_cbs_set_topics(struct ofono_cbs *cbs, const char *topics,
				ofono_cbs_set_cb_t cb, void *data)
{
	struct ril_cbs *cd = ofono_cbs_get_data(cbs);

	DBG_(cd, "%s", topics);
	ril_cbs_set_config(cd, topics, ril_cbs_cb, ril_cbs_cbd_free,
					ril_cbs_cbd_new(cd, cb, data));
}

static void ril_cbs_clear_topics(struct ofono_cbs *cbs,
				ofono_cbs_set_cb_t cb, void *data)
{
	struct ril_cbs *cd = ofono_cbs_get_data(cbs);

	DBG_(cd, "");
	ril_cbs_request_activation(cd, FALSE, ril_cbs_cb, ril_cbs_cbd_free,
					ril_cbs_cbd_new(cd, cb, data));
}

static void ril_cbs_notify(GRilIoChannel *io, guint code,
				const void *data, guint len, void *user_data)
{
	struct ril_cbs *cd = user_data;

	GASSERT(code == RIL_UNSOL_RESPONSE_NEW_BROADCAST_SMS);
	DBG_(cd, "%u bytes", len);
	ofono_cbs_notify(cd->cbs, data, len);
}

static void ril_cbs_probe_done_cb(GRilIoChannel *io, int status,
				const void *data, guint len, void *user_data)
{
	struct ril_cbs *cd = user_data;

	if (status == RIL_E_SUCCESS) {
		DBG_(cd, "registering for CB");
		cd->event_id = grilio_channel_add_unsol_event_handler(cd->io,
			ril_cbs_notify, RIL_UNSOL_RESPONSE_NEW_BROADCAST_SMS,
									cd);
		ofono_cbs_register(cd->cbs);
	} else {
		DBG_(cd, "failed to query CB config");
		ofono_cbs_remove(cd->cbs);
	}
}

static int ril_cbs_probe(struct ofono_cbs *cbs, unsigned int vendor,
								void *data)
{
	struct ril_modem *modem = data;
	struct ril_cbs *cd = g_try_new0(struct ril_cbs, 1);
	GRilIoRequest* req = grilio_request_new();

	ofono_cbs_set_data(cbs, cd);
	cd->log_prefix = (modem->log_prefix && modem->log_prefix[0]) ?
		g_strconcat(modem->log_prefix, " ", NULL) : g_strdup("");
	cd->cbs = cbs;

	DBG_(cd, "");
	cd->io = grilio_channel_ref(ril_modem_io(modem));
	cd->q = grilio_queue_new(cd->io);

	/*
	 * RIL_REQUEST_GSM_GET_BROADCAST_SMS_CONFIG often fails at startup
	 * especially if other RIL requests are running in parallel. We may
	 * have to retry a few times. Also, make it blocking in order to
	 * improve the chance of success.
	 */
	grilio_request_set_retry(req, RIL_CBS_CHECK_RETRY_MS,
						RIL_CBS_CHECK_RETRY_COUNT);
	grilio_request_set_blocking(req, TRUE);
	grilio_queue_send_request_full(cd->q, req,
				RIL_REQUEST_GSM_GET_BROADCAST_SMS_CONFIG,
				ril_cbs_probe_done_cb, NULL, cd);
	grilio_request_unref(req);
	return 0;
}

static void ril_cbs_remove(struct ofono_cbs *cbs)
{
	struct ril_cbs *cd = ofono_cbs_get_data(cbs);

	DBG_(cd, "");
	ofono_cbs_set_data(cbs, NULL);
	grilio_channel_remove_handler(cd->io, cd->event_id);
	grilio_channel_unref(cd->io);
	grilio_queue_cancel_all(cd->q, FALSE);
	grilio_queue_unref(cd->q);
	g_free(cd->log_prefix);
	g_free(cd);
}

const struct ofono_cbs_driver ril_cbs_driver = {
	.name           = RILMODEM_DRIVER,
	.probe          = ril_cbs_probe,
	.remove         = ril_cbs_remove,
	.set_topics     = ril_cbs_set_topics,
	.clear_topics   = ril_cbs_clear_topics
};

/*
 * Local Variables:
 * mode: C
 * c-basic-offset: 8
 * indent-tabs-mode: t
 * End:
 */
