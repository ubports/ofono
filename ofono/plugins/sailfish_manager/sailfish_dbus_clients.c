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

#include "sailfish_dbus_clients.h"

#include <ofono/gdbus.h>
#include <ofono/log.h>

#include "dbusaccess_peer.h"

struct sailfish_dbus_client {
	struct sailfish_dbus_clients* clients;
	DAPeer* peer;
	guint watch_id;
};

struct sailfish_dbus_clients {
	DBusConnection* conn;
	GHashTable* table;
	void (*disconnect_cb)(void *disconnect_cb_data);
	void *disconnect_cb_data;
};

static void sailfish_dbus_client_free(struct sailfish_dbus_client* client)
{
	/* Callers make sure that client parameter is not NULL */
	if (client->watch_id) {
		g_dbus_remove_watch(client->clients->conn, client->watch_id);
	}
	da_peer_unref(client->peer);
	g_slice_free(struct sailfish_dbus_client, client);
}

static void sailfish_dbus_client_free1(void* data)
{
	sailfish_dbus_client_free(data);
}

void sailfish_dbus_clients_remove_client(struct sailfish_dbus_client *client)
{
	if (client && client->clients) {
		struct sailfish_dbus_clients *clients = client->clients;

		g_hash_table_remove(clients->table, client->peer->name);
		if (clients->disconnect_cb &&
				!sailfish_dbus_clients_count(clients)) {
			clients->disconnect_cb(clients->disconnect_cb_data);
		}
	}
}

static void sailfish_dbus_client_disconnected(DBusConnection* connection,
						void* user_data)
{
	struct sailfish_dbus_client* client = user_data;

	/* This deallocates struct sailfish_dbus_client: */
	DBG("%s is gone", client->peer->name);
	sailfish_dbus_clients_remove_client(client);
}

struct sailfish_dbus_clients* sailfish_dbus_clients_new(DBusConnection* conn,
				void (*disconnect_cb)(void *disconnect_cb_data),
				void *disconnect_cb_data)
{
	struct sailfish_dbus_clients* self =
				g_slice_new0(struct sailfish_dbus_clients);

	self->conn = dbus_connection_ref(conn);
	self->table = g_hash_table_new_full(g_str_hash, g_str_equal, NULL,
						sailfish_dbus_client_free1);
	self->disconnect_cb = disconnect_cb;
	self->disconnect_cb_data = disconnect_cb_data;
	return self;
}

void sailfish_dbus_clients_free(struct sailfish_dbus_clients* self)
{
	if (self) {
		g_hash_table_destroy(self->table);
		dbus_connection_unref(self->conn);
		g_slice_free(struct sailfish_dbus_clients, self);
	}
}

guint sailfish_dbus_clients_count(struct sailfish_dbus_clients* self)
{
	return self ? g_hash_table_size(self->table) : 0;
}

static void sailfish_dbus_clients_register(struct sailfish_dbus_clients* self,
						DAPeer* peer)
{
	if (self && peer && !g_hash_table_contains(self->table, peer->name)) {
		struct sailfish_dbus_client* client =
				g_slice_new0(struct sailfish_dbus_client);

		client->clients = self;
		client->peer = da_peer_ref(peer);
		client->watch_id = g_dbus_add_disconnect_watch(
					self->conn, peer->name,
					sailfish_dbus_client_disconnected,
					client, NULL);
		if (client->watch_id) {
			DBG("%s is registered", peer->name);
			g_hash_table_replace(self->table,
						(gpointer)peer->name, client);
		} else {
			DBG("failed to register %s", peer->name);
			sailfish_dbus_client_free(client);
		}
	}
}

struct sailfish_dbus_client* sailfish_dbus_clients_lookup_client(
					struct sailfish_dbus_clients* self,
					DBusMessage *msg)
{
	if (self && msg) {
		DAPeer *peer = da_peer_get(DA_BUS_SYSTEM,
						dbus_message_get_sender(msg));

		if (peer)
			return g_hash_table_lookup(self->table, peer->name);
	}

	return NULL;
}

struct sailfish_dbus_client* sailfish_dbus_clients_new_client(
					struct sailfish_dbus_clients* self,
					DBusMessage *msg)
{
	if (self && msg) {
		DAPeer *peer = da_peer_get(DA_BUS_SYSTEM,
						dbus_message_get_sender(msg));

		if (peer) {
			sailfish_dbus_clients_register(self, peer);
			return g_hash_table_lookup(self->table, peer->name);
		}
	}

	return NULL;
}

void sailfish_dbus_clients_send(struct sailfish_dbus_clients* self,
					DBusMessage* msg)
{
	if (self && msg && g_hash_table_size(self->table)) {
		GHashTableIter it;
		gpointer key;
		const char* last_name = NULL;

		g_hash_table_iter_init(&it, self->table);
		g_hash_table_iter_next(&it, &key, NULL);
		last_name = key;

		while (g_hash_table_iter_next(&it, &key, NULL)) {
			DBusMessage* copy = dbus_message_copy(msg);

			dbus_message_set_destination(copy, (const char*)key);
			g_dbus_send_message(self->conn, copy);
		}

		/* The last one */
		dbus_message_set_destination(msg, last_name);
		g_dbus_send_message(self->conn, msg);
	}
}

void sailfish_dbus_clients_send_to(struct sailfish_dbus_client *client,
					DBusMessage* msg)
{
	if (client && msg) {
		dbus_message_set_destination(msg, client->peer->name);
		g_dbus_send_message(client->clients->conn, msg);
	}
}

static void append_variant(DBusMessageIter *iter, int type, const void *value)
{
	char sig[2];
	DBusMessageIter valueiter;

	sig[0] = type;
	sig[1] = 0;

	dbus_message_iter_open_container(iter, DBUS_TYPE_VARIANT,
						sig, &valueiter);
	dbus_message_iter_append_basic(&valueiter, type, value);
	dbus_message_iter_close_container(iter, &valueiter);
}

int sailfish_dbus_clients_signal_property_changed(
				struct sailfish_dbus_clients* self,
				const char *path,
				const char *interface,
				const char *name,
				int type, const void *value)
{
	if (self) {
		DBusMessage *signal;
		DBusMessageIter iter;

		signal = dbus_message_new_signal(path, interface,
						"PropertyChanged");
		if (signal == NULL) {
			ofono_error("Unable to allocate new signal for %s",
						interface);
			return -1;
		}

		dbus_message_iter_init_append(signal, &iter);
		dbus_message_iter_append_basic(&iter, DBUS_TYPE_STRING, &name);
		append_variant(&iter, type, value);
		sailfish_dbus_clients_send(self, signal);
	}
	return 0;
}

/*
 * Local Variables:
 * mode: C
 * c-basic-offset: 8
 * indent-tabs-mode: t
 * End:
 */
