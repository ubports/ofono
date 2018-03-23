/*
 *  oFono - Open Source Telephony
 *
 *  Copyright (C) 2018 Jolla Ltd. All rights reserved.
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

#include <ofono/dbus.h>

#include "ofono.h"
#include "dbus-queue.h"

#include <gutil_log.h>
#include <gutil_macros.h>

#define TEST_TIMEOUT                    (10)   /* seconds */
#define TEST_DBUS_INTERFACE            "test.interface"
#define TEST_DBUS_METHOD               "Test"
#define TEST_DBUS_PATH                 "/"

#define TEST_ERROR_CANCELED            "org.ofono.Error.Canceled"
#define TEST_ERROR_FAILED              "org.ofono.Error.Failed"

#define GDBUS_TEST_METHOD(fn) GDBUS_ASYNC_METHOD(TEST_DBUS_METHOD, \
				GDBUS_ARGS( { "arg", "i" }), NULL, fn)

static gboolean test_debug;

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

#define test_register_interface(methods,data) \
	g_assert(g_dbus_register_interface(ofono_dbus_get_connection(), \
				TEST_DBUS_PATH, TEST_DBUS_INTERFACE, \
				methods, NULL, NULL, data, NULL))

static void test_client_call(struct test_dbus_context* dbus, dbus_int32_t arg,
					DBusPendingCallNotifyFunction fn)
{
	DBusPendingCall *call;
	DBusConnection* conn = dbus->client_connection;
	DBusMessage *msg = dbus_message_new_method_call(NULL, TEST_DBUS_PATH,
					TEST_DBUS_INTERFACE, TEST_DBUS_METHOD);

	dbus_message_append_args(msg, DBUS_TYPE_INT32, &arg, DBUS_TYPE_INVALID);
	g_assert(dbus_connection_send_with_reply(conn, msg, &call,
						DBUS_TIMEOUT_INFINITE));
	dbus_pending_call_set_notify(call, fn, dbus, NULL);
	dbus_message_unref(msg);
}

static void test_expect_canceled(DBusPendingCall *call, void *unused)
{
	DBG("");
	test_dbus_check_error_reply(call, TEST_ERROR_CANCELED);
}

static void test_expect_failed(DBusPendingCall *call, void *unused)
{
	DBG("");
	test_dbus_check_error_reply(call, TEST_ERROR_FAILED);
}

/* ==== basic ==== */

static void test_basic(void)
{
	__ofono_dbus_queue_free(__ofono_dbus_queue_new());

	/* These are NULL tolerant: */
	__ofono_dbus_queue_free(NULL);
	__ofono_dbus_queue_reply_ok(NULL);
	__ofono_dbus_queue_reply_failed(NULL);
	__ofono_dbus_queue_reply_all_ok(NULL);
	__ofono_dbus_queue_reply_all_failed(NULL);
	__ofono_dbus_queue_reply_msg(NULL, NULL);
	g_assert(!__ofono_dbus_queue_pending(NULL));
	g_assert(!__ofono_dbus_queue_set_pending(NULL, NULL));
}

/* ==== free ==== */

struct test_free_data {
	struct test_dbus_context dbus;
	struct ofono_dbus_queue *queue;
};

static DBusMessage *test_free_cb(DBusMessage *msg, void *data)
{
	DBG("");
	return NULL;
}

static void test_free_reply(DBusPendingCall *call, void *dbus)
{
	struct test_free_data *test = G_CAST(dbus, struct test_free_data, dbus);

	DBG("");
	test_dbus_check_error_reply(call, TEST_ERROR_CANCELED);
	g_main_loop_quit(test->dbus.loop);
}

static DBusMessage *test_free_handler(DBusConnection *conn,
						DBusMessage *msg, void *data)
{
	struct test_free_data *test = data;

	DBG("");

	/* test_free_cb queues the message */
	__ofono_dbus_queue_request(test->queue, test_free_cb, msg, test);

	/* And this cancels it: */
	__ofono_dbus_queue_free(test->queue);
	test->queue = NULL;
	return NULL;
}

static const GDBusMethodTable test_free_methods[] = {
	{ GDBUS_TEST_METHOD(test_free_handler) },
	{ }
};

static void test_free_start(struct test_dbus_context *dbus)
{
	struct test_free_data *test = G_CAST(dbus, struct test_free_data, dbus);

	test_register_interface(test_free_methods, test);
	test_client_call(dbus, 0, test_free_reply);
}

static void test_free(void)
{
	struct test_free_data test;
	guint timeout = test_setup_timeout();

	memset(&test, 0, sizeof(test));
	test_dbus_setup(&test.dbus);
	test.dbus.start = test_free_start;
	test.queue = __ofono_dbus_queue_new();

	g_main_loop_run(test.dbus.loop);

	g_assert(!test.queue); /* Freed by test_free_handler */
	test_dbus_shutdown(&test.dbus);
	if (timeout) {
		g_source_remove(timeout);
	}
}

