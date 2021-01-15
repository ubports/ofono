/*
 *
 *  oFono - Open Source Telephony
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License version 2 as
 *  published by the Free Software Foundation.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
 *
 */

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <errno.h>

#include <glib.h>

#include <ofono/log.h>
#include <ofono/modem.h>
#include <ofono/netreg.h>
#include <ofono/netmon.h>

#include "gatchat.h"
#include "gatresult.h"

#include "common.h"
#include "gemaltomodem.h"
#include "drivers/atmodem/vendor.h"

static const char *smoni_prefix[] = { "^SMONI:", NULL };
static const char *csq_prefix[] = { "+CSQ:", NULL };

struct netmon_driver_data {
	GAtChat *chat;
};

struct req_cb_data {
	gint ref_count; /* Ref count */

	struct ofono_netmon *netmon;
	ofono_netmon_cb_t cb;
	void *data;

	struct ofono_network_operator op;

	int rssi;	/* CSQ: received signal strength indicator (RSSI) */

	union {
		struct {
			int arfcn;	/* SMONI: Absolute Radio Frequency Channel Number */
			int bcch;	/* SMONI: Receiving level of the BCCH carrier in dBm */
			int lac;	/* SMONI: Location Area Code */
			int ci;		/* SMONI: Cell ID */
		} gsm;
		struct {
			int uarfcn;	/* SMONI: UTRAN Absolute Radio Frequency Channel Number */
			int psc;	/* SMONI: Primary Scrambling Code */
			int ecno;	/* SMONI: Carrier to noise ratio in dB */
			int rscp;	/* SMONI: Received Signal Code Power in dBm */
			int lac;	/* SMONI: Location Area Code */
			int ci;		/* SMONI: Cell ID */
		} umts;
		struct {
			int euarfcn;	/* SMONI: E-UTRA Absolute Radio Frequency Channel Number */
			int rsrp;	/* SMONI: Reference Signal Received Power */
			int rsrq;	/* SMONI: Reference Signal Received Quality */
		} lte;
	} t;
};

static inline struct req_cb_data *req_cb_data_new0(void *cb, void *data,
							void *user)
{
	struct req_cb_data *ret = g_new0(struct req_cb_data, 1);

	ret->ref_count = 1;
	ret->netmon = user;
	ret->data = data;
	ret->cb = cb;

	return ret;
}

static inline struct req_cb_data *req_cb_data_ref(struct req_cb_data *cbd)
{
	if (cbd == NULL)
		return NULL;

	g_atomic_int_inc(&cbd->ref_count);

	return cbd;
}

static void req_cb_data_unref(gpointer user_data)
{
	struct req_cb_data *cbd = user_data;
	gboolean is_zero;

	if (cbd == NULL)
		return;

	is_zero = g_atomic_int_dec_and_test(&cbd->ref_count);

	if (is_zero == TRUE)
		g_free(cbd);
}

static gboolean gemalto_delayed_register(gpointer user_data)
{
	struct ofono_netmon *netmon = user_data;

	ofono_netmon_register(netmon);

	return FALSE;
}

static int gemalto_ecno_scale(int value)
{
	if (value < -24)
		return 0;

	if (value > 0)
		return 49;

	return 49 * (value + 24) / 24;
}

static int gemalto_rscp_scale(int value)
{
	if (value < -120)
		return 0;

	if (value > -24)
		return 96;

	return value + 120;
}

static int gemalto_rsrp_scale(int value)
{
	if (value < -140)
		return 0;

	if (value > -43)
		return 97;

	return value + 140;
}

static int gemalto_rsrq_scale(int value)
{
	if (2 * value < -39)
		return 0;

	if (2 * value > -5)
		return 34;

	return 2 * value + 39;
}

