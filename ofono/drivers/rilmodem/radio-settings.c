/*
 *
 *  oFono - Open Source Telephony
 *
 *  Copyright (C) 2008-2011  Intel Corporation. All rights reserved.
 *  Copyright (C) 2013 Jolla Ltd
 *  Contact: Jussi Kangas <jussi.kangas@tieto.com>
 *  Copyright (C) 2014 Canonical Ltd
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
#include <string.h>
#include <errno.h>

#include <glib.h>

#include <ofono/log.h>
#include <ofono/modem.h>
#include <ofono/radio-settings.h>

#include "gril.h"

#include "rilmodem.h"

/* Preferred network types */
#define PREF_NET_TYPE_GSM_WCDMA 0
#define PREF_NET_TYPE_GSM_ONLY 1
#define PREF_NET_TYPE_WCDMA 2
#define PREF_NET_TYPE_GSM_WCDMA_AUTO 3
#define PREF_NET_TYPE_CDMA_EVDO_AUTO 4
#define PREF_NET_TYPE_CDMA_ONLY 5
#define PREF_NET_TYPE_EVDO_ONLY 6
#define PREF_NET_TYPE_GSM_WCDMA_CDMA_EVDO_AUTO 7
#define PREF_NET_TYPE_LTE_CDMA_EVDO 8
#define PREF_NET_TYPE_LTE_GSM_WCDMA 9
#define PREF_NET_TYPE_LTE_CMDA_EVDO_GSM_WCDMA 10
#define PREF_NET_TYPE_LTE_ONLY 11
#define PREF_NET_TYPE_LTE_WCDMA 12
/* MTK specific network types */
#define MTK_PREF_NET_TYPE_BASE 30
#define MTK_PREF_NET_TYPE_LTE_GSM_WCDMA (MTK_PREF_NET_TYPE_BASE + 1)
#define MTK_PREF_NET_TYPE_LTE_GSM_WCDMA_MMDC (MTK_PREF_NET_TYPE_BASE + 2)
#define MTK_PREF_NET_TYPE_GSM_WCDMA_LTE (MTK_PREF_NET_TYPE_BASE + 3)
#define MTK_PREF_NET_TYPE_GSM_WCDMA_LTE_MMDC (MTK_PREF_NET_TYPE_BASE + 4)
#define MTK_PREF_NET_TYPE_LTE_GSM_TYPE (MTK_PREF_NET_TYPE_BASE + 5)
#define MTK_PREF_NET_TYPE_LTE_GSM_MMDC_TYPE (MTK_PREF_NET_TYPE_BASE + 6)

/*GSM Band*/
#define PREF_NET_BAND_GSM_AUTOMATIC 255
#define PREF_NET_BAND_GSM850 6
#define PREF_NET_BAND_GSM900_P 1
#define PREF_NET_BAND_GSM900_E 2
#define PREF_NET_BAND_GSM1800 4
#define PREF_NET_BAND_GSM1900 5

/*UMTS Band*/
#define PREF_NET_BAND_UMTS_AUTOMATIC 255
#define PREF_NET_BAND_UMTS_V 54
#define PREF_NET_BAND_UMTS_VIII 57
#define PREF_NET_BAND_UMTS_IV 53
#define PREF_NET_BAND_UMTS_II 51
#define PREF_NET_BAND_UMTS_I 50

struct radio_data {
	GRil *ril;
	gboolean fast_dormancy;
	gboolean pending_fd;
	unsigned int vendor;
};

static void ril_set_rat_cb(struct ril_msg *message, gpointer user_data)
{
	struct cb_data *cbd = user_data;
	struct ofono_radio_settings *rs = cbd->user;
	struct radio_data *rd = ofono_radio_settings_get_data(rs);
	ofono_radio_settings_rat_mode_set_cb_t cb = cbd->cb;

	if (message->error == RIL_E_SUCCESS) {
		g_ril_print_response_no_args(rd->ril, message);
		CALLBACK_WITH_SUCCESS(cb, cbd->data);
	} else {
		ofono_error("%s: rat mode setting failed", __func__);
		CALLBACK_WITH_FAILURE(cb, cbd->data);
	}
}