/* ==== cancel ==== */

struct test_cancel_data {
	struct test_dbus_context dbus;
	struct ofono_dbus_queue *queue;
};

static gboolean test_cancel_msg(void *data)
{
	struct test_cancel_data *test = data;

	/* This is will cancel the message: */
	__ofono_dbus_queue_reply_msg(test->queue, NULL);
	return G_SOURCE_REMOVE;
}

static DBusMessage *test_cancel_cb(DBusMessage *msg, void *data)
{
	DBG("");
	g_idle_add(test_cancel_msg, data);
	return NULL;
}

static void test_cancel_reply(DBusPendingCall *call, void *dbus)
{
	struct test_cancel_data *test =
		G_CAST(dbus, struct test_cancel_data, dbus);

	DBG("");
	test_dbus_check_error_reply(call, TEST_ERROR_CANCELED);
	g_main_loop_quit(test->dbus.loop);
}

static DBusMessage *test_cancel_handler(DBusConnection *conn,
						DBusMessage *msg, void *data)
{
	struct test_cancel_data *test = data;

	DBG("");

	__ofono_dbus_queue_request(test->queue, test_cancel_cb, msg, test);
	return NULL;
}

static const GDBusMethodTable test_cancel_methods[] = {
	{ GDBUS_TEST_METHOD(test_cancel_handler) },
	{ }
};

static void test_cancel_start(struct test_dbus_context *dbus)
{
	struct test_cancel_data *test =
		G_CAST(dbus, struct test_cancel_data, dbus);

	test_register_interface(test_cancel_methods, test);
	test_client_call(dbus, 0, test_cancel_reply);
}

static void test_cancel(void)
{
	struct test_cancel_data test;
	guint timeout = test_setup_timeout();

	memset(&test, 0, sizeof(test));
	test_dbus_setup(&test.dbus);
	test.dbus.start = test_cancel_start;
	test.queue = __ofono_dbus_queue_new();

	g_main_loop_run(test.dbus.loop);

	__ofono_dbus_queue_free(test.queue);
	test_dbus_shutdown(&test.dbus);
	if (timeout) {
		g_source_remove(timeout);
	}
}

/* ==== async ==== */

struct test_async_data {
	struct test_dbus_context dbus;
	struct ofono_dbus_queue *queue;
};

static gboolean test_async_complete(void *data)
{
	struct test_cancel_data *test = data;

	__ofono_dbus_queue_reply_fn(test->queue,
				dbus_message_new_method_return);
	return G_SOURCE_REMOVE;
}

static DBusMessage *test_async_cb(DBusMessage *msg, void *data)
{
	DBG("");
	g_idle_add(test_async_complete, data);
	return NULL;
}

static void test_async_last_reply(DBusPendingCall *call, void *dbus)
{
	struct test_async_data *test =
		G_CAST(dbus, struct test_async_data, dbus);

	DBG("");
	test_dbus_check_empty_reply(call, NULL);
	g_main_loop_quit(test->dbus.loop);
}

static DBusMessage *test_async_handler(DBusConnection *conn,
						DBusMessage *msg, void *data)
{
	struct test_cancel_data *test = data;

	DBG("");
	__ofono_dbus_queue_request(test->queue, test_async_cb, msg, data);
	return NULL;
}

static const GDBusMethodTable test_async_methods[] = {
	{ GDBUS_TEST_METHOD(test_async_handler) },
	{ }
};

static void test_async_start(struct test_dbus_context *dbus)
{
	struct test_async_data *test =
		G_CAST(dbus, struct test_async_data, dbus);

	test_register_interface(test_async_methods, test);
	test_client_call(dbus, 0, test_dbus_expect_empty_reply);
	test_client_call(dbus, 1, test_dbus_expect_empty_reply);
	test_client_call(dbus, 2, test_dbus_expect_empty_reply);
	test_client_call(dbus, 3, test_async_last_reply);
}

static void test_async(void)
{
	struct test_async_data test;
	guint timeout = test_setup_timeout();

	memset(&test, 0, sizeof(test));
	test_dbus_setup(&test.dbus);
	test.dbus.start = test_async_start;
	test.queue = __ofono_dbus_queue_new();

	g_main_loop_run(test.dbus.loop);

	__ofono_dbus_queue_free(test.queue);
	test_dbus_shutdown(&test.dbus);
	if (timeout) {
		g_source_remove(timeout);
	}
}

/* ==== sync ==== */

struct test_sync_data {
	struct test_dbus_context dbus;
	struct ofono_dbus_queue *queue;
};