static int gemalto_parse_smoni_gsm(GAtResultIter *iter,
					struct req_cb_data *cbd)
{
	/*
	 * ME is camping on a GSM (2G) cell:
	 * ^SMONI: ACT,ARFCN,BCCH,MCC,MNC,LAC,cell,C1,C2,NCC,BCC,GPRS,Conn_state
	 * ^SMONI: 2G,71,-61,262,02,0143,83BA,33,33,3,6,G,NOCONN
	 *
	 * ME is searching and could not (yet) find a suitable GSM (2G) cell:
	 * ^SMONI: ACT,ARFCN,BCCH,MCC,MNC,LAC,cell,C1,C2,NCC,BCC,GPRS,ARFCN,TS,timAdv,dBm,Q,ChMod
	 * ^SMONI: 2G,SEARCH,SEARCH
	 *
	 * ME is camping on a GSM cell but not registered to the network (only emergency call allowed):
	 * ^SMONI: ACT,ARFCN,BCCH,MCC,MNC,LAC,cell,C1,C2,NCC,BCC,GPRS,PWR,RXLev,ARFCN,TS,timAdv,dBm,Q,ChMod
	 * ^SMONI: 2G,673,-89,262,07,4EED,A500,16,16,7,4,G,5,-107,LIMSRV
	 *
	 * ME has a dedicated channel (for example call in progress):
	 * ^SMONI: ACT,ARFCN,BCCH,MCC,MNC,LAC,cell,C1,C2,NCC,BCC,GPRS,ARFCN,TS,timAdv,dBm,Q,ChMod
	 * ^SMONI: 2G,673,-80,262,07,4EED,A500,35,35,7,4,G,643,4,0,-80,0,S_FR
	 */

	enum smoni_gsm_field {
		SMONI_GSM_ARFCN,
		SMONI_GSM_BCCH,
		SMONI_GSM_MCC,
		SMONI_GSM_MNC,
		SMONI_GSM_LAC,
		SMONI_GSM_CI,
		SMONI_GSM_MAX,
	};

	const char *str;
	int number;
	int idx;

	cbd->t.gsm.arfcn = -1;
	cbd->t.gsm.bcch = -1;
	cbd->t.gsm.lac = -1;
	cbd->t.gsm.ci = -1;

	for (idx = 0; idx < SMONI_GSM_MAX; idx++) {
		switch (idx) {
		case SMONI_GSM_ARFCN:
			if (g_at_result_iter_next_number(iter, &number))
				cbd->t.gsm.arfcn = number;
			break;
		case SMONI_GSM_BCCH:
			if (g_at_result_iter_next_unquoted_string(iter, &str)) {
				if (sscanf(str, "%d", &number) == 1)
					cbd->t.gsm.bcch = number;
			}
			break;
		case SMONI_GSM_MCC:
			if (g_at_result_iter_next_number(iter, &number))
				snprintf(cbd->op.mcc, 4, "%d", number);
			break;
		case SMONI_GSM_MNC:
			if (g_at_result_iter_next_number(iter, &number))
				snprintf(cbd->op.mnc, 4, "%d", number);
			break;
		case SMONI_GSM_LAC:
			if (g_at_result_iter_next_unquoted_string(iter, &str)) {
				if (sscanf(str, "%x", &number) == 1)
					cbd->t.gsm.lac = number;
			}
			break;
		case SMONI_GSM_CI:
			if (g_at_result_iter_next_unquoted_string(iter, &str)) {
				if (sscanf(str, "%x", &number) == 1)
					cbd->t.gsm.ci = number;
			}
			break;
		default:
			break;
		}
	}

	DBG(" %-15s %s", "GSM.MCC", cbd->op.mcc);
	DBG(" %-15s %s", "GSM.MNC", cbd->op.mnc);
	DBG(" %-15s %d", "GSM.ARFCN", cbd->t.gsm.arfcn);
	DBG(" %-15s %d", "GSM.BCCH", cbd->t.gsm.bcch);
	DBG(" %-15s %d", "GSM.LAC", cbd->t.gsm.lac);
	DBG(" %-15s %d", "GSM.CELL", cbd->t.gsm.ci);

	return 0;
}

