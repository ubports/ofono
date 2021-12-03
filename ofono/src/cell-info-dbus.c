/*
 *  oFono - Open Source Telephony - RIL-based devices
 *
 *  Copyright (C) 2016-2021 Jolla Ltd.
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

#include "cell-info-dbus.h"

#include <ofono/cell-info.h>
#include <ofono/modem.h>
#include <ofono/dbus.h>
#include <ofono/dbus-clients.h>
#include <ofono/log.h>

#include <gdbus.h>

#include "ofono.h"

typedef struct cell_entry {
	guint cell_id;
	char *path;
	struct ofono_cell cell;
} CellEntry;

typedef struct cell_info_dbus {
	struct ofono_cell_info *info;
	CellInfoControl *ctl;
	DBusConnection *conn;
	char *path;
	gulong handler_id;
	guint next_cell_id;
	GSList *entries;
	struct ofono_dbus_clients *clients;
} CellInfoDBus;

#define CELL_INFO_DBUS_INTERFACE            "org.nemomobile.ofono.CellInfo"
#define CELL_INFO_DBUS_CELLS_ADDED_SIGNAL   "CellsAdded"
#define CELL_INFO_DBUS_CELLS_REMOVED_SIGNAL "CellsRemoved"
#define CELL_INFO_DBUS_UNSUBSCRIBED_SIGNAL  "Unsubscribed"

#define CELL_DBUS_INTERFACE_VERSION         (1)
#define CELL_DBUS_INTERFACE                 "org.nemomobile.ofono.Cell"
#define CELL_DBUS_REGISTERED_CHANGED_SIGNAL "RegisteredChanged"
#define CELL_DBUS_PROPERTY_CHANGED_SIGNAL   "PropertyChanged"
#define CELL_DBUS_REMOVED_SIGNAL            "Removed"

struct cell_property {
	const char *name;
	glong off;
	int flag;
};

#define CELL_GSM_PROPERTY(value,name) \
	{ #name, G_STRUCT_OFFSET(struct ofono_cell_info_gsm,name), value }
#define CELL_WCDMA_PROPERTY(value,name) \
	{ #name, G_STRUCT_OFFSET(struct ofono_cell_info_wcdma,name), value }
#define CELL_LTE_PROPERTY(value,name) \
	{ #name, G_STRUCT_OFFSET(struct ofono_cell_info_lte,name), value }

static const struct cell_property cell_gsm_properties [] = {
	CELL_GSM_PROPERTY(0x001,mcc),
	CELL_GSM_PROPERTY(0x002,mnc),
	CELL_GSM_PROPERTY(0x004,lac),
	CELL_GSM_PROPERTY(0x008,cid),
	CELL_GSM_PROPERTY(0x010,arfcn),
	CELL_GSM_PROPERTY(0x020,bsic),
	CELL_GSM_PROPERTY(0x040,signalStrength),
	CELL_GSM_PROPERTY(0x080,bitErrorRate),
	CELL_GSM_PROPERTY(0x100,timingAdvance)
};

static const struct cell_property cell_wcdma_properties [] = {
	CELL_WCDMA_PROPERTY(0x01,mcc),
	CELL_WCDMA_PROPERTY(0x02,mnc),
	CELL_WCDMA_PROPERTY(0x04,lac),
	CELL_WCDMA_PROPERTY(0x08,cid),
	CELL_WCDMA_PROPERTY(0x10,psc),
	CELL_WCDMA_PROPERTY(0x20,uarfcn),
	CELL_WCDMA_PROPERTY(0x40,signalStrength),
	CELL_WCDMA_PROPERTY(0x80,bitErrorRate)
};

static const struct cell_property cell_lte_properties [] = {
	CELL_LTE_PROPERTY(0x001,mcc),
	CELL_LTE_PROPERTY(0x002,mnc),
	CELL_LTE_PROPERTY(0x004,ci),
	CELL_LTE_PROPERTY(0x008,pci),
	CELL_LTE_PROPERTY(0x010,tac),
	CELL_LTE_PROPERTY(0x020,earfcn),
	CELL_LTE_PROPERTY(0x040,signalStrength),
	CELL_LTE_PROPERTY(0x080,rsrp),
	CELL_LTE_PROPERTY(0x100,rsrq),
	CELL_LTE_PROPERTY(0x200,rssnr),
	CELL_LTE_PROPERTY(0x400,cqi),
	CELL_LTE_PROPERTY(0x800,timingAdvance)
};

#define CELL_PROPERTY_REGISTERED 0x1000

typedef void (*cell_info_dbus_append_fn)(DBusMessageIter *it,
	const CellEntry *entry);

static void cell_info_dbus_set_updates_enabled(CellInfoDBus *dbus, gboolean on)
{
	cell_info_control_set_enabled(dbus->ctl, dbus, on);
	cell_info_control_set_update_interval(dbus->ctl, dbus, on ? 5000 : -1);
}

static const char *cell_info_dbus_cell_type_str(enum ofono_cell_type type)
{
	switch (type) {
	case OFONO_CELL_TYPE_GSM:
		return "gsm";
	case OFONO_CELL_TYPE_WCDMA:
		return "wcdma";
	case OFONO_CELL_TYPE_LTE:
		return "lte";
	default:
		return "unknown";
	}
};

static const struct cell_property *cell_info_dbus_cell_properties
	(enum ofono_cell_type type, int *count)
{
	switch (type) {
	case OFONO_CELL_TYPE_GSM:
		*count = G_N_ELEMENTS(cell_gsm_properties);
		return cell_gsm_properties;
	case OFONO_CELL_TYPE_WCDMA:
		*count = G_N_ELEMENTS(cell_wcdma_properties);
		return cell_wcdma_properties;
	case OFONO_CELL_TYPE_LTE:
		*count = G_N_ELEMENTS(cell_lte_properties);
		return cell_lte_properties;
	default:
		*count = 0;
		return NULL;
	}
};

static void cell_info_destroy_entry(CellEntry *entry)
{
	if (entry) {
		g_free(entry->path);
		g_free(entry);
	}
}

static DBusMessage *cell_info_dbus_reply(DBusMessage *msg,
	const CellEntry *entry, cell_info_dbus_append_fn append)
{
	DBusMessage *reply = dbus_message_new_method_return(msg);
	DBusMessageIter it;

	dbus_message_iter_init_append(reply, &it);
	append(&it, entry);
	return reply;
}

static void cell_info_dbus_append_version(DBusMessageIter *it,
	const CellEntry *entry)
{
	dbus_int32_t version = CELL_DBUS_INTERFACE_VERSION;

	dbus_message_iter_append_basic(it, DBUS_TYPE_INT32, &version);
}

static void cell_info_dbus_append_type(DBusMessageIter *it,
	const CellEntry *entry)
{
	const char *type = cell_info_dbus_cell_type_str(entry->cell.type);

	dbus_message_iter_append_basic(it, DBUS_TYPE_STRING, &type);
}

static void cell_info_dbus_append_registered(DBusMessageIter *it,
	const CellEntry *entry)
{
	const dbus_bool_t registered = (entry->cell.registered != FALSE);

	dbus_message_iter_append_basic(it, DBUS_TYPE_BOOLEAN, &registered);
}

static void cell_info_dbus_append_properties(DBusMessageIter *it,
	const CellEntry *entry)
{
	int i, n;
	DBusMessageIter dict;
	const struct ofono_cell *cell = &entry->cell;
	const struct cell_property *prop =
		cell_info_dbus_cell_properties(cell->type, &n);

	dbus_message_iter_open_container(it, DBUS_TYPE_ARRAY, "{sv}", &dict);
	for (i = 0; i < n; i++) {
		gint32 value = G_STRUCT_MEMBER(int, &cell->info, prop[i].off);
		if (value != OFONO_CELL_INVALID_VALUE) {
			ofono_dbus_dict_append(&dict, prop[i].name,
				DBUS_TYPE_INT32, &value);
		}
	}
	dbus_message_iter_close_container(it, &dict);
}

static void cell_info_dbus_append_all(DBusMessageIter *it, const CellEntry *ce)
{
	cell_info_dbus_append_version(it, ce);
	cell_info_dbus_append_type(it, ce);
	cell_info_dbus_append_registered(it, ce);
	cell_info_dbus_append_properties(it, ce);
}

static DBusMessage *cell_info_dbus_cell_get_all(DBusConnection *conn,
	DBusMessage *msg, void *data)
{
	return cell_info_dbus_reply(msg, (CellEntry*) data,
		cell_info_dbus_append_all);
}

static DBusMessage *cell_info_dbus_cell_get_version(DBusConnection *conn,
	DBusMessage *msg, void *data)
{
	return cell_info_dbus_reply(msg, (CellEntry*) data,
		cell_info_dbus_append_version);
}

static DBusMessage *cell_info_dbus_cell_get_type(DBusConnection *conn,
	DBusMessage *msg, void *data)
{
	return cell_info_dbus_reply(msg, (CellEntry*) data,
		cell_info_dbus_append_type);
}

static DBusMessage *cell_info_dbus_cell_get_registered(DBusConnection *conn,
	DBusMessage *msg, void *data)
{
	return cell_info_dbus_reply(msg, (CellEntry*) data,
		cell_info_dbus_append_registered);
}

static DBusMessage *cell_info_dbus_cell_get_properties(DBusConnection *conn,
	DBusMessage *msg, void *data)
{
	return cell_info_dbus_reply(msg, (CellEntry*) data,
		cell_info_dbus_append_properties);
}

static const GDBusMethodTable cell_info_dbus_cell_methods[] = {
	{ GDBUS_METHOD("GetAll", NULL,
			GDBUS_ARGS({ "version", "i" },
			           { "type", "s" },
			           { "registered", "b" },
			           { "properties", "a{sv}" }),
			cell_info_dbus_cell_get_all) },
	{ GDBUS_METHOD("GetInterfaceVersion", NULL,
			GDBUS_ARGS({ "version", "i" }),
			cell_info_dbus_cell_get_version) },
	{ GDBUS_METHOD("GetType", NULL,
			GDBUS_ARGS({ "type", "s" }),
			cell_info_dbus_cell_get_type) },
	{ GDBUS_METHOD("GetRegistered", NULL,
			GDBUS_ARGS({ "registered", "b" }),
			cell_info_dbus_cell_get_registered) },
	{ GDBUS_METHOD("GetProperties", NULL,
			GDBUS_ARGS({ "properties", "a{sv}" }),
			cell_info_dbus_cell_get_properties) },
	{ }
};

static const GDBusSignalTable cell_info_dbus_cell_signals[] = {
	{ GDBUS_SIGNAL(CELL_DBUS_REGISTERED_CHANGED_SIGNAL,
			GDBUS_ARGS({ "registered", "b" })) },
	{ GDBUS_SIGNAL(CELL_DBUS_PROPERTY_CHANGED_SIGNAL,
			GDBUS_ARGS({ "name", "s" }, { "value", "v" })) },
	{ GDBUS_SIGNAL(CELL_DBUS_REMOVED_SIGNAL,
			GDBUS_ARGS({})) },
	{ }
};

static CellEntry *cell_info_dbus_find_id(CellInfoDBus *dbus, guint id)
{
	GSList *l;

	for (l = dbus->entries; l; l = l->next) {
		CellEntry *entry = l->data;

		if (entry->cell_id == id) {
			return entry;
		}
	}
	return NULL;
}

static guint cell_info_dbus_next_cell_id(CellInfoDBus *dbus)
{
	while (cell_info_dbus_find_id(dbus, dbus->next_cell_id)) {
		dbus->next_cell_id++;
	}
	return dbus->next_cell_id++;
}

static const struct ofono_cell *cell_info_dbus_find_ofono_cell
	(struct ofono_cell_info *info, const struct ofono_cell *cell)
{
	const ofono_cell_ptr *c;

	for (c = info->cells; *c; c++) {
		if (!ofono_cell_compare_location(*c, cell)) {
			return *c;
		}
	}
	return NULL;
}

static CellEntry *cell_info_dbus_find_cell(CellInfoDBus *dbus,
	const struct ofono_cell *cell)
{
	if (cell) {
		GSList *l;

		for (l = dbus->entries; l; l = l->next) {
			CellEntry *e = l->data;

			if (!ofono_cell_compare_location(&e->cell, cell)) {
				return e;
			}
		}
	}
	return NULL;
}

static void cell_info_dbus_emit_path_list(CellInfoDBus *dbus, const char *name,
	GPtrArray *list)
{
	if (ofono_dbus_clients_count(dbus->clients)) {
		guint i;
		DBusMessageIter it, a;
		DBusMessage *signal = dbus_message_new_signal(dbus->path,
			CELL_INFO_DBUS_INTERFACE, name);

		dbus_message_iter_init_append(signal, &it);
		dbus_message_iter_open_container(&it, DBUS_TYPE_ARRAY, "o", &a);
		for (i = 0; i < list->len; i++) {
			const char* path = list->pdata[i];

			dbus_message_iter_append_basic(&a,
				DBUS_TYPE_OBJECT_PATH, &path);
		}
		dbus_message_iter_close_container(&it, &a);
		ofono_dbus_clients_signal(dbus->clients, signal);
		dbus_message_unref(signal);
	}
}

static int cell_info_dbus_compare(const struct ofono_cell *c1,
	const struct ofono_cell *c2)
{
	if (c1->type == c2->type) {
		int i, n, mask = 0;
		const struct cell_property *prop =
			cell_info_dbus_cell_properties(c1->type, &n);

		if (c1->registered != c2->registered) {
			mask |= CELL_PROPERTY_REGISTERED;
		}

		for (i = 0; i < n; i++) {
			const glong offset = prop[i].off;
			gint32 v1 = G_STRUCT_MEMBER(int, &c1->info, offset);
			gint32 v2 = G_STRUCT_MEMBER(int, &c2->info, offset);

			if (v1 != v2) {
				mask |= prop[i].flag;
			}
		}

		return mask;
	} else {
		return -1;
	}
}

static void cell_info_dbus_emit_signal(CellInfoDBus *dbus, const char *path,
	const char *intf, const char *name, int type, ...)
{
	if (ofono_dbus_clients_count(dbus->clients)) {
		va_list args;
		DBusMessage *signal = dbus_message_new_signal(path, intf, name);

		va_start(args, type);
		dbus_message_append_args_valist(signal, type, args);
		ofono_dbus_clients_signal(dbus->clients, signal);
		dbus_message_unref(signal);
		va_end(args);
	}
}

static void cell_info_dbus_property_changed(CellInfoDBus *dbus,
	const CellEntry *entry, int mask)
{
	int i, n;
	const struct ofono_cell *cell = &entry->cell;
	const struct cell_property *prop =
		cell_info_dbus_cell_properties(cell->type, &n);

	if (mask & CELL_PROPERTY_REGISTERED) {
		const dbus_bool_t registered = (cell->registered != FALSE);

		cell_info_dbus_emit_signal(dbus, entry->path,
			CELL_DBUS_INTERFACE,
			CELL_DBUS_REGISTERED_CHANGED_SIGNAL,
			DBUS_TYPE_BOOLEAN, &registered, DBUS_TYPE_INVALID);
		mask &= ~CELL_PROPERTY_REGISTERED;
	}

	for (i = 0; i < n && mask; i++) {
		if (mask & prop[i].flag) {
			ofono_dbus_clients_signal_property_changed(
				dbus->clients, entry->path,
				CELL_DBUS_INTERFACE, prop[i].name,
				DBUS_TYPE_INT32,
				G_STRUCT_MEMBER_P(&cell->info, prop[i].off));
			mask &= ~prop[i].flag;
		}
	}
}

static void cell_info_dbus_update_entries(CellInfoDBus *dbus, gboolean emit)
{
	GSList *l;
	GPtrArray* added = NULL;
	GPtrArray* removed = NULL;
	const ofono_cell_ptr *c;

	/* Remove non-existent cells */
	l = dbus->entries;
	while (l) {
		GSList *next = l->next;
		CellEntry *entry = l->data;

		if (!cell_info_dbus_find_ofono_cell(dbus->info, &entry->cell)) {
			DBG("%s removed", entry->path);
			dbus->entries = g_slist_delete_link(dbus->entries, l);
			cell_info_dbus_emit_signal(dbus, entry->path,
				CELL_DBUS_INTERFACE,
				CELL_DBUS_REMOVED_SIGNAL,
				DBUS_TYPE_INVALID);
			g_dbus_unregister_interface(dbus->conn, entry->path,
				CELL_DBUS_INTERFACE);
			if (emit) {
				if (!removed) {
					removed = g_ptr_array_new_with_free_func
						(g_free);
				}
				/* Steal the path */
				g_ptr_array_add(removed, entry->path);
				entry->path = NULL;
			}
			cell_info_destroy_entry(entry);
		}
		l = next;
	}

	/* Add new cells */
	for (c = dbus->info->cells; *c; c++) {
		const struct ofono_cell *cell = *c;
		CellEntry *entry = cell_info_dbus_find_cell(dbus, cell);

		if (entry) {
			if (emit) {
				const int diff = cell_info_dbus_compare(cell,
					&entry->cell);

				entry->cell = *cell;
				cell_info_dbus_property_changed(dbus, entry,
					diff);
			} else {
				entry->cell = *cell;
			}
		} else {
			entry = g_new0(CellEntry, 1);
			entry->cell = *cell;
			entry->cell_id = cell_info_dbus_next_cell_id(dbus);
			entry->path = g_strdup_printf("%s/cell_%u", dbus->path,
				entry->cell_id);
			dbus->entries = g_slist_append(dbus->entries, entry);
			DBG("%s added", entry->path);
			g_dbus_register_interface(dbus->conn, entry->path,
				CELL_DBUS_INTERFACE,
				cell_info_dbus_cell_methods,
				cell_info_dbus_cell_signals, NULL,
				entry, NULL);
			if (emit) {
				if (!added) {
					added = g_ptr_array_new();
				}
				g_ptr_array_add(added, entry->path);
			}
		}
	}

	if (removed) {
		cell_info_dbus_emit_path_list(dbus,
			CELL_INFO_DBUS_CELLS_REMOVED_SIGNAL, removed);
		g_ptr_array_free(removed, TRUE);
	}

	if (added) {
		cell_info_dbus_emit_path_list(dbus,
			CELL_INFO_DBUS_CELLS_ADDED_SIGNAL, added);
		g_ptr_array_free(added, TRUE);
	}
}

