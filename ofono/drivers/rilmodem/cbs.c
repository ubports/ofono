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
#include <string.h>
#include <stdio.h>
#include <ctype.h>

#include <glib.h>

#include <ofono/log.h>
#include <ofono/modem.h>
#include <ofono/cbs.h>
#include "util.h"

#include <gril.h>
#include <parcel.h>

#include "rilmodem.h"
#include "vendor.h"

struct cbs_data {
	GRil *ril;
	unsigned int vendor;
};

static void ril_cbs_set_cb(struct ril_msg *message, gpointer user_data)
{
	struct cb_data *cbd = user_data;
	ofono_cbs_set_cb_t cb = cbd->cb;
	struct cbs_data *cd = cbd->user;

	if (message->error == RIL_E_SUCCESS) {
		CALLBACK_WITH_SUCCESS(cb, cbd->data);
	} else {
		ofono_error("%s RILD reply failure: %s",
			g_ril_request_id_to_string(cd->ril, message->req),
			ril_error_to_string(message->error));
		CALLBACK_WITH_FAILURE(cb, cbd->data);
	}
}

static void ril_cbs_set_topics(struct ofono_cbs *cbs, const char *topics,
					ofono_cbs_set_cb_t cb, void *user_data)
{
	struct cbs_data *cd = ofono_cbs_get_data(cbs);
	struct cb_data *cbd = cb_data_new(cb, user_data, cd);
	int i = 0, from, to;
	const char *p, *pto;
	char **segments;
	struct parcel rilp;

	segments = g_strsplit(topics, ",", 0);

	while (segments[i])
		i++;

	parcel_init(&rilp);
	parcel_w_int32(&rilp, i);

	i = 0;
	while (segments[i]) {
		p = segments[i++];
		from = atoi(p);
		to = from;

		pto = strchr(p, '-');
		if (pto)
			to = atoi(pto + 1);

		parcel_w_int32(&rilp, from);
		parcel_w_int32(&rilp, to);

		parcel_w_int32(&rilp, 0);
		parcel_w_int32(&rilp, 0xFF);

		parcel_w_int32(&rilp, 1);
	}

	g_strfreev(segments);

	if (g_ril_send(cd->ril, RIL_REQUEST_GSM_SET_BROADCAST_SMS_CONFIG, &rilp,
			ril_cbs_set_cb, cbd, g_free) > 0)
		return;

	g_free(cbd);
	CALLBACK_WITH_FAILURE(cb, user_data);
}

static void ril_cbs_clear_topics(struct ofono_cbs *cbs,
					ofono_cbs_set_cb_t cb, void *user_data)
{
	ril_cbs_set_topics(cbs, "", cb, user_data);
}

static void ril_cbs_received(struct ril_msg *message, gpointer user_data)
{
	struct ofono_cbs *cbs = user_data;
	struct cbs_data *cd = ofono_cbs_get_data(cbs);
	struct parcel rilp;
	int pdulen;
	unsigned char *pdu;

	g_ril_print_unsol_no_args(cd->ril, message);

	DBG("req: %d; data_len: %d", message->req, (int) message->buf_len);

	g_ril_init_parcel(message, &rilp);
	pdu = parcel_r_raw(&rilp, &pdulen);

	if (!pdu || pdulen != 88) {
		ofono_error("%s: it isn't a gsm cell broadcast msg", __func__);
		return;
	}

	ofono_cbs_notify(cbs, pdu, pdulen);
	g_free(pdu);
}

static void ril_cbs_register(const struct ofono_error *error, void *data)
{
	struct ofono_cbs *cbs = data;
	struct cbs_data *cd = ofono_cbs_get_data(cbs);

	g_ril_register(cd->ril, RIL_UNSOL_RESPONSE_NEW_BROADCAST_SMS,
					ril_cbs_received, cbs);

	ofono_cbs_register(cbs);
}

static void get_cbs_config_cb(struct ril_msg *message,
					gpointer user_data)
{
	struct ofono_cbs *cbs = user_data;

	if (message->error != RIL_E_SUCCESS) {
		ofono_error("%s: RIL error %s", __func__,
				ril_error_to_string(message->error));
		ofono_cbs_remove(cbs);
		return;
	}

	ril_cbs_clear_topics(cbs, ril_cbs_register, cbs);
}

static int ril_cbs_probe(struct ofono_cbs *cbs, unsigned int vendor,
				void *user)
{
	GRil *ril = user;
	struct cbs_data *data;

	data = g_new0(struct cbs_data, 1);
	data->ril = g_ril_clone(ril);
	data->vendor = vendor;

	ofono_cbs_set_data(cbs, data);

	if (g_ril_send(ril, RIL_REQUEST_GSM_GET_BROADCAST_SMS_CONFIG, NULL,
			get_cbs_config_cb, cbs, NULL) == 0)
		ofono_error("%s: send failed", __func__);

	return 0;
}

static void ril_cbs_remove(struct ofono_cbs *cbs)
{
	struct cbs_data *data = ofono_cbs_get_data(cbs);

	ofono_cbs_set_data(cbs, NULL);

	g_ril_unref(data->ril);
	g_free(data);
}

static struct ofono_cbs_driver driver = {
	.name = RILMODEM,
	.probe = ril_cbs_probe,
	.remove = ril_cbs_remove,
	.set_topics = ril_cbs_set_topics,
	.clear_topics = ril_cbs_clear_topics,
};

void ril_cbs_init(void)
{
	ofono_cbs_driver_register(&driver);
}

void ril_cbs_exit(void)
{
	ofono_cbs_driver_unregister(&driver);
}
