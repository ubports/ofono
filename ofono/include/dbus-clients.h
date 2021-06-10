/*
 *  oFono - Open Source Telephony
 *
 *  Copyright (C) 2021 Jolla Ltd.
 *  Copyright (C) 2021 Open Mobile Platform LLC.
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

#ifndef OFONO_DBUS_CLIENTS_H
#define OFONO_DBUS_CLIENTS_H

#include <ofono/types.h>
#include <ofono/dbus.h>

/* Since mer/1.23+git31 */

struct ofono_dbus_clients;

typedef void (*ofono_dbus_clients_notify_func)(const char *name,
							void *user_data);

struct ofono_dbus_clients *ofono_dbus_clients_new(DBusConnection *conn,
		ofono_dbus_clients_notify_func notify, void *user_data);
void ofono_dbus_clients_free(struct ofono_dbus_clients *clients);

unsigned int ofono_dbus_clients_count(struct ofono_dbus_clients *clients);

ofono_bool_t ofono_dbus_clients_add(struct ofono_dbus_clients *clients,
							const char *name);
ofono_bool_t ofono_dbus_clients_remove(struct ofono_dbus_clients *clients,
							const char *name);

void ofono_dbus_clients_signal(struct ofono_dbus_clients *clients,
							DBusMessage *signal);
void ofono_dbus_clients_signal_property_changed(struct ofono_dbus_clients *dc,
		const char *path, const char *interface, const char *name,
		int type, const void *value);

#endif /* OFONO_DBUS_CLIENTS_H */

/*
 * Local Variables:
 * mode: C
 * c-basic-offset: 8
 * indent-tabs-mode: t
 * End:
 */
