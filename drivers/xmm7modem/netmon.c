/*
 *
 *  oFono - Open Source Telephony
 *
 *  Copyright (C) 2017  Intel Corporation. All rights reserved.
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
#include "xmm7modem.h"
#include "drivers/atmodem/vendor.h"

static const char *xmci_prefix[] = { "+XMCI:", NULL };

struct netmon_driver_data {
	GAtChat *chat;
	int xmci_mode;
};

enum xmci_ofono_type_info {
	XMCI_GSM_SERV_CELL,
	XMCI_GSM_NEIGH_CELL,
	XMCI_UMTS_SERV_CELL,
	XMCI_UMTS_NEIGH_CELL,
	XMCI_LTE_SERV_CELL,
	XMCI_LTE_NEIGH_CELL
};

/*
 * Returns the appropriate radio access technology.
 *
 * If we can not resolve to a specific radio access technolgy
 * we return OFONO_NETMON_CELL_TYPE_GSM by default.
 */
static int xmm7modem_map_radio_access_technology(int tech)
{
	switch (tech) {
	case XMCI_GSM_SERV_CELL:
	case XMCI_GSM_NEIGH_CELL:
		return OFONO_NETMON_CELL_TYPE_GSM;
	case XMCI_UMTS_SERV_CELL:
	case XMCI_UMTS_NEIGH_CELL:
		return OFONO_NETMON_CELL_TYPE_UMTS;
	case XMCI_LTE_SERV_CELL:
	case XMCI_LTE_NEIGH_CELL:
		return OFONO_NETMON_CELL_TYPE_LTE;
	}

	return OFONO_NETMON_CELL_TYPE_GSM;
}

static void xmci_cb(gboolean ok, GAtResult *result, gpointer user_data)
{
	struct cb_data *cbd = user_data;
	struct ofono_netmon *netmon = cbd->data;
	struct netmon_driver_data *nmd = ofono_netmon_get_data(netmon);
	ofono_netmon_cb_t cb = cbd->cb;
	struct ofono_error error;
	GAtResultIter iter;
	int number;
	int rxlev = -1;
	int ber = -1;
	int rscp = -1;
	int rsrp = -1;
	int ecn0 = -1;
	int rsrq = -1;
	int tech = -1;
	int type = -1;
	int ci = -1;
	const char *cell_id;
	char mcc[3];
	char mnc[3];

	DBG("ok %d", ok);

	decode_at_error(&error, g_at_result_final_response(result));

	if (!ok) {
		cb(&error, cbd->data);
		return;
	}

	g_at_result_iter_init(&iter, result);

	while (g_at_result_iter_next(&iter, "+XMCI:")) {
		if (!g_at_result_iter_next_number(&iter, &type))
			break;

		tech = xmm7modem_map_radio_access_technology(type);

		switch (type) {
		case XMCI_GSM_NEIGH_CELL:
		case XMCI_GSM_SERV_CELL:
			/* <MCC>,<MNC>,<LAC>,<CI>,<BSIC> */
			g_at_result_iter_next_number(&iter, &number);
			snprintf(mcc, 3, "%d", number);
			g_at_result_iter_next_number(&iter, &number);
			snprintf(mnc, 3, "%d", number);
			g_at_result_iter_skip_next(&iter);
			g_at_result_iter_next_string(&iter, &cell_id);
			sscanf(&cell_id[2], "%x", &number);
			ci = number != -1 ? number : 0;
			g_at_result_iter_skip_next(&iter);

			g_at_result_iter_next_number(&iter, &number);
			rxlev = number != 99 ? number : rxlev;

			g_at_result_iter_next_number(&iter, &number);
			ber = number != 99 ? number : ber;
			break;
		case XMCI_UMTS_NEIGH_CELL:
		case XMCI_UMTS_SERV_CELL:
			/*
			 * <MCC>,<MNC>,<LAC>,<CI><PSC>,<DLUARFNC>,
			 * <ULUARFCN>,<PATHLOSS>,<RSSI>
			 */
			g_at_result_iter_next_number(&iter, &number);
			snprintf(mcc, 3, "%d", number);
			g_at_result_iter_next_number(&iter, &number);
			snprintf(mnc, 3, "%d", number);
			g_at_result_iter_skip_next(&iter);
			g_at_result_iter_next_string(&iter, &cell_id);
			sscanf(&cell_id[2], "%x", &number);
			ci = number != -1 ? number : 0;
			g_at_result_iter_skip_next(&iter);
			g_at_result_iter_skip_next(&iter);
			g_at_result_iter_skip_next(&iter);
			g_at_result_iter_skip_next(&iter);
			g_at_result_iter_skip_next(&iter);

			g_at_result_iter_next_number(&iter, &number);
			rscp = number != 255 ? number : rscp;

			g_at_result_iter_next_number(&iter, &number);
			ecn0 = number != 255 ? number : ecn0;
			break;
		case XMCI_LTE_NEIGH_CELL:
		case XMCI_LTE_SERV_CELL:
			/*
			 * <MCC>,<MNC>,<TAC>,<CI>,<PCI>,<DLUARFNC>,
			 * <ULUARFCN>,<PATHLOSS_LTE>
			 */
			g_at_result_iter_next_number(&iter, &number);
			snprintf(mcc, 3, "%d", number);
			g_at_result_iter_next_number(&iter, &number);
			snprintf(mnc, 3, "%d", number);
			g_at_result_iter_skip_next(&iter);
			g_at_result_iter_next_string(&iter, &cell_id);
			sscanf(&cell_id[2], "%x", &number);
			ci = number != -1 ? number : 0;
			g_at_result_iter_skip_next(&iter);
			g_at_result_iter_skip_next(&iter);
			g_at_result_iter_skip_next(&iter);
			g_at_result_iter_skip_next(&iter);

			g_at_result_iter_next_number(&iter, &number);
			rsrq = number != 255 ? number : rsrq;

			g_at_result_iter_next_number(&iter, &number);
			rsrp = number != 255 ? number : rsrp;
			break;
		default:
			break;
		}

		if ((nmd->xmci_mode == 0) &&
				(type == XMCI_GSM_NEIGH_CELL ||
				type == XMCI_UMTS_NEIGH_CELL ||
				type == XMCI_LTE_NEIGH_CELL)) {
			ofono_netmon_neighbouring_cell_notify(netmon,
						tech,
						OFONO_NETMON_INFO_MCC, mcc,
						OFONO_NETMON_INFO_MNC, mnc,
						OFONO_NETMON_INFO_CI, ci,
						OFONO_NETMON_INFO_RXLEV, rxlev,
						OFONO_NETMON_INFO_BER, ber,
						OFONO_NETMON_INFO_RSCP, rscp,
						OFONO_NETMON_INFO_ECN0, ecn0,
						OFONO_NETMON_INFO_RSRQ, rsrq,
						OFONO_NETMON_INFO_RSRP, rsrp,
						OFONO_NETMON_INFO_INVALID);
		} else if ((nmd->xmci_mode == 1) &&
				(type == XMCI_GSM_SERV_CELL ||
				type == XMCI_UMTS_SERV_CELL ||
				type == XMCI_LTE_SERV_CELL)) {
			ofono_netmon_serving_cell_notify(netmon,
						tech,
						OFONO_NETMON_INFO_RXLEV, rxlev,
						OFONO_NETMON_INFO_BER, ber,
						OFONO_NETMON_INFO_RSCP, rscp,
						OFONO_NETMON_INFO_ECN0, ecn0,
						OFONO_NETMON_INFO_RSRQ, rsrq,
						OFONO_NETMON_INFO_RSRP, rsrp,
						OFONO_NETMON_INFO_INVALID);
			break;
		}
	}

	CALLBACK_WITH_SUCCESS(cb, cbd->data);
	nmd->xmci_mode = -1;
}

