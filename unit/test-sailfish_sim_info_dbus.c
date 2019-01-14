/*
 *  oFono - Open Source Telephony
 *
 *  Copyright (C) 2018-2019 Jolla Ltd.
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
#include "fake_watch.h"

#include "sailfish_sim_info.h"

#include <gutil_log.h>
#include <gutil_macros.h>

#include "ofono.h"

#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>

#define TEST_TIMEOUT                        (10)   /* seconds */
#define TEST_MODEM_PATH                     "/test"
#define TEST_ICCID                          "0000000000000000000"
#define TEST_IMSI                           "244120000000000"
#define TEST_MCC                            "244"
#define TEST_MNC                            "12"
#define TEST_DEFAULT_SPN                    TEST_MCC TEST_MNC
#define TEST_SPN                            "Test"

#define SIM_INFO_DBUS_INTERFACE             "org.nemomobile.ofono.SimInfo"
#define SIM_INFO_DBUS_INTERFACE_VERSION     (1)

#define SIM_INFO_DBUS_ICCID_CHANGED_SIGNAL  "CardIdentifierChanged"
#define SIM_INFO_DBUS_IMSI_CHANGED_SIGNAL   "SubscriberIdentityChanged"
#define SIM_INFO_DBUS_SPN_CHANGED_SIGNAL    "ServiceProviderNameChanged"

static gboolean test_debug;

/* Fake ofono_sim */

struct ofono_sim {
	const char *mcc;
	const char *mnc;
	const char *spn;
	enum ofono_sim_state state;
};

enum ofono_sim_state ofono_sim_get_state(struct ofono_sim *sim)
{
	return sim ? sim->state : OFONO_SIM_STATE_NOT_PRESENT;
}

const char *ofono_sim_get_mcc(struct ofono_sim *sim)
{
	return sim ? sim->mcc : NULL;
}

const char *ofono_sim_get_mnc(struct ofono_sim *sim)
{
	return sim ? sim->mnc : NULL;
}

/* Stubs (ofono) */

struct ofono_modem {
	const char *path;
	GSList *iflist;
	struct ofono_sim sim;
};

const char *ofono_modem_get_path(struct ofono_modem *modem)
{
	return modem->path;
}

static gint test_strcmp(gconstpointer a, gconstpointer b)
{
	return g_strcmp0(a, b);
}

static char *test_find_interface(struct ofono_modem *modem, const char *iface)
{
	GSList *l = g_slist_find_custom(modem->iflist, iface, test_strcmp);
	return l ? l->data : NULL;
}

void ofono_modem_add_interface(struct ofono_modem *modem, const char *iface)
{
	if (iface && !test_find_interface(modem, iface)) {
		DBG("%s %s", modem->path, iface);
		modem->iflist = g_slist_append(modem->iflist, g_strdup(iface));
	}
}

void ofono_modem_remove_interface(struct ofono_modem *modem, const char *iface)
{
	char *found = test_find_interface(modem, iface);
	if (found) {
		DBG("%s %s", modem->path, iface);
		modem->iflist = g_slist_remove(modem->iflist, found);
		g_free(found);
	}
}

/* Fake ofono_netreg */

struct ofono_netreg {
	const char *mcc;
	const char *mnc;
	const char *name;
	int location;
	int cellid;
	enum ofono_radio_access_mode technology;
	int status;
	struct ofono_watchlist *status_watches;
};

int ofono_netreg_get_status(struct ofono_netreg *netreg)
{
	return netreg ? (int) netreg->status : -1;
}

const char *ofono_netreg_get_mcc(struct ofono_netreg *netreg)
{
	return netreg ? netreg->mcc : NULL;
}

const char *ofono_netreg_get_mnc(struct ofono_netreg *netreg)
{
	return netreg ? netreg->mnc : NULL;
}

const char *ofono_netreg_get_name(struct ofono_netreg *netreg)
{
	return netreg ? netreg->name : NULL;
}

