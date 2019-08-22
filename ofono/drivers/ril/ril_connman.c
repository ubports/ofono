/*
 *  oFono - Open Source Telephony - RIL-based devices
 *
 *  Copyright (C) 2019 Jolla Ltd.
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License version 2 as
 *  published by the Free Software Foundation.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 *  GNU General Public License for more details.
 */

#include "ril_connman.h"

#include <ofono/log.h>

#include <gdbus.h>

#include <gutil_macros.h>
#include <gutil_misc.h>

#include <glib-object.h>

#define CONNMAN_BUS DBUS_BUS_SYSTEM
#define CONNMAN_SERVICE "net.connman"
#define CONNMAN_PATH "/"

#define CONNMAN_GET_PROPERTIES "GetProperties"
#define CONNMAN_GET_TECHNOLOGIES "GetTechnologies"
#define CONNMAN_PROPERTY_CHANGED "PropertyChanged"
#define CONNMAN_TECH_CONNECTED "Connected"
#define CONNMAN_TECH_TETHERING "Tethering"

#define CONNMAN_INTERFACE_(name) "net.connman." name
#define CONNMAN_MANAGER_INTERFACE CONNMAN_INTERFACE_("Manager")
#define CONNMAN_TECH_INTERFACE CONNMAN_INTERFACE_("Technology")

#define CONNMAN_TECH_PATH_(name) "/net/connman/technology/" name
#define CONNMAN_TECH_PATH_WIFI CONNMAN_TECH_PATH_("wifi")

#define CONNMAN_TECH_CONNECTED_BIT (0x01)
#define CONNMAN_TECH_TETHERING_BIT (0x02)
#define CONNMAN_TECH_ALL_PROPERTY_BITS (\
	CONNMAN_TECH_CONNECTED_BIT | \
	CONNMAN_TECH_TETHERING_BIT)

typedef GObjectClass ConnManObjectClass;

typedef struct connman_tech ConnManTech;

typedef struct connman_object {
	GObject object;
	struct ril_connman pub;
	guint32 pending_signals;
	DBusConnection *connection;
	DBusPendingCall *call;
	guint service_watch;
	guint signal_watch;
	GHashTable *techs;
	ConnManTech *wifi;
} ConnManObject;

G_DEFINE_TYPE(ConnManObject, connman_object, G_TYPE_OBJECT)
#define CONNMAN_OBJECT_TYPE (connman_object_get_type())
#define CONNMAN_OBJECT(obj) (G_TYPE_CHECK_INSTANCE_CAST((obj),\
	CONNMAN_OBJECT_TYPE, ConnManObject))

struct connman_tech {
	ConnManObject *obj;
	const char *path;
	gboolean connected;
	gboolean tethering;
};

typedef struct connman_closure {
	GCClosure cclosure;
	ril_connman_property_cb_t callback;
	gpointer user_data;
} ConnManClosure;

#define connman_closure_new() ((ConnManClosure *) \
    g_closure_new_simple(sizeof(ConnManClosure), NULL))

#define SIGNAL_PROPERTY_CHANGED_NAME    "ril-connman-property-changed"
#define SIGNAL_PROPERTY_DETAIL          "%x"
#define SIGNAL_PROPERTY_DETAIL_MAX_LEN  (8)

#define SIGNAL_BIT(property) (1 << (property - 1))
#define SIGNAL_BIT_(name) SIGNAL_BIT(RIL_CONNMAN_PROPERTY_##name)

enum connman_object_signal {
	SIGNAL_PROPERTY_CHANGED,
	SIGNAL_COUNT
};

static guint connman_object_signals[SIGNAL_COUNT];
static GQuark connman_object_property_quarks[RIL_CONNMAN_PROPERTY_COUNT - 1];

static inline ConnManObject *connman_object_cast(struct ril_connman *connman)
{
	return G_LIKELY(connman) ?
		CONNMAN_OBJECT(G_CAST(connman, ConnManObject, pub)) :
		NULL;
}

static inline const char *connman_iter_get_string(DBusMessageIter *it)
{
	const char *str = NULL;

	dbus_message_iter_get_basic(it, &str);
	return str;
}

