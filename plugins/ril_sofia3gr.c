/*
 *
 *  oFono - Open Source Telephony - RIL-based devices
 *
 *  Copyright (C) 2008-2015  Intel Corporation. All rights reserved.
 *  Copyright (C) 2012-2014 Canonical Ltd.
 *  Copyright (C) 2013 Jolla Ltd.
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
#include <errno.h>
#include <stdlib.h>
#include <string.h>

#include <unistd.h>
#include <glib.h>

#define OFONO_API_SUBJECT_TO_CHANGE
#include <ofono/plugin.h>
#include <ofono/log.h>
#include <ofono/types.h>
#include <ofono/modem.h>
#include <ofono/devinfo.h>
#include <ofono/netreg.h>
#include <ofono/sms.h>
#include <ofono/sim.h>
#include <ofono/gprs.h>
#include <ofono/gprs-context.h>

#include <gril/gril.h>

#include "drivers/rilmodem/rilmodem.h"
#include "drivers/rilmodem/vendor.h"

struct ril_data {
	GRil *ril;
};

static int ril_send_power(GRil *ril, ofono_bool_t online,
				GRilResponseFunc func,
				gpointer user_data,
				GDestroyNotify destroy)
{
	struct parcel rilp;

	DBG("%d", online);

	parcel_init(&rilp);
	parcel_w_int32(&rilp, 1);
	parcel_w_int32(&rilp, online);

	g_ril_append_print_buf(ril, "(%d)", online);

	return g_ril_send(ril, RIL_REQUEST_RADIO_POWER, &rilp,
						func, user_data, destroy);
}

static void ril_debug(const char *str, void *user_data)
{
	const char *prefix = user_data;

	ofono_info("%s%s", prefix, str);
}

static void ril_radio_state_changed(struct ril_msg *message,
							gpointer user_data)
{
	struct ofono_modem *modem = user_data;
	struct ril_data *rd = ofono_modem_get_data(modem);
	struct parcel rilp;
	int radio_state;

	g_ril_init_parcel(message, &rilp);
	radio_state = parcel_r_int32(&rilp);

	if (rilp.malformed) {
		ofono_error("%s: malformed parcel received", __func__);
		ofono_modem_set_powered(modem, FALSE);
		return;
	}

	g_ril_append_print_buf(rd->ril, "(state: %s)",
				ril_radio_state_to_string(radio_state));
	g_ril_print_unsol(rd->ril, message);

	switch (radio_state) {
	case RADIO_STATE_ON:
		break;
	case RADIO_STATE_UNAVAILABLE:
		ofono_modem_set_powered(modem, FALSE);
		break;
	case RADIO_STATE_OFF:
		break;
	}
}

static int ril_probe(struct ofono_modem *modem)
{
	struct ril_data *rd;

	DBG("");

	rd = g_new0(struct ril_data, 1);
	ofono_modem_set_data(modem, rd);

	return 0;
}

static void ril_remove(struct ofono_modem *modem)
{
	struct ril_data *rd = ofono_modem_get_data(modem);

	ofono_modem_set_data(modem, NULL);

	g_ril_unref(rd->ril);
	g_free(rd);
}

static void ril_pre_sim(struct ofono_modem *modem)
{
	struct ril_data *rd = ofono_modem_get_data(modem);

	DBG("");

	ofono_devinfo_create(modem, 0, "rilmodem", rd->ril);
	ofono_sim_create(modem, 0, "rilmodem", rd->ril);
}

static void ril_post_sim(struct ofono_modem *modem)
{
	struct ril_data *rd = ofono_modem_get_data(modem);
	struct ofono_gprs *gprs;
	struct ofono_gprs_context *gc;

	ofono_sms_create(modem, 0, "rilmodem", rd->ril);

	gprs = ofono_gprs_create(modem, 0, "rilmodem", rd->ril);
	gc = ofono_gprs_context_create(modem, 0, "rilmodem", rd->ril);

	if (gc) {
		ofono_gprs_context_set_type(gc,
					OFONO_GPRS_CONTEXT_TYPE_INTERNET);
		ofono_gprs_add_context(gprs, gc);
	}
}

static void ril_post_online(struct ofono_modem *modem)
{
	struct ril_data *rd = ofono_modem_get_data(modem);

	ofono_netreg_create(modem, 0, "rilmodem", rd->ril);
}

static void ril_set_online_cb(struct ril_msg *message, gpointer user_data)
{
	struct cb_data *cbd = user_data;
	ofono_modem_online_cb_t cb = cbd->cb;

	DBG("%d", message->error);

	if (message->error == RIL_E_SUCCESS)
		CALLBACK_WITH_SUCCESS(cb, cbd->data);
	else
		CALLBACK_WITH_FAILURE(cb, cbd->data);
}

static void ril_set_online(struct ofono_modem *modem, ofono_bool_t online,
			ofono_modem_online_cb_t cb, void *data)
{
	struct ril_data *rd = ofono_modem_get_data(modem);
	struct cb_data *cbd = cb_data_new(cb, data, rd);

	if (ril_send_power(rd->ril, online, ril_set_online_cb, cbd, g_free) > 0)
		return;

	CALLBACK_WITH_FAILURE(cb, cbd->data);
	g_free(cbd);
}

static void ril_init_power(struct ril_msg *message, gpointer user_data)
{
	struct ofono_modem *modem = user_data;
	ofono_bool_t powered;

	DBG("%d", message->error);

	powered = message->error != RIL_E_SUCCESS ? FALSE : TRUE;
	ofono_modem_set_powered(modem, powered);
}

static void ril_connected(struct ril_msg *message, gpointer user_data)
{
	struct ofono_modem *modem = user_data;
	struct ril_data *rd = ofono_modem_get_data(modem);

	DBG("");

	/* Set Modem Offline */
	if (ril_send_power(rd->ril, FALSE, ril_init_power, modem, NULL) > 0)
		return;

	ofono_modem_set_powered(modem, FALSE);
}

