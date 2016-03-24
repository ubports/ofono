/*
 *  oFono - Open Source Telephony - RIL-based devices
 *
 *  Copyright (C) 2016 Jolla Ltd.
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
#include "ril_sim_info.h"
#include "ril_log.h"

#include <ofono/dbus.h>

#include <gdbus.h>

#include "ofono.h"
#include "storage.h"

enum sim_info_event_id {
	SIM_INFO_EVENT_ICCID,
	SIM_INFO_EVENT_IMSI,
	SIM_INFO_EVENT_SPN,
	SIM_INFO_EVENT_COUNT
};

struct ril_sim_info_dbus {
	struct ril_modem *md;
	struct ril_sim_info *info;
	DBusConnection *conn;
	char *path;
	gulong handler_id[SIM_INFO_EVENT_COUNT];
};

#define RIL_SIM_INFO_DBUS_INTERFACE             "org.nemomobile.ofono.SimInfo"
#define RIL_SIM_INFO_DBUS_INTERFACE_VERSION     (1)

#define RIL_SIM_INFO_DBUS_ICCID_CHANGED_SIGNAL  "CardIdentifierChanged"
#define RIL_SIM_INFO_DBUS_IMSI_CHANGED_SIGNAL   "SubscriberIdentityChanged"
#define RIL_SIM_INFO_DBUS_SPN_CHANGED_SIGNAL    "ServiceProviderNameChanged"

static void ril_sim_info_dbus_append_string(DBusMessageIter *it, const char *s)
{
	if (!s) s = "";
	dbus_message_iter_append_basic(it, DBUS_TYPE_STRING, &s);
}

static DBusMessage *ril_sim_info_dbus_reply_with_string(DBusMessage *msg,
							const char *str)
{
	DBusMessage *reply = dbus_message_new_method_return(msg);
	DBusMessageIter iter;

	dbus_message_iter_init_append(reply, &iter);
	ril_sim_info_dbus_append_string(&iter, str);
	return reply;
}

static DBusMessage *ril_sim_info_dbus_get_all(DBusConnection *conn,
						DBusMessage *msg, void *data)
{
	struct ril_sim_info_dbus *dbus = data;
	struct ril_sim_info *info = dbus->info;
	DBusMessage *reply = dbus_message_new_method_return(msg);
	const dbus_int32_t version = RIL_SIM_INFO_DBUS_INTERFACE_VERSION;
	DBusMessageIter iter;

	dbus_message_iter_init_append(reply, &iter);
	dbus_message_iter_append_basic(&iter, DBUS_TYPE_INT32, &version);
	ril_sim_info_dbus_append_string(&iter, info->iccid);
	ril_sim_info_dbus_append_string(&iter, info->imsi);
	ril_sim_info_dbus_append_string(&iter, info->spn);
	return reply;
}

static DBusMessage *ril_sim_info_dbus_get_version(DBusConnection *dc,
						DBusMessage *msg, void *data)
{
	DBusMessage *reply = dbus_message_new_method_return(msg);
	dbus_int32_t version = RIL_SIM_INFO_DBUS_INTERFACE_VERSION;
	DBusMessageIter iter;

	dbus_message_iter_init_append(reply, &iter);
	dbus_message_iter_append_basic(&iter, DBUS_TYPE_INT32, &version);
	return reply;
}

static DBusMessage *ril_sim_info_dbus_get_iccid(DBusConnection *conn,
						DBusMessage *msg, void *data)
{
	struct ril_sim_info_dbus *dbus = data;
	return ril_sim_info_dbus_reply_with_string(msg, dbus->info->iccid);
}

static DBusMessage *ril_sim_info_dbus_get_imsi(DBusConnection *conn,
						DBusMessage *msg, void *data)
{
	struct ril_sim_info_dbus *dbus = data;
	return ril_sim_info_dbus_reply_with_string(msg, dbus->info->imsi);
}

static DBusMessage *ril_sim_info_dbus_get_spn(DBusConnection *conn,
						DBusMessage *msg, void *data)
{
	struct ril_sim_info_dbus *dbus = data;
	return ril_sim_info_dbus_reply_with_string(msg, dbus->info->spn);
}

static const GDBusMethodTable ril_sim_info_dbus_methods[] = {
	{ GDBUS_METHOD("GetAll",
			NULL, GDBUS_ARGS({"version", "i" },
					 {"iccid", "s" },
					 {"imsi", "s" }, 
					 {"spn" , "s"}),
			ril_sim_info_dbus_get_all) },
	{ GDBUS_METHOD("GetInterfaceVersion",
			NULL, GDBUS_ARGS({ "version", "i" }),
			ril_sim_info_dbus_get_version) },
	{ GDBUS_METHOD("GetCardIdentifier",
			NULL, GDBUS_ARGS({ "iccid", "s" }),
			ril_sim_info_dbus_get_iccid) },
	{ GDBUS_METHOD("GetSubscriberIdentity",
			NULL, GDBUS_ARGS({ "imsi", "s" }),
			ril_sim_info_dbus_get_imsi) },
	{ GDBUS_METHOD("GetServiceProviderName",
			NULL, GDBUS_ARGS({ "spn", "s" }),
			ril_sim_info_dbus_get_spn) },
	{ }
};

static const GDBusSignalTable ril_sim_info_dbus_signals[] = {
	{ GDBUS_SIGNAL(RIL_SIM_INFO_DBUS_ICCID_CHANGED_SIGNAL,
			GDBUS_ARGS({ "iccid", "s" })) },
	{ GDBUS_SIGNAL(RIL_SIM_INFO_DBUS_IMSI_CHANGED_SIGNAL,
			GDBUS_ARGS({ "imsi", "s" })) },
	{ GDBUS_SIGNAL(RIL_SIM_INFO_DBUS_SPN_CHANGED_SIGNAL,
			GDBUS_ARGS({ "spn", "s" })) },
	{ }
};

static void ril_sim_info_dbus_emit(struct ril_sim_info_dbus *dbus,
					const char *signal, const char *value)
{
	const char *arg = value;
	if (!arg) arg = "";
	g_dbus_emit_signal(dbus->conn, dbus->path, RIL_SIM_INFO_DBUS_INTERFACE,
			signal, DBUS_TYPE_STRING, &arg, DBUS_TYPE_INVALID);
}

static void ril_sim_info_dbus_iccid_cb(struct ril_sim_info *info, void *arg)
{
	struct ril_sim_info_dbus *dbus = arg;
	ril_sim_info_dbus_emit(dbus, RIL_SIM_INFO_DBUS_ICCID_CHANGED_SIGNAL,
								info->iccid);
}

static void ril_sim_info_dbus_imsi_cb(struct ril_sim_info *info, void *arg)
{
	struct ril_sim_info_dbus *dbus = arg;
	ril_sim_info_dbus_emit(dbus, RIL_SIM_INFO_DBUS_IMSI_CHANGED_SIGNAL,
								info->imsi);
}

static void ril_sim_info_dbus_spn_cb(struct ril_sim_info *info, void *arg)
{
	struct ril_sim_info_dbus *dbus = arg;
	ril_sim_info_dbus_emit(dbus, RIL_SIM_INFO_DBUS_SPN_CHANGED_SIGNAL,
								info->spn);
}

struct ril_sim_info_dbus *ril_sim_info_dbus_new(struct ril_modem *md,
						struct ril_sim_info *info)
{
	struct ril_sim_info_dbus *dbus = g_new0(struct ril_sim_info_dbus, 1);

	DBG("%s", ril_modem_get_path(md));
	dbus->md = md;
	dbus->path = g_strdup(ril_modem_get_path(md));
	dbus->info = ril_sim_info_ref(info);
	dbus->conn = dbus_connection_ref(ofono_dbus_get_connection());

	/* Register D-Bus interface */
	if (g_dbus_register_interface(dbus->conn, dbus->path,
			RIL_SIM_INFO_DBUS_INTERFACE, ril_sim_info_dbus_methods,
			ril_sim_info_dbus_signals, NULL, dbus, NULL)) {
		ofono_modem_add_interface(md->ofono,
						RIL_SIM_INFO_DBUS_INTERFACE);

		dbus->handler_id[SIM_INFO_EVENT_ICCID] =
			ril_sim_info_add_iccid_changed_handler(info,
				ril_sim_info_dbus_iccid_cb, dbus);
		dbus->handler_id[SIM_INFO_EVENT_IMSI] =
			ril_sim_info_add_imsi_changed_handler(info,
				ril_sim_info_dbus_imsi_cb, dbus);
		dbus->handler_id[SIM_INFO_EVENT_SPN] =
			ril_sim_info_add_spn_changed_handler(info,
				ril_sim_info_dbus_spn_cb, dbus);

		return dbus;
	} else {
		ofono_error("CellInfo D-Bus register failed");
		ril_sim_info_dbus_free(dbus);
		return NULL;
	}
}

void ril_sim_info_dbus_free(struct ril_sim_info_dbus *dbus)
{
	if (dbus) {
		unsigned int i;

		DBG("%s", dbus->path);
		g_dbus_unregister_interface(dbus->conn, dbus->path,
						RIL_SIM_INFO_DBUS_INTERFACE);
		ofono_modem_remove_interface(dbus->md->ofono,
						RIL_SIM_INFO_DBUS_INTERFACE);
		dbus_connection_unref(dbus->conn);

		for (i=0; i<G_N_ELEMENTS(dbus->handler_id); i++) {
			ril_sim_info_remove_handler(dbus->info,
							dbus->handler_id[i]);
		}
		ril_sim_info_unref(dbus->info);

		g_free(dbus->path);
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
