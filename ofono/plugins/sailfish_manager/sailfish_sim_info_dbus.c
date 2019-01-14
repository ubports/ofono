/*
 *  oFono - Open Source Telephony
 *
 *  Copyright (C) 2017-2019 Jolla Ltd.
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

#include "sailfish_sim_info.h"

#include <ofono/dbus.h>
#include <ofono/watch.h>

#include <gdbus.h>

#include "ofono.h"

enum watch_event_id {
	WATCH_EVENT_MODEM,
	WATCH_EVENT_COUNT
};

enum sim_info_event_id {
	SIM_INFO_EVENT_ICCID,
	SIM_INFO_EVENT_IMSI,
	SIM_INFO_EVENT_SPN,
	SIM_INFO_EVENT_COUNT
};

struct sailfish_sim_info_dbus {
	struct sailfish_sim_info *info;
	struct ofono_watch *watch;
	DBusConnection *conn;
	gulong watch_event_id[WATCH_EVENT_COUNT];
	gulong info_event_id[SIM_INFO_EVENT_COUNT];
};

#define SIM_INFO_DBUS_INTERFACE             "org.nemomobile.ofono.SimInfo"
#define SIM_INFO_DBUS_INTERFACE_VERSION     (1)

#define SIM_INFO_DBUS_ICCID_CHANGED_SIGNAL  "CardIdentifierChanged"
#define SIM_INFO_DBUS_IMSI_CHANGED_SIGNAL   "SubscriberIdentityChanged"
#define SIM_INFO_DBUS_SPN_CHANGED_SIGNAL    "ServiceProviderNameChanged"

static void sailfish_sim_info_dbus_append_version(DBusMessageIter *it)
{
	const dbus_int32_t version = SIM_INFO_DBUS_INTERFACE_VERSION;

	dbus_message_iter_append_basic(it, DBUS_TYPE_INT32, &version);
}

static void sailfish_sim_info_dbus_append_string(DBusMessageIter *it,
							const char *str)
{
	if (!str) str = "";
	dbus_message_iter_append_basic(it, DBUS_TYPE_STRING, &str);
}

static DBusMessage *sailfish_sim_info_dbus_reply_with_string(DBusMessage *msg,
							const char *str)
{
	DBusMessage *reply = dbus_message_new_method_return(msg);
	DBusMessageIter iter;

	dbus_message_iter_init_append(reply, &iter);
	sailfish_sim_info_dbus_append_string(&iter, str);
	return reply;
}

static DBusMessage *sailfish_sim_info_dbus_get_all(DBusConnection *conn,
						DBusMessage *msg, void *data)
{
	struct sailfish_sim_info_dbus *dbus = data;
	struct sailfish_sim_info *info = dbus->info;
	DBusMessage *reply = dbus_message_new_method_return(msg);
	DBusMessageIter it;

	dbus_message_iter_init_append(reply, &it);
	sailfish_sim_info_dbus_append_version(&it);
	sailfish_sim_info_dbus_append_string(&it, info->iccid);
	sailfish_sim_info_dbus_append_string(&it, info->imsi);
	sailfish_sim_info_dbus_append_string(&it, info->spn);
	return reply;
}

static DBusMessage *sailfish_sim_info_dbus_get_version(DBusConnection *dc,
						DBusMessage *msg, void *data)
{
	DBusMessage *reply = dbus_message_new_method_return(msg);
	DBusMessageIter it;

	dbus_message_iter_init_append(reply, &it);
	sailfish_sim_info_dbus_append_version(&it);
	return reply;
}

static DBusMessage *sailfish_sim_info_dbus_get_iccid(DBusConnection *conn,
						DBusMessage *msg, void *data)
{
	struct sailfish_sim_info_dbus *dbus = data;

	return sailfish_sim_info_dbus_reply_with_string(msg, dbus->info->iccid);
}

static DBusMessage *sailfish_sim_info_dbus_get_imsi(DBusConnection *conn,
						DBusMessage *msg, void *data)
{
	struct sailfish_sim_info_dbus *dbus = data;

	return sailfish_sim_info_dbus_reply_with_string(msg, dbus->info->imsi);
}

static DBusMessage *sailfish_sim_info_dbus_get_spn(DBusConnection *conn,
						DBusMessage *msg, void *data)
{
	struct sailfish_sim_info_dbus *dbus = data;

	return sailfish_sim_info_dbus_reply_with_string(msg, dbus->info->spn);
}

#define SIM_INFO_DBUS_VERSION_ARG   {"version", "i"}
#define SIM_INFO_DBUS_ICCID_ARG     {"iccid", "s"}
#define SIM_INFO_DBUS_IMSI_ARG      {"imsi", "s"}
#define SIM_INFO_DBUS_SPN_ARG       {"spn" , "s"}

#define SIM_INFO_DBUS_GET_ALL_ARGS \
	SIM_INFO_DBUS_VERSION_ARG, \
	SIM_INFO_DBUS_ICCID_ARG, \
	SIM_INFO_DBUS_IMSI_ARG, \
	SIM_INFO_DBUS_SPN_ARG

static const GDBusMethodTable sailfish_sim_info_dbus_methods[] = {
	{ GDBUS_METHOD("GetAll",
			NULL, GDBUS_ARGS(SIM_INFO_DBUS_GET_ALL_ARGS),
			sailfish_sim_info_dbus_get_all) },
	{ GDBUS_METHOD("GetInterfaceVersion",
			NULL, GDBUS_ARGS(SIM_INFO_DBUS_VERSION_ARG),
			sailfish_sim_info_dbus_get_version) },
	{ GDBUS_METHOD("GetCardIdentifier",
			NULL, GDBUS_ARGS(SIM_INFO_DBUS_ICCID_ARG),
			sailfish_sim_info_dbus_get_iccid) },
	{ GDBUS_METHOD("GetSubscriberIdentity",
			NULL, GDBUS_ARGS(SIM_INFO_DBUS_IMSI_ARG),
			sailfish_sim_info_dbus_get_imsi) },
	{ GDBUS_METHOD("GetServiceProviderName",
			NULL, GDBUS_ARGS(SIM_INFO_DBUS_SPN_ARG),
			sailfish_sim_info_dbus_get_spn) },
	{ }
};

static const GDBusSignalTable sailfish_sim_info_dbus_signals[] = {
	{ GDBUS_SIGNAL(SIM_INFO_DBUS_ICCID_CHANGED_SIGNAL,
			GDBUS_ARGS(SIM_INFO_DBUS_ICCID_ARG)) },
	{ GDBUS_SIGNAL(SIM_INFO_DBUS_IMSI_CHANGED_SIGNAL,
			GDBUS_ARGS(SIM_INFO_DBUS_IMSI_ARG)) },
	{ GDBUS_SIGNAL(SIM_INFO_DBUS_SPN_CHANGED_SIGNAL,
			GDBUS_ARGS(SIM_INFO_DBUS_SPN_ARG)) },
	{ }
};

static void sailfish_sim_info_dbus_modem_cb(struct ofono_watch *watch,
								void *data)
{
	if (watch->modem) {
		ofono_modem_add_interface(watch->modem,
						SIM_INFO_DBUS_INTERFACE);
	}
}

static void sailfish_sim_info_dbus_emit(struct sailfish_sim_info_dbus *dbus,
					const char *signal, const char *value)
{
	const char *arg = value;

	if (!arg) arg = "";
	g_dbus_emit_signal(dbus->conn, dbus->info->path,
			SIM_INFO_DBUS_INTERFACE, signal,
			DBUS_TYPE_STRING, &arg, DBUS_TYPE_INVALID);
}

static void sailfish_sim_info_dbus_iccid_cb(struct sailfish_sim_info *info,
								void *data)
{
	sailfish_sim_info_dbus_emit((struct sailfish_sim_info_dbus *)data,
		SIM_INFO_DBUS_ICCID_CHANGED_SIGNAL, info->iccid);
}

static void sailfish_sim_info_dbus_imsi_cb(struct sailfish_sim_info *info,
								void *data)
{
	sailfish_sim_info_dbus_emit((struct sailfish_sim_info_dbus *)data,
		SIM_INFO_DBUS_IMSI_CHANGED_SIGNAL, info->imsi);
}

static void sailfish_sim_info_dbus_spn_cb(struct sailfish_sim_info *info,
								void *data)
{
	sailfish_sim_info_dbus_emit((struct sailfish_sim_info_dbus *)data,
		SIM_INFO_DBUS_SPN_CHANGED_SIGNAL, info->spn);
}

struct sailfish_sim_info_dbus *sailfish_sim_info_dbus_new
					(struct sailfish_sim_info *info)
{
	struct sailfish_sim_info_dbus *dbus =
		g_slice_new0(struct sailfish_sim_info_dbus);

	DBG("%s", info->path);
	dbus->info = sailfish_sim_info_ref(info);
	dbus->watch = ofono_watch_new(info->path);
	dbus->conn = dbus_connection_ref(ofono_dbus_get_connection());

	/* Register D-Bus interface */
	if (g_dbus_register_interface(dbus->conn, dbus->info->path,
					SIM_INFO_DBUS_INTERFACE,
					sailfish_sim_info_dbus_methods,
					sailfish_sim_info_dbus_signals,
					NULL, dbus, NULL)) {
		if (dbus->watch->modem) {
			ofono_modem_add_interface(dbus->watch->modem,
					SIM_INFO_DBUS_INTERFACE);
		}

		dbus->watch_event_id[WATCH_EVENT_MODEM] =
			ofono_watch_add_modem_changed_handler(dbus->watch,
				sailfish_sim_info_dbus_modem_cb, dbus);
		dbus->info_event_id[SIM_INFO_EVENT_ICCID] =
			sailfish_sim_info_add_iccid_changed_handler(info,
				sailfish_sim_info_dbus_iccid_cb, dbus);
		dbus->info_event_id[SIM_INFO_EVENT_IMSI] =
			sailfish_sim_info_add_imsi_changed_handler(info,
				sailfish_sim_info_dbus_imsi_cb, dbus);
		dbus->info_event_id[SIM_INFO_EVENT_SPN] =
			sailfish_sim_info_add_spn_changed_handler(info,
				sailfish_sim_info_dbus_spn_cb, dbus);

		return dbus;
	} else {
		ofono_error("SimInfo D-Bus register failed");
		sailfish_sim_info_dbus_free(dbus);
		return NULL;
	}
}