static int ril_enable(struct ofono_modem *modem)
{
	struct ril_data *rd = ofono_modem_get_data(modem);

	DBG("");

	rd->ril = g_ril_new("/tmp/rild", OFONO_RIL_VENDOR_AOSP);
	if (rd->ril == NULL) {
		ofono_error("g_ril_new() failed to create modem!");
		return -EIO;
	}

	if (getenv("OFONO_RIL_TRACE"))
		g_ril_set_trace(rd->ril, TRUE);

	if (getenv("OFONO_RIL_HEX_TRACE"))
		g_ril_set_debugf(rd->ril, ril_debug, "Sofia3GR:");

	g_ril_register(rd->ril, RIL_UNSOL_RIL_CONNECTED,
						ril_connected, modem);

	g_ril_register(rd->ril, RIL_UNSOL_RESPONSE_RADIO_STATE_CHANGED,
					ril_radio_state_changed, modem);

	return -EINPROGRESS;
}

static int ril_disable(struct ofono_modem *modem)
{
	struct ril_data *rd = ofono_modem_get_data(modem);

	DBG("%p", modem);
	ril_send_power(rd->ril, FALSE, NULL, NULL, NULL);

	return 0;
}

static struct ofono_modem_driver ril_driver = {
	.name = "ril_sofia3gr",
	.probe = ril_probe,
	.remove = ril_remove,
	.enable = ril_enable,
	.disable = ril_disable,
	.pre_sim = ril_pre_sim,
	.post_sim = ril_post_sim,
	.post_online = ril_post_online,
	.set_online = ril_set_online,
};

static int ril_init(void)
{
	return ofono_modem_driver_register(&ril_driver);
}

static void ril_exit(void)
{
	ofono_modem_driver_unregister(&ril_driver);
}

OFONO_PLUGIN_DEFINE(ril_sofia3gr, "SoFiA 3GR RIL-based modem driver", VERSION,
			OFONO_PLUGIN_PRIORITY_DEFAULT, ril_init, ril_exit)
