/*
 *  oFono - Open Source Telephony - RIL-based devices
 *
 *  Copyright (C) 2016 Jolla Ltd.
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
#include "ril_cell_info.h"
#include "ril_util.h"
#include "ril_log.h"

#include "ofono.h"

struct ril_netmon {
	struct ofono_netmon *netmon;
	struct ril_cell_info *cell_info;
	guint register_id;
};

static inline struct ril_netmon *ril_netmon_get_data(struct ofono_netmon *ofono)
{
	return ofono ? ofono_netmon_get_data(ofono) : NULL;
}

static void ril_netmon_format_mccmnc(char *s_mcc, char *s_mnc, int mcc, int mnc)
{
	s_mcc[0] = 0;
	s_mnc[0] = 0;

	if (mcc >= 0 && mcc <= 999) {
		snprintf(s_mcc, OFONO_MAX_MCC_LENGTH + 1, "%03d", mcc);
		if (mnc >= 0 && mnc <= 999) {
			const int mnclen = mnclength(mcc, mnc);
			const char *format[] = { "%d", "%02d", "%03d" };
			const char *fmt = (mnclen > 0 &&
				mnclen <= G_N_ELEMENTS(format)) ? 
				format[mnclen - 1] : format[0];
			snprintf(s_mnc, OFONO_MAX_MNC_LENGTH + 1, fmt, mnc);
		}
	}
}

static void ril_netmon_notify_gsm(struct ofono_netmon *netmon,
				const struct ril_cell_info_gsm *gsm)
{
	char mcc[OFONO_MAX_MCC_LENGTH + 1];
	char mnc[OFONO_MAX_MNC_LENGTH + 1];

	ril_netmon_format_mccmnc(mcc, mnc, gsm->mcc, gsm->mnc);
	ofono_netmon_serving_cell_notify(netmon,
			OFONO_NETMON_CELL_TYPE_GSM,
			OFONO_NETMON_INFO_MCC, mcc,
			OFONO_NETMON_INFO_MNC, mnc,
			OFONO_NETMON_INFO_LAC, gsm->lac,
			OFONO_NETMON_INFO_CI, gsm->cid,
			OFONO_NETMON_INFO_RSSI, gsm->signalStrength,
			OFONO_NETMON_INFO_BER, gsm->bitErrorRate,
			OFONO_NETMON_INFO_INVALID);
}

static void ril_netmon_notify_wcdma(struct ofono_netmon *netmon,
				const struct ril_cell_info_wcdma *wcdma)
{
	char mcc[OFONO_MAX_MCC_LENGTH + 1];
	char mnc[OFONO_MAX_MNC_LENGTH + 1];

	ril_netmon_format_mccmnc(mcc, mnc, wcdma->mcc, wcdma->mnc);
	ofono_netmon_serving_cell_notify(netmon,
			OFONO_NETMON_CELL_TYPE_UMTS,
			OFONO_NETMON_INFO_MCC, mcc,
			OFONO_NETMON_INFO_MNC, mnc,
			OFONO_NETMON_INFO_LAC, wcdma->lac,
			OFONO_NETMON_INFO_CI, wcdma->cid,
			OFONO_NETMON_INFO_PSC, wcdma->psc,
			OFONO_NETMON_INFO_RSSI, wcdma->signalStrength,
			OFONO_NETMON_INFO_BER, wcdma->bitErrorRate,
			OFONO_NETMON_INFO_INVALID);
}

static void ril_netmon_notify_lte(struct ofono_netmon *netmon,
				const struct ril_cell_info_lte *lte)
{
	char mcc[OFONO_MAX_MCC_LENGTH + 1];
	char mnc[OFONO_MAX_MNC_LENGTH + 1];

	ril_netmon_format_mccmnc(mcc, mnc, lte->mcc, lte->mnc);
	ofono_netmon_serving_cell_notify(netmon,
			OFONO_NETMON_CELL_TYPE_LTE,
			OFONO_NETMON_INFO_MCC, mcc,
			OFONO_NETMON_INFO_MNC, mnc,
			OFONO_NETMON_INFO_CI, lte->ci,
			OFONO_NETMON_INFO_RSSI, lte->signalStrength,
			OFONO_NETMON_INFO_TIMING_ADVANCE, lte->timingAdvance,
			OFONO_NETMON_INFO_INVALID);
}

static void ril_netmon_request_update(struct ofono_netmon *netmon,
		ofono_netmon_cb_t cb, void *data)
{
	struct ril_netmon *nm = ril_netmon_get_data(netmon);
	struct ofono_error error;
	GSList *l;

	for (l = nm->cell_info->cells; l; l = l->next) {
		const struct ril_cell *cell = l->data;

		if (cell->registered) {
			switch (cell->type) {
			case RIL_CELL_INFO_TYPE_GSM:
				ril_netmon_notify_gsm(netmon,
							&cell->info.gsm);
				break;
			case RIL_CELL_INFO_TYPE_WCDMA:
				ril_netmon_notify_wcdma(netmon,
							&cell->info.wcdma);
				break;
			case RIL_CELL_INFO_TYPE_LTE:
				ril_netmon_notify_lte(netmon,
							&cell->info.lte);
				break;
			default:
				break;
			}
		}
	}

	cb(ril_error_ok(&error), data);
}

static gboolean ril_netmon_register(gpointer user_data)
{
	struct ril_netmon *nm = user_data;

	GASSERT(nm->register_id);
	nm->register_id = 0;
	ofono_netmon_register(nm->netmon);

	return G_SOURCE_REMOVE;
}

static int ril_netmon_probe(struct ofono_netmon *netmon, unsigned int vendor,
				void *data)
{
	struct ril_modem *modem = data;
	int ret;

	if (modem->cell_info) {
		struct ril_netmon *nm = g_slice_new0(struct ril_netmon);

		nm->cell_info = ril_cell_info_ref(modem->cell_info);
		nm->netmon = netmon;

		ofono_netmon_set_data(netmon, nm);
		nm->register_id = g_idle_add(ril_netmon_register, nm);
		ret = 0;
	} else {
		DBG("%s no", modem->log_prefix ? modem->log_prefix : "");
		ret = -1;
	}

	DBG("%s %d", modem->log_prefix ? modem->log_prefix : "", ret);
	return ret;
}

static void ril_netmon_remove(struct ofono_netmon *netmon)
{
	struct ril_netmon *nm = ril_netmon_get_data(netmon);

	DBG("");
	ofono_netmon_set_data(netmon, NULL);

	if (nm->register_id > 0) {
		g_source_remove(nm->register_id);
	}

	ril_cell_info_unref(nm->cell_info);
	g_slice_free(struct ril_netmon, nm);
}

const struct ofono_netmon_driver ril_netmon_driver = {
	.name                   = RILMODEM_DRIVER,
	.probe			= ril_netmon_probe,
	.remove			= ril_netmon_remove,
	.request_update		= ril_netmon_request_update,
};

/*
 * Local Variables:
 * mode: C
 * c-basic-offset: 8
 * indent-tabs-mode: t
 * End:
 */
