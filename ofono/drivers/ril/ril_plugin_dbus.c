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

#include <ofono/log.h>
#include <ofono/dbus.h>

#include <gutil_strv.h>
#include <gutil_log.h>
#include <gdbus.h>

#include "ofono.h"

struct ril_plugin_dbus {
	struct ril_plugin *plugin;
	DBusConnection *conn;
};

#define RIL_DBUS_PATH               "/"
#define RIL_DBUS_INTERFACE          "org.nemomobile.ofono.ModemManager"
#define RIL_DBUS_INTERFACE_VERSION  (2)

#define RIL_DBUS_ENABLED_MODEMS_CHANGED_SIGNAL      "EnabledModemsChanged"
#define RIL_DBUS_PRESENT_SIMS_CHANGED_SIGNAL        "PresentSimsChanged"
#define RIL_DBUS_DEFAULT_VOICE_SIM_CHANGED_SIGNAL   "DefaultVoiceSimChanged"
#define RIL_DBUS_DEFAULT_DATA_SIM_CHANGED_SIGNAL    "DefaultDataSimChanged"
#define RIL_DBUS_DEFAULT_VOICE_MODEM_CHANGED_SIGNAL "DefaultVoiceModemChanged"
#define RIL_DBUS_DEFAULT_DATA_MODEM_CHANGED_SIGNAL  "DefaultDataModemChanged"
#define RIL_DBUS_IMSI_AUTO                          "auto"

typedef gboolean
(*ril_plugin_dbus_slot_select_fn) (const struct ril_slot_info *);
typedef const char *
(*ril_plugin_dbus_slot_string_fn) (const struct ril_slot_info *);

static gboolean ril_plugin_dbus_enabled(const struct ril_slot_info *slot)
{
	return slot->enabled;
}

static gboolean ril_plugin_dbus_present(const struct ril_slot_info *slot)
{
	return slot->sim_present;
}

static void ril_plugin_dbus_append_path_array(DBusMessageIter *it,
	struct ril_plugin_dbus *dbus, ril_plugin_dbus_slot_select_fn selector)
{
	DBusMessageIter array;
	const struct ril_slot_info *const *ptr = dbus->plugin->slots;

	dbus_message_iter_open_container(it, DBUS_TYPE_ARRAY,
				DBUS_TYPE_OBJECT_PATH_AS_STRING, &array);

	while (*ptr) {
		const struct ril_slot_info *slot = *ptr++;
		if (!selector || selector(slot)) {
			const char *path = slot->path;
			dbus_message_iter_append_basic(&array,
						DBUS_TYPE_OBJECT_PATH, &path);
		}
	}

	dbus_message_iter_close_container(it, &array);
}

static void ril_plugin_dbus_append_boolean_array(DBusMessageIter *it,
	struct ril_plugin_dbus *dbus, ril_plugin_dbus_slot_select_fn value)
{
	DBusMessageIter array;
	const struct ril_slot_info *const *ptr = dbus->plugin->slots;

	dbus_message_iter_open_container(it, DBUS_TYPE_ARRAY,
				DBUS_TYPE_BOOLEAN_AS_STRING, &array);

	while (*ptr) {
		const struct ril_slot_info *slot = *ptr++;
		dbus_bool_t b = value(slot);

		dbus_message_iter_append_basic(&array, DBUS_TYPE_BOOLEAN, &b);
	}

	dbus_message_iter_close_container(it, &array);
}

static void ril_plugin_dbus_append_imsi(DBusMessageIter *it, const char *imsi)
{
	if (!imsi) imsi = RIL_DBUS_IMSI_AUTO;
	dbus_message_iter_append_basic(it, DBUS_TYPE_STRING, &imsi);
}

static void ril_plugin_dbus_append_path(DBusMessageIter *it, const char *path)
{
	if (!path) path = "";
	/* It's DBUS_TYPE_STRING because DBUS_TYPE_OBJECT_PATH can't be empty */
	dbus_message_iter_append_basic(it, DBUS_TYPE_STRING, &path);
}

static void ril_plugin_dbus_message_append_path_array(DBusMessage *msg,
	struct ril_plugin_dbus *dbus, ril_plugin_dbus_slot_select_fn fn)
{
	DBusMessageIter iter;

	dbus_message_iter_init_append(msg, &iter);
	ril_plugin_dbus_append_path_array(&iter, dbus, fn);
}

