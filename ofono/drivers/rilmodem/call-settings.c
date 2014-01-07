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
#include <ofono/call-settings.h>

#include "gril.h"
#include "grilutil.h"

#include "rilmodem.h"
#include "ril_constants.h"
#include "common.h"

struct settings_data {
	GRil *ril;
	guint timer_id;
};

static void ril_clip_cb(struct ril_msg *message, gpointer user_data)
{
	struct cb_data *cbd = user_data;
	ofono_call_settings_status_cb_t cb = cbd->cb;
	struct parcel rilp;
	int res = 0;

	if (message->error == RIL_E_SUCCESS) {
		ril_util_init_parcel(message, &rilp);

		/* data length of the response */
		res = parcel_r_int32(&rilp);

		if (res > 0)
			res = parcel_r_int32(&rilp);

		CALLBACK_WITH_SUCCESS(cb, res, cbd->data);
	} else
		CALLBACK_WITH_FAILURE(cb, -1, cbd->data);
}

static void ril_set_cb(struct ril_msg *message, gpointer user_data)
{
	struct cb_data *cbd = user_data;
	ofono_call_settings_set_cb_t cb = cbd->cb;

	if (message->error == RIL_E_SUCCESS)
		CALLBACK_WITH_SUCCESS(cb, cbd->data);
	else
		CALLBACK_WITH_FAILURE(cb, cbd->data);
}

static void ril_cw_set(struct ofono_call_settings *cs, int mode, int cls,
			ofono_call_settings_set_cb_t cb, void *data){
	struct settings_data *sd = ofono_call_settings_get_data(cs);
	struct cb_data *cbd = cb_data_new(cb, data);
	int ret = 0;
	struct parcel rilp;

	parcel_init(&rilp);

	parcel_w_int32(&rilp, 2);		/* Number of params */

	parcel_w_int32(&rilp, mode);	/* on/off */

	/* Modem seems to respond with error to all queries
	 * or settings made with bearer class
	 * BEARER_CLASS_DEFAULT. Design decision: If given
	 * class is BEARER_CLASS_DEFAULT let's map it to
	 * SERVICE_CLASS_VOICE effectively making it the
	 * default bearer. This in line with API which is
	 * contains only voice anyways.
	 */
	if (cls == BEARER_CLASS_DEFAULT)
		cls = BEARER_CLASS_VOICE;

	parcel_w_int32(&rilp, cls);		/* Service class */

	ret = g_ril_send(sd->ril, RIL_REQUEST_SET_CALL_WAITING,
			rilp.data, rilp.size, ril_set_cb, cbd, g_free);

	parcel_free(&rilp);

	/* In case of error free cbd and return the cb with failure */
	if (ret <= 0) {
		g_free(cbd);
		CALLBACK_WITH_FAILURE(cb, data);
	}
}

static void ril_cw_query_cb(struct ril_msg *message, gpointer user_data)
{
	struct cb_data *cbd = user_data;
	ofono_call_settings_status_cb_t cb = cbd->cb;
	struct parcel rilp;
	int res = 0;
	int sv = 0;

	if (message->error == RIL_E_SUCCESS) {
		ril_util_init_parcel(message, &rilp);

		/* first value in int[] is len so let's skip that */
		parcel_r_int32(&rilp);

		/* status of call waiting service, disabled is returned only if
		 * service is not active for any service class */
		res = parcel_r_int32(&rilp);
		DBG("CW enabled/disabled: %d", res);

		if (res > 0) {
			/* services for which call waiting is enabled, 27.007 7.12 */
			sv = parcel_r_int32(&rilp);
			DBG("CW enabled for: %d", sv);
		}

		CALLBACK_WITH_SUCCESS(cb, sv, cbd->data);
	} else
		CALLBACK_WITH_FAILURE(cb, -1, cbd->data);
}

static void ril_cw_query(struct ofono_call_settings *cs, int cls,
				ofono_call_settings_status_cb_t cb, void *data)
{
	struct settings_data *sd = ofono_call_settings_get_data(cs);
	struct cb_data *cbd = cb_data_new(cb, data);
	int ret = 0;
	struct parcel rilp;

	parcel_init(&rilp);

	parcel_w_int32(&rilp, 1);		/* Number of params */

	/*
	 * RILD expects service class to be 0 as certain carriers can reject the
	 * query with specific service class
	 */
	parcel_w_int32(&rilp, 0);

	ret = g_ril_send(sd->ril, RIL_REQUEST_QUERY_CALL_WAITING,
			rilp.data, rilp.size, ril_cw_query_cb, cbd, g_free);

	parcel_free(&rilp);

	/* In case of error free cbd and return the cb with failure */
	if (ret <= 0) {
		g_free(cbd);
		CALLBACK_WITH_FAILURE(cb, -1, data);
	}
}


