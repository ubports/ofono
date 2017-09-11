/*
 *
 *  oFono - Open Source Telephony
 *
 *  Copyright (C) 2008-2016  Intel Corporation. All rights reserved.
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

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <errno.h>

#include <glib.h>

#include <ofono/log.h>
#include <ofono/modem.h>
#include <ofono/netmon.h>

#include "gril.h"

#include "rilmodem.h"

/*
 * Defined below are copy of
 * RIL_CellInfoType defined in Ril.h
 */
#define NETMON_RIL_CELLINFO_TYPE_GSM		1
#define NETMON_RIL_CELLINFO_TYPE_CDMA		2
#define NETMON_RIL_CELLINFO_TYPE_LTE		3
#define NETMON_RIL_CELLINFO_TYPE_UMTS		4
#define NETMON_RIL_CELLINFO_TYPE_TDSCDMA	5

/* size of RIL_CellInfoGsm */
#define NETMON_RIL_CELLINFO_SIZE_GSM		24
/* size of RIL_CellInfoCDMA */
#define NETMON_RIL_CELLINFO_SIZE_CDMA		40
/* size of RIL_CellInfoLte */
#define NETMON_RIL_CELLINFO_SIZE_LTE		44
/* size of RIL_CellInfoWcdma */
#define NETMON_RIL_CELLINFO_SIZE_UMTS		28
/* size of RIL_CellInfoTdscdma */
#define NETMON_RIL_CELLINFO_SIZE_TDSCDMA	24

#define MSECS_RATE_INVALID	(0x7fffffff)
#define SECS_TO_MSECS(x)	((x) * 1000)

struct netmon_data {
	GRil *ril;
};

static gboolean ril_delayed_register(gpointer user_data)
{
	struct ofono_netmon *netmon = user_data;

	ofono_netmon_register(netmon);

	return FALSE;
}

static int ril_cell_type_to_size(int cell_type)
{
	switch (cell_type) {
	case NETMON_RIL_CELLINFO_TYPE_GSM:
		return NETMON_RIL_CELLINFO_SIZE_GSM;

	case NETMON_RIL_CELLINFO_TYPE_CDMA:
		return NETMON_RIL_CELLINFO_SIZE_CDMA;

	case NETMON_RIL_CELLINFO_TYPE_LTE:
		return NETMON_RIL_CELLINFO_SIZE_LTE;

	case NETMON_RIL_CELLINFO_TYPE_UMTS:
		return NETMON_RIL_CELLINFO_SIZE_UMTS;

	case NETMON_RIL_CELLINFO_TYPE_TDSCDMA:
		return NETMON_RIL_CELLINFO_SIZE_TDSCDMA;
	}

	return 0;
}

static int process_cellinfo_list(struct ril_msg *message,
					struct ofono_netmon *netmon)
{
	struct parcel rilp;
	int skip_len;
	int cell_info_cnt;
	int cell_type;
	int registered = 0;
	int mcc, mnc;
	int lac, cid, psc;
	int rssi, ber;
	char s_mcc[OFONO_MAX_MCC_LENGTH + 1];
	char s_mnc[OFONO_MAX_MNC_LENGTH + 1];
	int i, j;

	if (message->error != RIL_E_SUCCESS)
		return OFONO_ERROR_TYPE_FAILURE;

	g_ril_init_parcel(message, &rilp);

	cell_info_cnt = parcel_r_int32(&rilp);

	for (i = 0; i < cell_info_cnt; i++) {
		cell_type = parcel_r_int32(&rilp);

		registered = parcel_r_int32(&rilp);

		/* skipping unneeded timeStampType in Ril cell info */
		(void)parcel_r_int32(&rilp);

		/*skipping timeStamp which is a uint64_t type */
		(void)parcel_r_int32(&rilp);
		(void)parcel_r_int32(&rilp);

		if (registered)
			break;

		/*
		 * not serving cell,
		 * skip remainder of current cell info
		 */
		skip_len = ril_cell_type_to_size(cell_type)/sizeof(int);

		for (j = 0; j < skip_len; j++)
			(void)parcel_r_int32(&rilp);
	}

