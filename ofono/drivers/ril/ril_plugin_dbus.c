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

#include "ril_plugin.h"

#include <ofono/log.h>
#include <ofono/dbus.h>

#include <gutil_strv.h>
#include <gutil_log.h>
#include <gdbus.h>

#include "ofono.h"

typedef void (*ril_plugin_dbus_append_fn)(DBusMessageIter *it,
					struct ril_plugin_dbus *dbus);
typedef gboolean (*ril_plugin_dbus_slot_select_fn)
					(const struct ril_slot_info *slot);
typedef const char *(*ril_plugin_dbus_slot_string_fn)
					(const struct ril_slot_info *slot);

struct ril_plugin_dbus_request {
	DBusMessage *msg;
	ril_plugin_dbus_append_fn fn;
};

struct ril_plugin_dbus {
	struct ril_plugin *plugin;
	DBusConnection *conn;
	gboolean block_imei_req;
	GSList *blocked_imei_req;
	guint mms_watch;
};

#define RIL_DBUS_PATH               "/"
#define RIL_DBUS_INTERFACE          "org.nemomobile.ofono.ModemManager"
#define RIL_DBUS_INTERFACE_VERSION  (5)

#define RIL_DBUS_SIGNAL_ENABLED_MODEMS_CHANGED      "EnabledModemsChanged"
#define RIL_DBUS_SIGNAL_PRESENT_SIMS_CHANGED        "PresentSimsChanged"
#define RIL_DBUS_SIGNAL_DEFAULT_VOICE_SIM_CHANGED   "DefaultVoiceSimChanged"
#define RIL_DBUS_SIGNAL_DEFAULT_DATA_SIM_CHANGED    "DefaultDataSimChanged"
#define RIL_DBUS_SIGNAL_DEFAULT_VOICE_MODEM_CHANGED "DefaultVoiceModemChanged"
#define RIL_DBUS_SIGNAL_DEFAULT_DATA_MODEM_CHANGED  "DefaultDataModemChanged"
#define RIL_DBUS_SIGNAL_MMS_SIM_CHANGED             "MmsSimChanged"
#define RIL_DBUS_SIGNAL_MMS_MODEM_CHANGED           "MmsModemChanged"
#define RIL_DBUS_SIGNAL_READY_CHANGED               "ReadyChanged"
#define RIL_DBUS_IMSI_AUTO                          "auto"

static gboolean ril_plugin_dbus_enabled(const struct ril_slot_info *slot)
{
	return slot->enabled;
}

static gboolean ril_plugin_dbus_present(const struct ril_slot_info *slot)
{
	return slot->sim_present;
}

