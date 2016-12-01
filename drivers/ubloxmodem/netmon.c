/*
 *
 *  oFono - Open Source Telephony
 *
 *  Copyright (C) 2016  EndoCode AG. All rights reserved.
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
#include <ofono/netreg.h>
#include <ofono/netmon.h>

#include "gatchat.h"
#include "gatresult.h"

#include "common.h"
#include "ubloxmodem.h"
#include "drivers/atmodem/vendor.h"

static const char *cops_prefix[] = { "+COPS:", NULL };
static const char *cesq_prefix[] = { "+CESQ:", NULL };

struct netmon_driver_data {
	GAtChat *chat;
};

struct req_cb_data {
	gint ref_count; /* Ref count */

	struct ofono_netmon *netmon;

	ofono_netmon_cb_t cb;
	void *data;

	struct ofono_network_operator op;

	int rxlev;	/* CESQ: Received Signal Strength Indication */
	int ber;	/* CESQ: Bit Error Rate */
	int rscp;	/* CESQ: Received Signal Code Powe */
	int rsrp;	/* CESQ: Reference Signal Received Power */
	int ecn0;	/* CESQ: Received Energy Ratio */
	int rsrq;	/* CESQ: Reference Signal Received Quality */
};

/*
 * Returns the appropriate radio access technology.
 *
 * If we can not resolve to a specific radio access technolgy
 * we return OFONO_NETMON_CELL_TYPE_GSM by default.
 */
static int ublox_map_radio_access_technology(int tech)
{
	switch (tech) {
	case ACCESS_TECHNOLOGY_GSM:
	case ACCESS_TECHNOLOGY_GSM_COMPACT:
		return OFONO_NETMON_CELL_TYPE_GSM;
	case ACCESS_TECHNOLOGY_UTRAN:
	case ACCESS_TECHNOLOGY_UTRAN_HSDPA:
	case ACCESS_TECHNOLOGY_UTRAN_HSUPA:
	case ACCESS_TECHNOLOGY_UTRAN_HSDPA_HSUPA:
		return OFONO_NETMON_CELL_TYPE_UMTS;
	case ACCESS_TECHNOLOGY_EUTRAN:
		return OFONO_NETMON_CELL_TYPE_LTE;
	}

	return OFONO_NETMON_CELL_TYPE_GSM;
}

static inline struct req_cb_data *req_cb_data_new0(void *cb, void *data,
							void *user)
{
	struct req_cb_data *ret = g_new0(struct req_cb_data, 1);

	ret->ref_count = 1;
	ret->cb = cb;
	ret->data = data;
	ret->netmon = user;
	ret->rxlev = -1;
	ret->ber = -1;
	ret->rscp = -1;
	ret->rsrp = -1;
	ret->ecn0 = -1;
	ret->rsrq = -1;

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
	gboolean is_zero;
	struct req_cb_data *cbd = user_data;

	if (cbd == NULL)
		return;

	is_zero = g_atomic_int_dec_and_test(&cbd->ref_count);

	if (is_zero == TRUE)
		g_free(cbd);
}

static gboolean ublox_delayed_register(gpointer user_data)
{
	struct ofono_netmon *netmon = user_data;

	ofono_netmon_register(netmon);

	return FALSE;
}

static void ublox_netmon_finish_success(struct req_cb_data *cbd)
{
	struct ofono_netmon *nm = cbd->netmon;

	ofono_netmon_serving_cell_notify(nm,
					cbd->op.tech,
					OFONO_NETMON_INFO_RXLEV, cbd->rxlev,
					OFONO_NETMON_INFO_BER, cbd->ber,
					OFONO_NETMON_INFO_RSCP, cbd->rscp,
					OFONO_NETMON_INFO_ECN0, cbd->ecn0,
					OFONO_NETMON_INFO_RSRQ, cbd->rsrq,
					OFONO_NETMON_INFO_RSRP, cbd->rsrp,
					OFONO_NETMON_INFO_INVALID);

	CALLBACK_WITH_SUCCESS(cbd->cb, cbd->data);
}

