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

#include "sailfish_cell_info.h"
#include "sailfish_cell_info_dbus.h"
#include "fake_sailfish_cell_info.h"

#include <gutil_log.h>
#include <gutil_macros.h>

#include "ofono.h"

#define TEST_TIMEOUT                        (10)   /* seconds */
#define TEST_MODEM_PATH                     "/test"
#define TEST_SENDER                         ":1.0"

#define CELL_INFO_DBUS_INTERFACE            "org.nemomobile.ofono.CellInfo"
#define CELL_INFO_DBUS_CELLS_ADDED_SIGNAL   "CellsAdded"
#define CELL_INFO_DBUS_CELLS_REMOVED_SIGNAL "CellsRemoved"

#define CELL_DBUS_INTERFACE_VERSION         (1)
#define CELL_DBUS_INTERFACE                 "org.nemomobile.ofono.Cell"
#define CELL_DBUS_REGISTERED_CHANGED_SIGNAL "RegisteredChanged"
#define CELL_DBUS_PROPERTY_CHANGED_SIGNAL   "PropertyChanged"
#define CELL_DBUS_REMOVED_SIGNAL            "Removed"

static gboolean test_debug;

/* Stubs (ofono) */

struct ofono_modem {
	const char *path;
};

const char *ofono_modem_get_path(struct ofono_modem *modem)
{
	return modem->path;
}

void ofono_modem_add_interface(struct ofono_modem *modem, const char *iface)
{
	DBG("%s %s", modem->path, iface);
}

/* ==== common ==== */

static gboolean test_timeout(gpointer param)
{
	g_assert(!"TIMEOUT");
	return G_SOURCE_REMOVE;
}