static GQuark connman_object_property_quark(enum ril_connman_property p)
{
	/* For ANY property this function is expected to return zero */
	if (p > RIL_CONNMAN_PROPERTY_ANY && p < RIL_CONNMAN_PROPERTY_COUNT) {
		const int i = p - 1;

		if (G_UNLIKELY(!connman_object_property_quarks[i])) {
			char buf[SIGNAL_PROPERTY_DETAIL_MAX_LEN + 1];

			snprintf(buf, sizeof(buf), SIGNAL_PROPERTY_DETAIL, p);
			buf[sizeof(buf) - 1] = 0;
			connman_object_property_quarks[i] =
				g_quark_from_string(buf);
		}
		return connman_object_property_quarks[i];
	}
	return 0;
}

static void connman_object_property_changed(ConnManObject *self,
		enum ril_connman_property property, ConnManClosure *closure)
{
	closure->callback(&self->pub, property, closure->user_data);
}

static void connman_object_emit_property_change(ConnManObject *self,
						enum ril_connman_property p)
{
	self->pending_signals &= ~SIGNAL_BIT(p);
	g_signal_emit(self, connman_object_signals[SIGNAL_PROPERTY_CHANGED],
		connman_object_property_quark(p), p);
}

static void connman_object_emit_pending_signals(ConnManObject *self)
{
	struct ril_connman *connman = &self->pub;
	gboolean valid_changed, present_changed;
	enum ril_connman_property p;

	/* Handlers could drop their references to us */
	g_object_ref(self);

	/*
	 * PRESENT and VALID are the last signals to be emitted if the object
	 * BECOMES present and/or valid.
	 */
	if ((self->pending_signals & SIGNAL_BIT_(VALID)) &&
						connman->valid) {
		self->pending_signals &= ~SIGNAL_BIT_(VALID);
		valid_changed = TRUE;
	} else {
		valid_changed = FALSE;
	}
	if ((self->pending_signals & SIGNAL_BIT_(PRESENT)) &&
						connman->present) {
		self->pending_signals &= ~SIGNAL_BIT_(PRESENT);
		present_changed = TRUE;
	} else {
		present_changed = FALSE;
	}

	/*
	 * Emit the signals. Not that in case if valid has become FALSE,
	 * then VALID is emitted first, otherwise it's emitted last.
	 * Same thing with PRESENT.
	 */
	for (p = RIL_CONNMAN_PROPERTY_ANY + 1;
		p < RIL_CONNMAN_PROPERTY_COUNT && self->pending_signals;
		p++) {
		if (self->pending_signals & SIGNAL_BIT(p)) {
			connman_object_emit_property_change(self, p);
		}
	}

	/* Then emit PRESENT and VALID if necessary */
	if (present_changed) {
		connman_object_emit_property_change(self,
			RIL_CONNMAN_PROPERTY_PRESENT);
	}
	if (valid_changed) {
		connman_object_emit_property_change(self,
			RIL_CONNMAN_PROPERTY_VALID);
	}

	/* And release the temporary reference */
	g_object_unref(self);
}

static void connman_cancel_call(ConnManObject *self)
{
	if (self->call) {
		dbus_pending_call_cancel(self->call);
		dbus_pending_call_unref(self->call);
		self->call = NULL;
	}
}

static ConnManTech *connman_tech_new(ConnManObject *self, const char *path)
{
	ConnManTech *tech = g_new0(ConnManTech, 1);
	char *key = g_strdup(path);

	tech->obj = self;
	tech->path = key;
	g_hash_table_replace(self->techs, key, tech);
	return tech;
}

static void connman_invalidate(ConnManObject *self)
{
	struct ril_connman *connman = &self->pub;

	if (connman->valid) {
		connman->valid = FALSE;
		self->pending_signals |= SIGNAL_BIT_(VALID);
	}
}

static void connman_update_valid(ConnManObject *self)
{
	struct ril_connman *connman = &self->pub;
	const gboolean valid = (connman->present && !self->call);

	if (connman->valid != valid) {
		connman->valid = valid;
		self->pending_signals |= SIGNAL_BIT_(VALID);
	}
}

