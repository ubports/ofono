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
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 *  GNU General Public License for more details.
 */

#include "ril_plugin.h"
#include "ril_cell_info.h"
#include "ril_log.h"

#include <ofono/dbus.h>

#include <gdbus.h>

struct ril_cell_entry {
	guint cell_id;
	char *path;
	struct ril_cell cell;
};

struct ril_cell_info_dbus {
	struct ril_modem *md;
	struct ril_cell_info *info;
	DBusConnection *conn;
	char *path;
	gulong handler_id;
	guint next_cell_id;
	GSList *entries;
};

#define RIL_CELL_INFO_DBUS_INTERFACE             "org.nemomobile.ofono.CellInfo"
#define RIL_CELL_INFO_DBUS_CELLS_ADDED_SIGNAL    "CellsAdded"
#define RIL_CELL_INFO_DBUS_CELLS_REMOVED_SIGNAL  "CellsRemoved"

#define RIL_CELL_DBUS_INTERFACE_VERSION          (1)
#define RIL_CELL_DBUS_INTERFACE                  "org.nemomobile.ofono.Cell"
#define RIL_CELL_DBUS_REGISTERED_CHANGED_SIGNAL  "RegisteredChanged"
#define RIL_CELL_DBUS_PROPERTY_CHANGED_SIGNAL    "PropertyChanged"
#define RIL_CELL_DBUS_REMOVED_SIGNAL             "Removed"

struct ril_cell_property {
	const char *name;
	glong off;
	int flag;
};

