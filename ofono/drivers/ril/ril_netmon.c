/*
 *  oFono - Open Source Telephony - RIL-based devices
 *
 *  Copyright (C) 2016-2018 Jolla Ltd.
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

#include <sailfish_cell_info.h>

#include "ofono.h"

struct ril_netmon {
	struct ofono_netmon *netmon;
	struct sailfish_cell_info *cell_info;
	guint register_id;
};

/* This number must be in sync with ril_netmon_notify_ofono: */
#define RIL_NETMON_MAX_OFONO_PARAMS (8)

struct ril_netmon_ofono_param {
	enum ofono_netmon_info type;
	int value;
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
			const unsigned int mnclen = mnclength(mcc, mnc);
			const char *format[] = { "%d", "%02d", "%03d" };
			const char *fmt = (mnclen > 0 &&
				mnclen <= G_N_ELEMENTS(format)) ? 
				format[mnclen - 1] : format[0];
			snprintf(s_mnc, OFONO_MAX_MNC_LENGTH + 1, fmt, mnc);
		}
	}
}

static void ril_netmon_notify_ofono(struct ofono_netmon *netmon,
		enum ofono_netmon_cell_type type, int mcc, int mnc,
		struct ril_netmon_ofono_param *params, int nparams)
{
	char s_mcc[OFONO_MAX_MCC_LENGTH + 1];
	char s_mnc[OFONO_MAX_MNC_LENGTH + 1];
	int i;

	/* Better not to push uninitialized data to the stack ... */
	for (i = nparams; i < RIL_NETMON_MAX_OFONO_PARAMS; i++) {
		params[i].type = OFONO_NETMON_INFO_INVALID;
		params[i].value = SAILFISH_CELL_INVALID_VALUE;
	}

	ril_netmon_format_mccmnc(s_mcc, s_mnc, mcc, mnc);
	ofono_netmon_serving_cell_notify(netmon, type,
			OFONO_NETMON_INFO_MCC, s_mcc,
			OFONO_NETMON_INFO_MNC, s_mnc,
			params[0].type, params[0].value,
			params[1].type, params[1].value,
			params[2].type, params[2].value,
			params[3].type, params[3].value,
			params[4].type, params[4].value,
			params[5].type, params[5].value,
			params[6].type, params[6].value,
			params[7].type, params[7].value,
			OFONO_NETMON_INFO_INVALID);
}

static void ril_netmon_notify_gsm(struct ofono_netmon *netmon,
				const struct sailfish_cell_info_gsm *gsm)
{
	struct ril_netmon_ofono_param params[RIL_NETMON_MAX_OFONO_PARAMS];
	int n = 0;

	if (gsm->lac != SAILFISH_CELL_INVALID_VALUE) {
		params[n].type = OFONO_NETMON_INFO_LAC;
		params[n].value = gsm->lac;
		n++;
	}

	if (gsm->cid != SAILFISH_CELL_INVALID_VALUE) {
		params[n].type = OFONO_NETMON_INFO_CI;
		params[n].value = gsm->cid;
		n++;
	}

	if (gsm->arfcn != SAILFISH_CELL_INVALID_VALUE) {
		params[n].type = OFONO_NETMON_INFO_ARFCN;
		params[n].value = gsm->arfcn;
		n++;
	}

	if (gsm->signalStrength != SAILFISH_CELL_INVALID_VALUE) {
		params[n].type = OFONO_NETMON_INFO_RSSI;
		params[n].value = gsm->signalStrength;
		n++;
	}

	if (gsm->bitErrorRate != SAILFISH_CELL_INVALID_VALUE) {
		params[n].type = OFONO_NETMON_INFO_BER;
		params[n].value = gsm->bitErrorRate;
		n++;
	}

	ril_netmon_notify_ofono(netmon, OFONO_NETMON_CELL_TYPE_GSM,
					gsm->mcc, gsm->mnc, params, n);
}

