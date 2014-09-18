/*
 *
 *  oFono - Open Source Telephony
 *
 *  Copyright (C) 2008-2011  Intel Corporation. All rights reserved.
 *  Copyright (C) 2013 Jolla Ltd
 *  Contact: Jussi Kangas <jussi.kangas@tieto.com>
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
#include <ofono/sim.h>

#include "gril.h"
#include "grilutil.h"
#include "storage.h"

#include "rilmodem.h"

#include "ril_constants.h"

struct radio_data {
	GRil *ril;
	guint timer_id;
	int ratmode;
};

static void ril_set_rat_cb(struct ril_msg *message, gpointer user_data)
{
	struct cb_data *cbd = user_data;
	ofono_radio_settings_rat_mode_set_cb_t cb = cbd->cb;

	if (message->error == RIL_E_SUCCESS)
		CALLBACK_WITH_SUCCESS(cb, cbd->data);
	else {
		ofono_error("rat mode setting failed");
		CALLBACK_WITH_FAILURE(cb, cbd->data);
	}
}

static void ril_set_rat_mode(struct ofono_radio_settings *rs,
				enum ofono_radio_access_mode mode,
				ofono_radio_settings_rat_mode_set_cb_t cb,
				void *data)
{
	struct radio_data *rd = ofono_radio_settings_get_data(rs);
	struct cb_data *cbd = cb_data_new(cb, data);
	struct parcel rilp;
	int pref = rd->ratmode;
	int ret = 0;

	ofono_info("rat mode set %d", mode);

	parcel_init(&rilp);

	parcel_w_int32(&rilp, 1);			/* Number of params */

	switch (mode) {
	case OFONO_RADIO_ACCESS_MODE_GSM:
		pref = PREF_NET_TYPE_GSM_ONLY;
		break;
	case OFONO_RADIO_ACCESS_MODE_UMTS:
		pref = PREF_NET_TYPE_GSM_WCDMA_AUTO; /* according to UI design */
		break;
	case OFONO_RADIO_ACCESS_MODE_LTE:
		pref = PREF_NET_TYPE_LTE_ONLY;
	default:
		break;
	}

	parcel_w_int32(&rilp, pref);

	ret = g_ril_send(rd->ril, RIL_REQUEST_SET_PREFERRED_NETWORK_TYPE,
			rilp.data, rilp.size, ril_set_rat_cb,
			cbd, g_free);

	parcel_free(&rilp);

	if (ret <= 0) {
		ofono_error("unable to set rat mode");
		g_free(cbd);
		CALLBACK_WITH_FAILURE(cb, data);
	}
}

static void ril_force_rat_mode(struct radio_data *rd, int pref)
{
	struct parcel rilp;

	if (pref == rd->ratmode)
		return;

	DBG("pref ril rat mode %d, ril current %d", pref, rd->ratmode);

	parcel_init(&rilp);
	parcel_w_int32(&rilp, 1);
	parcel_w_int32(&rilp, rd->ratmode);
	g_ril_send(rd->ril,
		RIL_REQUEST_SET_PREFERRED_NETWORK_TYPE,
		rilp.data, rilp.size, NULL,
		NULL, g_free);
	parcel_free(&rilp);
}

static void ril_rat_mode_cb(struct ril_msg *message, gpointer user_data)
{
	DBG("");
	struct cb_data *cbd = user_data;
	ofono_radio_settings_rat_mode_query_cb_t cb = cbd->cb;
	struct parcel rilp;
	int mode = OFONO_RADIO_ACCESS_MODE_ANY;
	int pref;

	if (message->error == RIL_E_SUCCESS) {
		ril_util_init_parcel(message, &rilp);
		/* first item in int[] is len so let's skip that */
		parcel_r_int32(&rilp);
		pref = parcel_r_int32(&rilp);

		switch (pref) {
		case PREF_NET_TYPE_LTE_ONLY:
			mode = OFONO_RADIO_ACCESS_MODE_LTE;
		case PREF_NET_TYPE_GSM_ONLY:
			mode = OFONO_RADIO_ACCESS_MODE_GSM;
			break;
		case PREF_NET_TYPE_GSM_WCDMA_AUTO:/* according to UI design */
			if (!cb)
				ril_force_rat_mode(cbd->user, pref);
		case PREF_NET_TYPE_WCDMA:
		case PREF_NET_TYPE_GSM_WCDMA: /* according to UI design */
			mode = OFONO_RADIO_ACCESS_MODE_UMTS;
			break;
		case PREF_NET_TYPE_LTE_CDMA_EVDO:
		case PREF_NET_TYPE_LTE_GSM_WCDMA:
		case PREF_NET_TYPE_LTE_CMDA_EVDO_GSM_WCDMA:
			if (!cb)
				ril_force_rat_mode(cbd->user, pref);
			break;
		case PREF_NET_TYPE_CDMA_EVDO_AUTO:
		case PREF_NET_TYPE_CDMA_ONLY:
		case PREF_NET_TYPE_EVDO_ONLY:
		case PREF_NET_TYPE_GSM_WCDMA_CDMA_EVDO_AUTO:
		default:
			break;
		}
		ofono_info("rat mode %d (ril %d)", mode, pref);
		if (cb)
			CALLBACK_WITH_SUCCESS(cb, mode, cbd->data);
	} else {
		if (cb)
			CALLBACK_WITH_FAILURE(cb, -1, cbd->data);
		ofono_error("rat mode query failed");
	}
}