static gboolean connman_update_tethering(ConnManObject *self)
{
	struct ril_connman *connman = &self->pub;
	gboolean tethering = FALSE;
	GHashTableIter it;
	gpointer value;

	g_hash_table_iter_init(&it, self->techs);
	while (g_hash_table_iter_next(&it, NULL, &value)) {
		const ConnManTech *tech = value;

		if (tech->tethering) {
			tethering = TRUE;
			break;
		}
	}

	if (connman->tethering != tethering) {
		connman->tethering = tethering;
		self->pending_signals |= SIGNAL_BIT_(TETHERING);
		return TRUE;
	} else {
		return FALSE;
	}
}

static void connman_set_tech_tethering(ConnManTech *tech, gboolean tethering)
{
	if (tech->tethering != tethering) {
		ConnManObject *self = tech->obj;

		tech->tethering = tethering;
		DBG(CONNMAN_TECH_TETHERING " %s for %s",
				tethering ? "on" : "off", tech->path);
		if (tethering) {
			struct ril_connman *connman = &self->pub;

			if (G_LIKELY(!connman->tethering)) {
				/* Definitely tethering now */
				connman->tethering = TRUE;
				self->pending_signals |= SIGNAL_BIT_(TETHERING);
				DBG("Tethering on");
			}
		} else if (connman_update_tethering(self)) {
			/* Not tethering anymore */
			DBG("Tethering off");
		}
	}
}

static void connman_set_tech_connected(ConnManTech *tech, gboolean connected)
{
	if (tech->connected != connected) {
		ConnManObject *self = tech->obj;

		tech->connected = connected;
		DBG(CONNMAN_TECH_CONNECTED " %s for %s",
				connected ? "on" : "off", tech->path);
		if (tech == self->wifi) {
			struct ril_connman *connman = &self->pub;

			connman->wifi_connected = connected;
			self->pending_signals |= SIGNAL_BIT_(WIFI_CONNECTED);
			DBG("WiFi %sconnected", connected ? "" : "dis");
		}
	}
}

static int connman_tech_set_property(ConnManTech *tech, DBusMessageIter *it)
{
	DBusMessageIter var;
	DBusBasicValue value;
	const char *key = connman_iter_get_string(it);

	dbus_message_iter_next(it);
	dbus_message_iter_recurse(it, &var);
	dbus_message_iter_get_basic(&var, &value);
	if (!g_ascii_strcasecmp(key, CONNMAN_TECH_CONNECTED)) {
		if (dbus_message_iter_get_arg_type(&var) == DBUS_TYPE_BOOLEAN) {
			connman_set_tech_connected(tech, value.bool_val);
			return CONNMAN_TECH_CONNECTED_BIT;
		}
	} else if (!g_ascii_strcasecmp(key, CONNMAN_TECH_TETHERING)) {
		if (dbus_message_iter_get_arg_type(&var) == DBUS_TYPE_BOOLEAN) {
			connman_set_tech_tethering(tech, value.bool_val);
			return CONNMAN_TECH_TETHERING_BIT;
		}
	}
	return 0;
}

static void connman_tech_set_properties(ConnManTech *tech, DBusMessageIter *it)
{
	DBusMessageIter dict;
	int handled = 0;

	dbus_message_iter_recurse(it, &dict);
	while (dbus_message_iter_get_arg_type(&dict) == DBUS_TYPE_DICT_ENTRY) {
		DBusMessageIter entry;

		dbus_message_iter_recurse(&dict, &entry);
		handled |= connman_tech_set_property(tech, &entry);
		if (handled == CONNMAN_TECH_ALL_PROPERTY_BITS) {
			/* Ignore the rest */
			break;
		}
		dbus_message_iter_next(&dict);
	}
}

static gboolean connman_tech_property_changed(DBusConnection *conn,
					DBusMessage *msg, void *user_data)
{
	const char *path = dbus_message_get_path(msg);
	ConnManObject *self = CONNMAN_OBJECT(user_data);
	ConnManTech *tech = g_hash_table_lookup(self->techs, path);
	DBusMessageIter it;

	if (tech && dbus_message_has_signature(msg, "sv") &&
					dbus_message_iter_init(msg, &it)) {
		const char* name = connman_iter_get_string(&it);

		if (!connman_tech_set_property(tech, &it)) {
			DBG("%s changed for %s", name, path);
		}
		connman_object_emit_pending_signals(self);
	}
	return TRUE;
}