static DBusMessage *test_sync_cb(DBusMessage *msg, void *data)
{
	DBG("");
	return dbus_message_new_method_return(msg);
}

static void test_sync_reply(DBusPendingCall *call, void *dbus)
{
	struct test_sync_data *test = G_CAST(dbus, struct test_sync_data, dbus);

	DBG("");
	test_dbus_check_empty_reply(call, NULL);
	g_main_loop_quit(test->dbus.loop);
}

static DBusMessage *test_sync_handler(DBusConnection *conn,
						DBusMessage *msg, void *data)
{
	struct test_sync_data *test = data;

	DBG("");

	/* test_sync_cb immediately completes it */
	__ofono_dbus_queue_request(test->queue, test_sync_cb, msg, test);
	return NULL;
}

static const GDBusMethodTable test_sync_methods[] = {
	{ GDBUS_TEST_METHOD(test_sync_handler) },
	{ }
};

static void test_sync_start(struct test_dbus_context *dbus)
{
	struct test_sync_data *test = G_CAST(dbus, struct test_sync_data, dbus);

	test_register_interface(test_sync_methods, test);
	test_client_call(dbus, 0, test_sync_reply);
}

static void test_sync(void)
{
	struct test_sync_data test;
	guint timeout = test_setup_timeout();

	memset(&test, 0, sizeof(test));
	test_dbus_setup(&test.dbus);
	test.dbus.start = test_sync_start;
	test.queue = __ofono_dbus_queue_new();

	g_main_loop_run(test.dbus.loop);

	__ofono_dbus_queue_free(test.queue);
	test_dbus_shutdown(&test.dbus);
	if (timeout) {
		g_source_remove(timeout);
	}
}

/* ==== reply ==== */

struct test_reply_data {
	struct test_dbus_context dbus;
	struct ofono_dbus_queue *queue;
};

static void test_reply_last_reply(DBusPendingCall *call, void *dbus)
{
	struct test_reply_data *test =
		G_CAST(dbus, struct test_reply_data, dbus);

	DBG("");
	test_dbus_check_error_reply(call, TEST_ERROR_FAILED);
	g_main_loop_quit(test->dbus.loop);
}

static DBusMessage *test_reply_1(DBusMessage *msg, void *data)
{
	DBG("");
	return NULL;
}

static DBusMessage *test_reply_2(DBusMessage *msg, void *data)
{
	DBG("");
	return NULL;
}

static DBusMessage *test_reply_handler(DBusConnection *conn,
						DBusMessage *msg, void *data)
{
	struct test_reply_data *test = data;
	dbus_int32_t arg;

	g_assert(dbus_message_get_args(msg, NULL, DBUS_TYPE_INT32, &arg,
							DBUS_TYPE_INVALID));

	DBG("%d", arg);
	switch (arg) {
	case 0:
		/* Queue is empty, we can use __ofono_dbus_queue_set_pending */
		g_assert(__ofono_dbus_queue_set_pending(test->queue, msg));
		break;
	case 1:
	case 4:
		/* Queue is not empty anymore */
		g_assert(__ofono_dbus_queue_pending(test->queue));
		g_assert(!__ofono_dbus_queue_set_pending(test->queue, msg));
		__ofono_dbus_queue_request(test->queue, test_reply_1,
								msg, NULL);
		break;
	case 2:
		/* Same callback, different data */
		__ofono_dbus_queue_request(test->queue, test_reply_1,
								msg, test);
		break;
	case 3:
		__ofono_dbus_queue_request(test->queue, test_reply_2,
								msg, NULL);
		break;
	case 5:
		__ofono_dbus_queue_request(test->queue, test_reply_2,
								msg, NULL);
		/* This completes the first one, with NULL handler */
		__ofono_dbus_queue_reply_all_fn_param(test->queue, NULL, NULL);
		g_assert(__ofono_dbus_queue_pending(test->queue));

		/* And this one completes 2 others with test_reply_1 */
		__ofono_dbus_queue_reply_all_fn(test->queue,
					dbus_message_new_method_return);
		g_assert(__ofono_dbus_queue_pending(test->queue));

		/* This one test_reply_1 with different data */
		__ofono_dbus_queue_reply_all_fn(test->queue,
						__ofono_error_canceled);

		/* And this one fails 2 others with test_reply_2 */
		__ofono_dbus_queue_reply_all_fn(test->queue, NULL);
		g_assert(!__ofono_dbus_queue_pending(test->queue));

		/* And this one does nothing */
		__ofono_dbus_queue_reply_all_fn(test->queue,
					dbus_message_new_method_return);
		break;
	}

	return NULL;
}

static const GDBusMethodTable test_reply_methods[] = {
	{ GDBUS_TEST_METHOD(test_reply_handler) },
	{ }
};

