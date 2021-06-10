/*
 *  oFono - Open Source Telephony
 *
 *  Copyright (C) 2021 Jolla Ltd. All rights reserved.
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

#include "test-dbus.h"

#include <ofono/dbus-clients.h>
#include <ofono/dbus.h>
#include <ofono/log.h>
#include "ofono.h"

#include <gutil_log.h>
#include <gutil_macros.h>

#include <errno.h>

#define TEST_TIMEOUT                    (10)   /* seconds */
#define TEST_SENDER                     ":1.0"
#define TEST_SENDER_1                   ":1.1"

#define TEST_DBUS_PATH                  "/test"
#define TEST_DBUS_INTERFACE             "test.interface"
#define TEST_PROPERTY_CHANGED_SIGNAL    "PropertyChanged"
#define TEST_PROPERTY_NAME              "Test"
#define TEST_PROPERTY_VALUE             "test"

struct test_data {
	struct test_dbus_context dbus;
	struct ofono_dbus_clients *clients;
	int count;
};

static gboolean test_debug;

/* ==== dummy interface ==== */

#define test_register_interface(methods,signals,data) \
	g_assert(g_dbus_register_interface(ofono_dbus_get_connection(), \
				TEST_DBUS_PATH, TEST_DBUS_INTERFACE, \
				methods, signals, NULL, data, NULL))

#define test_register_dummy_interface() \
	test_register_interface(test_dummy_methods, \
				test_property_change_signal, NULL)

static DBusMessage *test_dummy_handler(DBusConnection *conn,
					DBusMessage *msg, void *data)
{
	g_assert_not_reached();
	return NULL;
}

static const GDBusMethodTable test_dummy_methods[] = {
	{ GDBUS_ASYNC_METHOD("Dummy", NULL, NULL, test_dummy_handler) },
	{ }
};

static const GDBusSignalTable test_property_change_signal[] = {
	{ GDBUS_SIGNAL("PropertyChanged",
			GDBUS_ARGS({ "name", "s" }, { "value", "v" })) },
	{ }
};

/* ==== common ==== */

static gboolean test_timeout(gpointer param)
{
	g_assert(!"TIMEOUT");
	return G_SOURCE_REMOVE;
}

static guint test_setup_timeout(void)
{
	if (test_debug) {
		return 0;
	} else {
		return g_timeout_add_seconds(TEST_TIMEOUT, test_timeout, NULL);
	}
}

static gboolean test_loop_quit(gpointer data)
{
	g_main_loop_quit(data);
	return G_SOURCE_REMOVE;
}

static void test_loop_quit_later(GMainLoop *loop)
{
	g_idle_add(test_loop_quit, loop);
}

/* ==== null ==== */

static void test_null(void)
{
	/* We are NULL tolerant: */
	ofono_dbus_clients_free(NULL);
	ofono_dbus_clients_signal(NULL, NULL);
	ofono_dbus_clients_signal_property_changed(NULL,NULL,NULL,NULL,0,NULL);
	g_assert(!ofono_dbus_clients_new(NULL, NULL, NULL));
	g_assert(!ofono_dbus_clients_count(NULL));
	g_assert(!ofono_dbus_clients_add(NULL, NULL));
	g_assert(!ofono_dbus_clients_remove(NULL, NULL));
}

/* ==== basic ==== */

static void test_basic_notify_func(const char *name, void *loop)
{
	g_assert_cmpstr(name, == ,TEST_SENDER);
	g_main_loop_quit(loop);
}

static void test_basic_start(struct test_dbus_context *dbus)
{
	struct test_data *test = G_CAST(dbus, struct test_data, dbus);
	const char *value = TEST_PROPERTY_VALUE;
	DBusMessage *signal =
		ofono_dbus_signal_new_property_changed(TEST_DBUS_PATH,
			TEST_DBUS_INTERFACE, TEST_PROPERTY_NAME,
			DBUS_TYPE_STRING, &value);

	test->clients = ofono_dbus_clients_new(ofono_dbus_get_connection(),
				test_basic_notify_func, test->dbus.loop);

	g_assert(!ofono_dbus_clients_add(test->clients, NULL));
	g_assert(ofono_dbus_clients_add(test->clients, TEST_SENDER));
	g_assert(ofono_dbus_clients_remove(test->clients, TEST_SENDER));
	g_assert(!ofono_dbus_clients_remove(test->clients, TEST_SENDER));

	/* OK to add the same thing twice */
	g_assert(ofono_dbus_clients_add(test->clients, TEST_SENDER));
	g_assert(ofono_dbus_clients_add(test->clients, TEST_SENDER));
	g_assert_cmpuint(ofono_dbus_clients_count(test->clients), == ,1);
	test_dbus_watch_disconnect_all();
	g_assert_cmpuint(ofono_dbus_clients_count(test->clients), == ,0);

	/* There's nothing to remove */
	g_assert(!ofono_dbus_clients_remove(test->clients, TEST_SENDER));
	g_assert(!ofono_dbus_clients_remove(test->clients, NULL));

	/* These have no effect because client list is empty: */
	ofono_dbus_clients_signal(test->clients, NULL);
	ofono_dbus_clients_signal(test->clients, signal);
	ofono_dbus_clients_signal_property_changed(test->clients, NULL, NULL,
							NULL, 0, NULL);
	ofono_dbus_clients_signal_property_changed(test->clients,
				TEST_DBUS_PATH, TEST_DBUS_INTERFACE,
				TEST_PROPERTY_NAME, DBUS_TYPE_STRING, &value);

	/* test_basic_notify_func() has called test_loop_quit_later() */
	dbus_message_unref(signal);
}

