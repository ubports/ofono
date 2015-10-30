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

#include "ril_plugin.h"
#include "ril_log.h"

#include <ofono/log.h>
#include <ofono/dbus.h>

#include <gdbus.h>

#include "ofono.h"
#include "storage.h"

struct ril_sim_dbus {
	char *path;
	char *imsi;
	char *name;
	char *default_name;
	gboolean enable_4g;
	GKeyFile *storage;
	DBusConnection *conn;
	struct ril_modem *md;
};

#define RIL_SIM_STORE                   "ril"
#define RIL_SIM_STORE_GROUP             "Settings"
#define RIL_SIM_STORE_ENABLE_4G         "Enable4G"
#define RIL_SIM_STORE_DISPLAY_NAME      "DisplayName"

#define RIL_SIM_DBUS_INTERFACE          "org.nemomobile.ofono.SimSettings"
#define RIL_SIM_DBUS_INTERFACE_VERSION  (1)

#define RIL_SIM_DBUS_DISPLAY_NAME_CHANGED_SIGNAL    "DisplayNameChanged"
#define RIL_SIM_DBUS_ENABLE_4G_CHANGED_SIGNAL       "Enable4GChanged"

static DBusMessage *ril_sim_dbus_get_all(DBusConnection *conn,
						DBusMessage *msg, void *data)
{
	struct ril_sim_dbus *dbus = data;
	DBusMessage *reply = dbus_message_new_method_return(msg);
	dbus_int32_t version = RIL_SIM_DBUS_INTERFACE_VERSION;
	dbus_bool_t enable_4g = dbus->enable_4g;
	DBusMessageIter iter;

	dbus_message_iter_init_append(reply, &iter);
	dbus_message_iter_append_basic(&iter, DBUS_TYPE_INT32, &version);
	dbus_message_iter_append_basic(&iter, DBUS_TYPE_BOOLEAN, &enable_4g);
	dbus_message_iter_append_basic(&iter, DBUS_TYPE_STRING, &dbus->name);
	return reply;
}

static DBusMessage *ril_sim_dbus_get_interface_version(DBusConnection *conn,
						DBusMessage *msg, void *data)
{
	DBusMessage *reply = dbus_message_new_method_return(msg);
	dbus_int32_t version = RIL_SIM_DBUS_INTERFACE_VERSION;
	DBusMessageIter iter;

	dbus_message_iter_init_append(reply, &iter);
	dbus_message_iter_append_basic(&iter, DBUS_TYPE_INT32, &version);
	return reply;
}

static DBusMessage *ril_sim_dbus_get_enable_4g(DBusConnection *conn,
						DBusMessage *msg, void *data)
{
	struct ril_sim_dbus *dbus = data;
	DBusMessage *reply = dbus_message_new_method_return(msg);
	dbus_bool_t enable_4g = dbus->enable_4g;
	DBusMessageIter iter;

	dbus_message_iter_init_append(reply, &iter);
	dbus_message_iter_append_basic(&iter, DBUS_TYPE_BOOLEAN, &enable_4g);
	return reply;
}

static DBusMessage *ril_sim_dbus_get_display_name(DBusConnection *conn,
						DBusMessage *msg, void *data)
{
	struct ril_sim_dbus *dbus = data;
	DBusMessage *reply = dbus_message_new_method_return(msg);
	DBusMessageIter iter;

	dbus_message_iter_init_append(reply, &iter);
	dbus_message_iter_append_basic(&iter, DBUS_TYPE_STRING, &dbus->name);
	return reply;
}

static void ril_sim_dbus_update_display_name(struct ril_sim_dbus *dbus,
							const char *name)
{
	if (g_strcmp0(dbus->name, name)) {
		g_free(dbus->name);
		dbus->name = g_strdup(name);
		g_key_file_set_string(dbus->storage, RIL_SIM_STORE_GROUP,
					RIL_SIM_STORE_DISPLAY_NAME, name);
		storage_sync(dbus->imsi, RIL_SIM_STORE, dbus->storage);
		g_dbus_emit_signal(dbus->conn, dbus->path,
				RIL_SIM_DBUS_INTERFACE,
				RIL_SIM_DBUS_DISPLAY_NAME_CHANGED_SIGNAL,
				DBUS_TYPE_STRING, &name, DBUS_TYPE_INVALID);
	}
}

static DBusMessage *ril_sim_dbus_set_display_name(DBusConnection *conn,
						DBusMessage *msg, void *data)
{
	DBusMessageIter iter;

	dbus_message_iter_init(msg, &iter);
	if (dbus_message_iter_get_arg_type(&iter) == DBUS_TYPE_STRING) {
		struct ril_sim_dbus *dbus = data;
		DBusBasicValue value;
		const char *name;

		dbus_message_iter_get_basic(&iter, &value);
		name = value.str;
		if (!name || !name[0]) name = dbus->default_name;
		ril_sim_dbus_update_display_name(dbus, name);
		return dbus_message_new_method_return(msg);
	} else {
		return __ofono_error_invalid_args(msg);
	}
}