static void ril_set_rat_mode(struct ofono_radio_settings *rs,
			enum ofono_radio_access_mode mode,
			ofono_radio_settings_rat_mode_set_cb_t cb,
			void *data)
{
	struct radio_data *rd = ofono_radio_settings_get_data(rs);
	struct cb_data *cbd = cb_data_new(cb, data, rs);
	struct parcel rilp;
	int pref = PREF_NET_TYPE_LTE_GSM_WCDMA;

	switch (mode) {
	case OFONO_RADIO_ACCESS_MODE_ANY:
		pref = PREF_NET_TYPE_LTE_GSM_WCDMA;
		break;
	case OFONO_RADIO_ACCESS_MODE_GSM:
		pref = PREF_NET_TYPE_GSM_ONLY;
		break;
	case OFONO_RADIO_ACCESS_MODE_UMTS:
		pref = PREF_NET_TYPE_GSM_WCDMA;
		break;
	case OFONO_RADIO_ACCESS_MODE_LTE:
		pref = PREF_NET_TYPE_LTE_GSM_WCDMA;
		break;
	}

	parcel_init(&rilp);

	parcel_w_int32(&rilp, 1);	/* Number of params */
	parcel_w_int32(&rilp, pref);

	g_ril_append_print_buf(rd->ril, "(%d)", pref);

	if (g_ril_send(rd->ril, RIL_REQUEST_SET_PREFERRED_NETWORK_TYPE,
				&rilp, ril_set_rat_cb, cbd, g_free) > 0)
		return;

	g_free(cbd);
	CALLBACK_WITH_FAILURE(cb, data);
}

static void ril_rat_mode_cb(struct ril_msg *message, gpointer user_data)
{
	struct cb_data *cbd = user_data;
	ofono_radio_settings_rat_mode_query_cb_t cb = cbd->cb;
	struct ofono_radio_settings *rs = cbd->user;
	struct radio_data *rd = ofono_radio_settings_get_data(rs);
	int mode;
	struct parcel rilp;
	int net_type;

	if (message->error != RIL_E_SUCCESS)
		goto error;

	g_ril_init_parcel(message, &rilp);
	if (parcel_r_int32(&rilp) != 1)
		goto error;

	net_type = parcel_r_int32(&rilp);

	if (rilp.malformed)
		goto error;

	g_ril_append_print_buf(rd->ril, "{%d}", net_type);
	g_ril_print_response(rd->ril, message);

	/* Try to translate special MTK settings */
	if (g_ril_vendor(rd->ril) == OFONO_RIL_VENDOR_MTK) {
		switch (net_type) {
		/* 4G preferred */
		case MTK_PREF_NET_TYPE_LTE_GSM_WCDMA:
		case MTK_PREF_NET_TYPE_LTE_GSM_WCDMA_MMDC:
		case MTK_PREF_NET_TYPE_LTE_GSM_TYPE:
		case MTK_PREF_NET_TYPE_LTE_GSM_MMDC_TYPE:
			net_type = PREF_NET_TYPE_LTE_GSM_WCDMA;
			break;
		/* 3G or 2G preferred over LTE */
		case MTK_PREF_NET_TYPE_GSM_WCDMA_LTE:
		case MTK_PREF_NET_TYPE_GSM_WCDMA_LTE_MMDC:
			net_type = PREF_NET_TYPE_GSM_WCDMA;
			break;
		}
	}

	if (net_type < 0 || net_type > PREF_NET_TYPE_LTE_ONLY) {
		ofono_error("%s: unknown network type", __func__);
		goto error;
	}

	/*
	 * GSM_WCDMA_AUTO -> ril.h: GSM/WCDMA (auto mode, according to PRL)
	 * PRL: preferred roaming list.
	 * This value is returned when selecting the slot as having 3G
	 * capabilities, so it is sort of the default for MTK modems.
	 */
	switch (net_type) {
	case PREF_NET_TYPE_WCDMA:
	case PREF_NET_TYPE_GSM_WCDMA:
	case PREF_NET_TYPE_GSM_WCDMA_AUTO:
		mode = OFONO_RADIO_ACCESS_MODE_UMTS;
		break;
	case PREF_NET_TYPE_GSM_ONLY:
		mode = OFONO_RADIO_ACCESS_MODE_GSM;
		break;
	case PREF_NET_TYPE_LTE_GSM_WCDMA:
		mode = OFONO_RADIO_ACCESS_MODE_LTE;
		break;
	default:
		ofono_error("%s: Unexpected preferred network type (%d)",
				__func__, net_type);
		mode = OFONO_RADIO_ACCESS_MODE_ANY;
		break;
	}

	CALLBACK_WITH_SUCCESS(cb, mode, cbd->data);
	return;

error:
	CALLBACK_WITH_FAILURE(cb, -1, cbd->data);
}

static void ril_query_rat_mode(struct ofono_radio_settings *rs,
			ofono_radio_settings_rat_mode_query_cb_t cb,
			void *data)
{
	struct radio_data *rd = ofono_radio_settings_get_data(rs);
	struct cb_data *cbd = cb_data_new(cb, data, rs);

	if (g_ril_send(rd->ril, RIL_REQUEST_GET_PREFERRED_NETWORK_TYPE,
				NULL, ril_rat_mode_cb, cbd, g_free) > 0)
		return;

	g_free(cbd);
	CALLBACK_WITH_FAILURE(cb, -1, data);
}