static void ril_plugin_dbus_signal_path_array(struct ril_plugin_dbus *dbus,
			const char *name, ril_plugin_dbus_slot_select_fn fn)
{
	DBusMessage *signal = dbus_message_new_signal(RIL_DBUS_PATH,
						RIL_DBUS_INTERFACE, name);

	ril_plugin_dbus_message_append_path_array(signal, dbus, fn);
	g_dbus_send_message(dbus->conn, signal);
}

static inline void ril_plugin_dbus_signal_imsi(struct ril_plugin_dbus *dbus,
				const char *name, const char *imsi)
{
	if (!imsi) imsi = RIL_DBUS_IMSI_AUTO;
	g_dbus_emit_signal(dbus->conn, RIL_DBUS_PATH, RIL_DBUS_INTERFACE,
			name, DBUS_TYPE_STRING, &imsi, DBUS_TYPE_INVALID);
}

static inline void ril_plugin_dbus_signal_path(struct ril_plugin_dbus *dbus,
				const char *name, const char *path)
{
	if (!path) path = "";
	g_dbus_emit_signal(dbus->conn, RIL_DBUS_PATH, RIL_DBUS_INTERFACE,
			name, DBUS_TYPE_STRING, &path, DBUS_TYPE_INVALID);
}

void ril_plugin_dbus_signal(struct ril_plugin_dbus *dbus, int mask)
{
	if (dbus) {
		if (mask & RIL_PLUGIN_SIGNAL_VOICE_IMSI) {
			ril_plugin_dbus_signal_imsi(dbus,
				RIL_DBUS_DEFAULT_VOICE_SIM_CHANGED_SIGNAL,
				dbus->plugin->default_voice_imsi);
		}
		if (mask & RIL_PLUGIN_SIGNAL_DATA_IMSI) {
			ril_plugin_dbus_signal_imsi(dbus,
				RIL_DBUS_DEFAULT_DATA_SIM_CHANGED_SIGNAL,
				dbus->plugin->default_data_imsi);
		}
		if (mask & RIL_PLUGIN_SIGNAL_ENABLED_SLOTS) {
			ril_plugin_dbus_signal_path_array(dbus,
				RIL_DBUS_ENABLED_MODEMS_CHANGED_SIGNAL,
				ril_plugin_dbus_enabled);
		}
		if (mask & RIL_PLUGIN_SIGNAL_VOICE_PATH) {
			ril_plugin_dbus_signal_path(dbus,
				RIL_DBUS_DEFAULT_VOICE_MODEM_CHANGED_SIGNAL,
				dbus->plugin->default_voice_path);
		}
		if (mask & RIL_PLUGIN_SIGNAL_DATA_PATH) {
			ril_plugin_dbus_signal_path(dbus,
				RIL_DBUS_DEFAULT_DATA_MODEM_CHANGED_SIGNAL,
				dbus->plugin->default_data_path);
		}
	}
}

void ril_plugin_dbus_signal_sim(struct ril_plugin_dbus *dbus, int index,
							gboolean present)
{
	dbus_bool_t value = present;
	g_dbus_emit_signal(dbus->conn, RIL_DBUS_PATH, RIL_DBUS_INTERFACE,
			RIL_DBUS_PRESENT_SIMS_CHANGED_SIGNAL,
			DBUS_TYPE_INT32, &index,
			DBUS_TYPE_BOOLEAN, &value,
			DBUS_TYPE_INVALID);
}

static DBusMessage *ril_plugin_dbus_reply_with_path_array(DBusMessage *msg,
	struct ril_plugin_dbus *dbus, ril_plugin_dbus_slot_select_fn fn)
{
	DBusMessage *reply = dbus_message_new_method_return(msg);

	ril_plugin_dbus_message_append_path_array(reply, dbus, fn);
	return reply;
}

static DBusMessage *ril_plugin_dbus_reply(DBusMessage *msg,
		struct ril_plugin_dbus *dbus,
		void (*append)(DBusMessageIter *, struct ril_plugin_dbus *))

{
	DBusMessage *reply = dbus_message_new_method_return(msg);
	DBusMessageIter iter;

	dbus_message_iter_init_append(reply, &iter);
	append(&iter, dbus);
	return reply;
}

