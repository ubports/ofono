/*
 *
 *  oFono - Open Source Telephony
 *
 *  Copyright (C) 2008-2011  Intel Corporation. All rights reserved.
 *  Copyright (C) 2013  Jolla Ltd. All rights reserved.
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

#include <string.h>
#include <stdio.h>
#include <glib.h>
#include <gdbus.h>

#define OFONO_API_SUBJECT_TO_CHANGE
#include <ofono/plugin.h>
#include <ofono/log.h>
#include <ofono/history.h>
#include <ofono/types.h>

#include "ofono.h"
#include "common.h"

#define SMS_HISTORY_INTERFACE "org.ofono.SmsHistory"

gboolean sms_history_interface_registered = FALSE;

static const GDBusSignalTable sms_history_signals[] = {
	{ GDBUS_SIGNAL("StatusReport",
		GDBUS_ARGS({ "message", "s" }, { "Delivered", "a{b}" })) },
	{ }
};

static void sms_history_cleanup(gpointer user)
{
	struct ofono_modem *modem = user;
	DBG("modem %p", modem);
	ofono_modem_remove_interface(modem, SMS_HISTORY_INTERFACE);
	sms_history_interface_registered = FALSE;
}

static gboolean sms_history_ensure_interface(
		struct ofono_modem *modem) {

	if (sms_history_interface_registered)
		return TRUE;

	/* Late initialization of the D-Bus interface */
	DBusConnection *conn = ofono_dbus_get_connection();
	if (conn == NULL)
		return FALSE;
	if (!g_dbus_register_interface(conn,
					ofono_modem_get_path(modem),
					SMS_HISTORY_INTERFACE,
					NULL, sms_history_signals, NULL,
					modem, sms_history_cleanup)) {
		ofono_error("Could not create %s interface",
				SMS_HISTORY_INTERFACE);
		return FALSE;
	}
	sms_history_interface_registered = TRUE;
	ofono_modem_add_interface(modem, SMS_HISTORY_INTERFACE);

	return TRUE;
}


static int sms_history_probe(struct ofono_history_context *context)
{
	ofono_debug("SMS History Probe for modem: %p", context->modem);
	sms_history_ensure_interface(context->modem);
	return 0;
}

static void sms_history_remove(struct ofono_history_context *context)
{
	ofono_debug("SMS History Remove for modem: %p", context->modem);
}

static void sms_history_sms_send_status(
					struct ofono_history_context *context,
					const struct ofono_uuid *uuid,
					time_t when,
					enum ofono_history_sms_status s)
{
	DBG("");

	if (!sms_history_ensure_interface(context->modem))
		return;

	if ((s == OFONO_HISTORY_SMS_STATUS_DELIVERED) 
			|| (s == OFONO_HISTORY_SMS_STATUS_DELIVER_FAILED)) {

		struct ofono_atom *atom = __ofono_modem_find_atom(
			context->modem, OFONO_ATOM_TYPE_SMS);
		if (atom == NULL)
			return;

		const char *path = __ofono_atom_get_path(atom);
		if (path == NULL)
			return;

		DBusConnection *conn = ofono_dbus_get_connection();
		if (conn == NULL)
			return;

		DBusMessage *signal;
		DBusMessageIter iter;
		DBusMessageIter dict;
		char msg_uuid_str[160]; /* modem path + '/message_' + UUID as string */
		const char *msg_uuid_ptr;

		int delivered = (s == OFONO_HISTORY_SMS_STATUS_DELIVERED);
		snprintf(msg_uuid_str, sizeof(msg_uuid_str), "%s%s%s", path, 
			"/message_", ofono_uuid_to_str(uuid));
		DBG("SMS %s delivery success: %d", msg_uuid_str, delivered);

		signal = dbus_message_new_signal(path, SMS_HISTORY_INTERFACE,
			"StatusReport");
		if (signal == NULL)
			return;

		dbus_message_iter_init_append(signal, &iter);
		msg_uuid_ptr = (char *)&msg_uuid_str;
		dbus_message_iter_append_basic(&iter, DBUS_TYPE_STRING,
			&msg_uuid_ptr);

		dbus_message_iter_open_container(&iter, DBUS_TYPE_ARRAY,
			OFONO_PROPERTIES_ARRAY_SIGNATURE, &dict);
		ofono_dbus_dict_append(&dict, "Delivered", DBUS_TYPE_BOOLEAN,
			&delivered);
		dbus_message_iter_close_container(&iter, &dict);

		g_dbus_send_message(conn, signal);
	}
}

static struct ofono_history_driver smshistory_driver = {
	.name = "SMS History",
	.probe = sms_history_probe,
	.remove = sms_history_remove,
	.sms_send_status = sms_history_sms_send_status,
};

static int sms_history_init(void)
{
	DBG("");
	return ofono_history_driver_register(&smshistory_driver);
}

static void sms_history_exit(void)
{
	DBG("");
	ofono_history_driver_unregister(&smshistory_driver);
}

OFONO_PLUGIN_DEFINE(sms_history, "SMS History Plugin",
			VERSION, OFONO_PLUGIN_PRIORITY_DEFAULT,
			sms_history_init, sms_history_exit)