static int gemalto_parse_smoni_umts(GAtResultIter *iter,
					struct req_cb_data *cbd)
{
	/*
	 * ME is camping on a UMTS (3G) cell:
	 * ^SMONI: ACT,UARFCN,PSC,EC/n0,RSCP,MCC,MNC,LAC,cell,SQual,SRxLev,,Conn_state
	 * ^SMONI: 3G,10564,296,-7.5,-79,262,02,0143,00228FF,-92,-78,NOCONN
	 *
	 * ME is searching and could not (yet) find a suitable UMTS (3G) cell:
	 * ^SMONI: ACT,UARFCN,PSC,EC/n0,RSCP,MCC,MNC,LAC,cell,SQual,SRxLev,PhysCh, SF,Slot,EC/n0,RSCP,ComMod,HSUPA,HSDPA
	 * ^SMONI: 3G,SEARCH,SEARCH
	 *
	 * ME is camping on a UMTS cell but not registered to the network (only emergency call allowed):
	 * ^SMONI: ACT,UARFCN,PSC,EC/n0,RSCP,MCC,MNC,LAC,cell,SQual,SRxLev,PhysCh, SF,Slot,EC/n0,RSCP,ComMod,HSUPA,HSDPA
	 * ^SMONI: 3G,10564,96,-7.5,-79,262,02,0143,00228FF,-92,-78,LIMSRV
	 *
	 * ME has a dedicated channel (for example call in progress):
	 * ^SMONI: ACT,UARFCN,PSC,EC/n0,RSCP,MCC,MNC,LAC,cell,SQual,SRxLev,PhysCh, SF,Slot,EC/n0,RSCP,ComMod,HSUPA,HSDPA
	 * ^SMONI: 3G,10737,131,-5,-93,260,01,7D3D,C80BC9A,--,--,----,---,-,-5,-93,0,01,06
	 */

	enum smoni_umts_field {
		SMONI_UMTS_UARFCN,
		SMONI_UMTS_PSC,
		SMONI_UMTS_ECN0,
		SMONI_UMTS_RSCP,
		SMONI_UMTS_MCC,
		SMONI_UMTS_MNC,
		SMONI_UMTS_LAC,
		SMONI_UMTS_CI,
		SMONI_UMTS_MAX,
	};

	const char *str;
	float fnumber;
	int number;
	int idx;

	cbd->t.umts.uarfcn = -1;
	cbd->t.umts.psc = -1;
	cbd->t.umts.ecno = -1;
	cbd->t.umts.rscp = -1;
	cbd->t.umts.lac = -1;
	cbd->t.umts.ci = -1;

	for (idx = 0; idx < SMONI_UMTS_MAX; idx++) {
		switch (idx) {
		case SMONI_UMTS_UARFCN:
			if (g_at_result_iter_next_number(iter, &number))
				cbd->t.umts.uarfcn = number;
			break;
		case SMONI_UMTS_PSC:
			if (g_at_result_iter_next_number(iter, &number))
				cbd->t.umts.psc = number;
			break;
		case SMONI_UMTS_ECN0:
			if (g_at_result_iter_next_unquoted_string(iter, &str)) {
				if (sscanf(str, "%f", &fnumber) == 1)
					cbd->t.umts.ecno =
						gemalto_ecno_scale((int)fnumber);
			}
			break;
		case SMONI_UMTS_RSCP:
			if (g_at_result_iter_next_unquoted_string(iter, &str)) {
				if (sscanf(str, "%d", &number) == 1)
					cbd->t.umts.rscp =
						gemalto_rscp_scale(number);
			}
			break;
		case SMONI_UMTS_MCC:
			if (g_at_result_iter_next_number(iter, &number))
				snprintf(cbd->op.mcc, 4, "%d", number);
			break;
		case SMONI_UMTS_MNC:
			if (g_at_result_iter_next_number(iter, &number))
				snprintf(cbd->op.mnc, 4, "%d", number);
			break;
		case SMONI_UMTS_LAC:
			if (g_at_result_iter_next_unquoted_string(iter, &str)) {
				if (sscanf(str, "%x", &number) == 1)
					cbd->t.umts.lac = number;
			}
			break;
		case SMONI_UMTS_CI:
			if (g_at_result_iter_next_unquoted_string(iter, &str)) {
				if (sscanf(str, "%x", &number) == 1)
					cbd->t.umts.ci = number;
			}
			break;
		default:
			break;
		}
	}