static const char *ril_plugin_dbus_imei(const struct ril_slot_info *slot)
{
	return slot->imei;
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

static void ril_plugin_dbus_append_string_array(DBusMessageIter *it,
	struct ril_plugin_dbus *dbus, ril_plugin_dbus_slot_string_fn fn)
{
	DBusMessageIter array;
	const struct ril_slot_info *const *ptr = dbus->plugin->slots;

	dbus_message_iter_open_container(it, DBUS_TYPE_ARRAY,
				DBUS_TYPE_STRING_AS_STRING, &array);

	while (*ptr) {
		const struct ril_slot_info *slot = *ptr++;
		const char *str = fn(slot);

		if (!str) str = "";
		dbus_message_iter_append_basic(&array, DBUS_TYPE_STRING, &str);
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

static void ril_plugin_dbus_append_boolean(DBusMessageIter *it, dbus_bool_t b)
{
	dbus_message_iter_append_basic(it, DBUS_TYPE_BOOLEAN, &b);
}

static void ril_plugin_dbus_append_string(DBusMessageIter *it, const char *str)
{
	if (!str) str = "";
	dbus_message_iter_append_basic(it, DBUS_TYPE_STRING, &str);
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

static inline void ril_plugin_dbus_signal_string(struct ril_plugin_dbus *dbus,
				const char *name, const char *str)
{
	if (!str) str = "";
	g_dbus_emit_signal(dbus->conn, RIL_DBUS_PATH, RIL_DBUS_INTERFACE,
			name, DBUS_TYPE_STRING, &str, DBUS_TYPE_INVALID);
}

static inline void ril_plugin_dbus_signal_boolean(struct ril_plugin_dbus *dbus,
				const char *name, dbus_bool_t value)
{
	g_dbus_emit_signal(dbus->conn, RIL_DBUS_PATH, RIL_DBUS_INTERFACE,
			name, DBUS_TYPE_BOOLEAN, &value, DBUS_TYPE_INVALID);
}

void ril_plugin_dbus_signal(struct ril_plugin_dbus *dbus, int mask)
{
	if (dbus) {
		if (mask & RIL_PLUGIN_SIGNAL_VOICE_IMSI) {
			ril_plugin_dbus_signal_imsi(dbus,
				RIL_DBUS_SIGNAL_DEFAULT_VOICE_SIM_CHANGED,
				dbus->plugin->default_voice_imsi);
		}
		if (mask & RIL_PLUGIN_SIGNAL_DATA_IMSI) {
			ril_plugin_dbus_signal_imsi(dbus,
				RIL_DBUS_SIGNAL_DEFAULT_DATA_SIM_CHANGED,
				dbus->plugin->default_data_imsi);
		}
		if (mask & RIL_PLUGIN_SIGNAL_MMS_IMSI) {
			ril_plugin_dbus_signal_string(dbus,
				RIL_DBUS_SIGNAL_MMS_SIM_CHANGED,
				dbus->plugin->mms_imsi);
		}
		if (mask & RIL_PLUGIN_SIGNAL_ENABLED_SLOTS) {
			ril_plugin_dbus_signal_path_array(dbus,
				RIL_DBUS_SIGNAL_ENABLED_MODEMS_CHANGED,
				ril_plugin_dbus_enabled);
		}
		if (mask & RIL_PLUGIN_SIGNAL_VOICE_PATH) {
			ril_plugin_dbus_signal_string(dbus,
				RIL_DBUS_SIGNAL_DEFAULT_VOICE_MODEM_CHANGED,
				dbus->plugin->default_voice_path);
		}
		if (mask & RIL_PLUGIN_SIGNAL_DATA_PATH) {
			ril_plugin_dbus_signal_string(dbus,
				RIL_DBUS_SIGNAL_DEFAULT_DATA_MODEM_CHANGED,
				dbus->plugin->default_data_path);
		}
		if (mask & RIL_PLUGIN_SIGNAL_MMS_PATH) {
			ril_plugin_dbus_signal_string(dbus,
				RIL_DBUS_SIGNAL_MMS_MODEM_CHANGED,
				dbus->plugin->mms_path);
		}
		if (mask & RIL_PLUGIN_SIGNAL_READY) {
			ril_plugin_dbus_signal_boolean(dbus,
				RIL_DBUS_SIGNAL_READY_CHANGED,
				dbus->plugin->ready);
		}
	}
}

void ril_plugin_dbus_signal_sim(struct ril_plugin_dbus *dbus, int index,
							gboolean present)
{
	dbus_bool_t value = present;
	g_dbus_emit_signal(dbus->conn, RIL_DBUS_PATH, RIL_DBUS_INTERFACE,
			RIL_DBUS_SIGNAL_PRESENT_SIMS_CHANGED,
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
		struct ril_plugin_dbus *dbus, ril_plugin_dbus_append_fn append)
{
	DBusMessage *reply = dbus_message_new_method_return(msg);
	DBusMessageIter iter;

	dbus_message_iter_init_append(reply, &iter);
	append(&iter, dbus);
	return reply;
}

static void ril_plugin_dbus_unblock_request(gpointer data, gpointer user_data)
{
	struct ril_plugin_dbus_request *req = data;

	DBG("unblocking IMEI request %p", req);
	__ofono_dbus_pending_reply(&req->msg, ril_plugin_dbus_reply(req->msg,
				(struct ril_plugin_dbus *)user_data, req->fn));
	g_free(req);
}

static void ril_plugin_dbus_cancel_request(gpointer data)
{
	struct ril_plugin_dbus_request *req = data;

	DBG("canceling IMEI request %p", req);
	__ofono_dbus_pending_reply(&req->msg, __ofono_error_canceled(req->msg));
	g_free(req);
}

void ril_plugin_dbus_block_imei_requests(struct ril_plugin_dbus *dbus,
								gboolean block)
{
	dbus->block_imei_req = block;
	if (!block && dbus->blocked_imei_req) {
		g_slist_foreach(dbus->blocked_imei_req,
				ril_plugin_dbus_unblock_request, dbus);
		g_slist_free(dbus->blocked_imei_req);
		dbus->blocked_imei_req = NULL;
	}
}

static DBusMessage *ril_plugin_dbus_imei_reply(DBusMessage *msg,
		struct ril_plugin_dbus *dbus, ril_plugin_dbus_append_fn fn)
{
	if (dbus->block_imei_req) {
		struct ril_plugin_dbus_request *req =
			g_new(struct ril_plugin_dbus_request, 1);

		req->msg = dbus_message_ref(msg);
		req->fn = fn;
		dbus->blocked_imei_req = g_slist_append(dbus->blocked_imei_req,
									req);
		DBG("blocking IMEI request %p", req);
		return NULL;
	} else {
		return ril_plugin_dbus_reply(msg, dbus, fn);
	}
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

static void ril_plugin_dbus_append_all3(DBusMessageIter *it,
						struct ril_plugin_dbus *dbus)
{
	ril_plugin_dbus_append_all2(it, dbus);
	ril_plugin_dbus_append_string_array(it, dbus, ril_plugin_dbus_imei);
}

static void ril_plugin_dbus_append_all4(DBusMessageIter *it,
						struct ril_plugin_dbus *dbus)
{
	ril_plugin_dbus_append_all3(it, dbus);
	ril_plugin_dbus_append_string(it, dbus->plugin->mms_imsi);
	ril_plugin_dbus_append_path(it, dbus->plugin->mms_path);
}

static void ril_plugin_dbus_append_all5(DBusMessageIter *it,
						struct ril_plugin_dbus *dbus)
{
	ril_plugin_dbus_append_all4(it, dbus);
	ril_plugin_dbus_append_boolean(it, dbus->plugin->ready);
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

static DBusMessage *ril_plugin_dbus_get_all3(DBusConnection *conn,
						DBusMessage *msg, void *data)
{
	return ril_plugin_dbus_imei_reply(msg, (struct ril_plugin_dbus *)data,
						ril_plugin_dbus_append_all3);
}

static DBusMessage *ril_plugin_dbus_get_all4(DBusConnection *conn,
						DBusMessage *msg, void *data)
{
	return ril_plugin_dbus_imei_reply(msg, (struct ril_plugin_dbus *)data,
						ril_plugin_dbus_append_all4);
}

static DBusMessage *ril_plugin_dbus_get_all5(DBusConnection *conn,
						DBusMessage *msg, void *data)
{
	return ril_plugin_dbus_imei_reply(msg, (struct ril_plugin_dbus *)data,
						ril_plugin_dbus_append_all5);
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

static void ril_plugin_dbus_append_imei_array(DBusMessageIter *it,
						struct ril_plugin_dbus *dbus)
{
	ril_plugin_dbus_append_string_array(it, dbus, ril_plugin_dbus_imei);
}

static DBusMessage *ril_plugin_dbus_get_imei(DBusConnection *conn,
						DBusMessage *msg, void *data)
{
	return ril_plugin_dbus_imei_reply(msg, (struct ril_plugin_dbus *)data,
					ril_plugin_dbus_append_imei_array);
}

static DBusMessage *ril_plugin_dbus_reply_with_string(DBusMessage *msg,
							const char *str)
{
	DBusMessage *reply = dbus_message_new_method_return(msg);
	DBusMessageIter iter;

	dbus_message_iter_init_append(reply, &iter);
	ril_plugin_dbus_append_string(&iter, str);
	return reply;
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

static DBusMessage *ril_plugin_dbus_get_mms_sim(DBusConnection *conn,
						DBusMessage *msg, void *data)
{
	struct ril_plugin_dbus *dbus = data;

	return ril_plugin_dbus_reply_with_string(msg, dbus->plugin->mms_imsi);
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

static DBusMessage *ril_plugin_dbus_get_mms_modem(DBusConnection *conn,
						DBusMessage *msg, void *data)
{
	struct ril_plugin_dbus *dbus = data;

	return ril_plugin_dbus_reply_with_path(msg, dbus->plugin->mms_path);
}

static DBusMessage *ril_plugin_dbus_get_ready(DBusConnection *conn,
						DBusMessage *msg, void *data)
{
	struct ril_plugin_dbus *dbus = data;
	DBusMessage *reply = dbus_message_new_method_return(msg);
	DBusMessageIter it;

	dbus_message_iter_init_append(reply, &it);
	ril_plugin_dbus_append_boolean(&it, dbus->plugin->ready);
	return reply;
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

static void ril_plugin_dbus_mms_disconnect(DBusConnection *conn, void *data)
{
	struct ril_plugin_dbus *dbus = data;

	dbus->mms_watch = 0;
	if (dbus->plugin->mms_imsi) {
		DBG("MMS client is gone");
		ril_plugin_set_mms_imsi(dbus->plugin, NULL);
	}
}

static DBusMessage *ril_plugin_dbus_set_mms_sim(DBusConnection *conn,
						DBusMessage *msg, void *data)
{
	DBusMessageIter iter;
	struct ril_plugin_dbus *dbus = data;

	GASSERT(conn == dbus->conn);
	dbus_message_iter_init(msg, &iter);
	if (dbus_message_iter_get_arg_type(&iter) == DBUS_TYPE_STRING) {
		DBusBasicValue value;
		const char *imsi;

		dbus_message_iter_get_basic(&iter, &value);
		imsi = value.str;

		/*
		 * MMS IMSI is not persistent and has to be eventually
		 * reset by the client or cleaned up if the client
		 * unexpectedly disappears.
		 */
		if (ril_plugin_set_mms_imsi(dbus->plugin, imsi)) {

			/*
			 * Clear the previous MMS owner
			 */
			if (dbus->mms_watch) {
				g_dbus_remove_watch(dbus->conn, dbus->mms_watch);
				dbus->mms_watch = 0;
			}

			if (dbus->plugin->mms_imsi &&
						dbus->plugin->mms_imsi[0]) {
				/*
				 * This client becomes the owner
				 */
				DBG("Owner: %s", dbus_message_get_sender(msg));
				dbus->mms_watch =
					g_dbus_add_disconnect_watch(dbus->conn,
						dbus_message_get_sender(msg),
						ril_plugin_dbus_mms_disconnect,
						dbus, NULL);
			}

			return ril_plugin_dbus_reply_with_string(msg,
						dbus->plugin->mms_path);
		} else {
			return __ofono_error_not_available(msg);
		}
	} else {
		return __ofono_error_invalid_args(msg);
	}
}

/*
 * The client can call GetInterfaceVersion followed by the appropriate
 * GetAllx call to get all settings in two steps. Alternatively, it can
 * call GetAll followed by GetAllx based on the interface version returned
 * by GetAll. In either case, two D-Bus calls are required, unless the
 * client is willing to make the assumption about the ofono version it's
 * talking to.
 */

#define RIL_DBUS_GET_ALL_ARGS \
	{"version", "i" }, \
	{"availableModems", "ao" }, \
	{"enabledModems", "ao" }, \
	{"defaultDataSim", "s" }, \
	{"defaultVoiceSim", "s" }, \
	{"defaultDataModem", "s" }, \
	{"defaultVoiceModem" , "s"}
#define RIL_DBUS_GET_ALL2_ARGS \
	RIL_DBUS_GET_ALL_ARGS, \
	{"presentSims" , "ab"}
#define RIL_DBUS_GET_ALL3_ARGS \
	RIL_DBUS_GET_ALL2_ARGS, \
	{"imei" , "as"}
#define RIL_DBUS_GET_ALL4_ARGS \
	RIL_DBUS_GET_ALL3_ARGS, \
	{"mmsSim", "s" }, \
	{"mmsModem" , "s"}
#define RIL_DBUS_GET_ALL5_ARGS \
	RIL_DBUS_GET_ALL4_ARGS, \
	{"ready" , "b"}

static const GDBusMethodTable ril_plugin_dbus_methods[] = {
	{ GDBUS_METHOD("GetAll",
			NULL, GDBUS_ARGS(RIL_DBUS_GET_ALL_ARGS),
			ril_plugin_dbus_get_all) },
	{ GDBUS_METHOD("GetAll2",
			NULL, GDBUS_ARGS(RIL_DBUS_GET_ALL2_ARGS),
			ril_plugin_dbus_get_all2) },
	{ GDBUS_ASYNC_METHOD("GetAll3",
			NULL, GDBUS_ARGS(RIL_DBUS_GET_ALL3_ARGS),
			ril_plugin_dbus_get_all3) },
	{ GDBUS_ASYNC_METHOD("GetAll4",
			NULL, GDBUS_ARGS(RIL_DBUS_GET_ALL4_ARGS),
			ril_plugin_dbus_get_all4) },
	{ GDBUS_ASYNC_METHOD("GetAll5",
			NULL, GDBUS_ARGS(RIL_DBUS_GET_ALL5_ARGS),
			ril_plugin_dbus_get_all5) },
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
	{ GDBUS_ASYNC_METHOD("GetIMEI",
			NULL, GDBUS_ARGS({ "imei", "as" }),
			ril_plugin_dbus_get_imei) },
	{ GDBUS_METHOD("GetDefaultDataSim",
			NULL, GDBUS_ARGS({ "imsi", "s" }),
			ril_plugin_dbus_get_default_data_sim) },
	{ GDBUS_METHOD("GetDefaultVoiceSim",
			NULL, GDBUS_ARGS({ "imsi", "s" }),
			ril_plugin_dbus_get_default_voice_sim) },
	{ GDBUS_METHOD("GetMmsSim",
			NULL, GDBUS_ARGS({ "imsi", "s" }),
			ril_plugin_dbus_get_mms_sim) },
	{ GDBUS_METHOD("GetDefaultDataModem",
			NULL, GDBUS_ARGS({ "path", "s" }),
			ril_plugin_dbus_get_default_data_modem) },
	{ GDBUS_METHOD("GetDefaultVoiceModem",
			NULL, GDBUS_ARGS({ "path", "s" }),
			ril_plugin_dbus_get_default_voice_modem) },
	{ GDBUS_METHOD("GetMmsModem",
			NULL, GDBUS_ARGS({ "path", "s" }),
			ril_plugin_dbus_get_mms_modem) },
	{ GDBUS_METHOD("GetReady",
			NULL, GDBUS_ARGS({ "ready", "b" }),
			ril_plugin_dbus_get_ready) },
	{ GDBUS_METHOD("SetEnabledModems",
			GDBUS_ARGS({ "modems", "ao" }), NULL,
			ril_plugin_dbus_set_enabled_modems) },
	{ GDBUS_METHOD("SetDefaultDataSim",
			GDBUS_ARGS({ "imsi", "s" }), NULL,
			ril_plugin_dbus_set_default_data_sim) },
	{ GDBUS_METHOD("SetDefaultVoiceSim",
			GDBUS_ARGS({ "imsi", "s" }), NULL,
			ril_plugin_dbus_set_default_voice_sim) },
	{ GDBUS_METHOD("SetMmsSim",
			GDBUS_ARGS({ "imsi", "s" }), NULL,
			ril_plugin_dbus_set_mms_sim) },
	{ }
};

static const GDBusSignalTable ril_plugin_dbus_signals[] = {
	{ GDBUS_SIGNAL(RIL_DBUS_SIGNAL_ENABLED_MODEMS_CHANGED,
			GDBUS_ARGS({ "modems", "ao" })) },
	{ GDBUS_SIGNAL(RIL_DBUS_SIGNAL_PRESENT_SIMS_CHANGED,
			GDBUS_ARGS({"index", "i" },
			{"present" , "b"})) },
	{ GDBUS_SIGNAL(RIL_DBUS_SIGNAL_DEFAULT_DATA_SIM_CHANGED,
			GDBUS_ARGS({ "imsi", "s" })) },
	{ GDBUS_SIGNAL(RIL_DBUS_SIGNAL_DEFAULT_VOICE_SIM_CHANGED,
			GDBUS_ARGS({ "imsi", "s" })) },
	{ GDBUS_SIGNAL(RIL_DBUS_SIGNAL_DEFAULT_DATA_MODEM_CHANGED,
			GDBUS_ARGS({ "path", "s" })) },
	{ GDBUS_SIGNAL(RIL_DBUS_SIGNAL_DEFAULT_VOICE_MODEM_CHANGED,
			GDBUS_ARGS({ "path", "s" })) },
	{ GDBUS_SIGNAL(RIL_DBUS_SIGNAL_MMS_SIM_CHANGED,
			GDBUS_ARGS({ "imsi", "s" })) },
	{ GDBUS_SIGNAL(RIL_DBUS_SIGNAL_MMS_MODEM_CHANGED,
			GDBUS_ARGS({ "path", "s" })) },
	{ GDBUS_SIGNAL(RIL_DBUS_SIGNAL_READY_CHANGED,
			GDBUS_ARGS({ "ready", "b" })) },
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
		if (dbus->mms_watch) {
			g_dbus_remove_watch(dbus->conn, dbus->mms_watch);
		}

		g_slist_free_full(dbus->blocked_imei_req,
					ril_plugin_dbus_cancel_request);
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