	if (!registered)
		return OFONO_ERROR_TYPE_FAILURE;

	if (cell_type == NETMON_RIL_CELLINFO_TYPE_GSM) {
		mcc = parcel_r_int32(&rilp);
		mnc = parcel_r_int32(&rilp);
		lac = parcel_r_int32(&rilp);
		cid = parcel_r_int32(&rilp);
		rssi = parcel_r_int32(&rilp);
		ber = parcel_r_int32(&rilp);

		if (mcc >= 0 && mcc <= 999)
			snprintf(s_mcc, sizeof(s_mcc), "%03d", mcc);
		else
			strcpy(s_mcc, "");

		if (mnc >= 0 && mnc <= 999)
			snprintf(s_mnc, sizeof(s_mnc), "%03d", mnc);
		else
			strcpy(s_mnc, "");

		lac = (lac >= 0 && lac <= 65535) ? lac : -1;
		cid = (cid >= 0 && cid <= 65535) ? cid : -1;
		rssi = (rssi >= 0 && rssi <= 31) ? rssi : -1;
		ber = (ber >= 0 && ber <= 7) ? ber : -1;

		ofono_netmon_serving_cell_notify(netmon,
				OFONO_NETMON_CELL_TYPE_GSM,
				OFONO_NETMON_INFO_MCC, s_mcc,
				OFONO_NETMON_INFO_MNC, s_mnc,
				OFONO_NETMON_INFO_LAC, lac,
				OFONO_NETMON_INFO_CI, cid,
				OFONO_NETMON_INFO_RSSI, rssi,
				OFONO_NETMON_INFO_BER, ber,
				OFONO_NETMON_INFO_INVALID);
	} else if (cell_type == NETMON_RIL_CELLINFO_TYPE_UMTS) {
		mcc = parcel_r_int32(&rilp);
		mnc = parcel_r_int32(&rilp);
		lac = parcel_r_int32(&rilp);
		cid = parcel_r_int32(&rilp);
		psc = parcel_r_int32(&rilp);
		rssi = parcel_r_int32(&rilp);
		ber = parcel_r_int32(&rilp);

		if (mcc >= 0 && mcc <= 999)
			snprintf(s_mcc, sizeof(s_mcc), "%03d", mcc);
		else
			strcpy(s_mcc, "");

		if (mnc >= 0 && mnc <= 999)
			snprintf(s_mnc, sizeof(s_mnc), "%03d", mnc);
		else
			strcpy(s_mnc, "");

		lac = (lac >= 0 && lac <= 65535) ? lac : -1;
		cid = (cid >= 0 && cid <= 268435455) ? cid : -1;
		psc = (psc >= 0 && rssi <= 511) ? psc : -1;
		rssi = (rssi >= 0 && rssi <= 31) ? rssi : -1;
		ber = (ber >= 0 && ber <= 7) ? ber : -1;

		ofono_netmon_serving_cell_notify(netmon,
				OFONO_NETMON_CELL_TYPE_UMTS,
				OFONO_NETMON_INFO_MCC, s_mcc,
				OFONO_NETMON_INFO_MNC, s_mnc,
				OFONO_NETMON_INFO_LAC, lac,
				OFONO_NETMON_INFO_CI, cid,
				OFONO_NETMON_INFO_PSC, psc,
				OFONO_NETMON_INFO_RSSI, rssi,
				OFONO_NETMON_INFO_BER, ber,
				OFONO_NETMON_INFO_INVALID);

	}

	return OFONO_ERROR_TYPE_NO_ERROR;
}

static void ril_netmon_update_cb(struct ril_msg *message, gpointer user_data)
{
	struct cb_data *cbd = user_data;
	ofono_netmon_cb_t cb = cbd->cb;
	struct ofono_netmon *netmon = cbd->data;

	if (process_cellinfo_list(message, netmon) ==
			OFONO_ERROR_TYPE_NO_ERROR) {
		CALLBACK_WITH_SUCCESS(cb, cbd->data);
		return;
	}

	CALLBACK_WITH_FAILURE(cb, cbd->data);
}

