/*
 *  oFono - Open Source Telephony - RIL-based devices
 *
 *  Copyright (C) 2015-2016 Jolla Ltd.
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
#include "ril_log.h"

#include <ofono/dbus.h>

#include <gdbus.h>

/* <mce/dbus-names.h> */
#define MCE_SERVICE             "com.nokia.mce"
#define MCE_SIGNAL_IF           "com.nokia.mce.signal"
#define MCE_REQUEST_IF          "com.nokia.mce.request"
#define MCE_REQUEST_PATH        "/com/nokia/mce/request"
#define MCE_DISPLAY_STATUS_GET  "get_display_status"
#define MCE_DISPLAY_SIG         "display_status_ind"
#define MCE_DISPLAY_DIM_STRING  "dimmed"
#define MCE_DISPLAY_ON_STRING   "on"
#define MCE_DISPLAY_OFF_STRING  "off"

typedef GObjectClass RilMceClass;
typedef struct ril_mce RilMce;

struct ril_mce_priv {
	GRilIoChannel *io;
	DBusConnection *conn;
	DBusPendingCall *req;
	guint daemon_watch;
	guint signal_watch;
};

enum ril_mce_signal {
	SIGNAL_DISPLAY_STATE_CHANGED,
	SIGNAL_COUNT
};

#define SIGNAL_DISPLAY_STATE_CHANGED_NAME   "ril-mce-display-state-changed"

static guint ril_mce_signals[SIGNAL_COUNT] = { 0 };

G_DEFINE_TYPE(RilMce, ril_mce, G_TYPE_OBJECT)
#define RIL_MCE_TYPE (ril_mce_get_type())
#define RIL_MCE(obj) (G_TYPE_CHECK_INSTANCE_CAST(obj,RIL_MCE_TYPE,RilMce))

static const char *ril_mce_display_state_string(enum ril_mce_display_state ds)
{
	switch (ds) {
	case RIL_MCE_DISPLAY_OFF:
		return MCE_DISPLAY_OFF_STRING;
	case RIL_MCE_DISPLAY_DIM:
		return MCE_DISPLAY_DIM_STRING;
	case RIL_MCE_DISPLAY_ON:
		return MCE_DISPLAY_ON_STRING;
	default:
		return NULL;
	}
}

static enum ril_mce_display_state ril_mce_parse_display_state(DBusMessage *msg)
{
	DBusMessageIter it;

	if (dbus_message_iter_init(msg, &it) &&
		dbus_message_iter_get_arg_type(&it) == DBUS_TYPE_STRING) {
		const char *value = NULL;

		dbus_message_iter_get_basic(&it, &value);
		if (!g_strcmp0(value, MCE_DISPLAY_OFF_STRING)) {
			return RIL_MCE_DISPLAY_OFF;
		} else if (!g_strcmp0(value, MCE_DISPLAY_DIM_STRING)) {
			return RIL_MCE_DISPLAY_DIM;
		} else {
			GASSERT(!g_strcmp0(value, MCE_DISPLAY_ON_STRING));
		}
	}

	return RIL_MCE_DISPLAY_ON;
}

static void ril_mce_update_display_state(struct ril_mce *self,
					enum ril_mce_display_state state)
{
	if (self->display_state != state) {
		self->display_state = state;
		g_signal_emit(self, ril_mce_signals[
					SIGNAL_DISPLAY_STATE_CHANGED], 0);
	}
}

static gboolean ril_mce_display_changed(DBusConnection *conn,
					DBusMessage *msg, void *user_data)
{
	enum ril_mce_display_state state = ril_mce_parse_display_state(msg);

	DBG("%s", ril_mce_display_state_string(state));
	ril_mce_update_display_state(RIL_MCE(user_data), state);
	return TRUE;
}

static void ril_mce_display_status_reply(DBusPendingCall *call, void *user_data)
{
	struct ril_mce *self = RIL_MCE(user_data);
	struct ril_mce_priv *priv = self->priv;
	DBusMessage *reply = dbus_pending_call_steal_reply(call);
	enum ril_mce_display_state state = ril_mce_parse_display_state(reply);

	GASSERT(priv->req);
	dbus_message_unref(reply);
	dbus_pending_call_unref(priv->req);
	priv->req = NULL;

	DBG("%s", ril_mce_display_state_string(state));
	ril_mce_update_display_state(self, state);
}