static void cell_info_dbus_cells_changed_cb(struct ofono_cell_info *info,
	void *data)
{
	DBG("");
	cell_info_dbus_update_entries((CellInfoDBus *) data, TRUE);
}

static DBusMessage *cell_info_dbus_error_failed(DBusMessage *msg,
	const char *explanation)
{
	return g_dbus_create_error(msg, OFONO_ERROR_INTERFACE ".Failed", "%s",
								explanation);
}

static DBusMessage *cell_info_dbus_get_cells(DBusConnection *conn,
	DBusMessage *msg, void *data)
{
	CellInfoDBus *dbus = data;
	const char *sender = dbus_message_get_sender(msg);

	if (ofono_dbus_clients_add(dbus->clients, sender)) {
		DBusMessage *reply = dbus_message_new_method_return(msg);
		DBusMessageIter it, a;
		GSList *l;

		cell_info_dbus_set_updates_enabled(dbus, TRUE);
		dbus_message_iter_init_append(reply, &it);
		dbus_message_iter_open_container(&it, DBUS_TYPE_ARRAY, "o", &a);
		for (l = dbus->entries; l; l = l->next) {
			const CellEntry *entry = l->data;

			dbus_message_iter_append_basic(&a,
					DBUS_TYPE_OBJECT_PATH, &entry->path);
		}
		dbus_message_iter_close_container(&it, &a);
		return reply;
	}
	return cell_info_dbus_error_failed(msg, "Operation failed");
}