static void ril_query_rat_mode(struct ofono_radio_settings *rs,
				ofono_radio_settings_rat_mode_query_cb_t cb,
				void *data){
	DBG("");
	struct radio_data *rd = ofono_radio_settings_get_data(rs);
	struct cb_data *cbd = cb_data_new(cb, data);
	int ret = 0;

	ofono_info("rat mode query");

	ret = g_ril_send(rd->ril, RIL_REQUEST_GET_PREFERRED_NETWORK_TYPE,
					 NULL, 0, ril_rat_mode_cb, cbd, g_free);

	/* In case of error free cbd and return the cb with failure */
	if (ret <= 0) {
		ofono_error("unable to send rat mode query");
		g_free(cbd);
		CALLBACK_WITH_FAILURE(cb, -1, data);
	}
}

static gboolean ril_get_net_config(struct radio_data *rsd)
{
	GKeyFile *keyfile;
	GError *err = NULL;
	char *config_path = RIL_CONFIG_DIR;
	char **alreadyset = NULL;
	gboolean needsconfig = FALSE;
	gboolean value = FALSE;
	gboolean found = FALSE;
	rsd->ratmode = PREF_NET_TYPE_GSM_WCDMA_AUTO;
	GDir *config_dir;
	const gchar *config_file;
	gsize length;
	gchar **codes = NULL;
	int i;

	/*
	 * First we need to check should the LTE be on
	 * or not
	 */

	keyfile = g_key_file_new();

	g_key_file_set_list_separator(keyfile, ',');

	config_dir = g_dir_open(config_path, 0, NULL);
	while ((config_file = g_dir_read_name(config_dir)) != NULL) {
		char *path = g_strconcat(RIL_CONFIG_DIR "/", config_file, NULL);
		DBG("Rilconfig handling %s", path);
		gboolean ok = g_key_file_load_from_file(keyfile, path, 0, &err);

		g_free(path);
		if (!ok) {
			g_error_free(err);
			DBG("Rilconfig file skipped");
			continue;
		}

		if (g_key_file_has_group(keyfile, LTE_FLAG))
			found = TRUE;
		else if (g_key_file_has_group(keyfile, MCC_LIST)) {
			codes = g_key_file_get_string_list(keyfile, MCC_LIST,
						MCC_KEY, &length, NULL);
			if (codes) {
				for (i = 0; codes[i]; i++) {
					if (g_str_equal(codes[i],
						ofono_sim_get_mcc(get_sim()))
							== TRUE) {
						found = TRUE;
						break;
					}
				}
				g_strfreev(codes);
			}
		}

		if (found) {
			rsd->ratmode = PREF_NET_TYPE_LTE_GSM_WCDMA;
			break;
		}
	}

	g_key_file_free(keyfile);
	g_dir_close(config_dir);

	/* Then we need to check if it already set */

	keyfile = storage_open(NULL, RIL_STORE);
	alreadyset = g_key_file_get_groups(keyfile, NULL);

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

static gboolean ril_delayed_register(gpointer user_data)
{
	struct ofono_radio_settings *rs = user_data;
	struct radio_data *rd = ofono_radio_settings_get_data(rs);

	rd->timer_id = 0;

	ofono_radio_settings_register(rs);
	return FALSE;
}

static int ril_radio_settings_probe(struct ofono_radio_settings *rs,
					unsigned int vendor,
					void *user)
{
	GRil *ril = user;
	struct cb_data *cbd = NULL;
	int ret;
	struct radio_data *rsd = g_try_new0(struct radio_data, 1);
	rsd->ril = g_ril_clone(ril);
	if (ril_get_net_config(rsd)) {
		cbd = cb_data_new2(rsd, NULL, NULL);
		ret = g_ril_send(rsd->ril,
					RIL_REQUEST_GET_PREFERRED_NETWORK_TYPE,
					NULL, 0, ril_rat_mode_cb, cbd, g_free);
		if (ret <= 0)
			g_free(cbd);
	}

	ofono_radio_settings_set_data(rs, rsd);
	rsd->timer_id = g_timeout_add_seconds(2, ril_delayed_register, rs);

	return 0;
}

static void ril_radio_settings_remove(struct ofono_radio_settings *rs)
{
	struct radio_data *rd = ofono_radio_settings_get_data(rs);
	ofono_radio_settings_set_data(rs, NULL);

	if (rd->timer_id > 0)
		g_source_remove(rd->timer_id);

	g_ril_unref(rd->ril);
	g_free(rd);
}

static struct ofono_radio_settings_driver driver = {
	.name				= "rilmodem",
	.probe				= ril_radio_settings_probe,
	.remove				= ril_radio_settings_remove,
	.query_rat_mode		= ril_query_rat_mode,
	.set_rat_mode		= ril_set_rat_mode,
};

void ril_radio_settings_init(void)
{
	ofono_radio_settings_driver_register(&driver);
}

void ril_radio_settings_exit(void)
{
	ofono_radio_settings_driver_unregister(&driver);
}
