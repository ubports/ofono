/*
 *
 *  oFono - Open Source Telephony
 *
 *  Copyright (C) 2017  Jonas Bonn. All rights reserved.
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
#include <ofono/netmon.h>

#include "qmi.h"
#include "nas.h"

#include "qmimodem.h"
#include "src/common.h"

struct netmon_data {
	struct qmi_service *nas;
};

static void get_rssi_cb(struct qmi_result *result, void *user_data)
{
	struct cb_data *cbd = user_data;
	struct ofono_netmon *netmon = cbd->user;
	ofono_netmon_cb_t cb = cbd->cb;
	struct {
		enum ofono_netmon_cell_type type;
		int rssi;
		int ber;
		int rsrq;
		int rsrp;
	} props;
	uint16_t len;
	int16_t rsrp;
	const struct {
		int8_t value;
		int8_t rat;
	} __attribute__((__packed__)) *rsrq;
	const struct {
		uint16_t count;
		struct {
			uint8_t rssi;
			int8_t rat;
		} __attribute__((__packed__)) info[0];
	} __attribute__((__packed__)) *rssi;
	const struct {
		uint16_t count;
		struct {
			uint16_t rate;
			int8_t rat;
		} __attribute__((__packed__)) info[0];
	} __attribute__((__packed__)) *ber;
	int i;
	uint16_t num;

	DBG("");

	if (qmi_result_set_error(result, NULL)) {
		CALLBACK_WITH_FAILURE(cb, cbd->data);
		return;
	}

	/* RSSI */
	rssi = qmi_result_get(result, 0x11, &len);
	if (rssi) {
		num = GUINT16_FROM_LE(rssi->count);
		for (i = 0; i < num; i++) {
			DBG("RSSI: %hhu on RAT %hhd",
				rssi->info[i].rssi,
				rssi->info[i].rat);
		}

		/* Get cell type from RSSI info... it will be the same
		 * for all the other entries
		 */
		props.type = qmi_nas_rat_to_tech(rssi->info[0].rat);
		switch (rssi->info[0].rat) {
		case QMI_NAS_NETWORK_RAT_GSM:
			props.type = OFONO_NETMON_CELL_TYPE_GSM;
			break;
		case QMI_NAS_NETWORK_RAT_UMTS:
			props.type = OFONO_NETMON_CELL_TYPE_UMTS;
			break;
		case QMI_NAS_NETWORK_RAT_LTE:
			props.type = OFONO_NETMON_CELL_TYPE_LTE;
			break;
		default:
			props.type = OFONO_NETMON_CELL_TYPE_GSM;
			break;
		}

		props.rssi = (rssi->info[0].rssi + 113) / 2;
		if (props.rssi > 31) props.rssi = 31;
		if (props.rssi < 0) props.rssi = 0;
	} else {
		props.type = QMI_NAS_NETWORK_RAT_GSM;
		props.rssi = -1;
	}

	/* Bit error rate */
	ber = qmi_result_get(result, 0x15, &len);
	if (ber) {
		num = GUINT16_FROM_LE(ber->count);
		for (i = 0; i < ber->count; i++) {
			DBG("Bit error rate: %hu on RAT %hhd",
				GUINT16_FROM_LE(ber->info[i].rate),
				ber->info[i].rat);
		}

		props.ber = GUINT16_FROM_LE(ber->info[0].rate);
		if (props.ber > 7)
			props.ber = -1;
	} else {
		props.ber = -1;
	}

	/* LTE RSRQ */
	rsrq = qmi_result_get(result, 0x16, &len);
	if (rsrq) {
		DBG("RSRQ: %hhd on RAT %hhd",
			rsrq->value,
			rsrq->rat);

		if (rsrq->value == 0) {
			props.rsrq = -1;
		} else {
			props.rsrq = (rsrq->value + 19) * 2;
			if (props.rsrq > 34) props.rsrq = 34;
			if (props.rsrq < 0) props.rsrq = 0;
		}
	} else {
		props.rsrq = -1;
	}

	/* LTE RSRP */
	if (qmi_result_get_int16(result, 0x18, &rsrp)) {
		DBG("Got LTE RSRP: %hd", rsrp);

		if (rsrp == 0) {
			props.rsrp = -1;
		} else {
			props.rsrp = rsrp + 140;
			if (props.rsrp > 97) props.rsrp = 97;
			if (props.rsrp < 0) props.rsrp = 0;
		}
	} else {
		props.rsrp = -1;
	}

	ofono_netmon_serving_cell_notify(netmon,
				props.type,
				OFONO_NETMON_INFO_RSSI, props.rssi,
				OFONO_NETMON_INFO_BER, props.ber,
				OFONO_NETMON_INFO_RSRQ, props.rsrq,
				OFONO_NETMON_INFO_RSRP, props.rsrp,
				OFONO_NETMON_INFO_INVALID);

	CALLBACK_WITH_SUCCESS(cb, cbd->data);
}

static void qmi_netmon_request_update(struct ofono_netmon *netmon,
					ofono_netmon_cb_t cb,
					void *user_data)
{
	struct netmon_data *data = ofono_netmon_get_data(netmon);
	struct cb_data *cbd = cb_data_new(cb, user_data);
	struct qmi_param *param;

	DBG("");

	cbd->user = netmon;

	param = qmi_param_new();
	if (!param)
		goto out;

	/* Request all signal strength items: mask=0xff */
	qmi_param_append_uint16(param, 0x10, 255);

	if (qmi_service_send(data->nas, QMI_NAS_GET_RSSI, param,
					get_rssi_cb, cbd, g_free) > 0)
		return;

	qmi_param_free(param);

out:
	CALLBACK_WITH_FAILURE(cb, cbd->data);

	g_free(cbd);
}

static void create_nas_cb(struct qmi_service *service, void *user_data)
{
	struct ofono_netmon *netmon = user_data;
	struct netmon_data *nmd = ofono_netmon_get_data(netmon);

	DBG("");

	if (!service) {
		ofono_error("Failed to request NAS service");
		ofono_netmon_remove(netmon);
		return;
	}

	nmd->nas = qmi_service_ref(service);

	ofono_netmon_register(netmon);
}

static int qmi_netmon_probe(struct ofono_netmon *netmon,
					unsigned int vendor, void *user_data)
{
	struct qmi_device *device = user_data;
	struct netmon_data *nmd;

	DBG("");

	nmd = g_new0(struct netmon_data, 1);

	ofono_netmon_set_data(netmon, nmd);

	qmi_service_create_shared(device, QMI_SERVICE_NAS,
					create_nas_cb, netmon, NULL);

	return 0;
}

static void qmi_netmon_remove(struct ofono_netmon *netmon)
{
	struct netmon_data *nmd = ofono_netmon_get_data(netmon);

	DBG("");

	ofono_netmon_set_data(netmon, NULL);

	qmi_service_unregister_all(nmd->nas);

	qmi_service_unref(nmd->nas);

	g_free(nmd);
}

static const struct ofono_netmon_driver driver = {
	.name			= "qmimodem",
	.probe			= qmi_netmon_probe,
	.remove			= qmi_netmon_remove,
	.request_update		= qmi_netmon_request_update,
};

void qmi_netmon_init(void)
{
	ofono_netmon_driver_register(&driver);
}

void qmi_netmon_exit(void)
{
	ofono_netmon_driver_unregister(&driver);
}
