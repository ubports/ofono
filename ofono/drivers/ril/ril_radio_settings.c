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

struct ril_radio_settings {
	GRilIoQueue *q;
	struct ofono_radio_settings *rs;
	enum ofono_radio_access_mode access_mode;
	gboolean enable_4g;
	int ratmode;
	guint query_rats_id;
};

struct ril_radio_settings_cbd {
	struct ril_radio_settings *rsd;
	union _ofono_radio_settings_cb {
		ofono_radio_settings_rat_mode_set_cb_t rat_mode_set;
		ofono_radio_settings_rat_mode_query_cb_t rat_mode_query;
		ofono_radio_settings_available_rats_query_cb_t available_rats;
		gpointer ptr;
	} cb;
	gpointer data;
};

#define ril_radio_settings_cbd_free g_free

static inline struct ril_radio_settings *ril_radio_settings_get_data(
					struct ofono_radio_settings *rs)
{
	return ofono_radio_settings_get_data(rs);
}

static struct ril_radio_settings_cbd *ril_radio_settings_cbd_new(
			struct ril_radio_settings *rsd, void *cb, void *data)
{
	struct ril_radio_settings_cbd *cbd;

	cbd = g_new0(struct ril_radio_settings_cbd, 1);
	cbd->rsd = rsd;
	cbd->cb.ptr = cb;
	cbd->data = data;
	return cbd;
}

static enum ofono_radio_access_mode ril_radio_settings_pref_to_mode(int pref)
{
	switch (pref) {
	case PREF_NET_TYPE_LTE_CDMA_EVDO:
	case PREF_NET_TYPE_LTE_GSM_WCDMA:
	case PREF_NET_TYPE_LTE_CMDA_EVDO_GSM_WCDMA:
	case PREF_NET_TYPE_LTE_ONLY:
	case PREF_NET_TYPE_LTE_WCDMA:
		return OFONO_RADIO_ACCESS_MODE_LTE;
	case PREF_NET_TYPE_GSM_ONLY:
		return OFONO_RADIO_ACCESS_MODE_GSM;
	case PREF_NET_TYPE_GSM_WCDMA_AUTO:
	case PREF_NET_TYPE_WCDMA:
	case PREF_NET_TYPE_GSM_WCDMA:
		return OFONO_RADIO_ACCESS_MODE_UMTS;
	default:
		return OFONO_RADIO_ACCESS_MODE_ANY;
	}
}

static int ril_radio_settings_mode_to_pref(struct ril_radio_settings *rsd,
					enum ofono_radio_access_mode mode)
{
	switch (mode) {
	case OFONO_RADIO_ACCESS_MODE_ANY:
	case OFONO_RADIO_ACCESS_MODE_LTE:
		if (rsd->enable_4g) {
			return PREF_NET_TYPE_LTE_WCDMA;
		}
		/* no break */
	case OFONO_RADIO_ACCESS_MODE_UMTS:
		return PREF_NET_TYPE_GSM_WCDMA_AUTO;
	case OFONO_RADIO_ACCESS_MODE_GSM:
		return PREF_NET_TYPE_GSM_ONLY;
	default:
		return -1;
	}
}

static void ril_radio_settings_submit_request(struct ril_radio_settings *rsd,
	GRilIoRequest* req, guint code, GRilIoChannelResponseFunc response,
							void *cb, void *data)
{
	grilio_queue_send_request_full(rsd->q, req, code, response,
				ril_radio_settings_cbd_free,
				ril_radio_settings_cbd_new(rsd, cb, data));
}

static void ril_radio_settings_set_rat_mode_cb(GRilIoChannel *io, int status,
				const void *data, guint len, void *user_data)
{
	struct ofono_error error;
	struct ril_radio_settings_cbd *cbd = user_data;
	ofono_radio_settings_rat_mode_set_cb_t cb = cbd->cb.rat_mode_set;

	if (status == RIL_E_SUCCESS) {
		cb(ril_error_ok(&error), cbd->data);
	} else {
		ofono_error("failed to set rat mode");
		cb(ril_error_failure(&error), cbd->data);
	}
}