static void xmm7modem_netmon_request_update(struct ofono_netmon *netmon,
					ofono_netmon_cb_t cb, void *data)
{
	struct netmon_driver_data *nmd = ofono_netmon_get_data(netmon);
	struct cb_data *cbd = cb_data_new(cb, data);
	nmd->xmci_mode = 1;

	DBG("xmm7modem netmon request update");

	if (g_at_chat_send(nmd->chat, "AT+XMCI=1", xmci_prefix,
				xmci_cb, cbd, g_free) > 0)
		return;

	g_free(cbd);
	CALLBACK_WITH_FAILURE(cb, data);
}

static void xmm7modem_neighbouring_cell_update(struct ofono_netmon *netmon,
					ofono_netmon_cb_t cb, void *data)
{
	struct netmon_driver_data *nmd = ofono_netmon_get_data(netmon);
	struct cb_data *cbd = cb_data_new(cb, data);
	nmd->xmci_mode = 0;

	DBG("xmm7modem netmon request neighbouring cell update");

	if (g_at_chat_send(nmd->chat, "AT+XMCI=0", xmci_prefix,
			xmci_cb, cbd, g_free) > 0)
		return;

	g_free(cbd);
	CALLBACK_WITH_FAILURE(cb, data);
}

static gboolean ril_delayed_register(gpointer user_data)
{
	struct ofono_netmon *netmon = user_data;

	ofono_netmon_register(netmon);

	return FALSE;
}

static int xmm7modem_netmon_probe(struct ofono_netmon *netmon,
					unsigned int vendor, void *user)
{
	GAtChat *chat = user;
	struct netmon_driver_data *nmd;

	DBG("xmm7modem netmon probe");

	nmd = g_new0(struct netmon_driver_data, 1);
	nmd->chat = g_at_chat_clone(chat);
	nmd->xmci_mode = -1;

	ofono_netmon_set_data(netmon, nmd);

	g_idle_add(ril_delayed_register, netmon);

	return 0;
}

static void xmm7modem_netmon_remove(struct ofono_netmon *netmon)
{
	struct netmon_driver_data *nmd = ofono_netmon_get_data(netmon);

	DBG("xmm7modem netmon remove");

	g_at_chat_unref(nmd->chat);

	ofono_netmon_set_data(netmon, NULL);

	g_free(nmd);
}

static const struct ofono_netmon_driver driver = {
	.name			= XMM7MODEM,
	.probe			= xmm7modem_netmon_probe,
	.remove			= xmm7modem_netmon_remove,
	.request_update		= xmm7modem_netmon_request_update,
	.neighbouring_cell_update = xmm7modem_neighbouring_cell_update,
};

void xmm_netmon_init(void)
{
	ofono_netmon_driver_register(&driver);
}

void xmm_netmon_exit(void)
{
	ofono_netmon_driver_unregister(&driver);
}