	DBG(" %-15s %s", "UMTS.MCC", cbd->op.mcc);
	DBG(" %-15s %s", "UMTS.MNC", cbd->op.mnc);
	DBG(" %-15s %d", "UMTS.UARFCN", cbd->t.umts.uarfcn);
	DBG(" %-15s %d", "UMTS.PSC", cbd->t.umts.psc);
	DBG(" %-15s %d", "UMTS.ECN0", cbd->t.umts.ecno);
	DBG(" %-15s %d", "UMTS.RSCP", cbd->t.umts.rscp);
	DBG(" %-15s %d", "UMTS.LAC", cbd->t.umts.lac);
	DBG(" %-15s %d", "UMTS.CELL", cbd->t.umts.ci);

	return 0;
}

static int gemalto_parse_smoni_lte(GAtResultIter *iter,
					struct req_cb_data *cbd)
{
	/*
	 * ME is camping on a LTE (4G) cell:
	 * ^SMONI: ACT,EARFCN,Band,DL bandwidth,UL bandwidth,Mode,MCC,MNC,TAC,Global Cell ID,Phys-ical Cell ID,Srxlev,RSRP,RSRQ,Conn_state
	 * ^SMONI: 4G,6300,20,10,10,FDD,262,02,BF75,0345103,350,33,-94,-7,NOCONN
	 *
	 * ME is searching and could not (yet) find a suitable LTE (4G) cell:
	 * ^SMONI: ACT,EARFCN,Band,DL bandwidth,UL bandwidth,Mode,MCC,MNC,TAC,Global Cell ID,Phys-ical Cell ID,Srxlev,RSRP,RSRQ,Conn_state
	 * ^SMONI: 4G,SEARCH
	 *
	 * ME is camping on a LTE (4G) cell but not registered to the network (only emergency call allowed):
	 * ^SMONI: ACT,EARFCN,Band,DL bandwidth,UL bandwidth,Mode,MCC,MNC,TAC,Global Cell ID,Phys-ical Cell ID,Srxlev,RSRP,RSRQ,Conn_state
	 * ^SMONI: 4G,6300,20,10,10,FDD,262,02,BF75,0345103,350,33,-94,-7,LIMSRV
	 *
	 * ME has a dedicated channel (for example call in progress):
	 * ^SMONI: ACT,EARFCN,Band,DL bandwidth,UL bandwidth,Mode,MCC,MNC,TAC,Global Cell ID,Phys-ical Cell ID,TX_power,RSRP,RSRQ,Conn_state
	 * ^SMONI: 4G,6300,20,10,10,FDD,262,02,BF75,0345103,350,90,-94,-7,CONN
	 */

	const char *str;
	int number;

	cbd->t.lte.euarfcn = -1;
	cbd->t.lte.rsrp = -1;
	cbd->t.lte.rsrq = -1;

	if (g_at_result_iter_next_number(iter, &number))
		cbd->t.lte.euarfcn = number;

	g_at_result_iter_skip_next(iter);
	g_at_result_iter_skip_next(iter);
	g_at_result_iter_skip_next(iter);
	g_at_result_iter_skip_next(iter);

	if (g_at_result_iter_next_number(iter, &number))
		snprintf(cbd->op.mcc, 4, "%d", number);

	if (g_at_result_iter_next_number(iter, &number))
		snprintf(cbd->op.mnc, 4, "%d", number);

	g_at_result_iter_skip_next(iter);
	g_at_result_iter_skip_next(iter);
	g_at_result_iter_skip_next(iter);
	g_at_result_iter_skip_next(iter);

	if (g_at_result_iter_next_unquoted_string(iter, &str)) {
		if (sscanf(str, "%d", &number) == 1)
			cbd->t.lte.rsrp = gemalto_rsrp_scale(number);
	}

	if (g_at_result_iter_next_unquoted_string(iter, &str)) {
		if (sscanf(str, "%d", &number) == 1)
			cbd->t.lte.rsrq = gemalto_rsrq_scale(number);
	}

	DBG(" %-15s %s", "LTE.MCC", cbd->op.mcc);
	DBG(" %-15s %s", "LTE.MNC", cbd->op.mnc);
	DBG(" %-15s %d", "LTE.EUARFCN", cbd->t.lte.euarfcn);
	DBG(" %-15s %d", "LTE.RSRP", cbd->t.lte.rsrp);
	DBG(" %-15s %d", "LTE.RSRQ", cbd->t.lte.rsrq);