static void ril_query_fast_dormancy(struct ofono_radio_settings *rs,
			ofono_radio_settings_fast_dormancy_query_cb_t cb,
			void *data)
{
	struct radio_data *rd = ofono_radio_settings_get_data(rs);

	CALLBACK_WITH_SUCCESS(cb, rd->fast_dormancy, data);
}

static void ril_display_state_cb(struct ril_msg *message, gpointer user_data)
{
	struct cb_data *cbd = user_data;
	struct ofono_radio_settings *rs = cbd->user;
	struct radio_data *rd = ofono_radio_settings_get_data(rs);
	ofono_radio_settings_fast_dormancy_set_cb_t cb = cbd->cb;

	if (message->error == RIL_E_SUCCESS) {
		g_ril_print_response_no_args(rd->ril, message);

		rd->fast_dormancy = rd->pending_fd;

		CALLBACK_WITH_SUCCESS(cb, cbd->data);
	} else {
		CALLBACK_WITH_FAILURE(cb, cbd->data);
	}
}

static void ril_set_fast_dormancy(struct ofono_radio_settings *rs,
				ofono_bool_t enable,
				ofono_radio_settings_fast_dormancy_set_cb_t cb,
				void *data)
{
	struct radio_data *rd = ofono_radio_settings_get_data(rs);
	struct cb_data *cbd = cb_data_new(cb, data, rs);
	struct parcel rilp;

	parcel_init(&rilp);
	parcel_w_int32(&rilp, 1);	/* Number of params */
	parcel_w_int32(&rilp, enable);

	g_ril_append_print_buf(rd->ril, "(%d)", enable);

	rd->pending_fd = enable;

	if (g_ril_send(rd->ril, RIL_REQUEST_SCREEN_STATE, &rilp,
			ril_display_state_cb, cbd, g_free) > 0)
		return;

	g_free(cbd);
	CALLBACK_WITH_FAILURE(cb, data);
}

static void ril_query_available_rats(struct ofono_radio_settings *rs,
			ofono_radio_settings_available_rats_query_cb_t cb,
			void *data)
{
	unsigned int available_rats;
	struct ofono_modem *modem = ofono_radio_settings_get_modem(rs);

	available_rats = OFONO_RADIO_ACCESS_MODE_GSM
				| OFONO_RADIO_ACCESS_MODE_UMTS;

	if (ofono_modem_get_boolean(modem, MODEM_PROP_LTE_CAPABLE))
		available_rats |= OFONO_RADIO_ACCESS_MODE_LTE;

	CALLBACK_WITH_SUCCESS(cb, available_rats, data);
}

static void ril_set_band_cb(struct ril_msg *message, gpointer user_data)
{
	struct cb_data *cbd = user_data;
	struct ofono_radio_settings *rs = cbd->user;
	struct radio_data *rd = ofono_radio_settings_get_data(rs);
	ofono_radio_settings_band_set_cb_t cb = cbd->cb;

	if (message->error == RIL_E_SUCCESS) {
		g_ril_print_response_no_args(rd->ril, message);

		CALLBACK_WITH_SUCCESS(cb, cbd->data);
	} else {
		CALLBACK_WITH_FAILURE(cb, cbd->data);
	}
}

