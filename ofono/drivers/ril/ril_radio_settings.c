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

#include "storage.h"

struct ril_radio_settings {
	GRilIoQueue *q;
	int ratmode;
	guint timer_id;
};

struct ril_radio_settings_cbd {
	struct ril_radio_settings *rsd;
	union _ofono_radio_settings_cb {
		ofono_radio_settings_rat_mode_set_cb_t rat_mode_set;
		ofono_radio_settings_rat_mode_query_cb_t rat_mode_query;
		gpointer ptr;
	} cb;
	gpointer data;
};

#define RIL_STORE "rilmodem"
#define LTE_FLAG "4gOn"

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
		ofono_error("rat mode setting failed");
		cb(ril_error_failure(&error), cbd->data);
	}
}

static void ril_radio_settings_set_rat_mode(struct ofono_radio_settings *rs,
		enum ofono_radio_access_mode mode,
		ofono_radio_settings_rat_mode_set_cb_t cb, void *data)
{
	struct ril_radio_settings *rsd = ril_radio_settings_get_data(rs);
	GRilIoRequest *req = grilio_request_sized_new(8);
	int pref = rsd->ratmode;

	ofono_info("rat mode set %d", mode);
	switch (mode) {
	case OFONO_RADIO_ACCESS_MODE_GSM:
		pref = PREF_NET_TYPE_GSM_ONLY;
		break;
	case OFONO_RADIO_ACCESS_MODE_UMTS:
		pref = PREF_NET_TYPE_GSM_WCDMA_AUTO;    /* per UI design */
		break;
	case OFONO_RADIO_ACCESS_MODE_LTE:
		pref = PREF_NET_TYPE_LTE_ONLY;
	default:
		break;
	}

	grilio_request_append_int32(req, 1);            /* Number of params */
	grilio_request_append_int32(req, pref);
	ril_radio_settings_submit_request(rsd, req,
			RIL_REQUEST_SET_PREFERRED_NETWORK_TYPE,
			ril_radio_settings_set_rat_mode_cb, cb, data);
	grilio_request_unref(req);
}

static void ril_radio_settings_force_rat(struct ril_radio_settings *rsd,
								int pref)
{
	if (pref != rsd->ratmode) {
		GRilIoRequest *req = grilio_request_sized_new(8);
		DBG("pref ril rat mode %d, ril current %d", pref, rsd->ratmode);

		grilio_request_append_int32(req, 1);
		grilio_request_append_int32(req, rsd->ratmode);
		grilio_queue_send_request(rsd->q, req,
				RIL_REQUEST_SET_PREFERRED_NETWORK_TYPE);
		grilio_request_unref(req);
	}
}

static void ril_radio_settings_query_rat_mode_cb(GRilIoChannel *io, int status,
				const void *data, guint len, void *user_data)
{
	struct ofono_error error;
	struct ril_radio_settings_cbd *cbd = user_data;
	ofono_radio_settings_rat_mode_query_cb_t cb = cbd->cb.rat_mode_query;

	DBG("");
	if (status == RIL_E_SUCCESS) {
		GRilIoParser rilp;
		int mode = OFONO_RADIO_ACCESS_MODE_ANY;
		int pref = -1;

		grilio_parser_init(&rilp, data, len);
		grilio_parser_get_int32(&rilp, NULL);
		grilio_parser_get_int32(&rilp, &pref);

		switch (pref) {
		case PREF_NET_TYPE_LTE_ONLY:
			mode = OFONO_RADIO_ACCESS_MODE_LTE;
		case PREF_NET_TYPE_GSM_ONLY:
			mode = OFONO_RADIO_ACCESS_MODE_GSM;
			break;
		case PREF_NET_TYPE_GSM_WCDMA_AUTO:/* according to UI design */
			if (!cb) {
				ril_radio_settings_force_rat(cbd->rsd, pref);
			}
		case PREF_NET_TYPE_WCDMA:
		case PREF_NET_TYPE_GSM_WCDMA: /* according to UI design */
			mode = OFONO_RADIO_ACCESS_MODE_UMTS;
			break;
		case PREF_NET_TYPE_LTE_CDMA_EVDO:
		case PREF_NET_TYPE_LTE_GSM_WCDMA:
		case PREF_NET_TYPE_LTE_CMDA_EVDO_GSM_WCDMA:
			if (!cb) {
				ril_radio_settings_force_rat(cbd->rsd, pref);
			}
			break;
		case PREF_NET_TYPE_CDMA_EVDO_AUTO:
		case PREF_NET_TYPE_CDMA_ONLY:
		case PREF_NET_TYPE_EVDO_ONLY:
		case PREF_NET_TYPE_GSM_WCDMA_CDMA_EVDO_AUTO:
		default:
			break;
		}

		ofono_info("rat mode %d (ril %d)", mode, pref);
		if (cb) {
			cb(ril_error_ok(&error), mode, cbd->data);
		}
	} else {
		ofono_error("rat mode query failed");
		if (cb) {
			cb(ril_error_failure(&error), -1, cbd->data);
		}
	}
}