static void test_reply_start(struct test_dbus_context *dbus)
{
	struct test_reply_data *test =
		G_CAST(dbus, struct test_reply_data, dbus);

	test_register_interface(test_reply_methods, test);
	test_client_call(dbus, 0, test_expect_failed);
	test_client_call(dbus, 1, test_dbus_expect_empty_reply);
	test_client_call(dbus, 2, test_expect_canceled);
	test_client_call(dbus, 3, test_expect_failed);
	test_client_call(dbus, 4, test_dbus_expect_empty_reply);
	test_client_call(dbus, 5, test_reply_last_reply);
}

static void test_reply(void)
{
	struct test_reply_data test;
	guint timeout = test_setup_timeout();

	memset(&test, 0, sizeof(test));
	test_dbus_setup(&test.dbus);
	test.dbus.start = test_reply_start;
	test.queue = __ofono_dbus_queue_new();

	g_main_loop_run(test.dbus.loop);

	__ofono_dbus_queue_free(test.queue);
	test_dbus_shutdown(&test.dbus);
	if (timeout) {
		g_source_remove(timeout);
	}
}

/* ==== ok ==== */

struct test_ok_data {
	struct test_dbus_context dbus;
	struct ofono_dbus_queue *queue;
};

static DBusMessage *test_ok_1(DBusMessage *msg, void *data)
{
	DBG("");
	return NULL;
}

static DBusMessage *test_ok_2(DBusMessage *msg, void *data)
{
	DBG("");
	return dbus_message_new_method_return(msg);
}

static void test_ok_last_reply(DBusPendingCall *call, void *dbus)
{
	struct test_ok_data *test = G_CAST(dbus, struct test_ok_data, dbus);

	DBG("");
	test_dbus_check_empty_reply(call, NULL);
	g_main_loop_quit(test->dbus.loop);
}

static DBusMessage *test_ok_handler(DBusConnection *conn,
						DBusMessage *msg, void *data)
{
	struct test_ok_data *test = data;
	dbus_int32_t arg;

	g_assert(dbus_message_get_args(msg, NULL, DBUS_TYPE_INT32, &arg,
							DBUS_TYPE_INVALID));

	DBG("%d", arg);
	if (arg == 0) {
		/* This is the first call, it blocks the queue */
		__ofono_dbus_queue_request(test->queue, test_ok_1, msg, test);
	} else {
		g_assert(__ofono_dbus_queue_pending(test->queue));
		__ofono_dbus_queue_request(test->queue, test_ok_2, msg, test);
		/* This is the second call, complete the first one.
		 * That unblocks the seconds one. */
		__ofono_dbus_queue_reply_ok(test->queue);

		/* This call has no effect, it's actually an error (the
		 * message has already been replied to) but such situation
		 * is handled by __ofono_dbus_queue_reply_msg */
		__ofono_dbus_queue_reply_msg(test->queue,
					dbus_message_new_method_return(msg));

		/* This one does nothing too */
		__ofono_dbus_queue_reply_fn(test->queue, NULL);
	}

	return NULL;
}

static const GDBusMethodTable test_ok_methods[] = {
	{ GDBUS_TEST_METHOD(test_ok_handler) },
	{ }
};

static void test_ok_start(struct test_dbus_context *dbus)
{
	struct test_ok_data *test = G_CAST(dbus, struct test_ok_data, dbus);

	test_register_interface(test_ok_methods, test);
	test_client_call(dbus, 0, test_dbus_check_empty_reply);
	test_client_call(dbus, 1, test_ok_last_reply);
}

static void test_ok(void)
{
	struct test_ok_data test;
	guint timeout = test_setup_timeout();

	memset(&test, 0, sizeof(test));
	test_dbus_setup(&test.dbus);
	test.dbus.start = test_ok_start;
	test.queue = __ofono_dbus_queue_new();

	g_main_loop_run(test.dbus.loop);

	__ofono_dbus_queue_free(test.queue);
	test_dbus_shutdown(&test.dbus);
	if (timeout) {
		g_source_remove(timeout);
	}
}

#define TEST_(name) "/dbus-queue/" name

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
	__ofono_log_init("test-dbus-queue",
				g_test_verbose() ? "*" : NULL,
				FALSE, FALSE);

	g_test_add_func(TEST_("basic"), test_basic);
	g_test_add_func(TEST_("free"), test_free);
	g_test_add_func(TEST_("cancel"), test_cancel);
	g_test_add_func(TEST_("async"), test_async);
	g_test_add_func(TEST_("sync"), test_sync);
	g_test_add_func(TEST_("reply"), test_reply);
	g_test_add_func(TEST_("ok"), test_ok);

	return g_test_run();
}

/*
 * Local Variables:
 * mode: C
 * c-basic-offset: 8
 * indent-tabs-mode: t
 * End:
 */
