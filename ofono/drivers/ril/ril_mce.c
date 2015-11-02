/*
 *  oFono - Open Source Telephony - RIL-based devices
 *
 *  Copyright (C) 2015 Jolla Ltd.
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License version 2 as
 *  published by the Free Software Foundation.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 */

#include "ril_mce.h"
#include "ril_constants.h"

#include <grilio_channel.h>
#include <grilio_request.h>

#include <mce/dbus-names.h>
#include <mce/mode-names.h>

#include <ofono/log.h>
#include <ofono/dbus.h>

#include <gdbus.h>

struct ril_mce {
	GRilIoChannel *io;
	DBusConnection *conn;
	int screen_state;
	guint daemon_watch;
	guint signal_watch;
};

static void ril_mce_send_screen_state(struct ril_mce *mce, gboolean on)
{
	GRilIoRequest *req = grilio_request_sized_new(8);

	grilio_request_append_int32(req, 1);    /* Number of params */
	grilio_request_append_int32(req, on);   /* screen on/off */

	grilio_channel_send_request(mce->io, req, RIL_REQUEST_SCREEN_STATE);
	grilio_request_unref(req);
}

static gboolean ril_mce_display_changed(DBusConnection *conn,
				DBusMessage *message, void *user_data)
{
	DBusMessageIter iter;

	if (dbus_message_iter_init(message, &iter) &&
		dbus_message_iter_get_arg_type(&iter) == DBUS_TYPE_STRING) {
		struct ril_mce *mce = user_data;
		const char *value = NULL;
		int state;

		dbus_message_iter_get_basic(&iter, &value);
		DBG(" %s", value);

		/* It is on if it's not off */
		state = (g_strcmp0(value, MCE_DISPLAY_OFF_STRING) != 0);
		if (mce->screen_state != state) {
			mce->screen_state = state;
			ril_mce_send_screen_state(mce, state);
		}
	} else {
		DBG("");
	}

	return TRUE;
}

static void ril_mce_connect(DBusConnection *conn, void *user_data)
{
	struct ril_mce *mce = user_data;

	DBG("");
	if (!mce->signal_watch) {
		mce->signal_watch = g_dbus_add_signal_watch(conn,
			MCE_SERVICE, NULL, MCE_SIGNAL_IF, MCE_DISPLAY_SIG,
			ril_mce_display_changed, mce, NULL);
	}
}

static void ril_mce_disconnect(DBusConnection *conn, void *user_data)
{
	struct ril_mce *mce = user_data;

	DBG("");
	if (mce->signal_watch) {
		g_dbus_remove_watch(conn, mce->signal_watch);
		mce->signal_watch = 0;
	}
}

struct ril_mce *ril_mce_new(GRilIoChannel *io)
{
	struct ril_mce *mce = g_new0(struct ril_mce, 1);

	mce->conn = dbus_connection_ref(ofono_dbus_get_connection());
	mce->io = grilio_channel_ref(io);
	mce->screen_state = -1;
	mce->daemon_watch = g_dbus_add_service_watch(mce->conn, MCE_SERVICE,
		ril_mce_connect, ril_mce_disconnect, mce, NULL);

	return mce;
}

void ril_mce_free(struct ril_mce *mce)
{
	if (mce) {
		if (mce->signal_watch) {
			g_dbus_remove_watch(mce->conn, mce->signal_watch);
		}
		if (mce->daemon_watch) {
			g_dbus_remove_watch(mce->conn, mce->daemon_watch);
		}
		dbus_connection_unref(mce->conn);
		grilio_channel_unref(mce->io);
		g_free(mce);
	}
}

/*
 * Local Variables:
 * mode: C
 * c-basic-offset: 8
 * indent-tabs-mode: t
 * End:
 */