	return 0;
}

static void gemalto_netmon_finish_success(struct req_cb_data *cbd)
{
	struct ofono_netmon *nm = cbd->netmon;

	switch (cbd->op.tech) {
	case OFONO_NETMON_CELL_TYPE_LTE:
		ofono_netmon_serving_cell_notify(nm, cbd->op.tech,
					OFONO_NETMON_INFO_MCC, cbd->op.mcc,
					OFONO_NETMON_INFO_MNC, cbd->op.mnc,
					OFONO_NETMON_INFO_RSSI, cbd->rssi,
					OFONO_NETMON_INFO_EARFCN, cbd->t.lte.euarfcn,
					OFONO_NETMON_INFO_RSRP, cbd->t.lte.rsrp,
					OFONO_NETMON_INFO_RSRQ, cbd->t.lte.rsrq,
					OFONO_NETMON_INFO_INVALID);
		break;
	case OFONO_NETMON_CELL_TYPE_UMTS:
		ofono_netmon_serving_cell_notify(nm, cbd->op.tech,
					OFONO_NETMON_INFO_MCC, cbd->op.mcc,
					OFONO_NETMON_INFO_MNC, cbd->op.mnc,
					OFONO_NETMON_INFO_RSSI, cbd->rssi,
					OFONO_NETMON_INFO_ARFCN, cbd->t.umts.uarfcn,
					OFONO_NETMON_INFO_PSC, cbd->t.umts.psc,
					OFONO_NETMON_INFO_ECN0, cbd->t.umts.ecno,
					OFONO_NETMON_INFO_RSCP, cbd->t.umts.rscp,
					OFONO_NETMON_INFO_LAC, cbd->t.umts.lac,
					OFONO_NETMON_INFO_CI, cbd->t.umts.ci,
					OFONO_NETMON_INFO_INVALID);
		break;
	case OFONO_NETMON_CELL_TYPE_GSM:
		ofono_netmon_serving_cell_notify(nm, cbd->op.tech,
					OFONO_NETMON_INFO_MCC, cbd->op.mcc,
					OFONO_NETMON_INFO_MNC, cbd->op.mnc,
					OFONO_NETMON_INFO_RSSI, cbd->rssi,
					OFONO_NETMON_INFO_ARFCN, cbd->t.gsm.arfcn,
					OFONO_NETMON_INFO_LAC, cbd->t.gsm.lac,
					OFONO_NETMON_INFO_CI, cbd->t.gsm.ci,
					OFONO_NETMON_INFO_INVALID);
		break;
	default:
		break;
	}

	CALLBACK_WITH_SUCCESS(cbd->cb, cbd->data);
}

static void csq_cb(gboolean ok, GAtResult *result, gpointer user_data)
{
	struct req_cb_data *cbd = user_data;
	struct ofono_error error;
	GAtResultIter iter;
	int rssi;

	DBG("ok %d", ok);

	decode_at_error(&error, g_at_result_final_response(result));

	if (!ok) {
		CALLBACK_WITH_FAILURE(cbd->cb, cbd->data);
		return;
	}

	g_at_result_iter_init(&iter, result);

	if (!g_at_result_iter_next(&iter, "+CSQ: ")) {
		cbd->rssi = -1;
		goto out;
	}

	if (!g_at_result_iter_next_number(&iter, &rssi) || rssi == 99)
		cbd->rssi = -1;
	else
		cbd->rssi = rssi;

	DBG(" RSSI %d ", cbd->rssi);

out:
	gemalto_netmon_finish_success(cbd);
}