static void ril_plugin_dbus_append_version(DBusMessageIter *it,
						struct ril_plugin_dbus *dbus)
{
	dbus_int32_t version = RIL_DBUS_INTERFACE_VERSION;

	dbus_message_iter_append_basic(it, DBUS_TYPE_INT32, &version);
}

static void ril_plugin_dbus_append_all(DBusMessageIter *it,
						struct ril_plugin_dbus *dbus)
{
	ril_plugin_dbus_append_version(it, dbus);
	ril_plugin_dbus_append_path_array(it, dbus, NULL);
	ril_plugin_dbus_append_path_array(it, dbus, ril_plugin_dbus_enabled);
	ril_plugin_dbus_append_imsi(it, dbus->plugin->default_data_imsi);
	ril_plugin_dbus_append_imsi(it, dbus->plugin->default_voice_imsi);
	ril_plugin_dbus_append_path(it, dbus->plugin->default_data_path);
	ril_plugin_dbus_append_path(it, dbus->plugin->default_voice_path);
}

static void ril_plugin_dbus_append_all2(DBusMessageIter *it,
						struct ril_plugin_dbus *dbus)
{
	ril_plugin_dbus_append_all(it, dbus);
	ril_plugin_dbus_append_boolean_array(it, dbus, ril_plugin_dbus_present);
}

static DBusMessage *ril_plugin_dbus_get_all(DBusConnection *conn,
						DBusMessage *msg, void *data)
{
	return ril_plugin_dbus_reply(msg, (struct ril_plugin_dbus *)data,
						ril_plugin_dbus_append_all);
}

static DBusMessage *ril_plugin_dbus_get_all2(DBusConnection *conn,
						DBusMessage *msg, void *data)
{
	return ril_plugin_dbus_reply(msg, (struct ril_plugin_dbus *)data,
						ril_plugin_dbus_append_all2);
}

static DBusMessage *ril_plugin_dbus_get_interface_version(DBusConnection *conn,
						DBusMessage *msg, void *data)
{
	return ril_plugin_dbus_reply(msg, (struct ril_plugin_dbus *)data,
						ril_plugin_dbus_append_version);
}

static DBusMessage *ril_plugin_dbus_get_available_modems(DBusConnection *conn,
						DBusMessage *msg, void *data)
{
	return ril_plugin_dbus_reply_with_path_array(msg,
					(struct ril_plugin_dbus *)data, NULL);
}

static DBusMessage *ril_plugin_dbus_get_enabled_modems(DBusConnection *conn,
						DBusMessage *msg, void *data)
{
	return ril_plugin_dbus_reply_with_path_array(msg,
		(struct ril_plugin_dbus *)data, ril_plugin_dbus_enabled);
}

static void ril_plugin_dbus_append_present_sims(DBusMessageIter *it,
						struct ril_plugin_dbus *dbus)
{
	ril_plugin_dbus_append_boolean_array(it, dbus, ril_plugin_dbus_present);
}

static DBusMessage *ril_plugin_dbus_get_present_sims(DBusConnection *conn,
						DBusMessage *msg, void *data)
{
	return ril_plugin_dbus_reply(msg, (struct ril_plugin_dbus *)data,
				ril_plugin_dbus_append_present_sims);
}

static DBusMessage *ril_plugin_dbus_reply_with_imsi(DBusMessage *msg,
							const char *imsi)
{
	DBusMessage *reply = dbus_message_new_method_return(msg);
	DBusMessageIter iter;

	dbus_message_iter_init_append(reply, &iter);
	ril_plugin_dbus_append_imsi(&iter, imsi);
	return reply;
}

static DBusMessage *ril_plugin_dbus_get_default_data_sim(DBusConnection *conn,
						DBusMessage *msg, void *data)
{
	struct ril_plugin_dbus *dbus = data;

	return ril_plugin_dbus_reply_with_imsi(msg,
					dbus->plugin->default_data_imsi);
}

static DBusMessage *ril_plugin_dbus_get_default_voice_sim(DBusConnection *conn,
						DBusMessage *msg, void *data)
{
	struct ril_plugin_dbus *dbus = data;

	return ril_plugin_dbus_reply_with_imsi(msg,
					dbus->plugin->default_voice_imsi);
}

static DBusMessage *ril_plugin_dbus_reply_with_path(DBusMessage *msg,
							const char *path)
{
	DBusMessage *reply = dbus_message_new_method_return(msg);
	DBusMessageIter iter;

	dbus_message_iter_init_append(reply, &iter);
	ril_plugin_dbus_append_path(&iter, path);
	return reply;
}