static GRilIoRequest *ril_radio_settings_set_pref_req(int pref)
{
	GRilIoRequest *req = grilio_request_sized_new(8);
	grilio_request_append_int32(req, 1);        /* Number of params */
	grilio_request_append_int32(req, pref);
	return req;
}

static int ril_radio_settings_parse_pref_resp(const void *data, guint len)
{
	GRilIoParser rilp;
	int pref = -1;

	grilio_parser_init(&rilp, data, len);
	grilio_parser_get_int32(&rilp, NULL);
	grilio_parser_get_int32(&rilp, &pref);
	return pref;
}

static void ril_radio_settings_set_rat_mode(struct ofono_radio_settings *rs,
		enum ofono_radio_access_mode mode,
		ofono_radio_settings_rat_mode_set_cb_t cb, void *data)
{
	struct ril_radio_settings *rsd = ril_radio_settings_get_data(rs);
	int pref = ril_radio_settings_mode_to_pref(rsd, mode);
	GRilIoRequest *req;

	if (pref < 0) pref = rsd->ratmode;
	DBG("rat mode set %d (ril %d)", mode, pref);
	req = ril_radio_settings_set_pref_req(pref);
	ril_radio_settings_submit_request(rsd, req,
			RIL_REQUEST_SET_PREFERRED_NETWORK_TYPE,
			ril_radio_settings_set_rat_mode_cb, cb, data);
	grilio_request_unref(req);
}

static void ril_radio_settings_query_rat_mode_cb(GRilIoChannel *io, int status,
				const void *data, guint len, void *user_data)
{
	struct ofono_error error;
	struct ril_radio_settings_cbd *cbd = user_data;
	struct ril_radio_settings *rsd = cbd->rsd;
	ofono_radio_settings_rat_mode_query_cb_t cb = cbd->cb.rat_mode_query;

	if (status == RIL_E_SUCCESS) {
		rsd->ratmode = ril_radio_settings_parse_pref_resp(data, len);
		DBG("rat mode %d (ril %d)",
			ril_radio_settings_pref_to_mode(rsd->ratmode),
			rsd->ratmode);
	} else {
		/*
		 * With certain versions of RIL, preferred network type
		 * queries don't work even though setting preferred network
		 * type does actually work. In this case, assume that our
		 * cached network type is the right one.
		 */
		ofono_error("rat mode query failed, assuming %d (ril %d)",
			ril_radio_settings_pref_to_mode(rsd->ratmode),
			rsd->ratmode);
	}

	cb(ril_error_ok(&error), ril_radio_settings_pref_to_mode(rsd->ratmode),
								cbd->data);
}

static void ril_radio_settings_query_rat_mode(struct ofono_radio_settings *rs,
		ofono_radio_settings_rat_mode_query_cb_t cb, void *data)
{
	struct ril_radio_settings *rsd = ril_radio_settings_get_data(rs);

	DBG("rat mode query");
	ril_radio_settings_submit_request(rsd, NULL,
			RIL_REQUEST_GET_PREFERRED_NETWORK_TYPE,
			ril_radio_settings_query_rat_mode_cb, cb, data);
}

static gboolean ril_radio_settings_query_available_rats_cb(gpointer data)
{
	struct ofono_error error;
	struct ril_radio_settings_cbd *cbd = data;
	guint rats = OFONO_RADIO_ACCESS_MODE_GSM | OFONO_RADIO_ACCESS_MODE_UMTS;

	if (cbd->rsd->enable_4g) {
		rats |= OFONO_RADIO_ACCESS_MODE_LTE;
	}

	GASSERT(cbd->rsd->query_rats_id);
	cbd->rsd->query_rats_id = 0;
	cbd->cb.available_rats(ril_error_ok(&error), rats, cbd->data);
	return FALSE;
}