static void cesq_cb(gboolean ok, GAtResult *result, gpointer user_data)
{
	enum cesq_ofono_netmon_info {
		CESQ_RXLEV,
		CESQ_BER,
		CESQ_RSCP,
		CESQ_ECN0,
		CESQ_RSRQ,
		CESQ_RSRP,
		_MAX,
	};

	struct req_cb_data *cbd = user_data;
	struct ofono_error error;
	GAtResultIter iter;
	int idx, number;

	DBG("ok %d", ok);

	decode_at_error(&error, g_at_result_final_response(result));

	if (!ok) {
		CALLBACK_WITH_FAILURE(cbd->cb, cbd->data);
		return;
	}

	g_at_result_iter_init(&iter, result);

	if (!g_at_result_iter_next(&iter, "+CESQ:")) {
		DBG(" CESQ: no result ");
		goto out;
	}

	for (idx = 0; idx < _MAX; idx++) {
		ok = g_at_result_iter_next_number(&iter, &number);

		if (!ok) {
			/* Ignore and do not fail */
			DBG(" CESQ: error parsing idx: %d ", idx);
			goto out;
		}

		switch (idx) {
		case CESQ_RXLEV:
			cbd->rxlev = number != 99 ? number:cbd->rxlev;
			break;
		case CESQ_BER:
			cbd->ber = number != 99 ? number:cbd->ber;
			break;
		case CESQ_RSCP:
			cbd->rscp = number != 255 ? number:cbd->rscp;
			break;
		case CESQ_ECN0:
			cbd->ecn0 = number != 255 ? number:cbd->ecn0;
			break;
		case CESQ_RSRQ:
			cbd->rsrq = number != 255 ? number:cbd->rsrq;
			break;
		case CESQ_RSRP:
			cbd->rsrp = number != 255 ? number:cbd->rsrp;
			break;
		}
	}

	DBG(" RXLEV	%d ", cbd->rxlev);
	DBG(" BER	%d ", cbd->ber);
	DBG(" RSCP	%d ", cbd->rscp);
	DBG(" ECN0	%d ", cbd->ecn0);
	DBG(" RSRQ	%d ", cbd->rsrq);
	DBG(" RSRP	%d ", cbd->rsrp);

	/*
	 * We never fail at this point we always send what we collected so
	 * far
	 */
out:
	ublox_netmon_finish_success(cbd);
}

static void cops_cb(gboolean ok, GAtResult *result, gpointer user_data)
{
	struct req_cb_data *cbd = user_data;
	struct ofono_netmon *nm = cbd->netmon;
	struct netmon_driver_data *nmd = ofono_netmon_get_data(nm);
	struct ofono_error error;
	GAtResultIter iter;
	int tech;

	DBG("ok %d", ok);

	decode_at_error(&error, g_at_result_final_response(result));

	if (!ok) {
		CALLBACK_WITH_FAILURE(cbd->cb, cbd->data);
		return;
	}

	g_at_result_iter_init(&iter, result);

	/* Do not fail */
	if (!g_at_result_iter_next(&iter, "+COPS:")) {
		CALLBACK_WITH_SUCCESS(cbd->cb, cbd->data);
		return;
	}

	g_at_result_iter_skip_next(&iter);
	g_at_result_iter_skip_next(&iter);
	g_at_result_iter_skip_next(&iter);

	/* Default to GSM */
	if (g_at_result_iter_next_number(&iter, &tech) == FALSE)
		cbd->op.tech = OFONO_NETMON_CELL_TYPE_GSM;
	else
		cbd->op.tech = ublox_map_radio_access_technology(tech);

	cbd = req_cb_data_ref(cbd);
	if (g_at_chat_send(nmd->chat, "AT+CESQ", cesq_prefix,
				cesq_cb, cbd, req_cb_data_unref) == 0) {
		CALLBACK_WITH_FAILURE(cbd->cb, cbd->data);
		req_cb_data_unref(cbd);
	}
}

static void ublox_netmon_request_update(struct ofono_netmon *netmon,
					ofono_netmon_cb_t cb, void *data)
{
	struct netmon_driver_data *nmd = ofono_netmon_get_data(netmon);
	struct req_cb_data *cbd;

	DBG("ublox netmon request update");

	cbd = req_cb_data_new0(cb, data, netmon);

	if (g_at_chat_send(nmd->chat, "AT+COPS?", cops_prefix,
				cops_cb, cbd, req_cb_data_unref) == 0) {
		CALLBACK_WITH_FAILURE(cbd->cb, cbd->data);
		req_cb_data_unref(cbd);
	}
}

static int ublox_netmon_probe(struct ofono_netmon *netmon,
					unsigned int vendor, void *user)
{
	GAtChat *chat = user;
	struct netmon_driver_data *nmd;

	DBG("ublox netmon probe");

	nmd = g_try_new0(struct netmon_driver_data, 1);
	if (nmd == NULL)
		return -ENOMEM;

	nmd->chat = g_at_chat_clone(chat);

	ofono_netmon_set_data(netmon, nmd);

	g_idle_add(ublox_delayed_register, netmon);

	return 0;
}

static void ublox_netmon_remove(struct ofono_netmon *netmon)
{
	struct netmon_driver_data *nmd = ofono_netmon_get_data(netmon);

	DBG("ublox netmon remove");

	g_at_chat_unref(nmd->chat);

	ofono_netmon_set_data(netmon, NULL);

	g_free(nmd);
}

static struct ofono_netmon_driver driver = {
	.name			= UBLOXMODEM,
	.probe			= ublox_netmon_probe,
	.remove			= ublox_netmon_remove,
	.request_update		= ublox_netmon_request_update,
};

void ublox_netmon_init(void)
{
	ofono_netmon_driver_register(&driver);
}

void ublox_netmon_exit(void)
{
	ofono_netmon_driver_unregister(&driver);
}