static DBusMessage *ril_plugin_dbus_get_default_data_modem(DBusConnection *conn,
						DBusMessage *msg, void *data)
{
	struct ril_plugin_dbus *dbus = data;

	return ril_plugin_dbus_reply_with_path(msg,
					dbus->plugin->default_data_path);
}

static DBusMessage *ril_plugin_dbus_get_default_voice_modem(DBusConnection *conn,
						DBusMessage *msg, void *data)
{
	struct ril_plugin_dbus *dbus = data;

	return ril_plugin_dbus_reply_with_path(msg,
					dbus->plugin->default_voice_path);
}

static DBusMessage *ril_plugin_dbus_set_enabled_modems(DBusConnection *conn,
						DBusMessage *msg, void *data)
{
	struct ril_plugin_dbus *dbus = data;
	DBusMessageIter iter;

	dbus_message_iter_init(msg, &iter);
	if (dbus_message_iter_get_arg_type(&iter) == DBUS_TYPE_ARRAY) {
		char **paths = NULL;
		DBusMessageIter array;

		dbus_message_iter_recurse(&iter, &array);
		while (dbus_message_iter_get_arg_type(&array) ==
						DBUS_TYPE_OBJECT_PATH) {
			DBusBasicValue value;

			dbus_message_iter_get_basic(&array, &value);
			paths = gutil_strv_add(paths, value.str);
			dbus_message_iter_next(&array);
		}

		ril_plugin_set_enabled_slots(dbus->plugin, paths);
		g_strfreev(paths);
		return dbus_message_new_method_return(msg);
	} else {
		return __ofono_error_invalid_args(msg);
	}
}

static DBusMessage *ril_plugin_dbus_set_imsi(struct ril_plugin_dbus *dbus,
		DBusMessage *msg, void (*apply)(struct ril_plugin *plugin,
							const char *imsi))
{
	DBusMessageIter iter;

	dbus_message_iter_init(msg, &iter);
	if (dbus_message_iter_get_arg_type(&iter) == DBUS_TYPE_STRING) {
		DBusBasicValue value;
		const char *imsi;

		dbus_message_iter_get_basic(&iter, &value);
		imsi = value.str;
		if (!g_strcmp0(imsi, RIL_DBUS_IMSI_AUTO)) imsi = NULL; 
		apply(dbus->plugin, imsi);
		return dbus_message_new_method_return(msg);
	} else {
		return __ofono_error_invalid_args(msg);
	}
}

static DBusMessage *ril_plugin_dbus_set_default_voice_sim(DBusConnection *conn,
						DBusMessage *msg, void *data)
{
	struct ril_plugin_dbus *dbus = data;

	GASSERT(conn == dbus->conn);
	return ril_plugin_dbus_set_imsi(dbus, msg,
					ril_plugin_set_default_voice_imsi);
}

static DBusMessage *ril_plugin_dbus_set_default_data_sim(DBusConnection *conn,
						DBusMessage *msg, void *data)
{
	struct ril_plugin_dbus *dbus = data;

	GASSERT(conn == dbus->conn);
	return ril_plugin_dbus_set_imsi(dbus, msg,
					ril_plugin_set_default_data_imsi);
}