static void ril_cellinfo_notify(struct ril_msg *message, gpointer user_data)
{
	struct ofono_netmon *netmon = user_data;

	process_cellinfo_list(message, netmon);
}

static void setup_cell_info_notify(struct ofono_netmon *netmon)
{
	struct netmon_data *nmd = ofono_netmon_get_data(netmon);
	struct parcel rilp;

	parcel_init(&rilp);

	parcel_w_int32(&rilp, 1);	/* Number of elements */

	parcel_w_int32(&rilp, MSECS_RATE_INVALID);

	if (g_ril_send(nmd->ril, RIL_REQUEST_SET_UNSOL_CELL_INFO_LIST_RATE,
			&rilp, NULL, NULL, NULL) == 0)
		ofono_error("%s: setup failed\n", __func__);

	if (g_ril_register(nmd->ril, RIL_UNSOL_CELL_INFO_LIST,
				ril_cellinfo_notify, netmon) == 0)
		ofono_error("%s: setup failed\n", __func__);
}

static int ril_netmon_probe(struct ofono_netmon *netmon,
		unsigned int vendor, void *user)
{
	GRil *ril = user;
	struct netmon_data *ud = g_new0(struct netmon_data, 1);

	ud->ril = g_ril_clone(ril);

	ofono_netmon_set_data(netmon, ud);

	setup_cell_info_notify(netmon);

	g_idle_add(ril_delayed_register, netmon);

	return 0;
}

static void ril_netmon_remove(struct ofono_netmon *netmon)
{
	struct netmon_data *nmd = ofono_netmon_get_data(netmon);

	ofono_netmon_set_data(netmon, NULL);
	g_ril_unref(nmd->ril);
}

static void ril_netmon_request_update(struct ofono_netmon *netmon,
		ofono_netmon_cb_t cb, void *data)
{
	struct netmon_data *nmd = ofono_netmon_get_data(netmon);
	struct cb_data *cbd = cb_data_new(cb, data, nmd);

	if (g_ril_send(nmd->ril, RIL_REQUEST_GET_CELL_INFO_LIST, NULL,
			ril_netmon_update_cb, cbd, g_free) > 0)
		return;

	g_free(cbd);
	CALLBACK_WITH_FAILURE(cb, data);
}

static void periodic_update_cb(struct ril_msg *message, gpointer user_data)
{
	struct cb_data *cbd = user_data;
	ofono_netmon_cb_t cb = cbd->cb;

	if (message->error != RIL_E_SUCCESS)
		CALLBACK_WITH_FAILURE(cb, cbd->data);

	CALLBACK_WITH_SUCCESS(cb, cbd->data);
}

static void ril_netmon_periodic_update(struct ofono_netmon *netmon,
			unsigned int enable, unsigned int period,
			ofono_netmon_cb_t cb, void *data)
{
	struct netmon_data *nmd = ofono_netmon_get_data(netmon);
	struct cb_data *cbd = cb_data_new(cb, data, nmd);
	struct parcel rilp;

	parcel_init(&rilp);

	parcel_w_int32(&rilp, 1);	/* Number of elements */

	if (enable)
		parcel_w_int32(&rilp, SECS_TO_MSECS(period));
	else
		parcel_w_int32(&rilp, MSECS_RATE_INVALID);

	if (g_ril_send(nmd->ril, RIL_REQUEST_SET_UNSOL_CELL_INFO_LIST_RATE,
			&rilp, periodic_update_cb, cbd, g_free) > 0)
		return;

	g_free(cbd);
	CALLBACK_WITH_FAILURE(cb, cbd->data);
}

static struct ofono_netmon_driver driver = {
	.name			= RILMODEM,
	.probe			= ril_netmon_probe,
	.remove			= ril_netmon_remove,
	.request_update		= ril_netmon_request_update,
	.enable_periodic_update	= ril_netmon_periodic_update,
};

void ril_netmon_init(void)
{
	ofono_netmon_driver_register(&driver);
}

void ril_netmon_exit(void)
{
	ofono_netmon_driver_unregister(&driver);
}
