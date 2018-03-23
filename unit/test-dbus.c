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

#include "test-dbus.h"
#include "ofono.h"

#include <dbus/dbus-glib-lowlevel.h>

#include <unistd.h>

struct test_polkit_auth_check {
	void (*function)(dbus_bool_t authorized, void *user_data);
	void *user_data;
};

struct test_dbus_watch {
	struct test_dbus_watch *next;
	guint id;
	DBusConnection *connection;
	GDBusWatchFunction disconnect;
	GDBusDestroyFunction destroy;
	void *user_data;
};

struct test_dbus_watch *test_dbus_watch_list;
static unsigned int test_last_id;

static gboolean polkit_check_authorization_cb(gpointer data)
{
	struct test_polkit_auth_check *check = data;

	check->function(TRUE, check->user_data);
	g_free(check);
	return G_SOURCE_REMOVE;
}

int polkit_check_authorization(DBusConnection *conn,
		const char *action, gboolean interaction,
		void (*function)(dbus_bool_t authorized, void *user_data),
		void *user_data, int timeout)
{
	struct test_polkit_auth_check *check =
		g_new(struct test_polkit_auth_check, 1);

	check->function = function;
	check->user_data = user_data;
	g_idle_add(polkit_check_authorization_cb, check);
	return 0;
}

static guint test_dbus_add_watch(DBusConnection *connection,
		GDBusWatchFunction disconnect, GDBusDestroyFunction destroy,
		void *user_data)
{
	struct test_dbus_watch *watch = g_new0(struct test_dbus_watch, 1);

	test_last_id++;
	watch->id = test_last_id;
	watch->connection = dbus_connection_ref(connection);
	watch->disconnect = disconnect;
	watch->destroy = destroy;
	watch->user_data = user_data;
	watch->next = test_dbus_watch_list;
	test_dbus_watch_list = watch;
	return test_last_id;
}

static gboolean test_dbus_watch_disconnect_one(void)
{
	struct test_dbus_watch *watch = test_dbus_watch_list;

	while (watch) {
		if (watch->disconnect) {
			GDBusWatchFunction disconnect = watch->disconnect;

			watch->disconnect = NULL;
			disconnect(watch->connection, watch->user_data);
			return TRUE;
		}
		watch = watch->next;
	}
	return FALSE;
}

void test_dbus_watch_disconnect_all(void)
{
	while (test_dbus_watch_disconnect_one());
}

static void test_dbus_watch_free(struct test_dbus_watch *watch)
{
	if (watch->destroy) {
		watch->destroy(watch->user_data);
	}
	dbus_connection_unref(watch->connection);
	g_free(watch);
}

static gboolean test_dbus_watch_remove_one(void)
{
	struct test_dbus_watch *watch = test_dbus_watch_list;

	if (watch) {
		test_dbus_watch_list = watch->next;
		test_dbus_watch_free(watch);
		return TRUE;
	}
	return FALSE;
}

void test_dbus_watch_remove_all(void)
{
	while (test_dbus_watch_remove_one());
}

guint g_dbus_add_signal_watch(DBusConnection *connection, const char *sender,
		const char *path, const char *interface, const char *member,
		GDBusSignalFunction function, void *user_data,
		GDBusDestroyFunction destroy)
{
    return test_dbus_add_watch(connection, NULL, destroy, user_data);
}

gboolean g_dbus_remove_watch(DBusConnection *connection, guint id)
{
	struct test_dbus_watch *prev = NULL;
	struct test_dbus_watch *watch = test_dbus_watch_list;

	while (watch) {
		if (watch->id == id) {
			if (prev) {
				prev->next = watch->next;
			} else {
				test_dbus_watch_list = watch->next;
			}
			test_dbus_watch_free(watch);
			return TRUE;
		}
		prev = watch;
		watch = watch->next;
	}
	ofono_warn("Unexpected id %u", id);
	return FALSE;
}

static gboolean test_dbus_loop_quit(gpointer data)
{
	g_main_loop_quit(data);
	return G_SOURCE_REMOVE;
}

static void test_dbus_loop_quit_later(GMainLoop *loop)
{
	g_idle_add(test_dbus_loop_quit, loop);
}

DBusMessage *test_dbus_find_signal(struct test_dbus_context *test,
		const char *path, const char *iface, const char *member)
{
	GSList *l;

	for (l = test->client_signals; l; l = l->next) {
		DBusMessage *msg = l->data;

		if (!g_strcmp0(dbus_message_get_interface(msg), iface) &&
			!g_strcmp0(dbus_message_get_member(msg), member) &&
			!g_strcmp0(dbus_message_get_path(msg), path)) {
			return msg;
		}
	}
	return NULL;
}

DBusMessage *test_dbus_take_signal(struct test_dbus_context *test,
		const char *path, const char *iface, const char *member)
{
	DBusMessage *m = test_dbus_find_signal(test, path, iface, member);

	if (m) {
		test->client_signals = g_slist_remove(test->client_signals, m);
		return m;
	}
	return NULL;
}

int test_dbus_get_int32(DBusMessageIter *it)
{
	dbus_uint32_t value;

	g_assert(dbus_message_iter_get_arg_type(it) == DBUS_TYPE_INT32);
	dbus_message_iter_get_basic(it, &value);
	dbus_message_iter_next(it);
	return value;
}

gboolean test_dbus_get_bool(DBusMessageIter *it)
{
	dbus_bool_t value;

	g_assert(dbus_message_iter_get_arg_type(it) == DBUS_TYPE_BOOLEAN);
	dbus_message_iter_get_basic(it, &value);
	dbus_message_iter_next(it);
	return value;
}

