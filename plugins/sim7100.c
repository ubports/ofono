/*
 *
 *  oFono - Open Source Telephony
 *
 *  Copyright (C) 2008-2011  Intel Corporation. All rights reserved.
 *  Copyright (C) 2009  Collabora Ltd. All rights reserved.
 *  Copyright 2018 Purism SPC
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

/*
 * This file was originally copied from g1.c and
 * modified by Bob Ham <bob.ham@puri.sm>
 */

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <stdlib.h>
#include <errno.h>

#include <glib.h>
#include <gatchat.h>
#include <gattty.h>

#define OFONO_API_SUBJECT_TO_CHANGE
#include <ofono/plugin.h>
#include <ofono/log.h>
#include <ofono/modem.h>
#include <ofono/call-barring.h>
#include <ofono/call-forwarding.h>
#include <ofono/call-meter.h>
#include <ofono/call-settings.h>
#include <ofono/devinfo.h>
#include <ofono/message-waiting.h>
#include <ofono/netreg.h>
#include <ofono/phonebook.h>
#include <ofono/sim.h>
#include <ofono/sms.h>
#include <ofono/ussd.h>
#include <ofono/voicecall.h>
#include <ofono/gprs.h>
#include <ofono/gprs-context.h>

#include <drivers/atmodem/vendor.h>

struct sim7100_data {
	GAtChat *at;
	GAtChat *ppp;
};

static void sim7100_debug(const char *str, void *user_data)
{
	const char *prefix = user_data;

	ofono_info("%s%s", prefix, str);
}

/* Detect hardware, and initialize if found */
static int sim7100_probe(struct ofono_modem *modem)
{
	struct sim7100_data *data;

	DBG("");

	data = g_try_new0(struct sim7100_data, 1);
	if (data == NULL)
		return -ENOMEM;

	ofono_modem_set_data(modem, data);

	return 0;
}

static void sim7100_remove(struct ofono_modem *modem)
{
	struct sim7100_data *data = ofono_modem_get_data(modem);

	DBG("");

	if (!data)
		return;

	if (data->at)
		g_at_chat_unref(data->at);

	if (data->ppp)
		g_at_chat_unref(data->ppp);

	ofono_modem_set_data(modem, NULL);
	g_free (data);
}

static void cfun_set_on_cb(gboolean ok, GAtResult *result, gpointer user_data)
{
	struct ofono_modem *modem = user_data;

	DBG("");

	if (ok)
		ofono_modem_set_powered(modem, TRUE);
}

static int open_device(struct ofono_modem *modem, const char *devkey,
			GAtChat **chatp)
{
	GIOChannel *channel;
	GAtSyntax *syntax;
	GAtChat *chat;
	const char *device;

	DBG("devkey=%s", devkey);

	device = ofono_modem_get_string(modem, devkey);
	if (device == NULL)
		return -EINVAL;

	channel = g_at_tty_open(device, NULL);
	if (channel == NULL)
		return -EIO;

	syntax = g_at_syntax_new_gsm_permissive();
	chat = g_at_chat_new(channel, syntax);
	g_at_syntax_unref(syntax);
	g_io_channel_unref(channel);

	if (chat == NULL)
		return -EIO;

	if (getenv("OFONO_AT_DEBUG"))
		g_at_chat_set_debug(chat, sim7100_debug, "");

	*chatp = chat;
	return 0;
}

static int sim7100_enable(struct ofono_modem *modem)
{
	struct sim7100_data *data = ofono_modem_get_data(modem);
	int err;

	DBG("");

	err = open_device(modem, "AT", &data->at);
	if (err < 0)
		return err;

	err = open_device(modem, "PPP", &data->ppp);
	if (err < 0)
		return err;

	/* ensure modem is in a known state; verbose on, echo/quiet off */
	g_at_chat_send(data->at, "ATE0Q0V1", NULL, NULL, NULL, NULL);

	/* power up modem */
	g_at_chat_send(data->at, "AT+CFUN=1", NULL, cfun_set_on_cb,
								modem, NULL);

	return 0;
}

static void cfun_set_off_cb(gboolean ok, GAtResult *result, gpointer user_data)
{
	struct ofono_modem *modem = user_data;
	struct sim7100_data *data = ofono_modem_get_data(modem);

	DBG("");

	g_at_chat_unref(data->ppp);
	g_at_chat_unref(data->at);
	data->at = data->ppp = NULL;

	if (ok)
		ofono_modem_set_powered(modem, FALSE);
}

static int sim7100_disable(struct ofono_modem *modem)
{
	struct sim7100_data *data = ofono_modem_get_data(modem);

	DBG("");

	/* power down modem */
	g_at_chat_cancel_all(data->ppp);
	g_at_chat_cancel_all(data->at);
	g_at_chat_unregister_all(data->ppp);
	g_at_chat_unregister_all(data->at);
	g_at_chat_send(data->at, "AT+CFUN=0", NULL, cfun_set_off_cb,
								modem, NULL);

	return -EINPROGRESS;
}

static void sim7100_pre_sim(struct ofono_modem *modem)
{
	struct sim7100_data *data = ofono_modem_get_data(modem);
	struct ofono_sim *sim;

	DBG("");

	ofono_devinfo_create(modem, 0, "atmodem", data->at);
	sim = ofono_sim_create(modem, 0, "atmodem", data->at);
	ofono_voicecall_create(modem, OFONO_VENDOR_SIMCOM, "atmodem", data->at);

	if (sim)
		ofono_sim_inserted_notify(sim, TRUE);
}

static void sim7100_post_sim(struct ofono_modem *modem)
{
	struct sim7100_data *data = ofono_modem_get_data(modem);
	struct ofono_message_waiting *mw;
	struct ofono_gprs *gprs = NULL;
	struct ofono_gprs_context *gc = NULL;

	DBG("");

	ofono_ussd_create(modem, 0, "atmodem", data->at);
	ofono_call_forwarding_create(modem, 0, "atmodem", data->at);
	ofono_call_settings_create(modem, 0, "atmodem", data->at);
	ofono_netreg_create(modem, 0, "atmodem", data->at);
	ofono_call_meter_create(modem, 0, "atmodem", data->at);
	ofono_call_barring_create(modem, 0, "atmodem", data->at);
	ofono_sms_create(modem, OFONO_VENDOR_SIMCOM, "atmodem", data->at);
	ofono_phonebook_create(modem, 0, "atmodem", data->at);

	gprs = ofono_gprs_create(modem, 0, "atmodem", data->at);
	gc = ofono_gprs_context_create(modem, 0, "atmodem", data->ppp);

	if (gprs && gc)
		ofono_gprs_add_context(gprs, gc);

	mw = ofono_message_waiting_create(modem);
	if (mw)
		ofono_message_waiting_register(mw);
}

static struct ofono_modem_driver sim7100_driver = {
	.name		= "sim7100",
	.probe		= sim7100_probe,
	.remove		= sim7100_remove,
	.enable		= sim7100_enable,
	.disable	= sim7100_disable,
	.pre_sim	= sim7100_pre_sim,
	.post_sim	= sim7100_post_sim,
};

static int sim7100_init(void)
{
	return ofono_modem_driver_register(&sim7100_driver);
}

static void sim7100_exit(void)
{
	ofono_modem_driver_unregister(&sim7100_driver);
}

OFONO_PLUGIN_DEFINE(sim7100, "SIMCom SIM7100E modem driver", VERSION,
		OFONO_PLUGIN_PRIORITY_DEFAULT, sim7100_init, sim7100_exit)