static DBusMessage *cell_info_dbus_unsubscribe(DBusConnection *conn,
	DBusMessage *msg, void *data)
{
	CellInfoDBus *dbus = data;
	const char *sender = dbus_message_get_sender(msg);

	DBG("%s", sender);
	if (ofono_dbus_clients_remove(dbus->clients, sender)) {
		DBusMessage *signal = dbus_message_new_signal(dbus->path,
			CELL_INFO_DBUS_INTERFACE,
			CELL_INFO_DBUS_UNSUBSCRIBED_SIGNAL);

		if (!ofono_dbus_clients_count(dbus->clients)) {
			cell_info_dbus_set_updates_enabled(dbus, FALSE);
		}
		dbus_message_set_destination(signal, sender);
		g_dbus_send_message(dbus->conn, signal);
		return dbus_message_new_method_return(msg);
	}
	return cell_info_dbus_error_failed(msg, "Not subscribed");
}

static const GDBusMethodTable cell_info_dbus_methods[] = {
	{ GDBUS_METHOD("GetCells", NULL,
			GDBUS_ARGS({ "paths", "ao" }),
			cell_info_dbus_get_cells) },
	{ GDBUS_METHOD("Unsubscribe", NULL, NULL,
			cell_info_dbus_unsubscribe) },
	{ }
};

