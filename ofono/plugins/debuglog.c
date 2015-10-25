/*
 *
 *  oFono - Open Source Telephony
 *
 *  Copyright (C) 2015 Jolla Ltd. All rights reserved.
 *  Contact: Slava Monich <slava.monich@jolla.com>
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License version 2 as
 *  published by the Free Software Foundation.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
 *
 */

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <gdbus.h>
#include <string.h>

#define OFONO_API_SUBJECT_TO_CHANGE
#include <ofono/plugin.h>
#include <ofono/log.h>
#include <ofono.h>

#define DEBUGLOG_INTERFACE      OFONO_SERVICE ".DebugLog"
#define DEBUGLOG_PATH           "/"
#define DEBUGLOG_CHANGED_SIGNAL "Changed"

static DBusConnection *connection = NULL;

extern struct ofono_debug_desc __start___debug[];
extern struct ofono_debug_desc __stop___debug[];

static void debuglog_signal(DBusConnection *conn, const char *name,
								guint flags)
{
	DBusMessage *signal = dbus_message_new_signal(DEBUGLOG_PATH,
				DEBUGLOG_INTERFACE, DEBUGLOG_CHANGED_SIGNAL);

	if (signal) {
		DBusMessageIter iter;
		const dbus_bool_t on = (flags & OFONO_DEBUG_FLAG_PRINT) != 0;

		dbus_message_iter_init_append(signal, &iter);
		dbus_message_iter_append_basic(&iter, DBUS_TYPE_STRING, &name);
		dbus_message_iter_append_basic(&iter, DBUS_TYPE_BOOLEAN, &on);
		g_dbus_send_message(conn, signal);
	}
}

static GHashTable *debuglog_update_flags_hash(GHashTable *hash,
					const char *name, guint flags)
{
	if (name) {
		gpointer key = (gpointer)name;
		guint value;
		if (!hash) {
			hash = g_hash_table_new_full(g_str_hash, g_str_equal,
								NULL, NULL);
		}

		value = GPOINTER_TO_INT(g_hash_table_lookup(hash, key));
		value |= flags;
		g_hash_table_insert(hash, key, GINT_TO_POINTER(value));
	}

	return hash;
}

static gboolean debuglog_match(const char* name, const char* pattern)
{
	return name && g_pattern_match_simple(pattern, name);
}

static void debuglog_update(DBusConnection *conn, const char* pattern,
					guint set_flags, guint clear_flags)
{
	const guint flags = set_flags | clear_flags;
	struct ofono_debug_desc *start = __start___debug;
	struct ofono_debug_desc *stop = __stop___debug;
	struct ofono_debug_desc *desc;
	GHashTable *hash = NULL;

	if (!start || !stop)
		return;


	for (desc = start; desc < stop; desc++) {
		const char *matched = NULL;

		if (debuglog_match(desc->file, pattern)) {
			matched = desc->file;
		} else if (debuglog_match(desc->name, pattern)) {
			matched = desc->name;
		}

		if (matched) {
			const guint old_flags = (desc->flags & flags);
			desc->flags |= set_flags;
			desc->flags &= ~clear_flags;
			if ((desc->flags & flags) != old_flags) {
				hash = debuglog_update_flags_hash(hash,
							matched, desc->flags);
				if (desc->notify) {
					desc->notify(desc);
				}
			}
		}
	}

	if (hash) {
		GList *entry, *names = g_hash_table_get_keys(hash);

		for (entry = names; entry; entry = entry->next) {
			debuglog_signal(conn, entry->data,
				GPOINTER_TO_INT(g_hash_table_lookup(
						hash, entry->data)));
		}

		g_list_free(names);
		g_hash_table_destroy(hash);
	}
}

static DBusMessage *debuglog_handle(DBusConnection *conn, DBusMessage *msg,
					guint set_flags, guint clear_flags)
{
	const char *pattern;

	if (dbus_message_get_args(msg, NULL, DBUS_TYPE_STRING, &pattern,
							DBUS_TYPE_INVALID)) {
		debuglog_update(conn, pattern, set_flags, clear_flags);
		return g_dbus_create_reply(msg, DBUS_TYPE_INVALID);
	} else {
		return __ofono_error_invalid_args(msg);
	}
}