static const GDBusMethodTable ril_plugin_dbus_methods[] = {
	{ GDBUS_METHOD("GetAll", NULL,
			GDBUS_ARGS({"version", "i" },
			{"availableModems", "ao" },
			{"enabledModems", "ao" },
			{"defaultDataSim", "s" },
			{"defaultVoiceSim", "s" },
			{"defaultDataModem", "s" },
			{"defaultVoiceModem" , "s"}),
			ril_plugin_dbus_get_all) },
	{ GDBUS_METHOD("GetAll2", NULL,
			GDBUS_ARGS({"version", "i" },
			{"availableModems", "ao" },
			{"enabledModems", "ao" },
			{"defaultDataSim", "s" },
			{"defaultVoiceSim", "s" },
			{"defaultDataModem", "s" },
			{"defaultVoiceModem" , "s"},
			{"presentSims" , "ab"}),
			ril_plugin_dbus_get_all2) },
	{ GDBUS_METHOD("GetInterfaceVersion",
			NULL, GDBUS_ARGS({ "version", "i" }),
			ril_plugin_dbus_get_interface_version) },
	{ GDBUS_METHOD("GetAvailableModems",
			NULL, GDBUS_ARGS({ "modems", "ao" }),
			ril_plugin_dbus_get_available_modems) },
	{ GDBUS_METHOD("GetEnabledModems",
			NULL, GDBUS_ARGS({ "modems", "ao" }),
			ril_plugin_dbus_get_enabled_modems) },
	{ GDBUS_METHOD("GetPresentSims",
			NULL, GDBUS_ARGS({ "presentSims", "ab" }),
			ril_plugin_dbus_get_present_sims) },
	{ GDBUS_METHOD("GetDefaultDataSim",
			NULL, GDBUS_ARGS({ "imsi", "s" }),
			ril_plugin_dbus_get_default_data_sim) },
	{ GDBUS_METHOD("GetDefaultVoiceSim",
			NULL, GDBUS_ARGS({ "imsi", "s" }),
			ril_plugin_dbus_get_default_voice_sim) },
	{ GDBUS_METHOD("GetDefaultDataModem",
			NULL, GDBUS_ARGS({ "path", "s" }),
			ril_plugin_dbus_get_default_data_modem) },
	{ GDBUS_METHOD("GetDefaultVoiceModem",
			NULL, GDBUS_ARGS({ "path", "s" }),
			ril_plugin_dbus_get_default_voice_modem) },
	{ GDBUS_METHOD("SetEnabledModems",
			GDBUS_ARGS({ "modems", "ao" }), NULL,
			ril_plugin_dbus_set_enabled_modems) },
	{ GDBUS_METHOD("SetDefaultDataSim",
			GDBUS_ARGS({ "imsi", "s" }), NULL,
			ril_plugin_dbus_set_default_data_sim) },
	{ GDBUS_METHOD("SetDefaultVoiceSim",
			GDBUS_ARGS({ "imsi", "s" }), NULL,
			ril_plugin_dbus_set_default_voice_sim) },
	{ }
};

static const GDBusSignalTable ril_plugin_dbus_signals[] = {
	{ GDBUS_SIGNAL(RIL_DBUS_ENABLED_MODEMS_CHANGED_SIGNAL,
			GDBUS_ARGS({ "modems", "ao" })) },
	{ GDBUS_SIGNAL(RIL_DBUS_PRESENT_SIMS_CHANGED_SIGNAL,
			GDBUS_ARGS({"index", "i" },
			{"present" , "b"})) },
	{ GDBUS_SIGNAL(RIL_DBUS_DEFAULT_DATA_SIM_CHANGED_SIGNAL,
			GDBUS_ARGS({ "imsi", "s" })) },
	{ GDBUS_SIGNAL(RIL_DBUS_DEFAULT_VOICE_SIM_CHANGED_SIGNAL,
			GDBUS_ARGS({ "imsi", "s" })) },
	{ GDBUS_SIGNAL(RIL_DBUS_DEFAULT_DATA_MODEM_CHANGED_SIGNAL,
			GDBUS_ARGS({ "path", "s" })) },
	{ GDBUS_SIGNAL(RIL_DBUS_DEFAULT_VOICE_MODEM_CHANGED_SIGNAL,
			GDBUS_ARGS({ "path", "s" })) },
	{ }
};

struct ril_plugin_dbus *ril_plugin_dbus_new(struct ril_plugin *plugin)
{
	struct ril_plugin_dbus *dbus = g_new0(struct ril_plugin_dbus, 1);

	dbus->conn = dbus_connection_ref(ofono_dbus_get_connection());
	dbus->plugin = plugin;
	if (g_dbus_register_interface(dbus->conn, RIL_DBUS_PATH,
			RIL_DBUS_INTERFACE, ril_plugin_dbus_methods,
			ril_plugin_dbus_signals, NULL, dbus, NULL)) {
		return dbus;
	} else {
		ofono_error("RIL D-Bus register failed");
		ril_plugin_dbus_free(dbus);
		return NULL;
	}
}

void ril_plugin_dbus_free(struct ril_plugin_dbus *dbus)
{
	if (dbus) {
		g_dbus_unregister_interface(dbus->conn, RIL_DBUS_PATH,
							RIL_DBUS_INTERFACE);
		dbus_connection_unref(dbus->conn);
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