static void ril_radio_settings_query_rat_mode(struct ofono_radio_settings *rs,
		ofono_radio_settings_rat_mode_query_cb_t cb, void *data)
{
	struct ril_radio_settings *rsd = ril_radio_settings_get_data(rs);

	ofono_info("rat mode query");
	ril_radio_settings_submit_request(rsd, NULL,
			RIL_REQUEST_GET_PREFERRED_NETWORK_TYPE,
			ril_radio_settings_query_rat_mode_cb, cb, data);
}

static gboolean ril_radio_settings_get_config(struct ril_radio_settings *rsd)
{
	gboolean needsconfig = FALSE;
	gboolean value = FALSE;

	/* Hmm... One file shared by all modems... Why?? */

	GKeyFile *keyfile = storage_open(NULL, RIL_STORE);
	char **alreadyset = g_key_file_get_groups(keyfile, NULL);

	if (alreadyset[0])
		value = g_key_file_get_boolean(
			keyfile, alreadyset[0], LTE_FLAG, NULL);
	else if (rsd->ratmode == PREF_NET_TYPE_GSM_WCDMA_AUTO)
		value = TRUE;

	if (!value && rsd->ratmode == PREF_NET_TYPE_LTE_GSM_WCDMA) {
			g_key_file_set_boolean(keyfile,
				LTE_FLAG, LTE_FLAG, TRUE);
			needsconfig = TRUE;
	} else if (value && rsd->ratmode == PREF_NET_TYPE_GSM_WCDMA_AUTO) {
			g_key_file_set_boolean(keyfile,
				LTE_FLAG, LTE_FLAG, FALSE);
			needsconfig = TRUE;
	}

	g_strfreev(alreadyset);
	storage_close(NULL, RIL_STORE, keyfile, TRUE);

	DBG("needsconfig %d, rat mode %d", needsconfig, rsd->ratmode);
	return needsconfig;
}

static gboolean ril_radio_settings_register(gpointer user_data)
{
	struct ofono_radio_settings *rs = user_data;
	struct ril_radio_settings *rsd = ril_radio_settings_get_data(rs);

	rsd->timer_id = 0;
	ofono_radio_settings_register(rs);

	if (ril_radio_settings_get_config(rsd)) {
		ril_radio_settings_submit_request(rsd, NULL,
			RIL_REQUEST_GET_PREFERRED_NETWORK_TYPE,
			ril_radio_settings_query_rat_mode_cb, NULL, NULL);
	}

	/* Single shot */
	return FALSE;
}

static int ril_radio_settings_probe(struct ofono_radio_settings *rs,
					unsigned int vendor, void *data)
{
	struct ril_modem *modem = data;
	struct ril_radio_settings *rsd = g_new0(struct ril_radio_settings, 1);

	DBG("");
	rsd->q = grilio_queue_new(ril_modem_io(modem));
	rsd->ratmode = ril_modem_4g_enabled(modem) ?
		PREF_NET_TYPE_LTE_GSM_WCDMA :
		PREF_NET_TYPE_GSM_WCDMA_AUTO;

	rsd->timer_id = g_idle_add(ril_radio_settings_register, rs);
	ofono_radio_settings_set_data(rs, rsd);
	return 0;
}

static void ril_radio_settings_remove(struct ofono_radio_settings *rs)
{
	struct ril_radio_settings *rd = ril_radio_settings_get_data(rs);

	DBG("");
	ofono_radio_settings_set_data(rs, NULL);
	if (rd->timer_id > 0) {
		g_source_remove(rd->timer_id);
        }

	grilio_queue_cancel_all(rd->q, FALSE);
	grilio_queue_unref(rd->q);
	g_free(rd);
}

const struct ofono_radio_settings_driver ril_radio_settings_driver = {
	.name           = RILMODEM_DRIVER,
	.probe          = ril_radio_settings_probe,
	.remove         = ril_radio_settings_remove,
	.query_rat_mode = ril_radio_settings_query_rat_mode,
	.set_rat_mode   = ril_radio_settings_set_rat_mode,
};

/*
 * Local Variables:
 * mode: C
 * c-basic-offset: 8
 * indent-tabs-mode: t
 * End:
 */