static void ril_sofia3gr_set_band(struct ofono_radio_settings *rs,
					enum ofono_radio_band_gsm band_gsm,
					enum ofono_radio_band_umts band_umts,
					ofono_radio_settings_band_set_cb_t cb,
					void *data)
{
	struct radio_data *rd = ofono_radio_settings_get_data(rs);
	struct cb_data *cbd = cb_data_new(cb, data, rs);
	struct parcel rilp;
	char cmd_buf[9], gsm_band[4], umts_band[4];
	/* RIL_OEM_HOOK_STRING_SET_BAND_PREFERENCE = 0x000000CE */
	int cmd_id = 0x000000CE;
	sprintf(cmd_buf, "%d", cmd_id);

	switch (band_gsm) {
	case OFONO_RADIO_BAND_GSM_ANY:
		sprintf(gsm_band, "%d", PREF_NET_BAND_GSM_AUTOMATIC);
		break;
	case OFONO_RADIO_BAND_GSM_850:
		sprintf(gsm_band, "%d", PREF_NET_BAND_GSM850);
		break;
	case OFONO_RADIO_BAND_GSM_900P:
		sprintf(gsm_band, "%d", PREF_NET_BAND_GSM900_P);
		break;
	case OFONO_RADIO_BAND_GSM_900E:
		sprintf(gsm_band, "%d", PREF_NET_BAND_GSM900_E);
		break;
	case OFONO_RADIO_BAND_GSM_1800:
		sprintf(gsm_band, "%d", PREF_NET_BAND_GSM1800);
		break;
	case OFONO_RADIO_BAND_GSM_1900:
		sprintf(gsm_band, "%d", PREF_NET_BAND_GSM1900);
		break;
	default:
		CALLBACK_WITH_FAILURE(cb,  data);
		return;
	}

	switch (band_umts) {
	case OFONO_RADIO_BAND_UMTS_ANY:
		sprintf(umts_band, "%d", PREF_NET_BAND_UMTS_AUTOMATIC);
		break;
	case OFONO_RADIO_BAND_UMTS_850:
		sprintf(umts_band, "%d", PREF_NET_BAND_UMTS_V);
		break;
	case OFONO_RADIO_BAND_UMTS_900:
		sprintf(umts_band, "%d", PREF_NET_BAND_UMTS_VIII);
		break;
	case OFONO_RADIO_BAND_UMTS_1700AWS:
		sprintf(umts_band, "%d", PREF_NET_BAND_UMTS_IV);
		break;
	case OFONO_RADIO_BAND_UMTS_1900:
		sprintf(umts_band, "%d", PREF_NET_BAND_UMTS_II);
		break;
	case OFONO_RADIO_BAND_UMTS_2100:
		sprintf(umts_band, "%d", PREF_NET_BAND_UMTS_I);
		break;
	default:
		CALLBACK_WITH_FAILURE(cb,  data);
		return;
	}

	parcel_init(&rilp);
	parcel_w_int32(&rilp, 3);	/* Number of params */
	parcel_w_string(&rilp, cmd_buf);
	parcel_w_string(&rilp, gsm_band);
	parcel_w_string(&rilp, umts_band);

	if (g_ril_send(rd->ril, RIL_REQUEST_OEM_HOOK_STRINGS, &rilp,
			ril_set_band_cb, cbd, g_free) > 0)
		return;

	g_free(cbd);
	CALLBACK_WITH_FAILURE(cb, data);
}

static void ril_set_band(struct ofono_radio_settings *rs,
			enum ofono_radio_band_gsm band_gsm,
			enum ofono_radio_band_umts band_umts,
			ofono_radio_settings_band_set_cb_t cb,
			void *data)
{
	struct radio_data *rd = ofono_radio_settings_get_data(rs);

	switch (rd->vendor) {
	case OFONO_RIL_VENDOR_IMC_SOFIA3GR:
		ril_sofia3gr_set_band(rs, band_gsm, band_umts, cb, data);
		return;
	default:
		break;
	}

	CALLBACK_WITH_FAILURE(cb, data);
}

static void ril_delayed_register(const struct ofono_error *error,
							void *user_data)
{
	struct ofono_radio_settings *rs = user_data;

	if (error->type == OFONO_ERROR_TYPE_NO_ERROR)
		ofono_radio_settings_register(rs);
	else
		ofono_error("%s: cannot set default fast dormancy", __func__);
}

static int ril_radio_settings_probe(struct ofono_radio_settings *rs,
					unsigned int vendor, void *user)
{
	GRil *ril = user;
	struct radio_data *rsd = g_new0(struct radio_data, 1);

	rsd->ril = g_ril_clone(ril);
	rsd->vendor = vendor;

	ofono_radio_settings_set_data(rs, rsd);

	ril_set_fast_dormancy(rs, FALSE, ril_delayed_register, rs);

	return 0;
}

static void ril_radio_settings_remove(struct ofono_radio_settings *rs)
{
	struct radio_data *rd = ofono_radio_settings_get_data(rs);
	ofono_radio_settings_set_data(rs, NULL);

	g_ril_unref(rd->ril);
	g_free(rd);
}

static struct ofono_radio_settings_driver driver = {
	.name			= RILMODEM,
	.probe			= ril_radio_settings_probe,
	.remove			= ril_radio_settings_remove,
	.query_rat_mode		= ril_query_rat_mode,
	.set_rat_mode		= ril_set_rat_mode,
	.set_band		= ril_set_band,
	.query_fast_dormancy	= ril_query_fast_dormancy,
	.set_fast_dormancy	= ril_set_fast_dormancy,
	.query_available_rats	= ril_query_available_rats
};

void ril_radio_settings_init(void)
{
	ofono_radio_settings_driver_register(&driver);
}

void ril_radio_settings_exit(void)
{
	ofono_radio_settings_driver_unregister(&driver);
}