static const GDBusMethodTable ril_sim_dbus_methods[] = {
	{ GDBUS_METHOD("GetAll",
			NULL, GDBUS_ARGS({ "settings", "ibs" }),
			ril_sim_dbus_get_all) },
	{ GDBUS_METHOD("GetInterfaceVersion",
			NULL, GDBUS_ARGS({ "version", "i" }),
			ril_sim_dbus_get_interface_version) },
	{ GDBUS_METHOD("GetEnable4G",
			NULL, GDBUS_ARGS({ "enable", "b" }),
			ril_sim_dbus_get_enable_4g) },
	{ GDBUS_METHOD("GetDisplayName",
			NULL, GDBUS_ARGS({ "name", "s" }),
			ril_sim_dbus_get_display_name) },
	{ GDBUS_METHOD("SetDisplayName",
			GDBUS_ARGS({ "name", "s" }), NULL,
			ril_sim_dbus_set_display_name) },
	{ }
};

static const GDBusSignalTable ril_sim_dbus_signals[] = {
	{ GDBUS_SIGNAL(RIL_SIM_DBUS_DISPLAY_NAME_CHANGED_SIGNAL,
			GDBUS_ARGS({ "name", "s" })) },
	{ GDBUS_SIGNAL(RIL_SIM_DBUS_ENABLE_4G_CHANGED_SIGNAL,
			GDBUS_ARGS({ "enabled", "b" })) },
	{ }
};

const char *ril_sim_dbus_imsi(struct ril_sim_dbus *dbus)
{
	return dbus ? dbus->imsi : NULL;
}

struct ril_sim_dbus *ril_sim_dbus_new(struct ril_modem *md)
{
	const char *imsi = ofono_sim_get_imsi(ril_modem_ofono_sim(md));

	if (imsi) {
		GError *error = NULL;
		const struct ril_modem_config *config= ril_modem_config(md);
		struct ril_sim_dbus *dbus = g_new0(struct ril_sim_dbus, 1);

		DBG("%s", ril_modem_get_path(md));
		dbus->md = md;
		dbus->conn = dbus_connection_ref(ofono_dbus_get_connection());
		dbus->path = g_strdup(ril_modem_get_path(md));
		dbus->imsi = g_strdup(imsi);
		dbus->default_name = g_strdup(config->default_name);

		/* Load settings */
		dbus->storage = storage_open(imsi, RIL_SIM_STORE);
		dbus->enable_4g = g_key_file_get_boolean(dbus->storage,
			RIL_SIM_STORE_GROUP, RIL_SIM_STORE_ENABLE_4G, &error);
		if (error) {
			dbus->enable_4g = config->enable_4g;
			g_error_free(error);
			error = NULL;
		}
		dbus->name = g_key_file_get_string(dbus->storage,
			RIL_SIM_STORE_GROUP, RIL_SIM_STORE_DISPLAY_NAME, NULL);
		if (!dbus->name) {
			dbus->name = g_strdup(config->default_name);
			GASSERT(dbus->name);
		}

		/* Register D-Bus interface */
		if (g_dbus_register_interface(dbus->conn, dbus->path,
				RIL_SIM_DBUS_INTERFACE, ril_sim_dbus_methods,
				ril_sim_dbus_signals, NULL, dbus, NULL)) {
			ofono_modem_add_interface(ril_modem_ofono_modem(md),
						RIL_SIM_DBUS_INTERFACE);
			return dbus;
		} else {
			ofono_error("RIL D-Bus register failed");
			ril_sim_dbus_free(dbus);
		}
	}

	return NULL;
}

void ril_sim_dbus_free(struct ril_sim_dbus *dbus)
{
	if (dbus) {
		DBG("%s", dbus->path);
		g_dbus_unregister_interface(dbus->conn, dbus->path,
						RIL_SIM_DBUS_INTERFACE);
		ofono_modem_remove_interface(ril_modem_ofono_modem(dbus->md),
						RIL_SIM_DBUS_INTERFACE);
		dbus_connection_unref(dbus->conn);	
		g_key_file_free(dbus->storage);
		g_free(dbus->path);
		g_free(dbus->imsi);
		g_free(dbus->name);
		g_free(dbus->default_name);
		g_free(dbus);
	}
}

/*
 * Local Variables:
 * mode: C
 * c-basic-offset: 8
 * indent-tabs-mode: t
 * End:
 */