static void connman_set_techs(ConnManObject *self, DBusMessageIter *it)
{
	DBusMessageIter list;

	dbus_message_iter_recurse(it, &list);
	while (dbus_message_iter_get_arg_type(&list) == DBUS_TYPE_STRUCT) {
		DBusMessageIter entry;
		const char *path;
		ConnManTech *tech;

		dbus_message_iter_recurse(&list, &entry);
		path = connman_iter_get_string(&entry);
		tech = connman_tech_new(self, path);

		DBG("%s", path);
		if (!g_strcmp0(path, CONNMAN_TECH_PATH_WIFI)) {
			/* WiFi is a special case */
			self->wifi = tech;
		}

		dbus_message_iter_next(&entry);
		connman_tech_set_properties(tech, &entry);
		dbus_message_iter_next(&list);
	}
}

static void connman_techs_reply(DBusPendingCall *call, void *user_data)
{
	ConnManObject *self = CONNMAN_OBJECT(user_data);
	DBusMessage *reply = dbus_pending_call_steal_reply(call);
	DBusError error;
	DBusMessageIter array;

	dbus_error_init(&error);
	if (dbus_set_error_from_message(&error, reply)) {
		DBG("Failed to get technologies: %s", error.message);
		dbus_error_free(&error);
	} else if (dbus_message_has_signature(reply, "a(oa{sv})") &&
				dbus_message_iter_init(reply, &array)) {
		connman_set_techs(self, &array);
	}

	dbus_message_unref(reply);
	dbus_pending_call_unref(self->call);
	self->call = NULL;
	connman_update_valid(self);
	connman_object_emit_pending_signals(self);
}

static void connman_get_techs(ConnManObject *self)
{
	DBusMessage *msg = dbus_message_new_method_call(CONNMAN_SERVICE,
				CONNMAN_PATH, CONNMAN_MANAGER_INTERFACE,
				CONNMAN_GET_TECHNOLOGIES);

	connman_cancel_call(self);
	if (g_dbus_send_message_with_reply(self->connection, msg,
					&self->call, DBUS_TIMEOUT_INFINITE)) {
		/* Not valid while any request is pending */
		connman_invalidate(self);
		dbus_pending_call_set_notify(self->call, connman_techs_reply,
								self, NULL);
	}
	dbus_message_unref(msg);
}

static void connman_appeared(DBusConnection *conn, void *user_data)
{
	ConnManObject *self = CONNMAN_OBJECT(user_data);
	struct ril_connman *connman = &self->pub;

	if (!connman->present) {
		DBG("connman is there");
		connman->present = TRUE;
		self->pending_signals |= SIGNAL_BIT_(PRESENT);
		connman_get_techs(self);
		connman_object_emit_pending_signals(self);
	}
}

static void connman_vanished(DBusConnection *conn, void *user_data)
{
	ConnManObject *self = CONNMAN_OBJECT(user_data);
	struct ril_connman *connman = &self->pub;

	if (connman->present) {

		DBG("connman has disappeared");
		g_hash_table_remove_all(self->techs);
		self->wifi = NULL;
		connman->present = FALSE;
		self->pending_signals |= SIGNAL_BIT_(PRESENT);
		if (connman->wifi_connected) {
			connman->wifi_connected = FALSE;
			self->pending_signals |= SIGNAL_BIT_(WIFI_CONNECTED);
		}
		if (connman->tethering) {
			connman->tethering = FALSE;
			self->pending_signals |= SIGNAL_BIT_(TETHERING);
		}
		connman_object_emit_pending_signals(self);
	}
}

static void connman_init(ConnManObject *self, DBusConnection *connection)
{
	self->connection = dbus_connection_ref(connection);
	self->service_watch = g_dbus_add_service_watch(self->connection,
		CONNMAN_SERVICE, connman_appeared, connman_vanished,
		self, NULL);
	self->signal_watch = g_dbus_add_signal_watch(self->connection,
		CONNMAN_SERVICE, NULL, CONNMAN_TECH_INTERFACE,
		CONNMAN_PROPERTY_CHANGED, connman_tech_property_changed,
		self, NULL);
}