#define RIL_CELL_GSM_PROPERTY(value,name) \
	{ #name, G_STRUCT_OFFSET(struct ril_cell_info_gsm,name), value }
#define RIL_CELL_WCDMA_PROPERTY(value,name) \
	{ #name, G_STRUCT_OFFSET(struct ril_cell_info_wcdma,name), value }
#define RIL_CELL_LTE_PROPERTY(value,name) \
	{ #name, G_STRUCT_OFFSET(struct ril_cell_info_lte,name), value }

static const struct ril_cell_property ril_cell_gsm_properties [] = {
	RIL_CELL_GSM_PROPERTY(0x01,mcc),
	RIL_CELL_GSM_PROPERTY(0x02,mnc),
	RIL_CELL_GSM_PROPERTY(0x04,lac),
	RIL_CELL_GSM_PROPERTY(0x08,cid),
	RIL_CELL_GSM_PROPERTY(0x10,signalStrength),
	RIL_CELL_GSM_PROPERTY(0x20,bitErrorRate)
};

static const struct ril_cell_property ril_cell_wcdma_properties [] = {
	RIL_CELL_WCDMA_PROPERTY(0x01,mcc),
	RIL_CELL_WCDMA_PROPERTY(0x02,mnc),
	RIL_CELL_WCDMA_PROPERTY(0x04,lac),
	RIL_CELL_WCDMA_PROPERTY(0x08,cid),
	RIL_CELL_WCDMA_PROPERTY(0x10,psc),
	RIL_CELL_WCDMA_PROPERTY(0x20,signalStrength),
	RIL_CELL_WCDMA_PROPERTY(0x40,bitErrorRate)
};

static const struct ril_cell_property ril_cell_lte_properties [] = {
	RIL_CELL_LTE_PROPERTY(0x001,mcc),
	RIL_CELL_LTE_PROPERTY(0x002,mnc),
	RIL_CELL_LTE_PROPERTY(0x004,ci),
	RIL_CELL_LTE_PROPERTY(0x008,pci),
	RIL_CELL_LTE_PROPERTY(0x010,tac),
	RIL_CELL_LTE_PROPERTY(0x020,signalStrength),
	RIL_CELL_LTE_PROPERTY(0x040,rsrp),
	RIL_CELL_LTE_PROPERTY(0x080,rsrq),
	RIL_CELL_LTE_PROPERTY(0x100,rssnr),
	RIL_CELL_LTE_PROPERTY(0x200,cqi),
	RIL_CELL_LTE_PROPERTY(0x400,timingAdvance)
};

#define RIL_CELL_PROPERTY_REGISTERED 0x1000

typedef void (*ril_cell_info_dbus_append_fn)(DBusMessageIter *it,
					const struct ril_cell_entry *entry);

static const char *ril_cell_info_dbus_cell_type_str(enum ril_cell_info_type t)
{
	switch (t) {
	case RIL_CELL_INFO_TYPE_GSM:
		return "gsm";
	case RIL_CELL_INFO_TYPE_CDMA:
		return "cdma";
	case RIL_CELL_INFO_TYPE_LTE:
		return "lte";
	case RIL_CELL_INFO_TYPE_WCDMA:
		return "wcdma";
	case RIL_CELL_INFO_TYPE_TD_SCDMA:
		return "tdscdma";
	case RIL_CELL_INFO_TYPE_NONE:
	default:
		return "unknown";
	}
};

static const struct ril_cell_property *ril_cell_info_dbus_cell_properties(
				enum ril_cell_info_type type, int *count)
{
	switch (type) {
	case RIL_CELL_INFO_TYPE_GSM:
		*count = G_N_ELEMENTS(ril_cell_gsm_properties);
		return ril_cell_gsm_properties;
	case RIL_CELL_INFO_TYPE_WCDMA:
		*count = G_N_ELEMENTS(ril_cell_wcdma_properties);
		return ril_cell_wcdma_properties;
	case RIL_CELL_INFO_TYPE_LTE:
		*count = G_N_ELEMENTS(ril_cell_lte_properties);
		return ril_cell_lte_properties;
	default:
		*count = 0;
		return NULL;
	}
};

static void ril_cell_info_destroy_entry(struct ril_cell_entry *entry)
{
	if (entry) {
		g_free(entry->path);
		g_free(entry);
	}
}

static DBusMessage *ril_cell_info_dbus_reply(DBusMessage *msg,
				const struct ril_cell_entry *entry,
				ril_cell_info_dbus_append_fn append)
{
	DBusMessage *reply = dbus_message_new_method_return(msg);
	DBusMessageIter it;

	dbus_message_iter_init_append(reply, &it);
	append(&it, entry);
	return reply;
}

static void ril_cell_info_dbus_append_version(DBusMessageIter *it,
					const struct ril_cell_entry *entry)
{
	dbus_int32_t version = RIL_CELL_DBUS_INTERFACE_VERSION;

	dbus_message_iter_append_basic(it, DBUS_TYPE_INT32, &version);
}

static void ril_cell_info_dbus_append_type(DBusMessageIter *it,
					const struct ril_cell_entry *entry)
{
	const char *type = ril_cell_info_dbus_cell_type_str(entry->cell.type);

	dbus_message_iter_append_basic(it, DBUS_TYPE_STRING, &type);
}

static void ril_cell_info_dbus_append_registered(DBusMessageIter *it,
					const struct ril_cell_entry *entry)
{
	dbus_bool_t registered = entry->cell.registered;

	dbus_message_iter_append_basic(it, DBUS_TYPE_BOOLEAN, &registered);
}

static void ril_cell_info_dbus_append_properties(DBusMessageIter *it,
					const struct ril_cell_entry *entry)
{
	int i, n;
	DBusMessageIter dict;
	const struct ril_cell *cell = &entry->cell;
	const struct ril_cell_property *prop =
		ril_cell_info_dbus_cell_properties(cell->type, &n);

	dbus_message_iter_open_container(it, DBUS_TYPE_ARRAY, "{sv}", &dict);
	for (i = 0; i < n; i++) {
		gint32 value = G_STRUCT_MEMBER(int, &cell->info, prop[i].off);
		if (value != INT_MAX) {
			ofono_dbus_dict_append(&dict, prop[i].name,
						DBUS_TYPE_INT32, &value);
		}
	}
	dbus_message_iter_close_container(it, &dict);
}

static void ril_cell_info_dbus_append_all(DBusMessageIter *it,
					const struct ril_cell_entry *entry)
{
	ril_cell_info_dbus_append_version(it, entry);
	ril_cell_info_dbus_append_type(it, entry);
	ril_cell_info_dbus_append_registered(it, entry);
	ril_cell_info_dbus_append_properties(it, entry);
}

static DBusMessage *ril_cell_info_dbus_cell_get_all(DBusConnection *conn,
						DBusMessage *msg, void *data)
{
	return ril_cell_info_dbus_reply(msg, (struct ril_cell_entry*)data,
				ril_cell_info_dbus_append_all);
}

static DBusMessage *ril_cell_info_dbus_cell_get_version(DBusConnection *conn,
						DBusMessage *msg, void *data)
{
	return ril_cell_info_dbus_reply(msg, (struct ril_cell_entry*)data,
				ril_cell_info_dbus_append_version);
}

static DBusMessage *ril_cell_info_dbus_cell_get_type(DBusConnection *conn,
						DBusMessage *msg, void *data)
{
	return ril_cell_info_dbus_reply(msg, (struct ril_cell_entry*)data,
				ril_cell_info_dbus_append_type);
}

static DBusMessage *ril_cell_info_dbus_cell_get_registered(DBusConnection *conn,
						DBusMessage *msg, void *data)
{
	return ril_cell_info_dbus_reply(msg, (struct ril_cell_entry*)data,
				ril_cell_info_dbus_append_registered);
}

static DBusMessage *ril_cell_info_dbus_cell_get_properties(DBusConnection *conn,
						DBusMessage *msg, void *data)
{
	return ril_cell_info_dbus_reply(msg, (struct ril_cell_entry*)data,
				ril_cell_info_dbus_append_properties);
}

static const GDBusMethodTable ril_cell_info_dbus_cell_methods[] = {
	{ GDBUS_METHOD("GetAll", NULL,
			GDBUS_ARGS({ "version", "i" },
			           { "type", "s" },
			           { "registered", "b" },
			           { "properties", "a{sv}" }),
			ril_cell_info_dbus_cell_get_all) },
	{ GDBUS_METHOD("GetInterfaceVersion", NULL,
			GDBUS_ARGS({ "version", "i" }),
			ril_cell_info_dbus_cell_get_version) },
	{ GDBUS_METHOD("GetType", NULL,
			GDBUS_ARGS({ "type", "s" }),
			ril_cell_info_dbus_cell_get_type) },
	{ GDBUS_METHOD("GetRegistered", NULL,
			GDBUS_ARGS({ "registered", "b" }),
			ril_cell_info_dbus_cell_get_registered) },
	{ GDBUS_METHOD("GetProperties", NULL,
			GDBUS_ARGS({ "properties", "a{sv}" }),
			ril_cell_info_dbus_cell_get_properties) },
	{ }
};

static const GDBusSignalTable ril_cell_info_dbus_cell_signals[] = {
	{ GDBUS_SIGNAL(RIL_CELL_DBUS_REGISTERED_CHANGED_SIGNAL,
			GDBUS_ARGS({ "registered", "b" })) },
	{ GDBUS_SIGNAL(RIL_CELL_DBUS_PROPERTY_CHANGED_SIGNAL,
			GDBUS_ARGS({ "name", "s" }, { "value", "v" })) },
	{ GDBUS_SIGNAL(RIL_CELL_DBUS_REMOVED_SIGNAL,
			GDBUS_ARGS({})) },
	{ }
};

static struct ril_cell_entry *ril_cell_info_dbus_find_id(
		struct ril_cell_info_dbus *dbus, guint id)
{
	GSList *l;
	for (l = dbus->entries; l; l = l->next) {
		struct ril_cell_entry *entry = l->data;
		if (entry->cell_id == id) {
			return entry;
		}
	}
	return NULL;
}

static guint ril_cell_info_dbus_next_cell_id(struct ril_cell_info_dbus *dbus)
{
	while (ril_cell_info_dbus_find_id(dbus, dbus->next_cell_id)) {
		dbus->next_cell_id++;
	}
	return dbus->next_cell_id++;
}

static struct ril_cell_entry *ril_cell_info_dbus_find_cell(
		struct ril_cell_info_dbus *dbus, const struct ril_cell *cell)
{
	if (cell) {
		GSList *l;
		for (l = dbus->entries; l; l = l->next) {
			struct ril_cell_entry *entry = l->data;
			if (!ril_cell_compare_location(&entry->cell, cell)) {
				return entry;
			}
		}
	}
	return NULL;
}

static void ril_cell_info_dbus_emit_path_list(struct ril_cell_info_dbus *dbus,
					const char *name, GPtrArray *list)
{
	guint i;
	DBusMessageIter it, array;
	DBusMessage *signal = dbus_message_new_signal(dbus->path,
					RIL_CELL_INFO_DBUS_INTERFACE, name);

	dbus_message_iter_init_append(signal, &it);
	dbus_message_iter_open_container(&it, DBUS_TYPE_ARRAY, "o", &array);
	for (i = 0; i < list->len; i++) {
		const char* path = list->pdata[i];
		dbus_message_iter_append_basic(&array, DBUS_TYPE_OBJECT_PATH,
								&path);
	}
	dbus_message_iter_close_container(&it, &array);

	g_dbus_send_message(dbus->conn, signal);
}

static int ril_cell_info_dbus_compare(const struct ril_cell *c1,
					const struct ril_cell *c2)
{
	if (c1->type == c2->type) {
		int i, n, mask = 0;
		const struct ril_cell_property *prop =
			ril_cell_info_dbus_cell_properties(c1->type, &n);

		if (c1->registered != c2->registered) {
			mask |= RIL_CELL_PROPERTY_REGISTERED;
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

static void ril_cell_info_dbus_property_changed(struct ril_cell_info_dbus *dbus,
				const struct ril_cell_entry *entry, int mask)
{
	int i, n;
	const struct ril_cell *cell = &entry->cell;
	const struct ril_cell_property *prop =
		ril_cell_info_dbus_cell_properties(cell->type, &n);

	if (mask & RIL_CELL_PROPERTY_REGISTERED) {
		dbus_bool_t registered = cell->registered;
		g_dbus_emit_signal(dbus->conn, entry->path,
			RIL_CELL_DBUS_INTERFACE,
			RIL_CELL_DBUS_REGISTERED_CHANGED_SIGNAL,
			DBUS_TYPE_BOOLEAN, &registered, DBUS_TYPE_INVALID);
		mask &= ~RIL_CELL_PROPERTY_REGISTERED;
	}

	for (i = 0; i < n && mask; i++) {
		if (mask & prop[i].flag) {
			ofono_dbus_signal_property_changed(dbus->conn,
				entry->path, RIL_CELL_DBUS_INTERFACE,
				prop[i].name, DBUS_TYPE_INT32,
				G_STRUCT_MEMBER_P(&cell->info, prop[i].off));
			mask &= ~prop[i].flag;
		}
	}
}

static void ril_cell_info_dbus_update_entries(struct ril_cell_info_dbus *dbus,
							gboolean emit_signals)
{
	GSList *l;
	GPtrArray* added = NULL;
	GPtrArray* removed = NULL;

	/* Remove non-existent cells */
	l = dbus->entries;
	while (l) {
		GSList *next = l->next;
		struct ril_cell_entry *entry = l->data;
		if (!g_slist_find_custom(dbus->info->cells, &entry->cell,
						ril_cell_compare_func)) {
			DBG("%s removed", entry->path);
			dbus->entries = g_slist_delete_link(dbus->entries, l);
			g_dbus_emit_signal(dbus->conn, entry->path,
					RIL_CELL_DBUS_INTERFACE,
					RIL_CELL_DBUS_REMOVED_SIGNAL,
					DBUS_TYPE_INVALID);
			g_dbus_unregister_interface(dbus->conn, entry->path,
						RIL_CELL_DBUS_INTERFACE);
			if (emit_signals) {
				if (!removed) {
					removed =
						g_ptr_array_new_with_free_func(
								g_free);
				}
				/* Steal the path */
				g_ptr_array_add(removed, entry->path);
				entry->path = NULL;
			}
			ril_cell_info_destroy_entry(entry);
		}
		l = next;
	}

	/* Add new cells */
	for (l = dbus->info->cells; l; l = l->next) {
		const struct ril_cell *cell = l->data;
		struct ril_cell_entry *entry =
			ril_cell_info_dbus_find_cell(dbus, cell);

		if (entry) {
			if (emit_signals) {
				int diff = ril_cell_info_dbus_compare(cell,
								&entry->cell);
				entry->cell = *cell;
				ril_cell_info_dbus_property_changed(dbus,
								entry, diff);
			} else {
				entry->cell = *cell;
			}
		} else {
			entry = g_new0(struct ril_cell_entry, 1);
			entry->cell = *cell;
			entry->cell_id = ril_cell_info_dbus_next_cell_id(dbus);
			entry->path = g_strdup_printf("%s/cell_%u", dbus->path,
							entry->cell_id);
			dbus->entries = g_slist_append(dbus->entries, entry);
			DBG("%s added", entry->path);
			g_dbus_register_interface(dbus->conn, entry->path,
					RIL_CELL_DBUS_INTERFACE,
					ril_cell_info_dbus_cell_methods,
					ril_cell_info_dbus_cell_signals, NULL,
					entry, NULL);
			if (emit_signals) {
				if (!added) {
					added = g_ptr_array_new();
				}
				g_ptr_array_add(added, entry->path);
			}
		}
	}

	if (removed) {
		ril_cell_info_dbus_emit_path_list(dbus,
			RIL_CELL_INFO_DBUS_CELLS_REMOVED_SIGNAL, removed);
		g_ptr_array_free(removed, TRUE);
	}

	if (added) {
		ril_cell_info_dbus_emit_path_list(dbus,
			RIL_CELL_INFO_DBUS_CELLS_ADDED_SIGNAL, added);
		g_ptr_array_free(added, TRUE);
	}
}

static void ril_cell_info_dbus_cells_changed_cb(struct ril_cell_info *info,
								void *arg)
{
	DBG("");
	ril_cell_info_dbus_update_entries((struct ril_cell_info_dbus *)arg,
								TRUE);
}

static DBusMessage *ril_cell_info_dbus_get_cells(DBusConnection *conn,
						DBusMessage *msg, void *data)
{
	struct ril_cell_info_dbus *dbus = data;
	DBusMessage *reply = dbus_message_new_method_return(msg);
	DBusMessageIter it, array;
	GSList *l;

	dbus_message_iter_init_append(reply, &it);
	dbus_message_iter_open_container(&it, DBUS_TYPE_ARRAY, "o", &array);
	for (l = dbus->entries; l; l = l->next) {
		const struct ril_cell_entry *entry = l->data;
		dbus_message_iter_append_basic(&array, DBUS_TYPE_OBJECT_PATH,
								&entry->path);
	}
	dbus_message_iter_close_container(&it, &array);
	return reply;
}

static const GDBusMethodTable ril_cell_info_dbus_methods[] = {
	{ GDBUS_METHOD("GetCells", NULL,
			GDBUS_ARGS({ "paths", "ao" }),
			ril_cell_info_dbus_get_cells) },
	{ }
};

static const GDBusSignalTable ril_cell_info_dbus_signals[] = {
	{ GDBUS_SIGNAL(RIL_CELL_INFO_DBUS_CELLS_ADDED_SIGNAL,
			GDBUS_ARGS({ "paths", "ao" })) },
	{ GDBUS_SIGNAL(RIL_CELL_INFO_DBUS_CELLS_REMOVED_SIGNAL,
			GDBUS_ARGS({ "paths", "ao" })) },
	{ }
};

struct ril_cell_info_dbus *ril_cell_info_dbus_new(struct ril_modem *md,
						struct ril_cell_info *info)
{
	struct ril_cell_info_dbus *dbus = g_new0(struct ril_cell_info_dbus, 1);

	DBG("%s", ril_modem_get_path(md));
	dbus->md = md;
	dbus->path = g_strdup(ril_modem_get_path(md));
	dbus->conn = dbus_connection_ref(ofono_dbus_get_connection());
	dbus->info = ril_cell_info_ref(info);
	dbus->handler_id = ril_cell_info_add_cells_changed_handler(info,
				ril_cell_info_dbus_cells_changed_cb, dbus);

	/* Register D-Bus interface */
	if (g_dbus_register_interface(dbus->conn, dbus->path,
			RIL_CELL_INFO_DBUS_INTERFACE, ril_cell_info_dbus_methods,
			ril_cell_info_dbus_signals, NULL, dbus, NULL)) {
		ofono_modem_add_interface(md->ofono,
						RIL_CELL_INFO_DBUS_INTERFACE);
		ril_cell_info_dbus_update_entries(dbus, FALSE);
		return dbus;
	} else {
		ofono_error("RIL D-Bus register failed");
		ril_cell_info_dbus_free(dbus);
		return NULL;
	}
}

void ril_cell_info_dbus_free(struct ril_cell_info_dbus *dbus)
{
	if (dbus) {
		GSList *l;

		DBG("%s", dbus->path);
		g_dbus_unregister_interface(dbus->conn, dbus->path,
						RIL_CELL_INFO_DBUS_INTERFACE);
		ofono_modem_remove_interface(dbus->md->ofono,
						RIL_CELL_INFO_DBUS_INTERFACE);

		/* Unregister cells */
		l = dbus->entries;
		while (l) {
			struct ril_cell_entry *entry = l->data;
			g_dbus_unregister_interface(dbus->conn, entry->path,
						RIL_CELL_DBUS_INTERFACE);
			ril_cell_info_destroy_entry(entry);
			l = l->next;
		}
		g_slist_free(dbus->entries);

		dbus_connection_unref(dbus->conn);

		ril_cell_info_remove_handler(dbus->info, dbus->handler_id);
		ril_cell_info_unref(dbus->info);

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
