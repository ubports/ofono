/*
 *
 *  oFono - Open Source Telephony
 *
 *  Copyright (C) 2008-2011  Intel Corporation. All rights reserved.
 *  Copyright (C) 2009  Collabora Ltd. All rights reserved.
 *  Copyright (C) 2020  Pavel Machek. All rights reserved.
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

#include <drivers/atmodem/atutil.h>
#include <drivers/atmodem/vendor.h>

static void droid_debug(const char *str, void *user_data)
{
	const char *prefix = user_data;

	ofono_info("%s%s", prefix, str);
}

/* Detect hardware, and initialize if found */
static int droid_probe(struct ofono_modem *modem)
{
	DBG("");

	return 0;
}

static void droid_remove(struct ofono_modem *modem)
{
	GAtChat *chat = ofono_modem_get_data(modem);

	DBG("");

	if (chat) {
		g_at_chat_unref(chat);
		ofono_modem_set_data(modem, NULL);
	}
}

static void cfun_set_on_cb(gboolean ok, GAtResult *result, gpointer user_data)
{
	struct ofono_modem *modem = user_data;

	DBG("");

	if (ok)
		ofono_modem_set_powered(modem, TRUE);
}

/* power up hardware */
static int droid_enable(struct ofono_modem *modem)
{
	GAtChat *chat;

	DBG("");

	chat = at_util_open_device(modem, "Device", droid_debug, "", NULL);
	ofono_modem_set_data(modem, chat);

	/* ensure modem is in a known state; verbose on, echo/quiet off */
	g_at_chat_send(chat, "ATE0Q0V1", NULL, NULL, NULL, NULL);

	/* power up modem */
	g_at_chat_send(chat, "AT+CFUN=1", NULL, cfun_set_on_cb, modem, NULL);

	return 0;
}

static void cfun_set_off_cb(gboolean ok, GAtResult *result, gpointer user_data)
{
	struct ofono_modem *modem = user_data;
	GAtChat *chat = ofono_modem_get_data(modem);

	DBG("");

	g_at_chat_unref(chat);
	ofono_modem_set_data(modem, NULL);

	if (ok)
		ofono_modem_set_powered(modem, FALSE);
}

static int droid_disable(struct ofono_modem *modem)
{
	GAtChat *chat = ofono_modem_get_data(modem);

	DBG("");

	/* power down modem */
	g_at_chat_cancel_all(chat);
	g_at_chat_unregister_all(chat);
	g_at_chat_send(chat, "AT+CFUN=0", NULL, cfun_set_off_cb, modem, NULL);

	return -EINPROGRESS;
}

static void droid_pre_sim(struct ofono_modem *modem)
{
	GAtChat *chat = ofono_modem_get_data(modem);
	struct ofono_sim *sim;

	DBG("");

	ofono_devinfo_create(modem, 0, "atmodem", chat);
	sim = ofono_sim_create(modem, OFONO_VENDOR_DROID, "atmodem", chat);
	ofono_voicecall_create(modem, OFONO_VENDOR_DROID, "atmodem", chat);

	if (sim)
		ofono_sim_inserted_notify(sim, TRUE);
}

static void droid_post_sim(struct ofono_modem *modem)
{
	GAtChat *chat = ofono_modem_get_data(modem);
	struct ofono_message_waiting *mw;

	DBG("");

	ofono_ussd_create(modem, 0, "atmodem", chat);
	ofono_call_forwarding_create(modem, 0, "atmodem", chat);
	ofono_call_settings_create(modem, 0, "atmodem", chat);
	ofono_netreg_create(modem, 0, "atmodem", chat);
	/*
	 * Droid 4 modem has problems with AT+CPUC?, avoid call meter for now.
	 */
	ofono_call_barring_create(modem, 0, "atmodem", chat);
	ofono_sms_create(modem, OFONO_VENDOR_DROID, "atmodem", chat);
	ofono_phonebook_create(modem, 0, "atmodem", chat);

	mw = ofono_message_waiting_create(modem);
	if (mw)
		ofono_message_waiting_register(mw);
}

static struct ofono_modem_driver droid_driver = {
	.name		= "droid",
	.probe		= droid_probe,
	.remove		= droid_remove,
	.enable		= droid_enable,
	.disable	= droid_disable,
	.pre_sim	= droid_pre_sim,
	.post_sim	= droid_post_sim,
};

static int droid_init(void)
{
	return ofono_modem_driver_register(&droid_driver);
}

static void droid_exit(void)
{
	ofono_modem_driver_unregister(&droid_driver);
}

/* Modem in Motorola Droid has few different interfaces:
 * -- gsmmux over serial -- using very non-standard commands
 * -- QMI -- unfortunately not usable without gsmmux
 * -- standard AT over ttyUSB4 -- unfortunately quite broken
 *
 * This driver is for the standard AT commands.
 */

OFONO_PLUGIN_DEFINE(droid, "Motorola Droid modem driver", VERSION,
			OFONO_PLUGIN_PRIORITY_DEFAULT, droid_init, droid_exit)
