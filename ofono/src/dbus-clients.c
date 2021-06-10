/*
 *  oFono - Open Source Telephony
 *
 *  Copyright (C) 2017-2018 Jolla Ltd.
 *  Copyright (C) 2020 Open Mobile Platform LLC.
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

#include <ofono/dbus-clients.h>
#include <ofono/gdbus.h>
#include <ofono/log.h>

struct ofono_dbus_client {
	struct ofono_dbus_clients *clients;
	char *name;
	unsigned int watch_id;
};

struct ofono_dbus_clients {
	DBusConnection* conn;
	GHashTable* table;
	ofono_dbus_clients_notify_func notify;
	void *user_data;
};

/* Compatible with GDestroyNotify */
static void ofono_dbus_client_free(struct ofono_dbus_client *client)
{
	struct ofono_dbus_clients *clients = client->clients;

	/* Callers make sure that client parameter is not NULL */
	if (client->watch_id) {
		g_dbus_remove_watch(clients->conn, client->watch_id);
	}
	g_free(client->name);
	g_slice_free(struct ofono_dbus_client, client);
}

static void ofono_dbus_clients_disconnect_notify(DBusConnection *connection,
							void *user_data)
{
	struct ofono_dbus_client *client = user_data;
	struct ofono_dbus_clients *self = client->clients;
	char *name = client->name;

	/*
	 * Steal the name so that it doesn't get freed by
	 * ofono_dbus_client_free(). We want to pass it to
	 * the callback but first we need to delete client's
	 * entry from the hashtable.
	 */
	client->name = NULL;
	DBG("%s is gone", name);
	g_hash_table_remove(self->table, name);
	if (self->notify) {
		self->notify(name, self->user_data);
	}
	g_free(name);
}

struct ofono_dbus_clients *ofono_dbus_clients_new(DBusConnection *conn,
		ofono_dbus_clients_notify_func notify, void *user_data)
{
	if (conn) {
		struct ofono_dbus_clients *self =
			g_slice_new0(struct ofono_dbus_clients);

		self->conn = dbus_connection_ref(conn);
		self->table = g_hash_table_new_full(g_str_hash, g_str_equal,
				NULL, (GDestroyNotify) ofono_dbus_client_free);
		self->notify = notify;
		self->user_data = user_data;
		return self;
	}
	return NULL;
}

void ofono_dbus_clients_free(struct ofono_dbus_clients *self)
{
	if (self) {
		g_hash_table_destroy(self->table);
		dbus_connection_unref(self->conn);
		g_slice_free(struct ofono_dbus_clients, self);
	}
}

unsigned int ofono_dbus_clients_count(struct ofono_dbus_clients *self)
{
	return self ? g_hash_table_size(self->table) : 0;
}

ofono_bool_t ofono_dbus_clients_add(struct ofono_dbus_clients *self,
							const char *name)
{
	if (self && name) {
		struct ofono_dbus_client *client =
				g_slice_new0(struct ofono_dbus_client);

		client->clients = self;
		client->name = g_strdup(name);
		client->watch_id = g_dbus_add_disconnect_watch(self->conn,
			client->name, ofono_dbus_clients_disconnect_notify,
			client, NULL);

		if (client->watch_id) {
			DBG("%s is registered", client->name);
			g_hash_table_replace(self->table, (gpointer)
				client->name, client);
			return TRUE;
		} else {
			DBG("failed to register %s", client->name);
			ofono_dbus_client_free(client);
		}
	}
	return FALSE;
}

ofono_bool_t ofono_dbus_clients_remove(struct ofono_dbus_clients *self,
							const char *name)
{
	return self && name && g_hash_table_remove(self->table, name);
}

void ofono_dbus_clients_signal(struct ofono_dbus_clients *self,
							DBusMessage *signal)
{
	if (self && signal && g_hash_table_size(self->table)) {
		GHashTableIter it;
		gpointer key;
		const char *last_name = NULL;

		g_hash_table_iter_init(&it, self->table);
		g_hash_table_iter_next(&it, &key, NULL);
		last_name = key;

		while (g_hash_table_iter_next(&it, &key, NULL)) {
			DBusMessage *copy = dbus_message_copy(signal);

			dbus_message_set_destination(copy, key);
			g_dbus_send_message(self->conn, copy);
		}

		/*
		 * The last one. Note that g_dbus_send_message() unrefs
		 * the message, we need compensate for that by adding a
		 * reference. The caller still owns the message when this
		 * function returns.
		 */
		dbus_message_ref(signal);
		dbus_message_set_destination(signal, last_name);
		g_dbus_send_message(self->conn, signal);
	}
}

void ofono_dbus_clients_signal_property_changed(struct ofono_dbus_clients *self,
		const char *path, const char *interface, const char *name,
		int type, const void *value)
{
	if (self && g_hash_table_size(self->table)) {
		DBusMessage *sig = ofono_dbus_signal_new_property_changed(path,
			interface, name, type, value);

		ofono_dbus_clients_signal(self, sig);
		dbus_message_unref(sig);
	}
}

/*
 * Local Variables:
 * mode: C
 * c-basic-offset: 8
 * indent-tabs-mode: t
 * End:
 */
