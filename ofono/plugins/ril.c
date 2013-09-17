/*
 *
 *  oFono - Open Source Telephony - RIL-based devices
 *
 *  Copyright (C) 2008-2011  Intel Corporation. All rights reserved.
 *  Copyright (C) 2012 Canonical Ltd.
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
#include <glib.h>
#include <gril.h>
#include <parcel.h>

#define OFONO_API_SUBJECT_TO_CHANGE
#include <ofono/plugin.h>
#include <ofono/log.h>
#include <ofono/modem.h>
#include <ofono/devinfo.h>
#include <ofono/phonebook.h>
#include <ofono/netreg.h>
#include <ofono/voicecall.h>
#include <ofono/sms.h>
#include <ofono/cbs.h>
#include <ofono/sim.h>
#include <ofono/ussd.h>
#include <ofono/call-forwarding.h>
#include <ofono/call-settings.h>
#include <ofono/call-barring.h>
#include <ofono/call-meter.h>
#include <ofono/call-volume.h>
#include <ofono/radio-settings.h>
#include <ofono/gprs.h>
#include <ofono/gprs-context.h>
#include <ofono/audio-settings.h>
#include <ofono/types.h>

#include "drivers/rilmodem/rilmodem.h"

#define MAX_POWER_ON_RETRIES 5
#define MAX_SIM_STATUS_RETRIES 15

struct ril_data {
	GRil *modem;
	int power_on_retries;
	int sim_status_retries;
	ofono_bool_t connected;
	ofono_bool_t have_sim;
	ofono_bool_t online;
	ofono_bool_t reported;
};

static int send_get_sim_status(struct ofono_modem *modem);

static void ril_debug(const char *str, void *user_data)
{
	const char *prefix = user_data;

	ofono_info("%s%s", prefix, str);
}

static void sim_status_cb(struct ril_msg *message, gpointer user_data)
{
	struct ofono_modem *modem = user_data;
	struct ril_data *ril = ofono_modem_get_data(modem);
	struct sim_status status;
	struct sim_app *apps[MAX_UICC_APPS];

	DBG("");

	/*
	 * ril.h claims this should NEVER fail!
	 * However this isn't quite true.  So,
	 * on anything other than SUCCESS, we
	 * log an error, and schedule another
	 * GET_SIM_STATUS request.
	 */

	if (message->error != RIL_E_SUCCESS) {
		ril->sim_status_retries++;

		ofono_error("GET_SIM_STATUS reques failed: %d; retries: %d",
				message->error, ril->sim_status_retries);

		if (ril->sim_status_retries < MAX_SIM_STATUS_RETRIES)
			g_timeout_add_seconds(2, (GSourceFunc)
				send_get_sim_status, (gpointer) modem);
		else
			ofono_error("Max retries for GET_SIM_STATUS exceeded!");
	} else {

		/* Returns TRUE if cardstate == PRESENT */
		if (ril_util_parse_sim_status(ril->modem, message,
						&status, apps)) {
			DBG("have_sim = TRUE; num_apps: %d",
				status.num_apps);

			if (status.num_apps)
				ril_util_free_sim_apps(apps, status.num_apps);

			ril->have_sim = TRUE;
		} else {
			ofono_warn("No SIM card present.");
		}
		// We cannot power on modem, but we need to get
		// certain interfaces up to be able to make emergency calls
		// in offline mode and without SIM
		ofono_modem_set_powered(modem, TRUE);
	}
}

static int send_get_sim_status(struct ofono_modem *modem)
{
	struct ril_data *ril = ofono_modem_get_data(modem);
	int request = RIL_REQUEST_GET_SIM_STATUS;
	guint ret;

	ret = g_ril_send(ril->modem, request,
				NULL, 0, sim_status_cb, modem, NULL);

	g_ril_print_request_no_args(ril->modem, ret, request);

	return ret;
}

static int ril_probe(struct ofono_modem *modem)
{
	struct ril_data *ril = NULL;

	ril = g_try_new0(struct ril_data, 1);
	if (ril == NULL) {
		errno = ENOMEM;
		goto error;
	}

	ril->modem = NULL;

	ofono_modem_set_data(modem, ril);

	return 0;

error:
	g_free(ril);

	return -errno;
}

static void ril_remove(struct ofono_modem *modem)
{
	struct ril_data *ril = ofono_modem_get_data(modem);


	ofono_modem_set_data(modem, NULL);

	if (!ril)
		return;

	g_ril_unref(ril->modem);

	g_free(ril);
}

