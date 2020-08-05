
#include <gdbus/gdbus.h>
#include <ofono/dbus.h>
#include "sailfish_dbus_clients.h"

struct sailfish_dbus_clients {
	DBusConnection *conn;
	guint n_clients;
	void (*disconnected_cb)(void *user_data);
	void *disconnected_cb_data;
};

struct sailfish_dbus_client {
	struct sailfish_dbus_clients *clients;
};

static struct sailfish_dbus_client fake_client;

struct sailfish_dbus_clients* sailfish_dbus_clients_new(DBusConnection* conn,
				void (*disconnected_cb)(void *user_data),
				void *user_data)
{
	struct sailfish_dbus_clients* self =
				g_slice_new0(struct sailfish_dbus_clients);
	self->conn = dbus_connection_ref(conn);
	self->disconnected_cb = disconnected_cb;
	self->disconnected_cb_data = user_data;
	return self;
}

void sailfish_dbus_clients_free(struct sailfish_dbus_clients* self)
{
	if (self) {
		dbus_connection_unref(self->conn);
		g_slice_free(struct sailfish_dbus_clients, self);
	}
}

guint sailfish_dbus_clients_count(struct sailfish_dbus_clients* self)
{
	return self->n_clients;
}

struct sailfish_dbus_client* sailfish_dbus_clients_new_client(
                                        struct sailfish_dbus_clients* self,
                                        DBusMessage *msg)
{
	if (self && msg) {
		self->n_clients++;
	}

	fake_client.clients = self;
	return &fake_client;
}

struct sailfish_dbus_client *sailfish_dbus_clients_lookup_client(
					struct sailfish_dbus_clients* self,
					DBusMessage *msg)
{
	return &fake_client;
}

void sailfish_dbus_clients_remove_client(struct sailfish_dbus_client* client)
{
	if (client && client->clients && client->clients->n_clients) {
		struct sailfish_dbus_clients* clients = client->clients;

		clients->n_clients--;
		clients->disconnected_cb(clients->disconnected_cb_data);
	}
}

void sailfish_dbus_clients_send(struct sailfish_dbus_clients* self,
					DBusMessage* signal)
{
	if (self && signal) {
		g_dbus_send_message(self->conn, signal);
	}
}

void sailfish_dbus_clients_send_to(struct sailfish_dbus_client* client,
					DBusMessage* signal)
{
	if (client && client->clients && signal) {
		g_dbus_send_message(client->clients->conn, signal);
	}
}

int sailfish_dbus_clients_signal_property_changed(
				struct sailfish_dbus_clients* self,
				const char *path,
				const char *interface,
				const char *name,
				int type, const void *value)
{
	ofono_dbus_signal_property_changed(self->conn,
					path, interface, name, type, value);
	return 0;
}