const char *test_dbus_get_string(DBusMessageIter *it)
{
	const char *value;

	g_assert(dbus_message_iter_get_arg_type(it) == DBUS_TYPE_STRING);
	dbus_message_iter_get_basic(it, &value);
	dbus_message_iter_next(it);
	return value;
}

const char *test_dbus_get_object_path(DBusMessageIter *it)
{
	const char *value;

	g_assert(dbus_message_iter_get_arg_type(it) == DBUS_TYPE_OBJECT_PATH);
	dbus_message_iter_get_basic(it, &value);
	dbus_message_iter_next(it);
	return value;
}

void test_dbus_check_empty_reply(DBusPendingCall *call, void *unused)
{
	DBusMessage *reply = dbus_pending_call_steal_reply(call);
	DBusMessageIter it;

	g_assert(dbus_message_get_type(reply) ==
					DBUS_MESSAGE_TYPE_METHOD_RETURN);

	dbus_message_iter_init(reply, &it);
	g_assert(dbus_message_iter_get_arg_type(&it) == DBUS_TYPE_INVALID);

	dbus_message_unref(reply);
	dbus_pending_call_unref(call);
}

void test_dbus_expect_empty_reply(DBusPendingCall *call, void *data)
{
	struct test_dbus_context *test = data;

	DBG("");
	test_dbus_check_empty_reply(call, data);
	test_dbus_loop_quit_later(test->loop);
}

void test_dbus_check_error_reply(DBusPendingCall *call, const char *error)
{
	DBusMessage *msg = dbus_pending_call_steal_reply(call);
	const char *name;

	g_assert(dbus_message_get_type(msg) == DBUS_MESSAGE_TYPE_ERROR);
	name = dbus_message_get_error_name(msg);
	g_assert(!g_strcmp0(name, error));
	dbus_message_unref(msg);
	dbus_pending_call_unref(call);
}

void test_dbus_check_string_reply(DBusPendingCall *call, const char *str)
{
	DBusMessage *reply = dbus_pending_call_steal_reply(call);
	DBusMessageIter it;

	DBG("");
	g_assert(dbus_message_get_type(reply) ==
					DBUS_MESSAGE_TYPE_METHOD_RETURN);

	dbus_message_iter_init(reply, &it);
	g_assert(!g_strcmp0(test_dbus_get_string(&it), str));
	g_assert(dbus_message_iter_get_arg_type(&it) == DBUS_TYPE_INVALID);

	dbus_message_unref(reply);
}

void test_dbus_message_unref(gpointer data)
{
	dbus_message_unref(data);
}

static DBusHandlerResult test_dbus_client_message_cb(DBusConnection *conn,
						DBusMessage *msg, void *data)
{
	struct test_dbus_context *test = data;

	if (dbus_message_get_type(msg) == DBUS_MESSAGE_TYPE_SIGNAL) {
		DBG("signal %s.%s \"%s\"", dbus_message_get_interface(msg),
						dbus_message_get_member(msg),
						dbus_message_get_path(msg));
		test->client_signals = g_slist_append(test->client_signals,
						dbus_message_ref(msg));
		return DBUS_HANDLER_RESULT_HANDLED;
	} else {
		return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
	}
}

static void test_dbus_connection_cb(DBusServer *server, DBusConnection *conn,
								void *data)
{
	struct test_dbus_context *test = data;

	DBG("");
	g_assert(!test->server_connection);
	test->server_connection = dbus_connection_ref(conn);
	dbus_connection_setup_with_g_main(test->server_connection,
				g_main_loop_get_context(test->loop));

	/* Start the test */
	__ofono_dbus_init(test->server_connection);
	test->start(test);
}

void test_dbus_setup(struct test_dbus_context *test)
{
	char *path;
	char *address;
	GMainContext *context;

	/* Generate unique non-existent path */
	path = g_dir_make_tmp ("test-dbus.XXXXXX", NULL);
	g_assert(path);
	rmdir(path);

	address = g_strconcat("unix:path=", path, NULL);

	test->loop = g_main_loop_new(NULL, TRUE);
	context = g_main_loop_get_context(test->loop);

	test->server = dbus_server_listen(address, NULL);
	g_assert(test->server);
	DBG("listening on %s", address);

	dbus_server_setup_with_g_main(test->server, context);
	dbus_server_set_new_connection_function(test->server,
					test_dbus_connection_cb, test, NULL);

	test->client_connection = dbus_connection_open_private(address, NULL);
	g_assert(test->client_connection);
	dbus_connection_setup_with_g_main(test->client_connection, context);
	g_assert(dbus_connection_add_filter(test->client_connection,
				test_dbus_client_message_cb, test, NULL));
	DBG("connected on %s", address);
	g_free(address);
	g_free(path);
}

void test_dbus_shutdown(struct test_dbus_context *test)
{
	test_dbus_watch_disconnect_all();
	test_dbus_watch_remove_all();
	__ofono_dbus_cleanup();
	if (test->server_connection) {
		dbus_connection_close(test->server_connection);
		dbus_connection_unref(test->server_connection);
	}
	dbus_connection_close(test->client_connection);
	dbus_connection_unref(test->client_connection);
	dbus_server_disconnect(test->server);
	dbus_server_unref(test->server);
	g_main_loop_unref(test->loop);
	g_slist_free_full(test->client_signals, test_dbus_message_unref);
}

/*
 * Local Variables:
 * mode: C
 * c-basic-offset: 8
 * indent-tabs-mode: t
 * End:
 */
