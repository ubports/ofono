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

struct settings_data {
	GRil *ril;
};

static void ril_clip_cb(struct ril_msg *message, gpointer user_data)
{
	struct cb_data *cbd = user_data;
	ofono_call_settings_status_cb_t cb = cbd->cb;
	struct parcel rilp;
	int res = 0;

	if (message->error == RIL_E_SUCCESS) {
		ril_util_init_parcel(message, &rilp);

		res = parcel_r_int32(&rilp);

		if (res > 0)
			res = parcel_r_int32(&rilp);

		CALLBACK_WITH_SUCCESS(cb, res, cbd->data);
	} else {
		CALLBACK_WITH_FAILURE(cb, -1, cbd->data);
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

static gboolean ril_delayed_register(gpointer user_data)
{
	struct ofono_call_settings *cs = user_data;

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

	g_timeout_add_seconds(2, ril_delayed_register, cs);

	return 0;
}

static void ril_call_settings_remove(struct ofono_call_settings *cs)
{
	struct settings_data *sd = ofono_call_settings_get_data(cs);
	ofono_call_settings_set_data(cs, NULL);
	g_ril_unref(sd->ril);
	g_free(sd);
}

static struct ofono_call_settings_driver driver = {
	.name			= "rilmodem",
	.probe			= ril_call_settings_probe,
	.remove			= ril_call_settings_remove,
	.clip_query		= ril_clip_query
};

void ril_call_settings_init(void)
{
	ofono_call_settings_driver_register(&driver);
}

void ril_call_settings_exit(void)
{
	ofono_call_settings_driver_unregister(&driver);
}