static void ril_mce_connect(DBusConnection *conn, void *user_data)
{
	struct ril_mce *self = RIL_MCE(user_data);
	struct ril_mce_priv *priv = self->priv;

	DBG("");
	if (!priv->req) {
		DBusMessage *msg = dbus_message_new_method_call(MCE_SERVICE,
					MCE_REQUEST_PATH, MCE_REQUEST_IF,
					MCE_DISPLAY_STATUS_GET);
		if (g_dbus_send_message_with_reply(conn, msg, &priv->req, -1)) {
			dbus_pending_call_set_notify(priv->req,
						ril_mce_display_status_reply,
						self, NULL);
			dbus_message_unref(msg);
		}
	}
	if (!priv->signal_watch) {
		priv->signal_watch = g_dbus_add_signal_watch(conn,
			MCE_SERVICE, NULL, MCE_SIGNAL_IF, MCE_DISPLAY_SIG,
			ril_mce_display_changed, self, NULL);
	}
}

static void ril_mce_disconnect(DBusConnection *conn, void *user_data)
{
	struct ril_mce *self = user_data;
	struct ril_mce_priv *priv = self->priv;

	DBG("");
	if (priv->signal_watch) {
		g_dbus_remove_watch(conn, priv->signal_watch);
		priv->signal_watch = 0;
	}
	if (priv->req) {
		dbus_pending_call_cancel(priv->req);
		dbus_pending_call_unref(priv->req);
	}
}

struct ril_mce *ril_mce_new()
{
	struct ril_mce *self = g_object_new(RIL_MCE_TYPE, NULL);
	struct ril_mce_priv *priv = self->priv;

	DBG("");
	priv->daemon_watch = g_dbus_add_service_watch(priv->conn, MCE_SERVICE,
		ril_mce_connect, ril_mce_disconnect, self, NULL);
	return self;
}

struct ril_mce *ril_mce_ref(struct ril_mce *self)
{
	if (G_LIKELY(self)) {
		g_object_ref(RIL_MCE(self));
		return self;
	} else {
		return NULL;
	}
}

void ril_mce_unref(struct ril_mce *self)
{
	if (G_LIKELY(self)) {
		g_object_unref(RIL_MCE(self));
	}
}

gulong ril_mce_add_display_state_changed_handler(struct ril_mce *self,
					ril_mce_cb_t cb, void *arg)
{
	return (G_LIKELY(self) && G_LIKELY(cb)) ? g_signal_connect(self,
		SIGNAL_DISPLAY_STATE_CHANGED_NAME, G_CALLBACK(cb), arg) : 0;
}

void ril_mce_remove_handler(struct ril_mce *self, gulong id)
{
	if (G_LIKELY(self) && G_LIKELY(id)) {
		g_signal_handler_disconnect(self, id);
	}
}

static void ril_mce_init(struct ril_mce *self)
{
	struct ril_mce_priv *priv = G_TYPE_INSTANCE_GET_PRIVATE(self,
		RIL_MCE_TYPE, struct ril_mce_priv);

	priv->conn = dbus_connection_ref(ofono_dbus_get_connection());
	self->priv = priv;
}

static void ril_mce_dispose(GObject *object)
{
	struct ril_mce *self = RIL_MCE(object);
	struct ril_mce_priv *priv = self->priv;

	if (priv->signal_watch) {
		g_dbus_remove_watch(priv->conn, priv->signal_watch);
		priv->signal_watch = 0;
	}
	if (priv->daemon_watch) {
		g_dbus_remove_watch(priv->conn, priv->daemon_watch);
		priv->daemon_watch = 0;
	}
	if (priv->req) {
		dbus_pending_call_cancel(priv->req);
		dbus_pending_call_unref(priv->req);
	}
	G_OBJECT_CLASS(ril_mce_parent_class)->dispose(object);
}

static void ril_mce_finalize(GObject *object)
{
	struct ril_mce *self = RIL_MCE(object);
	struct ril_mce_priv *priv = self->priv;

	dbus_connection_unref(priv->conn);
	G_OBJECT_CLASS(ril_mce_parent_class)->finalize(object);
}

static void ril_mce_class_init(RilMceClass *klass)
{
	GObjectClass *object_class = G_OBJECT_CLASS(klass);

	object_class->dispose = ril_mce_dispose;
	object_class->finalize = ril_mce_finalize;
	g_type_class_add_private(klass, sizeof(struct ril_mce_priv));
	ril_mce_signals[SIGNAL_DISPLAY_STATE_CHANGED] =
		g_signal_new(SIGNAL_DISPLAY_STATE_CHANGED_NAME,
			G_OBJECT_CLASS_TYPE(klass), G_SIGNAL_RUN_FIRST,
			0, NULL, NULL, NULL, G_TYPE_NONE, 0);
}

/*
 * Local Variables:
 * mode: C
 * c-basic-offset: 8
 * indent-tabs-mode: t
 * End:
 */