static void ril_radio_settings_query_available_rats(
		struct ofono_radio_settings *rs,
		ofono_radio_settings_available_rats_query_cb_t cb, void *data)
{
	struct ril_radio_settings *rsd = ril_radio_settings_get_data(rs);

	DBG("");
	GASSERT(!rsd->query_rats_id);
	rsd->query_rats_id = g_idle_add_full(G_PRIORITY_DEFAULT_IDLE,
				ril_radio_settings_query_available_rats_cb,
				ril_radio_settings_cbd_new(rsd, cb, data),
				ril_radio_settings_cbd_free);
}

static void ril_radio_settings_init_query_cb(GRilIoChannel *io, int status,
				const void *data, guint len, void *user_data)
{
	int pref;
	struct ril_radio_settings *rsd = user_data;
	enum ofono_radio_access_mode mode;

	if (status == RIL_E_SUCCESS) {
		pref = ril_radio_settings_parse_pref_resp(data, len);
		DBG("rat mode %d", pref);
	} else {
		ofono_error("initial rat mode query failed");
		pref = ril_radio_settings_mode_to_pref(rsd,
						OFONO_RADIO_ACCESS_MODE_ANY);
	}

	mode = ril_radio_settings_pref_to_mode(pref);

	if (!rsd->enable_4g && mode == OFONO_RADIO_ACCESS_MODE_LTE) {
		rsd->ratmode = ril_radio_settings_mode_to_pref(rsd,
						OFONO_RADIO_ACCESS_MODE_UMTS);
	} else {
		rsd->ratmode = pref;
	}

	if (rsd->ratmode != pref || status != RIL_E_SUCCESS) {
		GRilIoRequest *req;

		DBG("forcing rat mode %d", rsd->ratmode);
		req = ril_radio_settings_set_pref_req(rsd->ratmode);
		grilio_queue_send_request(rsd->q, req,
				RIL_REQUEST_SET_PREFERRED_NETWORK_TYPE);
		grilio_request_unref(req);
	}

	ofono_radio_settings_register(rsd->rs);
}

static int ril_radio_settings_probe(struct ofono_radio_settings *rs,
					unsigned int vendor, void *data)
{
	struct ril_modem *modem = data;
	struct ril_radio_settings *rsd = g_new0(struct ril_radio_settings, 1);

	DBG("");
	rsd->rs = rs;
	rsd->q = grilio_queue_new(ril_modem_io(modem));
	rsd->enable_4g = ril_modem_4g_enabled(modem);
	grilio_queue_send_request_full(rsd->q, NULL,
				RIL_REQUEST_GET_PREFERRED_NETWORK_TYPE,
				ril_radio_settings_init_query_cb, NULL, rsd);

	ofono_radio_settings_set_data(rs, rsd);
	return 0;
}

static void ril_radio_settings_remove(struct ofono_radio_settings *rs)
{
	struct ril_radio_settings *rsd = ril_radio_settings_get_data(rs);

	DBG("");
	ofono_radio_settings_set_data(rs, NULL);
	if (rsd->query_rats_id > 0) {
		g_source_remove(rsd->query_rats_id);
        }

	grilio_queue_cancel_all(rsd->q, FALSE);
	grilio_queue_unref(rsd->q);
	g_free(rsd);
}

const struct ofono_radio_settings_driver ril_radio_settings_driver = {
	.name                 = RILMODEM_DRIVER,
	.probe                = ril_radio_settings_probe,
	.remove               = ril_radio_settings_remove,
	.query_rat_mode       = ril_radio_settings_query_rat_mode,
	.set_rat_mode         = ril_radio_settings_set_rat_mode,
	.query_available_rats = ril_radio_settings_query_available_rats
};

/*
 * Local Variables:
 * mode: C
 * c-basic-offset: 8
 * indent-tabs-mode: t
 * End:
 */