static DBusMessage *debuglog_enable(DBusConnection *conn,
					DBusMessage *msg, void *data)
{
	return debuglog_handle(conn, msg, OFONO_DEBUG_FLAG_PRINT, 0);
}

static DBusMessage *debuglog_disable(DBusConnection *conn,
					DBusMessage *msg, void *data)
{
	return debuglog_handle(conn, msg, 0, OFONO_DEBUG_FLAG_PRINT);
}

static gint debuglog_list_compare(gconstpointer a, gconstpointer b)
{
	return strcmp(a, b);
}

static void debuglog_list_append(DBusMessageIter *iter, const char *name,
							guint flags)
{
	DBusMessageIter entry;
	dbus_bool_t enabled = (flags & OFONO_DEBUG_FLAG_PRINT) != 0;

	dbus_message_iter_open_container(iter, DBUS_TYPE_STRUCT, NULL, &entry);
	dbus_message_iter_append_basic(&entry, DBUS_TYPE_STRING, &name);
	dbus_message_iter_append_basic(&entry, DBUS_TYPE_BOOLEAN, &enabled);
	dbus_message_iter_close_container(iter, &entry);
}

static DBusMessage *debuglog_list(DBusConnection *conn,
					DBusMessage *msg, void *data)
{
	DBusMessage *reply = dbus_message_new_method_return(msg);

	if (reply) {
		struct ofono_debug_desc *start = __start___debug;
		struct ofono_debug_desc *stop = __stop___debug;
		DBusMessageIter iter, array;

		dbus_message_iter_init_append(reply, &iter);
		dbus_message_iter_open_container(&iter, DBUS_TYPE_ARRAY,
							"(sb)", &array);

		if (start && stop) {
			struct ofono_debug_desc *desc;
			GList *names, *entry;
			GHashTable *hash = g_hash_table_new_full(g_str_hash,
						g_str_equal, NULL, NULL);

			for (desc = start; desc < stop; desc++) {
				debuglog_update_flags_hash(hash, desc->file,
								desc->flags);
				debuglog_update_flags_hash(hash, desc->name,
								desc->flags);
			}

			names = g_list_sort(g_hash_table_get_keys(hash),
						debuglog_list_compare);
			for (entry = names; entry; entry = entry->next) {
				const char *name = entry->data;
				debuglog_list_append(&array, name,
					GPOINTER_TO_INT(g_hash_table_lookup(
								hash, name)));
			}

			g_list_free(names);
			g_hash_table_destroy(hash);
		}

		dbus_message_iter_close_container(&iter, &array);
	}

	return reply;
}

static const GDBusMethodTable debuglog_methods[] = {
	{ GDBUS_METHOD("Enable", GDBUS_ARGS({ "pattern", "s" }), NULL,
							debuglog_enable) },
	{ GDBUS_METHOD("Disable", GDBUS_ARGS({ "pattern", "s" }), NULL,
							debuglog_disable) },
	{ GDBUS_METHOD("List", NULL, GDBUS_ARGS({ "list", "a(sb)" }),
							debuglog_list) },
	{ },
};

static const GDBusSignalTable debuglog_signals[] = {
	{ GDBUS_SIGNAL(DEBUGLOG_CHANGED_SIGNAL,
			GDBUS_ARGS({ "name", "s" }, { "enabled", "b" })) },
	{ }
};

static int debuglog_init(void)
{
	DBG("");

	connection = ofono_dbus_get_connection();
	if (!connection)
		return -1;

	if (!g_dbus_register_interface(connection, DEBUGLOG_PATH,
		DEBUGLOG_INTERFACE, debuglog_methods, debuglog_signals,
		NULL, NULL, NULL)) {
		ofono_error("debuglog: failed to register "
							DEBUGLOG_INTERFACE);
		return -1;
	}

	return 0;
}

static void debuglog_exit(void)
{
	DBG("");

	if (connection) {
		g_dbus_unregister_interface(connection, DEBUGLOG_PATH,
							DEBUGLOG_INTERFACE);
		dbus_connection_unref(connection);
		connection = NULL;
	}
}

OFONO_PLUGIN_DEFINE(debuglog, "Debug log control interface",
			VERSION, OFONO_PLUGIN_PRIORITY_DEFAULT,
			debuglog_init, debuglog_exit)