static void ril_pre_sim(struct ofono_modem *modem)
{
	DBG("enter");
	struct ril_data *ril = ofono_modem_get_data(modem);
	struct ofono_sim *sim;

	sim = ofono_sim_create(modem, 0, "rilmodem", ril->modem);
	ofono_voicecall_create(modem, 0, "rilmodem", ril->modem);

	if (sim && ril->have_sim)
		ofono_sim_inserted_notify(sim, TRUE);
}

static void ril_post_sim(struct ofono_modem *modem)
{
	struct ril_data *ril = ofono_modem_get_data(modem);
	struct ofono_gprs *gprs;
	struct ofono_gprs_context *gc;

	/* TODO: this function should setup:
	 *  - stk ( SIM toolkit )
	 */
	ofono_sms_create(modem, 0, "rilmodem", ril->modem);

	gprs = ofono_gprs_create(modem, 0, "rilmodem", ril->modem);
	gc = ofono_gprs_context_create(modem, 0, "rilmodem", ril->modem);

	if (gprs && gc) {
		DBG("calling gprs_add_context");
		ofono_gprs_add_context(gprs, gc);
	}

	ofono_radio_settings_create(modem, 0, "rilmodem", ril->modem);
	ofono_phonebook_create(modem, 0, "rilmodem", ril->modem);
	ofono_call_forwarding_create(modem, 0, "rilmodem", ril->modem);
}

static void ril_post_online(struct ofono_modem *modem)
{
	DBG("enter");
	struct ril_data *ril = ofono_modem_get_data(modem);

	ofono_call_volume_create(modem, 0, "rilmodem", ril->modem);

	ofono_netreg_create(modem, 0, "rilmodem", ril->modem);
	ofono_ussd_create(modem, 0, "rilmodem", ril->modem);
	ofono_call_settings_create(modem, 0, "rilmodem", ril->modem);
	ofono_cbs_create(modem, 0, "rilmodem", ril->modem);
}

static void ril_set_online_cb(struct ril_msg *message, gpointer user_data)
{
	DBG("enter");
	ofono_bool_t online_state;
	struct cb_data *cbd = user_data;
	ofono_modem_online_cb_t cb = cbd->cb;

	if (message->error == RIL_E_SUCCESS) {
		CALLBACK_WITH_SUCCESS(cb, cbd->data);
	}
	else
		CALLBACK_WITH_FAILURE(cb, cbd->data);
}

static void ril_set_online(struct ofono_modem *modem, ofono_bool_t online,
				ofono_modem_online_cb_t callback, void *data)
{
	DBG("Set online state (online = 1, offline = 0)): %i", online);
	struct ril_data *ril = ofono_modem_get_data(modem);
	struct cb_data *cbd = cb_data_new(callback, data);
	struct parcel rilp;
	int ret = 0;

	parcel_init(&rilp);
	parcel_w_int32(&rilp, 1);	/* Number of params */
	parcel_w_int32(&rilp, online);	/* Radio ON = 1, Radio OFF = 0 */
	DBG("1");
	ret = g_ril_send(ril->modem, RIL_REQUEST_RADIO_POWER, rilp.data,
				rilp.size, ril_set_online_cb, cbd, g_free);

	parcel_free(&rilp);
	DBG("2");
	if (ret <= 0) {
		g_free(cbd);
		CALLBACK_WITH_FAILURE(callback, data);
	} else {
		if (online)
			current_online_state = RIL_ONLINE_PREF;
		else
			current_online_state = RIL_OFFLINE;
	}
}

static void ril_connected(struct ril_msg *message, gpointer user_data)
{
	struct ofono_modem *modem = (struct ofono_modem *) user_data;
	struct ril_data *ril = ofono_modem_get_data(modem);

	/* TODO: make conditional */
	ofono_debug("[UNSOL]< %s", ril_unsol_request_to_string(message->req));
	/* TODO: make conditional */

	/* TODO: need a disconnect function to restart things! */
	ril->connected = TRUE;

	send_get_sim_status(modem);
}

