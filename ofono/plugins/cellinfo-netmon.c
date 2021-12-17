/*
 *  oFono - Open Source Telephony
 *
 *  Copyright (C) 2021 Jolla Ltd.
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License version 2 as
 *  published by the Free Software Foundation.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 *  GNU General Public License for more details.
 */

#include "ofono.h"
#include "cell-info-control.h"

#include <ofono/cell-info.h>
#include <ofono/log.h>
#include <ofono/modem.h>
#include <ofono/netmon.h>
#include <ofono/plugin.h>
#include <ofono/sim-mnclength.h>

#include <glib.h>

#include <stdio.h>

struct cellinfo_netmon_data {
	struct ofono_netmon *netmon;
	CellInfoControl *ctl;
	guint register_id;
	guint update_id;
};

struct cellinfo_netmon_update_cbd {
	struct cellinfo_netmon_data *nm;
	struct ofono_cell_info *info;
	unsigned long event_id;
	ofono_netmon_cb_t cb;
	void *data;
};

#define CALLBACK_WITH_SUCCESS(f, args...)		\
	do {						\
		struct ofono_error e;			\
		e.type = OFONO_ERROR_TYPE_NO_ERROR;	\
		e.error = 0;				\
		f(&e, ##args);				\
	} while (0)

#define NETMON_UPDATE_INTERVAL_MS 500
#define NETMON_UPDATE_SHORT_TIMEOUT_MS 10000
#define NETMON_UPDATE_LONG_TIMEOUT_MS 10000

/* This number must be in sync with cellinfo_netmon_notify: */
#define NETMON_MAX_OFONO_PARAMS (8)

struct cellinfo_netmon_notify_param {
	enum ofono_netmon_info type;
	int value;
};

/* -Wformat-truncation was introduced in GCC 7 */
#if __GNUC__ >= 7
#  define BEGIN_IGNORE_FORMAT_TRUNCATION \
	_Pragma("GCC diagnostic push") \
	_Pragma("GCC diagnostic ignored \"-Wformat-truncation\"")
#  define END_IGNORE_FORMAT_TRUNCATION \
	_Pragma ("GCC diagnostic pop")
#else
#  define BEGIN_IGNORE_FORMAT_TRUNCATION
#  define END_IGNORE_FORMAT_TRUNCATION
#endif

static inline struct cellinfo_netmon_data *
cellinfo_netmon_get_data(struct ofono_netmon *ofono)
{
	return ofono ? ofono_netmon_get_data(ofono) : NULL;
}

static void cellinfo_netmon_format_mccmnc(char *s_mcc, char *s_mnc,
		int mcc, int mnc)
{
	s_mcc[0] = 0;
	s_mnc[0] = 0;

	if (mcc >= 0 && mcc <= 999) {
		BEGIN_IGNORE_FORMAT_TRUNCATION
		snprintf(s_mcc, OFONO_MAX_MCC_LENGTH + 1, "%03d", mcc);
		END_IGNORE_FORMAT_TRUNCATION
		if (mnc >= 0 && mnc <= 999) {
			const int mnclen =
				ofono_sim_mnclength_get_mnclength_mccmnc(mcc,
									mnc);

			if (mnclen >= 0) {
				BEGIN_IGNORE_FORMAT_TRUNCATION
				snprintf(s_mnc, OFONO_MAX_MNC_LENGTH, "%0*d",
								mnclen, mnc);
				END_IGNORE_FORMAT_TRUNCATION
				s_mnc[OFONO_MAX_MNC_LENGTH] = 0;
			}
		}
	}
}

static void cellinfo_netmon_notify(struct ofono_netmon *netmon,
		enum ofono_netmon_cell_type type, int mcc, int mnc,
		struct cellinfo_netmon_notify_param *params, int nparams)
{
	char s_mcc[OFONO_MAX_MCC_LENGTH + 1];
	char s_mnc[OFONO_MAX_MNC_LENGTH + 1];
	int i;

	/* Better not to push uninitialized data to the stack ... */
	for (i = nparams; i < NETMON_MAX_OFONO_PARAMS; i++) {
		params[i].type = OFONO_NETMON_INFO_INVALID;
		params[i].value = OFONO_CELL_INVALID_VALUE;
	}

	cellinfo_netmon_format_mccmnc(s_mcc, s_mnc, mcc, mnc);
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

static void cellinfo_netmon_notify_gsm(struct ofono_netmon *netmon,
		const struct ofono_cell_info_gsm *gsm)
{
	struct cellinfo_netmon_notify_param params[NETMON_MAX_OFONO_PARAMS];
	int n = 0;

	if (gsm->lac != OFONO_CELL_INVALID_VALUE) {
		params[n].type = OFONO_NETMON_INFO_LAC;
		params[n].value = gsm->lac;
		n++;
	}

	if (gsm->cid != OFONO_CELL_INVALID_VALUE) {
		params[n].type = OFONO_NETMON_INFO_CI;
		params[n].value = gsm->cid;
		n++;
	}

	if (gsm->arfcn != OFONO_CELL_INVALID_VALUE) {
		params[n].type = OFONO_NETMON_INFO_ARFCN;
		params[n].value = gsm->arfcn;
		n++;
	}

	if (gsm->signalStrength != OFONO_CELL_INVALID_VALUE) {
		params[n].type = OFONO_NETMON_INFO_RSSI;
		params[n].value = gsm->signalStrength;
		n++;
	}

	if (gsm->bitErrorRate != OFONO_CELL_INVALID_VALUE) {
		params[n].type = OFONO_NETMON_INFO_BER;
		params[n].value = gsm->bitErrorRate;
		n++;
	}

	cellinfo_netmon_notify(netmon, OFONO_NETMON_CELL_TYPE_GSM,
		gsm->mcc, gsm->mnc, params, n);
}

static void cellinfo_netmon_notify_wcdma(struct ofono_netmon *netmon,
		const struct ofono_cell_info_wcdma *wcdma)
{
	struct cellinfo_netmon_notify_param params[NETMON_MAX_OFONO_PARAMS];
	int n = 0;

	if (wcdma->lac != OFONO_CELL_INVALID_VALUE) {
		params[n].type = OFONO_NETMON_INFO_LAC;
		params[n].value = wcdma->lac;
		n++;
	}

	if (wcdma->cid != OFONO_CELL_INVALID_VALUE) {
		params[n].type = OFONO_NETMON_INFO_CI;
		params[n].value = wcdma->cid;
		n++;
	}

	if (wcdma->psc != OFONO_CELL_INVALID_VALUE) {
		params[n].type = OFONO_NETMON_INFO_PSC;
		params[n].value = wcdma->psc;
		n++;
	}

	if (wcdma->uarfcn != OFONO_CELL_INVALID_VALUE) {
		params[n].type = OFONO_NETMON_INFO_ARFCN;
		params[n].value = wcdma->uarfcn;
		n++;
	}

	if (wcdma->signalStrength != OFONO_CELL_INVALID_VALUE) {
		params[n].type = OFONO_NETMON_INFO_RSSI;
		params[n].value = wcdma->signalStrength;
		n++;
	}

	if (wcdma->bitErrorRate != OFONO_CELL_INVALID_VALUE) {
		params[n].type = OFONO_NETMON_INFO_BER;
		params[n].value = wcdma->bitErrorRate;
		n++;
	}

	cellinfo_netmon_notify(netmon, OFONO_NETMON_CELL_TYPE_UMTS,
		wcdma->mcc, wcdma->mnc, params, n);
}

static void cellinfo_netmon_notify_lte(struct ofono_netmon *netmon,
		const struct ofono_cell_info_lte *lte)
{
	struct cellinfo_netmon_notify_param params[NETMON_MAX_OFONO_PARAMS];
	int n = 0;

	if (lte->ci != OFONO_CELL_INVALID_VALUE) {
		params[n].type = OFONO_NETMON_INFO_CI;
		params[n].value = lte->ci;
		n++;
	}

	if (lte->earfcn != OFONO_CELL_INVALID_VALUE) {
		params[n].type = OFONO_NETMON_INFO_EARFCN;
		params[n].value = lte->earfcn;
		n++;
	}

	if (lte->signalStrength != OFONO_CELL_INVALID_VALUE) {
		params[n].type = OFONO_NETMON_INFO_RSSI;
		params[n].value = lte->signalStrength;
		n++;
	}

	if (lte->rsrp != OFONO_CELL_INVALID_VALUE) {
		params[n].type = OFONO_NETMON_INFO_RSRQ;
		params[n].value = lte->rsrp;
		n++;
	}

	if (lte->rsrq != OFONO_CELL_INVALID_VALUE) {
		params[n].type = OFONO_NETMON_INFO_RSRP;
		params[n].value = lte->rsrq;
		n++;
	}

	if (lte->cqi != OFONO_CELL_INVALID_VALUE) {
		params[n].type = OFONO_NETMON_INFO_CQI;
		params[n].value = lte->cqi;
		n++;
	}

	if (lte->timingAdvance != OFONO_CELL_INVALID_VALUE) {
		params[n].type = OFONO_NETMON_INFO_TIMING_ADVANCE;
		params[n].value = lte->timingAdvance;
		n++;
	}

	cellinfo_netmon_notify(netmon, OFONO_NETMON_CELL_TYPE_LTE,
		lte->mcc, lte->mnc, params, n);
}

static gboolean cellinfo_netmon_notify_cell(struct ofono_netmon *netmon,
		const struct ofono_cell *cell)
{
	if (cell->registered) {
		switch (cell->type) {
		case OFONO_CELL_TYPE_GSM:
			cellinfo_netmon_notify_gsm(netmon, &cell->info.gsm);
			return TRUE;
		case OFONO_CELL_TYPE_WCDMA:
			cellinfo_netmon_notify_wcdma(netmon, &cell->info.wcdma);
			return TRUE;
		case OFONO_CELL_TYPE_LTE:
			cellinfo_netmon_notify_lte(netmon, &cell->info.lte);
			return TRUE;
		default:
			break;
		}
	}
	return FALSE;
}

static guint cellinfo_netmon_notify_cells(struct ofono_netmon *netmon,
		struct ofono_cell_info *info)
{
	guint n = 0;

	if (info && info->cells) {
		const ofono_cell_ptr *ptr;

		for (ptr = info->cells; *ptr; ptr++) {
			if (cellinfo_netmon_notify_cell(netmon, *ptr)) {
				/*
				 * We could actually break here because
				 * there shouldn't be more than one cell
				 * in a registered state...
				 */
				n++;
			}
		}
	}

	return n;
}

static gboolean cellinfo_netmon_have_registered_cells
		(struct ofono_cell_info *info)
{
	if (info && info->cells) {
		const ofono_cell_ptr *ptr;

		for (ptr = info->cells; *ptr; ptr++) {
			if ((*ptr)->registered) {
				return TRUE;
			}
		}
	}

	return FALSE;
}

static void cellinfo_netmon_request_update_event(struct ofono_cell_info *info,
		void *user_data)
{
	struct cellinfo_netmon_update_cbd *cbd = user_data;
	struct cellinfo_netmon_data *nm = cbd->nm;

	if (cellinfo_netmon_notify_cells(nm->netmon, info)) {
		ofono_netmon_cb_t cb = cbd->cb;
		void *data = cbd->data;

		/* Removing the source destroys cellinfo_netmon_update_cbd */
		DBG("%s received update", nm->ctl->path);
		g_source_remove(nm->update_id);
		nm->update_id = 0;
		CALLBACK_WITH_SUCCESS(cb, data);
	}
}

static gboolean cellinfo_netmon_request_update_timeout(gpointer data)
{
	struct cellinfo_netmon_update_cbd *cbd = data;
	struct cellinfo_netmon_data *nm = cbd->nm;

	nm->update_id = 0;
	DBG("%s update timed out", nm->ctl->path);
	CALLBACK_WITH_SUCCESS(cbd->cb, cbd->data);
	return G_SOURCE_REMOVE;
}

static void cellinfo_netmon_request_update_destroy(gpointer data)
{
	struct cellinfo_netmon_update_cbd *cbd = data;
	struct cellinfo_netmon_data *nm = cbd->nm;

	cell_info_control_drop_requests(nm->ctl, cbd);
	ofono_cell_info_remove_handler(cbd->info, cbd->event_id);
	ofono_cell_info_unref(cbd->info);
	g_free(cbd);
}

static void cellinfo_netmon_request_update(struct ofono_netmon *netmon,
		ofono_netmon_cb_t cb, void *data)
{
	struct cellinfo_netmon_data *nm = cellinfo_netmon_get_data(netmon);
	struct ofono_cell_info *info = nm->ctl->info;
	struct cellinfo_netmon_update_cbd *cbd =
		g_new(struct cellinfo_netmon_update_cbd, 1);

	cbd->cb = cb;
	cbd->data = data;
	cbd->nm = nm;
	cbd->info = ofono_cell_info_ref(info);
	cbd->event_id = ofono_cell_info_add_change_handler(info,
				cellinfo_netmon_request_update_event, cbd);

	/* Temporarily enable updates and wait */
	DBG("%s waiting for update", nm->ctl->path);
	cell_info_control_set_update_interval(nm->ctl, cbd,
				NETMON_UPDATE_INTERVAL_MS);
	cell_info_control_set_enabled(nm->ctl, cbd, TRUE);

	/* Use shorter timeout if we already have something */
	nm->update_id = g_timeout_add_full(G_PRIORITY_DEFAULT_IDLE,
				cellinfo_netmon_have_registered_cells(info) ?
				NETMON_UPDATE_SHORT_TIMEOUT_MS :
				NETMON_UPDATE_LONG_TIMEOUT_MS,
				cellinfo_netmon_request_update_timeout,
				cbd, cellinfo_netmon_request_update_destroy);
}

static void cellinfo_netmon_enable_periodic_update(struct ofono_netmon *netmon,
		unsigned int enable, unsigned int period_sec,
		ofono_netmon_cb_t cb, void *data)
{
	struct cellinfo_netmon_data *nm = cellinfo_netmon_get_data(netmon);
	CellInfoControl *ctl = nm->ctl;

	if (ctl) {
		const int ms = period_sec * 1000;

		if (enable) {
			cell_info_control_set_update_interval(ctl, nm, ms);
			cell_info_control_set_enabled(ctl, nm, TRUE);
		} else {
			cell_info_control_set_enabled(ctl, nm, FALSE);
			cell_info_control_set_update_interval(ctl, nm, ms);
		}
	}

	CALLBACK_WITH_SUCCESS(cb, data);
}

static gboolean cellinfo_netmon_register(gpointer user_data)
{
	struct cellinfo_netmon_data *nm = user_data;

	nm->register_id = 0;
	ofono_netmon_register(nm->netmon);

	return G_SOURCE_REMOVE;
}

static int cellinfo_netmon_probe(struct ofono_netmon *netmon,
		unsigned int vendor, void *modem)
{
	const char *path = ofono_modem_get_path(modem);
	struct cellinfo_netmon_data *nm =
		g_new0(struct cellinfo_netmon_data, 1);

	nm->netmon = netmon;
	nm->ctl = cell_info_control_get(path);

	ofono_netmon_set_data(netmon, nm);
	nm->register_id = g_idle_add(cellinfo_netmon_register, nm);
	DBG("%s", path);

	return 0;
}

static void cellinfo_netmon_remove(struct ofono_netmon *netmon)
{
	struct cellinfo_netmon_data *nm = cellinfo_netmon_get_data(netmon);

	DBG("%s", nm->ctl ? nm->ctl->path : "?");
	ofono_netmon_set_data(netmon, NULL);

	if (nm->update_id) {
		g_source_remove(nm->update_id);
	}

	if (nm->register_id) {
		g_source_remove(nm->register_id);
	}

	cell_info_control_drop_requests(nm->ctl, nm);
	cell_info_control_unref(nm->ctl);
	g_free(nm);
}

const struct ofono_netmon_driver cellinfo_netmon_driver = {
	.name                   = "cellinfo",
	.probe			= cellinfo_netmon_probe,
	.remove			= cellinfo_netmon_remove,
	.request_update		= cellinfo_netmon_request_update,
	.enable_periodic_update = cellinfo_netmon_enable_periodic_update
};

static int cellinfo_netmon_init(void)
{
	return ofono_netmon_driver_register(&cellinfo_netmon_driver);
}

static void cellinfo_netmon_exit(void)
{
	ofono_netmon_driver_unregister(&cellinfo_netmon_driver);
}

OFONO_PLUGIN_DEFINE(cellinfo_netmon, "CellInfo NetMon Plugin",
	OFONO_VERSION, OFONO_PLUGIN_PRIORITY_DEFAULT,
	cellinfo_netmon_init, cellinfo_netmon_exit)

/*
 * Local Variables:
 * mode: C
 * c-basic-offset: 8
 * indent-tabs-mode: t
 * End:
 */