struct ril_connman *ril_connman_new()
{
	static ConnManObject *instance = NULL;

	if (instance) {
		g_object_ref(instance);
		return &instance->pub;
	} else {
		DBusError error;
		DBusConnection *connection;

		dbus_error_init(&error);
		connection = dbus_bus_get(CONNMAN_BUS, NULL);

		if (connection) {
			instance = g_object_new(CONNMAN_OBJECT_TYPE, NULL);
			connman_init(instance, connection);
			dbus_connection_unref(connection);
			g_object_add_weak_pointer(G_OBJECT(instance),
				(gpointer*)(&instance));
			return &instance->pub;
		} else {
			ofono_error("Unable to attach to connman bus: %s",
				error.message);
			dbus_error_free(&error);
			return NULL;
		}
	}
}

struct ril_connman *ril_connman_ref(struct ril_connman *connman)
{
	ConnManObject *self = connman_object_cast(connman);

	if (G_LIKELY(self)) {
		g_object_ref(self);
		return connman;
	} else {
		return NULL;
	}
}

void ril_connman_unref(struct ril_connman *connman)
{
	ConnManObject *self = connman_object_cast(connman);

	if (G_LIKELY(self)) {
		g_object_unref(self);
	}
}

gulong ril_connman_add_property_changed_handler(struct ril_connman *connman,
	enum ril_connman_property p, ril_connman_property_cb_t cb, void *arg)
{
	ConnManObject *self = connman_object_cast(connman);

	if (G_LIKELY(self) && G_LIKELY(cb)) {
		/*
		 * We can't directly connect the provided callback because
		 * it expects the first parameter to point to public part
		 * of the object but glib will call it with ConnManObject
		 * as the first parameter. connman_object_property_changed()
		 * will do the conversion.
		 */
		ConnManClosure *closure = connman_closure_new();
		GCClosure *cc = &closure->cclosure;

		cc->closure.data = closure;
		cc->callback = G_CALLBACK(connman_object_property_changed);
		closure->callback = cb;
		closure->user_data = arg;

		return g_signal_connect_closure_by_id(self,
			connman_object_signals[SIGNAL_PROPERTY_CHANGED],
			connman_object_property_quark(p), &cc->closure, FALSE);
	}
	return 0;
}

void ril_connman_remove_handler(struct ril_connman *connman, gulong id)
{
	if (G_LIKELY(id)) {
		ConnManObject *self = connman_object_cast(connman);

		if (G_LIKELY(self)) {
			g_signal_handler_disconnect(self, id);
		}
	}
}

void ril_connman_remove_handlers(struct ril_connman *connman, gulong *ids,
								int n)
{
	gutil_disconnect_handlers(connman_object_cast(connman), ids, n);
}

static void connman_object_init(ConnManObject *self)
{
	self->techs = g_hash_table_new_full(g_str_hash, g_str_equal,
							g_free, g_free);
}

static void connman_object_finalize(GObject *object)
{
	ConnManObject *self = CONNMAN_OBJECT(object);

	connman_cancel_call(self);
	g_hash_table_destroy(self->techs);
	g_dbus_remove_watch(self->connection, self->service_watch);
	g_dbus_remove_watch(self->connection, self->signal_watch);
	dbus_connection_unref(self->connection);
	G_OBJECT_CLASS(connman_object_parent_class)->finalize(object);
}

static void connman_object_class_init(ConnManObjectClass *klass)
{
	G_OBJECT_CLASS(klass)->finalize = connman_object_finalize;
	connman_object_signals[SIGNAL_PROPERTY_CHANGED] =
		g_signal_new(SIGNAL_PROPERTY_CHANGED_NAME,
			G_OBJECT_CLASS_TYPE(klass),
			G_SIGNAL_RUN_FIRST | G_SIGNAL_DETAILED,
			0, NULL, NULL, NULL, G_TYPE_NONE, 1, G_TYPE_UINT);
}

/*
 * Local Variables:
 * mode: C
 * c-basic-offset: 8
 * indent-tabs-mode: t
 * End:
 */