static const GDBusSignalTable cell_info_dbus_signals[] = {
	{ GDBUS_SIGNAL(CELL_INFO_DBUS_CELLS_ADDED_SIGNAL,
			GDBUS_ARGS({ "paths", "ao" })) },
	{ GDBUS_SIGNAL(CELL_INFO_DBUS_CELLS_REMOVED_SIGNAL,
			GDBUS_ARGS({ "paths", "ao" })) },
	{ GDBUS_SIGNAL(CELL_INFO_DBUS_UNSUBSCRIBED_SIGNAL,
			GDBUS_ARGS({})) },
	{ }
};

static void cell_info_dbus_disconnect_cb(const char *name, void *data)
{
	CellInfoDBus *dbus = data;

	if (!ofono_dbus_clients_count(dbus->clients)) {
		cell_info_dbus_set_updates_enabled(dbus, FALSE);
	}
}

CellInfoDBus *cell_info_dbus_new(struct ofono_modem *modem,
	CellInfoControl *ctl)
{
	if (modem && ctl && ctl->info) {
		struct ofono_cell_info *info = ctl->info;
		CellInfoDBus *dbus = g_new0(CellInfoDBus, 1);

		DBG("%s", ofono_modem_get_path(modem));
		dbus->path = g_strdup(ofono_modem_get_path(modem));
		dbus->conn = dbus_connection_ref(ofono_dbus_get_connection());
		dbus->info = ofono_cell_info_ref(info);
		dbus->ctl = cell_info_control_ref(ctl);
		dbus->handler_id = ofono_cell_info_add_change_handler(info,
			cell_info_dbus_cells_changed_cb, dbus);

		/* Register D-Bus interface */
		if (g_dbus_register_interface(dbus->conn, dbus->path,
			CELL_INFO_DBUS_INTERFACE,
			cell_info_dbus_methods,
			cell_info_dbus_signals,
			NULL, dbus, NULL)) {
			ofono_modem_add_interface(modem,
				CELL_INFO_DBUS_INTERFACE);
			cell_info_dbus_update_entries(dbus, FALSE);
			dbus->clients = ofono_dbus_clients_new(dbus->conn,
				cell_info_dbus_disconnect_cb, dbus);
			return dbus;
		} else {
			ofono_error("CellInfo D-Bus register failed");
			cell_info_dbus_free(dbus);
		}
	}
	return NULL;
}

void cell_info_dbus_free(CellInfoDBus *dbus)
{
	if (dbus) {
		GSList *l;

		DBG("%s", dbus->path);
		ofono_dbus_clients_free(dbus->clients);
		g_dbus_unregister_interface(dbus->conn, dbus->path,
			CELL_INFO_DBUS_INTERFACE);

		/* Unregister cells */
		l = dbus->entries;
		while (l) {
			CellEntry *entry = l->data;
			g_dbus_unregister_interface(dbus->conn, entry->path,
				CELL_DBUS_INTERFACE);
			cell_info_destroy_entry(entry);
			l = l->next;
		}
		g_slist_free(dbus->entries);

		dbus_connection_unref(dbus->conn);

		ofono_cell_info_remove_handler(dbus->info, dbus->handler_id);
		ofono_cell_info_unref(dbus->info);

		cell_info_control_drop_requests(dbus->ctl, dbus);
		cell_info_control_unref(dbus->ctl);

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