static int ril_enable(struct ofono_modem *modem)
{
	DBG("enter");
	struct ril_data *ril = ofono_modem_get_data(modem);

	ril->have_sim = FALSE;

	ril->modem = g_ril_new();

	/* NOTE: Since AT modems open a tty, and then call
	 * g_at_chat_new(), they're able to return -EIO if
	 * the first fails, and -ENOMEM if the second fails.
	 * in our case, we already return -EIO if the ril_new
	 * fails.  If this is important, we can create a ril_socket
	 * abstraction... ( probaby not a bad idea ).
	 */

	if (ril->modem == NULL) {
		DBG("g_ril_new() failed to create modem!");
		return -EIO;
	}

	if (getenv("OFONO_RIL_TRACE"))
		g_ril_set_trace(ril->modem, TRUE);

	if (getenv("OFONO_RIL_HEX_TRACE"))
		g_ril_set_debugf(ril->modem, ril_debug, "Device: ");

	g_ril_register(ril->modem, RIL_UNSOL_RIL_CONNECTED,
			ril_connected, modem);

	ofono_devinfo_create(modem, 0, "rilmodem", ril->modem);

	return -EINPROGRESS;
}

static int ril_disable(struct ofono_modem *modem)
{
	struct ril_data *ril = ofono_modem_get_data(modem);
	struct parcel rilp;
	int request = RIL_REQUEST_RADIO_POWER;
	guint ret;

	DBG("%p", modem);

	parcel_init(&rilp);
	parcel_w_int32(&rilp, 1); /* size of array */
	parcel_w_int32(&rilp, 0); /* POWER=OFF */

	/* fire and forget i.e. not waiting for the callback*/
	ret = g_ril_send(ril->modem, request, rilp.data,
			 rilp.size, NULL, NULL, NULL);

	g_ril_append_print_buf(ril->modem, "(0)");
	g_ril_print_request(ril->modem, ret, request);

	parcel_free(&rilp);

	return 0;
}

static struct ofono_modem_driver ril_driver = {
	.name = "ril",
	.probe = ril_probe,
	.remove = ril_remove,
	.enable = ril_enable,
	.disable = ril_disable,
	.pre_sim = ril_pre_sim,
	.post_sim = ril_post_sim,
	.post_online = ril_post_online,
	.set_online = ril_set_online,
};

/*
 * Note - as an aal+ container doesn't include a running udev,
 * the udevng plugin will never detect a modem, and thus modem
 * creation for a RIL-based modem needs to be hard-coded.
 *
 * Typically, udevng would create the modem, which in turn would
 * lead to this plugin's probe function being called.
 *
 * This is a first attempt at registering like this.
 *
 * IMPORTANT - this code relies on the fact that the 'rilmodem' is
 * added to top-level Makefile's builtin_modules *after* 'ril'.
 * This has means 'rilmodem' will already be registered before we try
 * to create and register the modem.  In standard ofono, 'udev'/'udevng'
 * is initialized last due to the fact that it's the first module
 * added in the top-level Makefile.
 */
static int ril_init(void)
{
	DBG("enter");
	int retval = 0;
	struct ofono_modem *modem;

	if ((retval = ofono_modem_driver_register(&ril_driver))) {
		DBG("ofono_modem_driver_register returned: %d", retval);
	return retval;
	}

	/* everything after _modem_driver_register, is
	 * non-standard ( see udev comment above ).
	 * usually called by undevng::create_modem
	 *
	 * args are name (optional) & type
	 */
	modem = ofono_modem_create(NULL, "ril");
	if (modem == NULL) {
		DBG("ofono_modem_create failed for ril");
		return -ENODEV;
	}

	/* This causes driver->probe() to be called... */
	retval = ofono_modem_register(modem);
	DBG("ofono_modem_register returned: %d", retval);

	/* kickstart the modem:
	 * causes core modem code to call
	 * - set_powered(TRUE) - which in turn
	 *   calls driver->enable()
	 *
	 * - driver->pre_sim()
	 *
	 * Could also be done via:
	 *
	 * - a DBus call to SetProperties w/"Powered=TRUE" *1
	 * - sim_state_watch ( handles SIM removal? LOCKED states? **2
	 * - ofono_modem_set_powered()
	 */
	ofono_modem_reset(modem);

	return retval;
}

static void ril_exit(void)
{
	DBG("");
	if (current_passwd)
		g_free(current_passwd);
	ofono_modem_driver_unregister(&ril_driver);
}

OFONO_PLUGIN_DEFINE(ril, "RIL modem driver", VERSION,
			OFONO_PLUGIN_PRIORITY_DEFAULT, ril_init, ril_exit)