static guint test_setup_timeout(void)
{
	if (!test_debug) {
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

static DBusMessage *test_new_cell_info_call(const char *method)
{
	DBusMessage *msg = dbus_message_new_method_call(NULL, TEST_MODEM_PATH,
					CELL_INFO_DBUS_INTERFACE, method);

	g_assert(dbus_message_set_sender(msg, TEST_SENDER));
	return msg;
}

static DBusMessage *test_new_cell_call(const char *path, const char *method)
{
	DBusMessage *msg = dbus_message_new_method_call(NULL, path,
					CELL_DBUS_INTERFACE, method);

	g_assert(dbus_message_set_sender(msg, TEST_SENDER));
	return msg;
}

static void test_submit_get_all_call(DBusConnection* connection,
		const char *cell_path, DBusPendingCallNotifyFunction notify,
		void *data)
{
	DBusMessage *msg;
	DBusPendingCall* call;

	msg = test_new_cell_call(cell_path, "GetAll");
	g_assert(dbus_connection_send_with_reply(connection, msg, &call,
						DBUS_TIMEOUT_INFINITE));
	dbus_pending_call_set_notify(call, notify, data, NULL);
	dbus_message_unref(msg);
}

static void test_check_object_path_array_va(DBusMessageIter *it,
						const char *path1, va_list va)
{
	DBusMessageIter array;

	g_assert(dbus_message_iter_get_arg_type(it) == DBUS_TYPE_ARRAY);
	dbus_message_iter_recurse(it, &array);
	dbus_message_iter_next(it);

	if (path1) {
		const char *path;

		g_assert(!g_strcmp0(test_dbus_get_object_path(&array), path1));
		while ((path = va_arg(va, char*)) != NULL) {
			g_assert(!g_strcmp0(test_dbus_get_object_path(&array),
									path));
		}
	}

	g_assert(dbus_message_iter_get_arg_type(&array) == DBUS_TYPE_INVALID);
	g_assert(dbus_message_iter_get_arg_type(it) == DBUS_TYPE_INVALID);
}

static void test_check_object_path_array(DBusMessageIter *it,
						const char *path1, ...)
{
	va_list va;

	va_start(va, path1);
	test_check_object_path_array_va(it, path1, va);
	va_end(va);
}

static void test_check_get_cells_reply(DBusPendingCall *call,
						const char *path1, ...)
{
	DBusMessage *reply = dbus_pending_call_steal_reply(call);
	DBusMessageIter it;
	va_list va;

	g_assert(dbus_message_get_type(reply) ==
					DBUS_MESSAGE_TYPE_METHOD_RETURN);
	dbus_message_iter_init(reply, &it);
	va_start(va, path1);
	test_check_object_path_array_va(&it, path1, va);
	va_end(va);

	dbus_message_unref(reply);
}

static void test_check_get_all_reply(DBusPendingCall *call,
			const struct sailfish_cell *cell, const char *type)
{
	DBusMessage *reply = dbus_pending_call_steal_reply(call);
	DBusMessageIter it, array;

	g_assert(dbus_message_get_type(reply) ==
					DBUS_MESSAGE_TYPE_METHOD_RETURN);
	dbus_message_iter_init(reply, &it);
	g_assert(test_dbus_get_int32(&it) == CELL_DBUS_INTERFACE_VERSION);
	g_assert(!g_strcmp0(test_dbus_get_string(&it), type));
	g_assert(test_dbus_get_bool(&it) == (cell->registered != FALSE));
	g_assert(dbus_message_iter_get_arg_type(&it) == DBUS_TYPE_ARRAY);
	dbus_message_iter_recurse(&it, &array);
	dbus_message_iter_next(&it);
	/* Validate the properties? */
	g_assert(dbus_message_iter_get_arg_type(&it) == DBUS_TYPE_INVALID);
	dbus_message_unref(reply);
}

static struct sailfish_cell *test_cell_init_gsm1(struct sailfish_cell *cell)
{
	struct sailfish_cell_info_gsm *gsm = &cell->info.gsm;

	memset(cell, 0, sizeof(*cell));
	cell->type = SAILFISH_CELL_TYPE_GSM;
	cell->registered = TRUE;
	gsm->mcc = 244;
	gsm->mnc = 5;
	gsm->lac = 9007;
	gsm->cid = 42335;
	gsm->arfcn = INT_MAX;
	gsm->bsic = INT_MAX;
	gsm->signalStrength = 26;
	gsm->bitErrorRate = 99;
	gsm->timingAdvance = INT_MAX;
	return cell;
}

static struct sailfish_cell *test_cell_init_gsm2(struct sailfish_cell *cell)
{
	struct sailfish_cell_info_gsm *gsm = &cell->info.gsm;

	memset(cell, 0, sizeof(*cell));
	cell->type = SAILFISH_CELL_TYPE_GSM;
	cell->registered = FALSE;
	gsm->mcc = 244;
	gsm->mnc = 5;
	gsm->lac = 9007;
	gsm->cid = 35600;
	gsm->arfcn = INT_MAX;
	gsm->bsic = INT_MAX;
	gsm->signalStrength = 8;
	gsm->bitErrorRate = 99;
	gsm->timingAdvance = INT_MAX;
	return cell;
}

static struct sailfish_cell *test_cell_init_wcdma1(struct sailfish_cell *cell)
{
	struct sailfish_cell_info_wcdma *wcdma = &cell->info.wcdma;

	memset(cell, 0, sizeof(*cell));
	cell->type = SAILFISH_CELL_TYPE_WCDMA;
	cell->registered = TRUE;
	wcdma->mcc = 250;
	wcdma->mnc = 99;
	wcdma->lac = 14760;
	wcdma->cid = 149331616;
	wcdma->psc = 371;
	wcdma->uarfcn = INT_MAX;
	wcdma->signalStrength = 4;
	wcdma->bitErrorRate = 99;
	return cell;
}

static struct sailfish_cell *test_cell_init_wcdma2(struct sailfish_cell *cell)
{
	struct sailfish_cell_info_wcdma *wcdma = &cell->info.wcdma;

	memset(cell, 0, sizeof(*cell));
	cell->type = SAILFISH_CELL_TYPE_WCDMA;
	cell->registered = FALSE;
	wcdma->mcc = INT_MAX;
	wcdma->mnc = INT_MAX;
	wcdma->lac = INT_MAX;
	wcdma->cid = INT_MAX;
	wcdma->psc = INT_MAX;
	wcdma->uarfcn = INT_MAX;
	wcdma->signalStrength = 5;
	wcdma->bitErrorRate = 99;
	return cell;
}

static struct sailfish_cell *test_cell_init_lte(struct sailfish_cell *cell)
{
	struct sailfish_cell_info_lte *lte = &cell->info.lte;

	memset(cell, 0, sizeof(*cell));
	cell->type = SAILFISH_CELL_TYPE_LTE;
	cell->registered = TRUE;
	lte->mcc = 244;
	lte->mnc = 91;
	lte->ci = 36591883;
	lte->pci = 309;
	lte->tac = 4030;
	lte->earfcn = INT_MAX;
	lte->signalStrength = 17;
	lte->rsrp = 106;
	lte->rsrq = 6;
	lte->rssnr = INT_MAX;
	lte->cqi = INT_MAX;
	lte->timingAdvance = INT_MAX;
	return cell;
}

/* ==== Misc ==== */

static void test_misc(void)
{
	struct ofono_modem modem;

	modem.path = TEST_MODEM_PATH;

	/* NULL resistance */
	g_assert(!sailfish_cell_info_dbus_new(NULL, NULL));
	g_assert(!sailfish_cell_info_dbus_new(&modem, NULL));
	sailfish_cell_info_dbus_free(NULL);

	/* Calling __ofono_dbus_cleanup() without __ofono_dbus_init() is ok */
	__ofono_dbus_cleanup();
}

/* ==== GetCells ==== */

struct test_get_cells_data {
	struct ofono_modem modem;
	struct test_dbus_context context;
	struct sailfish_cell_info *info;
	struct sailfish_cell_info_dbus *dbus;
};

static void test_get_cells_call(struct test_get_cells_data *test,
				DBusPendingCallNotifyFunction notify)
{
	DBusPendingCall *call;
	DBusConnection *connection = test->context.client_connection;
	DBusMessage *msg = test_new_cell_info_call("GetCells");

	g_assert(dbus_connection_send_with_reply(connection, msg, &call,
						DBUS_TIMEOUT_INFINITE));
	dbus_pending_call_set_notify(call, notify, test, NULL);
	dbus_message_unref(msg);
}

static void test_get_cells_start_reply3(DBusPendingCall *call, void *data)
{
 	struct test_get_cells_data *test = data;
	DBusMessageIter it;
	DBusMessage *signal = test_dbus_take_signal(&test->context,
				test->modem.path, CELL_INFO_DBUS_INTERFACE,
				CELL_INFO_DBUS_CELLS_REMOVED_SIGNAL);

	DBG("");
	test_check_get_cells_reply(call, "/test/cell_1", NULL);
	dbus_pending_call_unref(call);

	/* Validate the signal */
	g_assert(signal);
	dbus_message_iter_init(signal, &it);
	test_check_object_path_array(&it, "/test/cell_0", NULL);
	dbus_message_unref(signal);

	test_loop_quit_later(test->context.loop);
}

static void test_get_cells_start_reply2(DBusPendingCall *call, void *data)
{
 	struct test_get_cells_data *test = data;
	const char *cell_added = "/test/cell_1";
	struct sailfish_cell cell;
	DBusMessageIter it;
	DBusMessage *signal = test_dbus_take_signal(&test->context,
				test->modem.path, CELL_INFO_DBUS_INTERFACE,
				CELL_INFO_DBUS_CELLS_ADDED_SIGNAL);

	DBG("");
	test_check_get_cells_reply(call, "/test/cell_0", cell_added, NULL);
	dbus_pending_call_unref(call);

	/* Validate the signal */
	g_assert(signal);
	dbus_message_iter_init(signal, &it);
	test_check_object_path_array(&it, cell_added, NULL);
	dbus_message_unref(signal);

	/* Remove "/test/cell_0" */
	g_assert(fake_cell_info_remove_cell(test->info,
						test_cell_init_gsm1(&cell)));
	fake_cell_info_cells_changed(test->info);
	test_get_cells_call(test, test_get_cells_start_reply3);
}

static void test_get_cells_start_reply1(DBusPendingCall *call, void *data)
{
 	struct test_get_cells_data *test = data;
	struct sailfish_cell cell;

	DBG("");
	test_check_get_cells_reply(call, "/test/cell_0", NULL);
	dbus_pending_call_unref(call);

	/* Add "/test/cell_1" */
	fake_cell_info_add_cell(test->info, test_cell_init_gsm2(&cell));
	fake_cell_info_cells_changed(test->info);
	test_get_cells_call(test, test_get_cells_start_reply2);
}

static void test_get_cells_start(struct test_dbus_context *context)
{
	struct sailfish_cell cell;
	struct test_get_cells_data *test =
		G_CAST(context, struct test_get_cells_data, context);

	DBG("");
	test->info = fake_cell_info_new();
	fake_cell_info_add_cell(test->info, test_cell_init_gsm1(&cell));

	test->dbus = sailfish_cell_info_dbus_new(&test->modem, test->info);
	g_assert(test->dbus);

	test_get_cells_call(test, test_get_cells_start_reply1);
}

static void test_get_cells(void)
{
	struct test_get_cells_data test;
	guint timeout = test_setup_timeout();

	memset(&test, 0, sizeof(test));
	test.modem.path = TEST_MODEM_PATH;
	test.context.start = test_get_cells_start;
	test_dbus_setup(&test.context);

	g_main_loop_run(test.context.loop);

	sailfish_cell_info_unref(test.info);
	sailfish_cell_info_dbus_free(test.dbus);
	test_dbus_shutdown(&test.context);
	if (timeout) {
		g_source_remove(timeout);
	}
}

/* ==== GetAll ==== */

struct test_get_all_data {
	struct ofono_modem modem;
	struct test_dbus_context context;
	struct sailfish_cell_info_dbus *dbus;
	struct sailfish_cell cell;
	const char *type;
};

static void test_get_all_reply(DBusPendingCall *call, void *data)
{
 	struct test_get_all_data *test = data;

	DBG("");
	test_check_get_all_reply(call, &test->cell, test->type);
	dbus_pending_call_unref(call);

	test_loop_quit_later(test->context.loop);
}

static void test_get_all_start(struct test_dbus_context *context)
{
	struct sailfish_cell_info *info;
	struct test_get_all_data *test =
		G_CAST(context, struct test_get_all_data, context);

	DBG("");
	info = fake_cell_info_new();
	fake_cell_info_add_cell(info, &test->cell);
	test->dbus = sailfish_cell_info_dbus_new(&test->modem, info);
	g_assert(test->dbus);
	sailfish_cell_info_unref(info);

	test_submit_get_all_call(context->client_connection, "/test/cell_0",
						test_get_all_reply, test);
}

static void test_get_all(const struct sailfish_cell *cell, const char *type)
{
	struct test_get_all_data test;
	guint timeout = test_setup_timeout();

	memset(&test, 0, sizeof(test));
	test.modem.path = TEST_MODEM_PATH;
	test.context.start = test_get_all_start;
	test.cell = *cell;
	test.type = type;
	test_dbus_setup(&test.context);

	g_main_loop_run(test.context.loop);

	sailfish_cell_info_dbus_free(test.dbus);
	test_dbus_shutdown(&test.context);
	if (timeout) {
		g_source_remove(timeout);
	}
}

static void test_get_all1(void)
{
	struct sailfish_cell cell;

	test_get_all(test_cell_init_gsm1(&cell), "gsm");
}

static void test_get_all2(void)
{
	struct sailfish_cell cell;

	test_get_all(test_cell_init_wcdma2(&cell), "wcdma");
}

static void test_get_all3(void)
{
	struct sailfish_cell cell;

	test_get_all(test_cell_init_lte(&cell), "lte");
}

static void test_get_all4(void)
{
	struct sailfish_cell cell;

	/* Invalid cell */
	memset(&cell, 0xff, sizeof(cell));
	test_get_all(&cell, "unknown");
}

/* ==== GetInterfaceVersion ==== */

struct test_get_version_data {
	struct ofono_modem modem;
	struct test_dbus_context context;
	struct sailfish_cell_info_dbus *dbus;
};

static void test_get_version_reply(DBusPendingCall *call, void *data)
{
 	struct test_get_version_data *test = data;
	DBusMessage *reply = dbus_pending_call_steal_reply(call);
	dbus_int32_t version;

	DBG("");
	g_assert(dbus_message_get_type(reply) ==
					DBUS_MESSAGE_TYPE_METHOD_RETURN);
	g_assert(dbus_message_get_args(reply, NULL,
					DBUS_TYPE_INT32, &version,
					DBUS_TYPE_INVALID));
	g_assert(version == CELL_DBUS_INTERFACE_VERSION);
	dbus_message_unref(reply);
	dbus_pending_call_unref(call);

	test_loop_quit_later(test->context.loop);
}

static void test_get_version_start(struct test_dbus_context *context)
{
	DBusPendingCall *call;
	DBusMessage *msg;
	struct sailfish_cell cell;
	struct sailfish_cell_info *info;
	struct test_get_version_data *test =
		G_CAST(context, struct test_get_version_data, context);

	DBG("");
	info = fake_cell_info_new();
	fake_cell_info_add_cell(info, test_cell_init_gsm1(&cell));
	test->dbus = sailfish_cell_info_dbus_new(&test->modem, info);
	g_assert(test->dbus);
	sailfish_cell_info_unref(info);

	msg = test_new_cell_call("/test/cell_0", "GetInterfaceVersion");
	g_assert(dbus_connection_send_with_reply(context->client_connection,
					msg, &call, DBUS_TIMEOUT_INFINITE));
	dbus_pending_call_set_notify(call, test_get_version_reply, test, NULL);
	dbus_message_unref(msg);
}

static void test_get_version(void)
{
	struct test_get_version_data test;
	guint timeout = test_setup_timeout();

	memset(&test, 0, sizeof(test));
	test.modem.path = TEST_MODEM_PATH;
	test.context.start = test_get_version_start;
	test_dbus_setup(&test.context);

	g_main_loop_run(test.context.loop);

	sailfish_cell_info_dbus_free(test.dbus);
	test_dbus_shutdown(&test.context);
	if (timeout) {
		g_source_remove(timeout);
	}
}

/* ==== GetType ==== */

struct test_get_type_data {
	struct ofono_modem modem;
	struct test_dbus_context context;
	struct sailfish_cell_info_dbus *dbus;
};

static void test_get_type_reply(DBusPendingCall *call, void *data)
{
 	struct test_get_type_data *test = data;
	DBusMessage *reply = dbus_pending_call_steal_reply(call);
	DBusMessageIter it;

	DBG("");
	g_assert(dbus_message_get_type(reply) ==
					DBUS_MESSAGE_TYPE_METHOD_RETURN);
	dbus_message_iter_init(reply, &it);
	g_assert(!g_strcmp0(test_dbus_get_string(&it), "wcdma"));
	g_assert(dbus_message_iter_get_arg_type(&it) == DBUS_TYPE_INVALID);
	dbus_message_unref(reply);
	dbus_pending_call_unref(call);

	test_loop_quit_later(test->context.loop);
}

static void test_get_type_start(struct test_dbus_context *context)
{
	DBusPendingCall *call;
	DBusMessage *msg;
	struct sailfish_cell cell;
	struct sailfish_cell_info *info;
	struct test_get_type_data *test =
		G_CAST(context, struct test_get_type_data, context);

	DBG("");
	info = fake_cell_info_new();
	fake_cell_info_add_cell(info, test_cell_init_wcdma1(&cell));
	test->dbus = sailfish_cell_info_dbus_new(&test->modem, info);
	g_assert(test->dbus);
	sailfish_cell_info_unref(info);

	msg = test_new_cell_call("/test/cell_0", "GetType");
	g_assert(dbus_connection_send_with_reply(context->client_connection,
					msg, &call, DBUS_TIMEOUT_INFINITE));
	dbus_pending_call_set_notify(call, test_get_type_reply, test, NULL);
	dbus_message_unref(msg);
}

static void test_get_type(void)
{
	struct test_get_type_data test;
	guint timeout = test_setup_timeout();

	memset(&test, 0, sizeof(test));
	test.modem.path = TEST_MODEM_PATH;
	test.context.start = test_get_type_start;
	test_dbus_setup(&test.context);

	g_main_loop_run(test.context.loop);

	sailfish_cell_info_dbus_free(test.dbus);
	test_dbus_shutdown(&test.context);
	if (timeout) {
		g_source_remove(timeout);
	}
}

/* ==== GetRegistered ==== */

struct test_get_registered_data {
	struct ofono_modem modem;
	struct test_dbus_context context;
	struct sailfish_cell_info_dbus *dbus;
};

static void test_get_registered_reply(DBusPendingCall *call, void *data)
{
 	struct test_get_registered_data *test = data;
	DBusMessage *reply = dbus_pending_call_steal_reply(call);
	DBusMessageIter it;

	DBG("");
	g_assert(dbus_message_get_type(reply) ==
					DBUS_MESSAGE_TYPE_METHOD_RETURN);
	dbus_message_iter_init(reply, &it);
	g_assert(test_dbus_get_bool(&it) == TRUE);
	g_assert(dbus_message_iter_get_arg_type(&it) == DBUS_TYPE_INVALID);
	dbus_message_unref(reply);
	dbus_pending_call_unref(call);

	test_loop_quit_later(test->context.loop);
}

static void test_get_registered_start(struct test_dbus_context *context)
{
	DBusPendingCall *call;
	DBusMessage *msg;
	struct sailfish_cell cell;
	struct sailfish_cell_info *info;
	struct test_get_registered_data *test =
		G_CAST(context, struct test_get_registered_data, context);

	DBG("");
	info = fake_cell_info_new();
	fake_cell_info_add_cell(info, test_cell_init_wcdma1(&cell));
	test->dbus = sailfish_cell_info_dbus_new(&test->modem, info);
	g_assert(test->dbus);
	sailfish_cell_info_unref(info);

	msg = test_new_cell_call("/test/cell_0", "GetRegistered");
	g_assert(dbus_connection_send_with_reply(context->client_connection,
					msg, &call, DBUS_TIMEOUT_INFINITE));
	dbus_pending_call_set_notify(call, test_get_registered_reply, test,
									NULL);
	dbus_message_unref(msg);
}

static void test_get_registered(void)
{
	struct test_get_registered_data test;
	guint timeout = test_setup_timeout();

	memset(&test, 0, sizeof(test));
	test.modem.path = TEST_MODEM_PATH;
	test.context.start = test_get_registered_start;
	test_dbus_setup(&test.context);

	g_main_loop_run(test.context.loop);

	sailfish_cell_info_dbus_free(test.dbus);
	test_dbus_shutdown(&test.context);
	if (timeout) {
		g_source_remove(timeout);
	}
}

/* ==== GetProperties ==== */

struct test_get_properties_data {
	struct ofono_modem modem;
	struct test_dbus_context context;
	struct sailfish_cell_info_dbus *dbus;
};

static void test_get_properties_reply(DBusPendingCall *call, void *data)
{
 	struct test_get_properties_data *test = data;
	DBusMessage *reply = dbus_pending_call_steal_reply(call);
	DBusMessageIter it, array;

	DBG("");
	g_assert(dbus_message_get_type(reply) ==
					DBUS_MESSAGE_TYPE_METHOD_RETURN);
	dbus_message_iter_init(reply, &it);
	g_assert(dbus_message_iter_get_arg_type(&it) == DBUS_TYPE_ARRAY);
	dbus_message_iter_recurse(&it, &array);
	dbus_message_iter_next(&it);
	/* Validate the properties? */
	g_assert(dbus_message_iter_get_arg_type(&it) == DBUS_TYPE_INVALID);
	dbus_message_unref(reply);
	dbus_pending_call_unref(call);

	test_loop_quit_later(test->context.loop);
}

static void test_get_properties_start(struct test_dbus_context *context)
{
	DBusPendingCall *call;
	DBusMessage *msg;
	struct sailfish_cell cell;
	struct sailfish_cell_info *info;
	struct test_get_properties_data *test =
		G_CAST(context, struct test_get_properties_data, context);

	DBG("");
	info = fake_cell_info_new();
	fake_cell_info_add_cell(info, test_cell_init_wcdma2(&cell));
	test->dbus = sailfish_cell_info_dbus_new(&test->modem, info);
	g_assert(test->dbus);
	sailfish_cell_info_unref(info);

	msg = test_new_cell_call("/test/cell_0", "GetProperties");
	g_assert(dbus_connection_send_with_reply(context->client_connection,
					msg, &call, DBUS_TIMEOUT_INFINITE));
	dbus_pending_call_set_notify(call, test_get_properties_reply, test,
									NULL);
	dbus_message_unref(msg);
}

static void test_get_properties(void)
{
	struct test_get_properties_data test;
	guint timeout = test_setup_timeout();

	memset(&test, 0, sizeof(test));
	test.modem.path = TEST_MODEM_PATH;
	test.context.start = test_get_properties_start;
	test_dbus_setup(&test.context);

	g_main_loop_run(test.context.loop);

	sailfish_cell_info_dbus_free(test.dbus);
	test_dbus_shutdown(&test.context);
	if (timeout) {
		g_source_remove(timeout);
	}
}

/* ==== RegisteredChanged ==== */

struct test_registered_changed_data {
	struct ofono_modem modem;
	struct test_dbus_context context;
	struct sailfish_cell_info *info;
	struct sailfish_cell_info_dbus *dbus;
	struct sailfish_cell cell;
	const char *type;
	const char *cell_path;
};

static void test_registered_changed_reply(DBusPendingCall *call, void *data)
{
 	struct test_registered_changed_data *test = data;

	DBG("");
	test_check_get_all_reply(call, &test->cell, test->type);
	dbus_pending_call_unref(call);

	test_loop_quit_later(test->context.loop);
}

static void test_registered_changed_start(struct test_dbus_context *context)
{
	struct test_registered_changed_data *test =
		G_CAST(context, struct test_registered_changed_data, context);
	struct sailfish_cell *first_cell;

	DBG("");
	test->info = fake_cell_info_new();
	fake_cell_info_add_cell(test->info, &test->cell);
	test->dbus = sailfish_cell_info_dbus_new(&test->modem, test->info);
	g_assert(test->dbus);

	/* Trigger "RegisteredChanged" signal */
	first_cell = test->info->cells->data;
	test->cell.registered =
	first_cell->registered = !first_cell->registered;
	fake_cell_info_cells_changed(test->info);

	test_submit_get_all_call(context->client_connection, test->cell_path,
				test_registered_changed_reply, test);
}

static void test_registered_changed(void)
{
	struct test_registered_changed_data test;
	guint timeout = test_setup_timeout();

	memset(&test, 0, sizeof(test));
	test.modem.path = TEST_MODEM_PATH;
	test.context.start = test_registered_changed_start;
	test_cell_init_gsm1(&test.cell);
	test.type = "gsm";
	test.cell_path = "/test/cell_0";
	test_dbus_setup(&test.context);

	g_main_loop_run(test.context.loop);

	/* We must have received "RegisteredChanged" signal */
	g_assert(test_dbus_find_signal(&test.context, test.cell_path,
		CELL_DBUS_INTERFACE, CELL_DBUS_REGISTERED_CHANGED_SIGNAL));

	sailfish_cell_info_unref(test.info);
	sailfish_cell_info_dbus_free(test.dbus);
	test_dbus_shutdown(&test.context);
	if (timeout) {
		g_source_remove(timeout);
	}
}

/* ==== PropertyChanged ==== */

struct test_property_changed_data {
	struct ofono_modem modem;
	struct test_dbus_context context;
	struct sailfish_cell_info *info;
	struct sailfish_cell_info_dbus *dbus;
	struct sailfish_cell cell;
	const char *type;
	const char *cell_path;
};

static void test_property_changed_reply1(DBusPendingCall *call, void *data)
{
 	struct test_property_changed_data *test = data;

	DBG("");
	test_check_get_all_reply(call, &test->cell, test->type);
	dbus_pending_call_unref(call);

	test_loop_quit_later(test->context.loop);
}

static void test_property_changed_start(struct test_dbus_context *context)
{
	struct test_property_changed_data *test =
		G_CAST(context, struct test_property_changed_data, context);
	struct sailfish_cell *first_cell;

	DBG("");
	test->info = fake_cell_info_new();
	fake_cell_info_add_cell(test->info, &test->cell);
	test->dbus = sailfish_cell_info_dbus_new(&test->modem, test->info);
	g_assert(test->dbus);

	/* Trigger "PropertyChanged" signal */
	first_cell = test->info->cells->data;
	test->cell.info.gsm.signalStrength =
		(++(first_cell->info.gsm.signalStrength));
	fake_cell_info_cells_changed(test->info);

	test_submit_get_all_call(context->client_connection, test->cell_path,
				test_property_changed_reply1, test);
}

static void test_property_changed(void)
{
	struct test_property_changed_data test;
	guint timeout = test_setup_timeout();

	memset(&test, 0, sizeof(test));
	test.modem.path = TEST_MODEM_PATH;
	test.context.start = test_property_changed_start;
	test_cell_init_gsm1(&test.cell);
	test.type = "gsm";
	test.cell_path = "/test/cell_0";
	test_dbus_setup(&test.context);

	g_main_loop_run(test.context.loop);

	/* We must have received "PropertyChanged" signal */
	g_assert(test_dbus_find_signal(&test.context, test.cell_path,
		CELL_DBUS_INTERFACE, CELL_DBUS_PROPERTY_CHANGED_SIGNAL));

	sailfish_cell_info_unref(test.info);
	sailfish_cell_info_dbus_free(test.dbus);
	test_dbus_shutdown(&test.context);
	if (timeout) {
		g_source_remove(timeout);
	}
}

#define TEST_(name) "/sailfish_cell_info_dbus/" name

int main(int argc, char *argv[])
{
	int i;

	g_test_init(&argc, &argv, NULL);
	for (i=1; i<argc; i++) {
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
	__ofono_log_init("test-sailfish_cell_info_dbus",
				g_test_verbose() ? "*" : NULL,
				FALSE, FALSE);

	g_test_add_func(TEST_("Misc"), test_misc);
	g_test_add_func(TEST_("GetCells"), test_get_cells);
	g_test_add_func(TEST_("GetAll1"), test_get_all1);
	g_test_add_func(TEST_("GetAll2"), test_get_all2);
	g_test_add_func(TEST_("GetAll3"), test_get_all3);
	g_test_add_func(TEST_("GetAll4"), test_get_all4);
	g_test_add_func(TEST_("GetInterfaceVersion"), test_get_version);
	g_test_add_func(TEST_("GetType"), test_get_type);
	g_test_add_func(TEST_("GetRegistered"), test_get_registered);
	g_test_add_func(TEST_("GetProperties"), test_get_properties);
	g_test_add_func(TEST_("RegisteredChanged"), test_registered_changed);
	g_test_add_func(TEST_("PropertyChanged"), test_property_changed);

	return g_test_run();
}

/*
 * Local Variables:
 * mode: C
 * c-basic-offset: 8
 * indent-tabs-mode: t
 * End:
 */