static void ril_netmon_notify_wcdma(struct ofono_netmon *netmon,
				const struct sailfish_cell_info_wcdma *wcdma)
{
	struct ril_netmon_ofono_param params[RIL_NETMON_MAX_OFONO_PARAMS];
	int n = 0;

	if (wcdma->lac != SAILFISH_CELL_INVALID_VALUE) {
		params[n].type = OFONO_NETMON_INFO_LAC;
		params[n].value = wcdma->lac;
		n++;
	}

	if (wcdma->cid != SAILFISH_CELL_INVALID_VALUE) {
		params[n].type = OFONO_NETMON_INFO_CI;
		params[n].value = wcdma->cid;
		n++;
	}

	if (wcdma->psc != SAILFISH_CELL_INVALID_VALUE) {
		params[n].type = OFONO_NETMON_INFO_PSC;
		params[n].value = wcdma->psc;
		n++;
	}

	if (wcdma->uarfcn != SAILFISH_CELL_INVALID_VALUE) {
		params[n].type = OFONO_NETMON_INFO_ARFCN;
		params[n].value = wcdma->uarfcn;
		n++;
	}

	if (wcdma->signalStrength != SAILFISH_CELL_INVALID_VALUE) {
		params[n].type = OFONO_NETMON_INFO_RSSI;
		params[n].value = wcdma->signalStrength;
		n++;
	}

	if (wcdma->bitErrorRate != SAILFISH_CELL_INVALID_VALUE) {
		params[n].type = OFONO_NETMON_INFO_BER;
		params[n].value = wcdma->bitErrorRate;
		n++;
	}

	ril_netmon_notify_ofono(netmon, OFONO_NETMON_CELL_TYPE_UMTS,
					wcdma->mcc, wcdma->mnc, params, n);
}

static void ril_netmon_notify_lte(struct ofono_netmon *netmon,
				const struct sailfish_cell_info_lte *lte)
{
	struct ril_netmon_ofono_param params[RIL_NETMON_MAX_OFONO_PARAMS];
	int n = 0;

	if (lte->ci != SAILFISH_CELL_INVALID_VALUE) {
		params[n].type = OFONO_NETMON_INFO_CI;
		params[n].value = lte->ci;
		n++;
	}

	if (lte->earfcn != SAILFISH_CELL_INVALID_VALUE) {
		params[n].type = OFONO_NETMON_INFO_EARFCN;
		params[n].value = lte->earfcn;
		n++;
	}

	if (lte->signalStrength != SAILFISH_CELL_INVALID_VALUE) {
		params[n].type = OFONO_NETMON_INFO_RSSI;
		params[n].value = lte->signalStrength;
		n++;
	}

	if (lte->rsrp != SAILFISH_CELL_INVALID_VALUE) {
		params[n].type = OFONO_NETMON_INFO_RSRQ;
		params[n].value = lte->rsrp;
		n++;
	}

	if (lte->rsrq != SAILFISH_CELL_INVALID_VALUE) {
		params[n].type = OFONO_NETMON_INFO_RSRP;
		params[n].value = lte->rsrq;
		n++;
	}

	if (lte->cqi != SAILFISH_CELL_INVALID_VALUE) {
		params[n].type = OFONO_NETMON_INFO_CQI;
		params[n].value = lte->cqi;
		n++;
	}

	if (lte->timingAdvance != SAILFISH_CELL_INVALID_VALUE) {
		params[n].type = OFONO_NETMON_INFO_TIMING_ADVANCE;
		params[n].value = lte->timingAdvance;
		n++;
	}

	ril_netmon_notify_ofono(netmon, OFONO_NETMON_CELL_TYPE_LTE,
					lte->mcc, lte->mnc, params, n);
}

static void ril_netmon_request_update(struct ofono_netmon *netmon,
		ofono_netmon_cb_t cb, void *data)
{
	struct ril_netmon *nm = ril_netmon_get_data(netmon);
	struct ofono_error error;
	GSList *l;

	for (l = nm->cell_info->cells; l; l = l->next) {
		const struct sailfish_cell *cell = l->data;

		if (cell->registered) {
			switch (cell->type) {
			case SAILFISH_CELL_TYPE_GSM:
				ril_netmon_notify_gsm(netmon,
							&cell->info.gsm);
				break;
			case SAILFISH_CELL_TYPE_WCDMA:
				ril_netmon_notify_wcdma(netmon,
							&cell->info.wcdma);
				break;
			case SAILFISH_CELL_TYPE_LTE:
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

		nm->cell_info = sailfish_cell_info_ref(modem->cell_info);
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

	sailfish_cell_info_unref(nm->cell_info);
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
