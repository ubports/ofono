/*
 *  oFono - Open Source Telephony
 *
 *  Copyright (C) 2018 Jolla Ltd.
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

#ifndef TEST_DBUS_H
#define TEST_DBUS_H

#include <ofono/gdbus.h>

struct test_dbus_context {
	GMainLoop *loop;
	DBusServer *server;
	DBusConnection *server_connection;
	DBusConnection *client_connection;
	GSList* client_signals;
	void (*start)(struct test_dbus_context *test);
	guint timeout_id;
};

void test_dbus_setup(struct test_dbus_context *context);
void test_dbus_shutdown(struct test_dbus_context *context);

void test_dbus_watch_disconnect_all(void);
void test_dbus_watch_remove_all(void);

int test_dbus_get_int32(DBusMessageIter *it);
gboolean test_dbus_get_bool(DBusMessageIter *it);
const char *test_dbus_get_string(DBusMessageIter *it);
const char *test_dbus_get_object_path(DBusMessageIter *it);

void test_dbus_check_error_reply(DBusPendingCall *call, const char *error);
void test_dbus_check_string_reply(DBusPendingCall *call, const char *str);
void test_dbus_check_empty_reply(DBusPendingCall *call, void *unused);
void test_dbus_expect_empty_reply(DBusPendingCall *call, void *data);

DBusMessage *test_dbus_find_signal(struct test_dbus_context *test,
		const char *path, const char *iface, const char *member);
DBusMessage *test_dbus_take_signal(struct test_dbus_context *test,
		const char *path, const char *iface, const char *member);

#endif /* TEST_DBUS_H */

/*
 * Local Variables:
 * mode: C
 * c-basic-offset: 8
 * indent-tabs-mode: t
 * End:
 */