static void test_basic(void)
{
	struct test_data test;
	guint timeout = test_setup_timeout();

	memset(&test, 0, sizeof(test));
	test_dbus_setup(&test.dbus);
	test.dbus.start = test_basic_start;

	g_main_loop_run(test.dbus.loop);

	g_assert(test.clients);
	ofono_dbus_clients_free(test.clients);
	test_dbus_shutdown(&test.dbus);
	if (timeout) {
		g_source_remove(timeout);
	}
}

/* ==== signal ==== */

static void test_signal_handle(struct test_dbus_context *dbus, DBusMessage *msg)
{
	struct test_data *test = G_CAST(dbus, struct test_data, dbus);

	g_assert_cmpstr(dbus_message_get_path(msg), == ,TEST_DBUS_PATH);
	g_assert_cmpstr(dbus_message_get_interface(msg), == ,
						TEST_DBUS_INTERFACE);
	g_assert_cmpstr(dbus_message_get_member(msg), == ,
						TEST_PROPERTY_CHANGED_SIGNAL);
	test->count++;
	if (test->count == 2) {
		test_loop_quit_later(dbus->loop);
	}
}

static void test_signal_start(struct test_dbus_context *dbus)
{
	struct test_data *test = G_CAST(dbus, struct test_data, dbus);
	const char *value = TEST_PROPERTY_VALUE;

	test_register_dummy_interface();
	test->clients = ofono_dbus_clients_new(ofono_dbus_get_connection(),
								NULL, NULL);

	g_assert(ofono_dbus_clients_add(test->clients, TEST_SENDER));
	g_assert(ofono_dbus_clients_add(test->clients, TEST_SENDER_1));
	g_assert_cmpuint(ofono_dbus_clients_count(test->clients), == ,2);

	ofono_dbus_clients_signal_property_changed(test->clients,
				TEST_DBUS_PATH, TEST_DBUS_INTERFACE,
				TEST_PROPERTY_NAME, DBUS_TYPE_STRING, &value);
	/* And wait for 2 signals to arrive */
}

static void test_signal(void)
{
	struct test_data test;
	guint timeout = test_setup_timeout();

	memset(&test, 0, sizeof(test));
	test_dbus_setup(&test.dbus);
	test.dbus.start = test_signal_start;
	test.dbus.handle_signal = test_signal_handle;

	g_main_loop_run(test.dbus.loop);

	g_assert_cmpuint(ofono_dbus_clients_count(test.clients), == ,2);
	test_dbus_watch_disconnect_all();
	g_assert_cmpuint(ofono_dbus_clients_count(test.clients), == ,0);
	ofono_dbus_clients_free(test.clients);

	test_dbus_shutdown(&test.dbus);
	if (timeout) {
		g_source_remove(timeout);
	}
}

#define TEST_(name) "/dbus-clients/" name

int main(int argc, char *argv[])
{
	int i;

	g_test_init(&argc, &argv, NULL);
	for (i = 1; i < argc; i++) {
		const char *arg = argv[i];
		if (!strcmp(arg, "-d") || !strcmp(arg, "--debug")) {
			test_debug = TRUE;
		} else {
			GWARN("Unsupported command line option %s", arg);
		}
	}

	gutil_log_timestamp = FALSE;
	gutil_log_default.level = g_test_verbose() ?
		GLOG_LEVEL_VERBOSE : GLOG_LEVEL_NONE;
	__ofono_log_init("test-dbus-clients",
				g_test_verbose() ? "*" : NULL,
				FALSE, FALSE);

	g_test_add_func(TEST_("null"), test_null);
	g_test_add_func(TEST_("basic"), test_basic);
	g_test_add_func(TEST_("signal"), test_signal);

	return g_test_run();
}

/*
 * Local Variables:
 * mode: C
 * c-basic-offset: 8
 * indent-tabs-mode: t
 * End:
 */