static void smoni_cb(gboolean ok, GAtResult *result, gpointer user_data)
{
	struct req_cb_data *cbd = user_data;
	struct ofono_netmon *nm = cbd->netmon;
	struct netmon_driver_data *nmd = ofono_netmon_get_data(nm);
	struct ofono_error error;
	const char *technology;
	GAtResultIter iter;
	int ret;

	DBG("ok %d", ok);

	decode_at_error(&error, g_at_result_final_response(result));

	if (!ok) {
		CALLBACK_WITH_FAILURE(cbd->cb, cbd->data);
		return;
	}

	g_at_result_iter_init(&iter, result);

	/* do not fail */

	if (!g_at_result_iter_next(&iter, "^SMONI: ")) {
		CALLBACK_WITH_SUCCESS(cbd->cb, cbd->data);
		return;
	}

	if (!g_at_result_iter_next_unquoted_string(&iter, &technology)) {
		DBG("^SMONI: failed to parse technology");
		CALLBACK_WITH_SUCCESS(cbd->cb, cbd->data);
		return;
	}

	if (strcmp(technology, "2G") == 0) {
		cbd->op.tech = OFONO_NETMON_CELL_TYPE_GSM;
	} else if (strcmp(technology, "3G") == 0) {
		cbd->op.tech = OFONO_NETMON_CELL_TYPE_UMTS;
	} else if (strcmp(technology, "4G") == 0) {
		cbd->op.tech = OFONO_NETMON_CELL_TYPE_LTE;
	} else {
		/* fall-back to GSM by default */
		DBG("^SMONI: unexpected technology: %s", technology);
		cbd->op.tech = OFONO_NETMON_CELL_TYPE_GSM;
	}

	switch (cbd->op.tech) {
	case OFONO_NETMON_CELL_TYPE_LTE:
		ret = gemalto_parse_smoni_lte(&iter, cbd);
		break;
	case OFONO_NETMON_CELL_TYPE_UMTS:
		ret = gemalto_parse_smoni_umts(&iter, cbd);
		break;
	case OFONO_NETMON_CELL_TYPE_GSM:
		ret = gemalto_parse_smoni_gsm(&iter, cbd);
		break;
	default:
		break;
	}

	if (ret) {
		CALLBACK_WITH_SUCCESS(cbd->cb, cbd->data);
		return;
	}

	cbd = req_cb_data_ref(cbd);
	if (g_at_chat_send(nmd->chat, "AT+CSQ", csq_prefix,
				csq_cb, cbd, req_cb_data_unref))
		return;

	req_cb_data_unref(cbd);
	CALLBACK_WITH_FAILURE(cbd->cb, cbd->data);
}

static void gemalto_netmon_request_update(struct ofono_netmon *netmon,
						ofono_netmon_cb_t cb,
						void *data)
{
	struct netmon_driver_data *nmd = ofono_netmon_get_data(netmon);
	struct req_cb_data *cbd;

	DBG("gemalto netmon request update");

	cbd = req_cb_data_new0(cb, data, netmon);

	if (g_at_chat_send(nmd->chat, "AT^SMONI", smoni_prefix,
				smoni_cb, cbd, req_cb_data_unref))
		return;

	req_cb_data_unref(cbd);
	CALLBACK_WITH_FAILURE(cbd->cb, cbd->data);
}

static int gemalto_netmon_probe(struct ofono_netmon *netmon,
					unsigned int vendor, void *user)
{
	struct netmon_driver_data *nmd = g_new0(struct netmon_driver_data, 1);
	GAtChat *chat = user;

	DBG("gemalto netmon probe");

	nmd->chat = g_at_chat_clone(chat);

	ofono_netmon_set_data(netmon, nmd);

	g_idle_add(gemalto_delayed_register, netmon);

	return 0;
}

static void gemalto_netmon_remove(struct ofono_netmon *netmon)
{
	struct netmon_driver_data *nmd = ofono_netmon_get_data(netmon);

	DBG("gemalto netmon remove");

	g_at_chat_unref(nmd->chat);

	ofono_netmon_set_data(netmon, NULL);

	g_free(nmd);
}

static const struct ofono_netmon_driver driver = {
	.name			= "gemaltomodem",
	.probe			= gemalto_netmon_probe,
	.remove			= gemalto_netmon_remove,
	.request_update		= gemalto_netmon_request_update,
};

void gemalto_netmon_init(void)
{
	ofono_netmon_driver_register(&driver);
}

void gemalto_netmon_exit(void)
{
	ofono_netmon_driver_unregister(&driver);
}
