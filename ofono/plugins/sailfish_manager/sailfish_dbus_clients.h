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

#ifndef SAILFISH_DBUS_CLIENT_H
#define SAILFISH_DBUS_CLIENT_H

#include <glib.h>
#include <ofono/dbus.h>

struct sailfish_dbus_clients;
struct sailfish_dbus_client;

struct sailfish_dbus_clients* sailfish_dbus_clients_new(DBusConnection* conn,
				void (*disconnected_cb)(void *user_data),
				void *user_data);

void sailfish_dbus_clients_free(struct sailfish_dbus_clients* self);

void sailfish_dbus_clients_remove_client(struct sailfish_dbus_client *client);

guint sailfish_dbus_clients_count(struct sailfish_dbus_clients* clients);

struct sailfish_dbus_client* sailfish_dbus_clients_new_client(
                                        struct sailfish_dbus_clients* self,
                                        DBusMessage *msg);

struct sailfish_dbus_client *sailfish_dbus_clients_lookup_client(
					struct sailfish_dbus_clients* clients,
					DBusMessage *msg);

void sailfish_dbus_clients_send(struct sailfish_dbus_clients* clients,
					DBusMessage* signal);

void sailfish_dbus_clients_send_to(struct sailfish_dbus_client *client,
					DBusMessage* msg);

int sailfish_dbus_clients_signal_property_changed(
				struct sailfish_dbus_clients* self,
				const char *path,
				const char *interface,
				const char *name,
				int type, const void *value);

#endif /* SAILFISH_DBUS_CLIENT_H */

/*
 * Local Variables:
 * mode: C
 * c-basic-offset: 4
 * indent-tabs-mode: nil
 * End:
 */