unsigned int __ofono_netreg_add_status_watch(struct ofono_netreg *netreg,
				ofono_netreg_status_notify_cb_t notify,
				void *data, ofono_destroy_func destroy)
{
	struct ofono_watchlist_item *item =
		g_new0(struct ofono_watchlist_item, 1);

	DBG("%p", netreg);
	g_assert(netreg);
	g_assert(notify);
	item->notify = notify;
	item->destroy = destroy;
	item->notify_data = data;
	return __ofono_watchlist_add_item(netreg->status_watches, item);
}

gboolean __ofono_netreg_remove_status_watch(struct ofono_netreg *netreg,
						unsigned int id)
{
	return __ofono_watchlist_remove_item(netreg->status_watches, id);
}

/* Utilities */

static int rmdir_r(const char *path)
{
	DIR *d = opendir(path);

	if (d) {
		const struct dirent *p;
		int r = 0;

		while (!r && (p = readdir(d))) {
			char *buf;
			struct stat st;

			if (!strcmp(p->d_name, ".") ||
						!strcmp(p->d_name, "..")) {
				continue;
			}

			buf = g_strdup_printf("%s/%s", path, p->d_name);
			if (!stat(buf, &st)) {
				r =  S_ISDIR(st.st_mode) ? rmdir_r(buf) :
								unlink(buf);
			}
			g_free(buf);
		}
		closedir(d);
		return r ? r : rmdir(path);
	} else {
		return -1;
	}
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

/* ==== Misc ==== */

static void test_misc(void)
{
	/* NULL resistance */
	g_assert(!sailfish_sim_info_dbus_new_path(NULL));
	sailfish_sim_info_dbus_free(NULL);
}

/* ==== GetAll ==== */

struct test_get_all_data {
	struct ofono_modem modem;
	struct test_dbus_context context;
	struct sailfish_sim_info_dbus *dbus;
	struct ofono_watch *watch;
	const char *iccid;
};

static void test_submit_get_all_call(struct test_get_all_data *test,
					DBusPendingCallNotifyFunction notify)
{
	DBusPendingCall *call;
	DBusConnection* connection = test->context.client_connection;
	DBusMessage *msg = dbus_message_new_method_call(NULL, test->modem.path,
					SIM_INFO_DBUS_INTERFACE, "GetAll");

	g_assert(dbus_connection_send_with_reply(connection, msg, &call,
						DBUS_TIMEOUT_INFINITE));
	dbus_pending_call_set_notify(call, notify, test, NULL);
	dbus_message_unref(msg);
}

static void test_check_get_all_reply(struct test_get_all_data *test,
						DBusPendingCall *call)
{
	DBusMessage *reply = dbus_pending_call_steal_reply(call);
	DBusMessageIter it;

	g_assert(dbus_message_get_type(reply) ==
					DBUS_MESSAGE_TYPE_METHOD_RETURN);
	dbus_message_iter_init(reply, &it);
	g_assert(test_dbus_get_int32(&it) == SIM_INFO_DBUS_INTERFACE_VERSION);
	g_assert(!g_strcmp0(test_dbus_get_string(&it), test->iccid));
	g_assert(!g_strcmp0(test_dbus_get_string(&it), ""));
	g_assert(!g_strcmp0(test_dbus_get_string(&it), ""));
	g_assert(dbus_message_iter_get_arg_type(&it) == DBUS_TYPE_INVALID);
	dbus_message_unref(reply);
}

static void test_get_all_reply(DBusPendingCall *call, void *data)
{
 	struct test_get_all_data *test = data;

	DBG("");
	test_check_get_all_reply(test, call);
	dbus_pending_call_unref(call);

	test_loop_quit_later(test->context.loop);
}

static void test_get_all1_start(struct test_dbus_context *context)
{
	struct test_get_all_data *test =
		G_CAST(context, struct test_get_all_data, context);
	const char *path = test->modem.path;

	DBG("");
	test->dbus = sailfish_sim_info_dbus_new_path(path);
	g_assert(test->dbus);

	test_submit_get_all_call(test, test_get_all_reply);
}

static void test_get_all1(void)
{
	struct test_get_all_data test;
	guint timeout = test_setup_timeout();

	rmdir_r(STORAGEDIR);
	memset(&test, 0, sizeof(test));
	test.modem.path = TEST_MODEM_PATH;
	test.context.start = test_get_all1_start;
	test.watch = ofono_watch_new(test.modem.path);
	test.watch->modem = &test.modem;
	test.iccid = "";
	test_dbus_setup(&test.context);

	g_main_loop_run(test.context.loop);

	ofono_watch_unref(test.watch);
	sailfish_sim_info_dbus_free(test.dbus);
	test_dbus_shutdown(&test.context);
	if (timeout) {
		g_source_remove(timeout);
	}
	rmdir_r(STORAGEDIR);
}

/* ==== GetAll2 ==== */

static void test_get_all2_start(struct test_dbus_context *context)
{
	struct test_get_all_data *test =
		G_CAST(context, struct test_get_all_data, context);
	const char *path = test->modem.path;
	struct ofono_watch *watch = test->watch;

	DBG("");
	test->dbus = sailfish_sim_info_dbus_new_path(path);
	g_assert(test->dbus);

	/* Tell ofono_watch that we have a modem */
	test->watch->modem = &test->modem;
	fake_watch_set_ofono_sim(watch, &test->modem.sim);
	fake_watch_set_ofono_iccid(watch, test->iccid);
	fake_watch_signal_queue(watch, FAKE_WATCH_SIGNAL_MODEM_CHANGED);
	fake_watch_emit_queued_signals(watch);

	test_submit_get_all_call(test, test_get_all_reply);
}

static void test_get_all2(void)
{
	struct test_get_all_data test;
	guint timeout = test_setup_timeout();

	rmdir_r(STORAGEDIR);
	memset(&test, 0, sizeof(test));
	test.modem.path = TEST_MODEM_PATH;
	test.context.start = test_get_all2_start;
	test.watch = ofono_watch_new(test.modem.path);
	test.iccid = TEST_ICCID;
	test_dbus_setup(&test.context);

	g_main_loop_run(test.context.loop);

	/* "CardIdentifierChanged" is expected */
	g_assert(test_dbus_find_signal(&test.context, test.modem.path,
		SIM_INFO_DBUS_INTERFACE, SIM_INFO_DBUS_ICCID_CHANGED_SIGNAL));

	ofono_watch_unref(test.watch);
	sailfish_sim_info_dbus_free(test.dbus);
	test_dbus_shutdown(&test.context);
	if (timeout) {
		g_source_remove(timeout);
	}
	rmdir_r(STORAGEDIR);
}

/* ==== GetInterfaceVersion ==== */

struct test_get_version_data {
	struct ofono_modem modem;
	struct test_dbus_context context;
	struct sailfish_sim_info_dbus *dbus;
};

static void test_get_version_reply(DBusPendingCall *call, void *data)
{
 	struct test_get_version_data *test = data;
	DBusMessage *reply = dbus_pending_call_steal_reply(call);
	DBusMessageIter it;

	DBG("");
	g_assert(dbus_message_get_type(reply) ==
					DBUS_MESSAGE_TYPE_METHOD_RETURN);
	dbus_message_iter_init(reply, &it);
	g_assert(test_dbus_get_int32(&it) == SIM_INFO_DBUS_INTERFACE_VERSION);
	g_assert(dbus_message_iter_get_arg_type(&it) == DBUS_TYPE_INVALID);
	dbus_message_unref(reply);
	dbus_pending_call_unref(call);

	test_loop_quit_later(test->context.loop);
}

static void test_get_version_start(struct test_dbus_context *context)
{
	DBusMessage *msg;
	DBusPendingCall *call;
	struct test_get_version_data *test =
		G_CAST(context, struct test_get_version_data, context);
	const char *path = test->modem.path;

	DBG("");
	test->dbus = sailfish_sim_info_dbus_new_path(path);
	g_assert(test->dbus);

	msg = dbus_message_new_method_call(NULL, test->modem.path,
			SIM_INFO_DBUS_INTERFACE, "GetInterfaceVersion");
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

	sailfish_sim_info_dbus_free(test.dbus);
	test_dbus_shutdown(&test.context);
	if (timeout) {
		g_source_remove(timeout);
	}
}

/* ==== GetCardIdentifier ==== */

struct test_get_iccid_data {
	struct ofono_modem modem;
	struct test_dbus_context context;
	struct sailfish_sim_info_dbus *dbus;
	struct ofono_watch *watch;
	const char *iccid;
	const char *result;
};

static void test_get_iccid_reply(DBusPendingCall *call, void *data)
{
 	struct test_get_iccid_data *test = data;

	DBG("");
	test_dbus_check_string_reply(call, test->result);
	dbus_pending_call_unref(call);

	test_loop_quit_later(test->context.loop);
}

static void test_get_iccid_start(struct test_dbus_context *context)
{
	DBusMessage *msg;
	DBusPendingCall *call;
	struct test_get_iccid_data *test =
		G_CAST(context, struct test_get_iccid_data, context);
	const char *path = test->modem.path;

	DBG("");
	test->dbus = sailfish_sim_info_dbus_new_path(path);
	fake_watch_set_ofono_iccid(test->watch, test->iccid);
	fake_watch_emit_queued_signals(test->watch);
	g_assert(test->dbus);

	msg = dbus_message_new_method_call(NULL, test->modem.path,
			SIM_INFO_DBUS_INTERFACE, "GetCardIdentifier");
	g_assert(dbus_connection_send_with_reply(context->client_connection,
					msg, &call, DBUS_TIMEOUT_INFINITE));
	dbus_pending_call_set_notify(call, test_get_iccid_reply, test, NULL);
	dbus_message_unref(msg);
}

static void test_get_iccid(const char *init_iccid, const char *set_iccid,
							const char *result)
{
	struct test_get_iccid_data test;
	guint timeout = test_setup_timeout();

	memset(&test, 0, sizeof(test));
	test.iccid = set_iccid;
	test.result = result;
	test.modem.path = TEST_MODEM_PATH;
	test.context.start = test_get_iccid_start;
	test.watch = ofono_watch_new(test.modem.path);
	test.watch->modem = &test.modem;
	fake_watch_set_ofono_iccid(test.watch, init_iccid);
	fake_watch_emit_queued_signals(test.watch);
	test_dbus_setup(&test.context);

	g_main_loop_run(test.context.loop);

	/* "CardIdentifierChanged" is expected */
	g_assert(test_dbus_find_signal(&test.context, test.modem.path,
		SIM_INFO_DBUS_INTERFACE, SIM_INFO_DBUS_ICCID_CHANGED_SIGNAL));

	ofono_watch_unref(test.watch);
	sailfish_sim_info_dbus_free(test.dbus);
	test_dbus_shutdown(&test.context);
	if (timeout) {
		g_source_remove(timeout);
	}
}

static void test_get_iccid1(void)
{
	test_get_iccid(NULL, TEST_ICCID, TEST_ICCID);
}

/* ==== GetCardIdentifier2 ==== */

static void test_get_iccid2(void)
{
	test_get_iccid(TEST_ICCID, NULL, "");
}

/* ==== GetSubscriberIdentity ==== */

struct test_get_string_data {
	struct ofono_modem modem;
	struct test_dbus_context context;
	struct sailfish_sim_info_dbus *dbus;
	struct ofono_watch *watch;
	const char *method;
	const char *result;
};

static void test_get_string_reply(DBusPendingCall *call, void *data)
{
 	struct test_get_string_data *test = data;

	DBG("");
	test_dbus_check_string_reply(call, test->result);
	dbus_pending_call_unref(call);

	test_loop_quit_later(test->context.loop);
}

static void test_get_string_start(struct test_dbus_context *context)
{
	DBusMessage *msg;
	DBusPendingCall *call;
	struct test_get_string_data *test =
		G_CAST(context, struct test_get_string_data, context);
	const char *path = test->modem.path;
	struct ofono_sim *sim = &test->modem.sim;
	struct ofono_watch *watch = test->watch;

	DBG("%s", test->method);
	test->dbus = sailfish_sim_info_dbus_new_path(path);
	sim->mcc = TEST_MCC;
	sim->mnc = TEST_MNC;
	sim->state = OFONO_SIM_STATE_READY;
	fake_watch_signal_queue(watch, FAKE_WATCH_SIGNAL_SIM_STATE_CHANGED);
	fake_watch_set_ofono_imsi(watch, TEST_IMSI);
	fake_watch_emit_queued_signals(watch);
	g_assert(test->dbus);

	msg = dbus_message_new_method_call(NULL, test->modem.path,
					SIM_INFO_DBUS_INTERFACE, test->method);
	g_assert(dbus_connection_send_with_reply(context->client_connection,
					msg, &call, DBUS_TIMEOUT_INFINITE));
	dbus_pending_call_set_notify(call, test_get_string_reply, test, NULL);
	dbus_message_unref(msg);
}

static void test_get_string(const char *method, const char *result)
{
	struct test_get_string_data test;
	guint timeout = test_setup_timeout();

	rmdir_r(STORAGEDIR);
	memset(&test, 0, sizeof(test));
	test.method = method;
	test.result = result;
	test.modem.path = TEST_MODEM_PATH;
	test.context.start = test_get_string_start;
	test.watch = ofono_watch_new(test.modem.path);
	test.watch->modem = &test.modem;
	fake_watch_set_ofono_iccid(test.watch, TEST_ICCID);
	fake_watch_set_ofono_sim(test.watch, &test.modem.sim);
	fake_watch_emit_queued_signals(test.watch);
	test_dbus_setup(&test.context);

	g_main_loop_run(test.context.loop);

	/* Verify signals */
	g_assert(test_dbus_find_signal(&test.context, test.modem.path,
		SIM_INFO_DBUS_INTERFACE, SIM_INFO_DBUS_IMSI_CHANGED_SIGNAL));
	g_assert(test_dbus_find_signal(&test.context, test.modem.path,
		SIM_INFO_DBUS_INTERFACE, SIM_INFO_DBUS_SPN_CHANGED_SIGNAL));

	ofono_watch_unref(test.watch);
	sailfish_sim_info_dbus_free(test.dbus);
	test_dbus_shutdown(&test.context);
	if (timeout) {
		g_source_remove(timeout);
	}
	rmdir_r(STORAGEDIR);
}

static void test_get_imsi(void)
{
	test_get_string("GetSubscriberIdentity", TEST_IMSI);
}

/* ==== GetServiceProviderName ==== */

static void test_get_spn(void)
{
	test_get_string("GetServiceProviderName", TEST_DEFAULT_SPN);
}

#define TEST_(name) "/sailfish_sim_info_dbus/" name

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
	__ofono_log_init("test-sailfish_sim_info_dbus",
				g_test_verbose() ? "*" : NULL,
				FALSE, FALSE);

	g_test_add_func(TEST_("Misc"), test_misc);
	g_test_add_func(TEST_("GetAll1"), test_get_all1);
	g_test_add_func(TEST_("GetAll2"), test_get_all2);
	g_test_add_func(TEST_("GetInterfaceVersion"), test_get_version);
	g_test_add_func(TEST_("GetCardIdentifier1"), test_get_iccid1);
	g_test_add_func(TEST_("GetCardIdentifier2"), test_get_iccid2);
	g_test_add_func(TEST_("GetSubscriberIdentity"), test_get_imsi);
	g_test_add_func(TEST_("GetServiceProviderName"), test_get_spn);

	return g_test_run();
}

/*
 * Local Variables:
 * mode: C
 * c-basic-offset: 8
 * indent-tabs-mode: t
 * End:
 */
