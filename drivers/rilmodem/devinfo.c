/*
 *
 *  oFono - Open Source Telephony - RIL Modem Support
 *
 *  Copyright (C) 2008-2011  Intel Corporation. All rights reserved.
 *  Copyright (C) 2012-2013 Canonical Ltd.
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
#include <string.h>
#include <errno.h>

#include <glib.h>

#include <ofono/log.h>
#include <ofono/modem.h>
#include <ofono/devinfo.h>

#include "gril.h"

#include "rilmodem.h"

static void ril_query_manufacturer(struct ofono_devinfo *info,
					ofono_devinfo_query_cb_t cb,
					void *data)
{
	/* TODO: Implement properly */
	CALLBACK_WITH_SUCCESS(cb, "Fake Modem Manufacturer", data);
}

static void ril_query_model(struct ofono_devinfo *info,
				ofono_devinfo_query_cb_t cb,
				void *data)
{
	/* TODO: Implement properly */
	CALLBACK_WITH_SUCCESS(cb, "Fake Modem Model", data);
}

static void query_revision_cb(struct ril_msg *message, gpointer user_data)
{
	struct cb_data *cbd = user_data;
	ofono_devinfo_query_cb_t cb = cbd->cb;
	GRil *ril = cbd->user;
	struct parcel rilp;
	char *revision;

	if (message->error != RIL_E_SUCCESS)
		goto error;

	g_ril_init_parcel(message, &rilp);
	revision = parcel_r_string(&rilp);

	g_ril_append_print_buf(ril, "{%s}", revision);
	g_ril_print_response(ril, message);

	CALLBACK_WITH_SUCCESS(cb, revision, cbd->data);
	g_free(revision);
	return;

error:
	CALLBACK_WITH_FAILURE(cb, NULL, cbd->data);
}

static void ril_query_revision(struct ofono_devinfo *info,
				ofono_devinfo_query_cb_t cb,
				void *data)
{
	GRil *ril = ofono_devinfo_get_data(info);
	struct cb_data *cbd = cb_data_new(cb, data, ril);

	if (g_ril_send(ril, RIL_REQUEST_BASEBAND_VERSION, NULL,
			query_revision_cb, cbd, g_free) > 0)
		return;

	g_free(cbd);
	CALLBACK_WITH_FAILURE(cb, NULL, data);
}

static void query_svn_cb(struct ril_msg *message, gpointer user_data)
{
	struct cb_data *cbd = user_data;
	ofono_devinfo_query_cb_t cb = cbd->cb;
	GRil *ril = cbd->user;
	struct parcel rilp;
	char *imeisv;

	if (message->error != RIL_E_SUCCESS)
		goto error;

	g_ril_init_parcel(message, &rilp);

	imeisv = parcel_r_string(&rilp);

	g_ril_append_print_buf(ril, "{%s}", imeisv);
	g_ril_print_response(ril, message);

	CALLBACK_WITH_SUCCESS(cb, imeisv, cbd->data);
	g_free(imeisv);
	return;

error:
	CALLBACK_WITH_FAILURE(cb, NULL, cbd->data);
}

static void ril_query_svn(struct ofono_devinfo *info,
				ofono_devinfo_query_cb_t cb,
				void *data)
{
	GRil *ril = ofono_devinfo_get_data(info);
	struct cb_data *cbd = cb_data_new(cb, data, ril);

	if (g_ril_send(ril, RIL_REQUEST_GET_IMEISV, NULL,
			query_svn_cb, cbd, g_free) > 0)
		return;

	g_free(cbd);
	CALLBACK_WITH_FAILURE(cb, NULL, data);
}

static void query_serial_cb(struct ril_msg *message, gpointer user_data)
{
	struct cb_data *cbd = user_data;
	ofono_devinfo_query_cb_t cb = cbd->cb;
	GRil *ril = cbd->user;
	struct parcel rilp;
	char *imei;

	if (message->error != RIL_E_SUCCESS)
		goto error;

	g_ril_init_parcel(message, &rilp);
	imei = parcel_r_string(&rilp);

	g_ril_append_print_buf(ril, "{%s}", imei);
	g_ril_print_response(ril, message);

	CALLBACK_WITH_SUCCESS(cb, imei, cbd->data);
	g_free(imei);
	return;

error:
	CALLBACK_WITH_FAILURE(cb, NULL, cbd->data);
}

static void ril_query_serial(struct ofono_devinfo *info,
				ofono_devinfo_query_cb_t cb,
				void *data)
{
	GRil *ril = ofono_devinfo_get_data(info);
	struct cb_data *cbd = cb_data_new(cb, data, ril);

	/*
	 * TODO: make it support both RIL_REQUEST_GET_IMEI (deprecated) and
	 * RIL_REQUEST_DEVICE_IDENTITY depending on the rild version used
	 */
	if (g_ril_send(ril, RIL_REQUEST_GET_IMEI, NULL,
			query_serial_cb, cbd, g_free) > 0)
		return;

	g_free(cbd);
	CALLBACK_WITH_FAILURE(cb, NULL, data);
}

static gboolean ril_delayed_register(gpointer user_data)
{
	struct ofono_devinfo *info = user_data;

	DBG("");

	ofono_devinfo_register(info);

	return FALSE;
}

static int ril_devinfo_probe(struct ofono_devinfo *info, unsigned int vendor,
				void *data)
{
	GRil *ril = g_ril_clone(data);

	ofono_devinfo_set_data(info, ril);
	g_idle_add(ril_delayed_register, info);

	return 0;
}

static void ril_devinfo_remove(struct ofono_devinfo *info)
{
	GRil *ril = ofono_devinfo_get_data(info);

	ofono_devinfo_set_data(info, NULL);

	g_ril_unref(ril);
}

static const struct ofono_devinfo_driver driver = {
	.name			= RILMODEM,
	.probe			= ril_devinfo_probe,
	.remove			= ril_devinfo_remove,
	.query_manufacturer	= ril_query_manufacturer,
	.query_model		= ril_query_model,
	.query_revision		= ril_query_revision,
	.query_serial		= ril_query_serial,
	.query_svn		= ril_query_svn
};

void ril_devinfo_init(void)
{
	ofono_devinfo_driver_register(&driver);
}

void ril_devinfo_exit(void)
{
	ofono_devinfo_driver_unregister(&driver);
}