struct sailfish_sim_info_dbus *sailfish_sim_info_dbus_new_path
							(const char *path)
{
	struct sailfish_sim_info_dbus *dbus = NULL;
	struct sailfish_sim_info *info = sailfish_sim_info_new(path);

	if (info) {
		dbus = sailfish_sim_info_dbus_new(info);
		sailfish_sim_info_unref(info);
	}

	return dbus;
}

void sailfish_sim_info_dbus_free(struct sailfish_sim_info_dbus *dbus)
{
	if (dbus) {
		DBG("%s", dbus->info->path);
		g_dbus_unregister_interface(dbus->conn, dbus->info->path,
						SIM_INFO_DBUS_INTERFACE);
		if (dbus->watch->modem) {
			ofono_modem_remove_interface(dbus->watch->modem,
						SIM_INFO_DBUS_INTERFACE);
		}
		dbus_connection_unref(dbus->conn);

		ofono_watch_remove_all_handlers(dbus->watch,
						dbus->watch_event_id);
		ofono_watch_unref(dbus->watch);

		sailfish_sim_info_remove_all_handlers(dbus->info,
						dbus->info_event_id);
		sailfish_sim_info_unref(dbus->info);

		g_slice_free(struct sailfish_sim_info_dbus, dbus);
	}
}

/*
 * Local Variables:
 * mode: C
 * c-basic-offset: 8
 * indent-tabs-mode: t
 * End:
 */
