/*
 *  oFono - Open Source Telephony
 *
 *  Copyright (C) 2013 Jolla Ltd
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

#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <glib.h>
#include <ofono/log.h>
#include <ofono/modem.h>
#include <ofono/oemraw.h>
#include "common.h"
#include "gril.h"
#include "rilmodem.h"

struct oem_raw_data {
	GRil *ril;
	unsigned int vendor;
	guint timer_id;
};

static gboolean ril_oemraw_delayed_register(gpointer user_data)
{
	struct ofono_oem_raw *raw = user_data;
	struct oem_raw_data *od = ofono_oem_raw_get_data(raw);

	DBG("");

	od->timer_id = 0;

	ofono_oem_raw_dbus_register(raw);
	return FALSE; /* This makes the timeout a single-shot */
}

static int ril_oemraw_probe(struct ofono_oem_raw *raw, unsigned int vendor,
				void *data)
{
	GRil *ril = data;
	struct oem_raw_data *od;

	DBG("");

	od = g_new0(struct oem_raw_data, 1);

	od->ril = g_ril_clone(ril);
	od->vendor = vendor;
	ofono_oem_raw_set_data(raw, od);

	od->timer_id = g_timeout_add_seconds(1, ril_oemraw_delayed_register,
									raw);

	return 0;
}

static void ril_oemraw_remove(struct ofono_oem_raw *raw)
{
	struct oem_raw_data *od;

	DBG("");

	od = ofono_oem_raw_get_data(raw);

	ofono_oem_raw_set_data(raw, NULL);

	if (od->timer_id)
		g_source_remove(od->timer_id);

	g_ril_unref(od->ril);
	g_free(od);
}

static void ril_oemraw_request_cb(struct ril_msg *msg,
					 gpointer user_data)
{
	struct ofono_error error;
	struct ofono_oem_raw_results result;
	struct cb_data *cbd = user_data;
	ofono_oem_raw_query_cb_t cb = cbd->cb;

	if (msg && msg->error == RIL_E_SUCCESS) {
		decode_ril_error(&error, "OK");
	} else {
		DBG("error:%d len:%d unsol:%d req:%d serial_no:%d",
			msg->error, msg->buf_len, msg->unsolicited,
			msg->req, msg->serial_no);
		CALLBACK_WITH_FAILURE(cb, NULL, cbd->data);
		return;
	}

	result.data = msg->buf;
	result.length = msg->buf_len;

	cb(&error, &result, cbd->data);
}

static void ril_oemraw_request(struct ofono_oem_raw *raw,
			       const struct ofono_oem_raw_request *request,
			       ofono_oem_raw_query_cb_t cb, void *data)
{
	int ret;
	int i;
	struct cb_data *cbd;
	struct oem_raw_data *od;
	struct parcel parcel;

	cbd = cb_data_new(cb, data);
	od = ofono_oem_raw_get_data(raw);
	parcel_init(&parcel);

	for (i = 0; i < request->length; i++) {
		/*DBG("Byte: 0x%x", request->data[i]); Enable for debugging*/
		parcel_w_byte(&parcel, request->data[i]);
	}

	ret = g_ril_send(od->ril, RIL_REQUEST_OEM_HOOK_RAW, parcel.data,
				parcel.size, ril_oemraw_request_cb, cbd,
				g_free);

	parcel_free(&parcel);

	if (ret <= 0) {
		g_free(cbd);
		DBG("Failed to issue an OEM RAW request to RIL: result=%d ",
			ret);
		CALLBACK_WITH_FAILURE(cb, NULL, data);
	}
	return;
}

static struct ofono_oem_raw_driver driver = {
	.name				= "rilmodem",
	.probe				= ril_oemraw_probe,
	.remove				= ril_oemraw_remove,
	.request			= ril_oemraw_request,
};

void ril_oemraw_init(void)
{
	DBG("");
	ofono_oem_raw_driver_register(&driver);
}

void ril_oemraw_exit(void)
{
	DBG("");
	ofono_oem_raw_driver_unregister(&driver);
}