static void ril_clip_query(struct ofono_call_settings *cs,
			ofono_call_settings_status_cb_t cb, void *data)
{
	struct settings_data *sd = ofono_call_settings_get_data(cs);
	struct cb_data *cbd = cb_data_new(cb, data);
	int ret = 0;

	ret = g_ril_send(sd->ril, RIL_REQUEST_QUERY_CLIP,
			NULL, 0, ril_clip_cb, cbd, g_free);

	/* In case of error free cbd and return the cb with failure */
	if (ret <= 0) {
		g_free(cbd);
		CALLBACK_WITH_FAILURE(cb, -1, data);
	}
}

static void ril_clir_cb(struct ril_msg *message, gpointer user_data)
{
	struct cb_data *cbd = user_data;
	ofono_call_settings_clir_cb_t cb = cbd->cb;
	struct parcel rilp;
	int override, network;


	if (message->error == RIL_E_SUCCESS) {
		ril_util_init_parcel(message, &rilp);
		/*first value in int[] is len so let's skip that*/
		parcel_r_int32(&rilp);
		/* Set HideCallerId property from network */
		override = parcel_r_int32(&rilp);
		/* CallingLineRestriction indicates the state of
		the CLIR supplementary service in the network */
		network = parcel_r_int32(&rilp);

		CALLBACK_WITH_SUCCESS(cb, override, network, cbd->data);
	} else
		CALLBACK_WITH_FAILURE(cb, -1, -1, cbd->data);
}

static void ril_clir_query(struct ofono_call_settings *cs,
			ofono_call_settings_clir_cb_t cb, void *data)
{
	struct settings_data *sd = ofono_call_settings_get_data(cs);
	struct cb_data *cbd = cb_data_new(cb, data);
	int ret = 0;

	ret = g_ril_send(sd->ril, RIL_REQUEST_GET_CLIR,
			NULL, 0, ril_clir_cb, cbd, g_free);

	if (ret <= 0) {
		g_free(cbd);
		CALLBACK_WITH_FAILURE(cb, -1, -1, data);
	}
}


static void ril_clir_set(struct ofono_call_settings *cs, int mode,
			ofono_call_settings_set_cb_t cb, void *data)
{
	struct settings_data *sd = ofono_call_settings_get_data(cs);
	struct cb_data *cbd = cb_data_new(cb, data);
	int ret = 0;
	struct parcel rilp;

	parcel_init(&rilp);

	parcel_w_int32(&rilp, 1); /* Number of params */

	parcel_w_int32(&rilp, mode); /* for outgoing calls */

	ret = g_ril_send(sd->ril, RIL_REQUEST_SET_CLIR,
			rilp.data, rilp.size, ril_set_cb, cbd, g_free);

	parcel_free(&rilp);

	if (ret <= 0) {
		g_free(cbd);
		CALLBACK_WITH_FAILURE(cb, data);
	}
}

static gboolean ril_delayed_register(gpointer user_data)
{
	struct ofono_call_settings *cs = user_data;
	struct settings_data *sd = ofono_call_settings_get_data(cs);

	sd->timer_id = 0;

	ofono_call_settings_register(cs);

	return FALSE;
}

static int ril_call_settings_probe(struct ofono_call_settings *cs,
					unsigned int vendor, void *user)
{
	GRil *ril = user;

	struct settings_data *sd = g_try_new0(struct settings_data, 1);

	sd->ril = g_ril_clone(ril);

	ofono_call_settings_set_data(cs, sd);

	sd->timer_id = g_timeout_add_seconds(2, ril_delayed_register, cs);

	return 0;
}

static void ril_call_settings_remove(struct ofono_call_settings *cs)
{
	struct settings_data *sd = ofono_call_settings_get_data(cs);
	ofono_call_settings_set_data(cs, NULL);

	if (sd->timer_id > 0)
		g_source_remove(sd->timer_id);

	g_ril_unref(sd->ril);
	g_free(sd);
}

static struct ofono_call_settings_driver driver = {
	.name			= "rilmodem",
	.probe			= ril_call_settings_probe,
	.remove			= ril_call_settings_remove,
	.clip_query		= ril_clip_query,
	.cw_query		= ril_cw_query,
	.cw_set			= ril_cw_set,
	.clir_query		= ril_clir_query,
	.clir_set		= ril_clir_set

	/*
	 * Not supported in RIL API
	 * .colp_query		= ril_colp_query,
	 * .colr_query		= ril_colr_query
	*/
};

void ril_call_settings_init(void)
{
	ofono_call_settings_driver_register(&driver);
}

void ril_call_settings_exit(void)
{
	ofono_call_settings_driver_unregister(&driver);
}
