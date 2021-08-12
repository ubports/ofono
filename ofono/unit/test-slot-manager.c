/*
 *  oFono - Open Source Telephony
 *
 *  Copyright (C) 2017-2021 Jolla Ltd.
 *  Copyright (C) 2019-2020 Open Mobile Platform LLC.
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

#include <ofono/slot.h>
#include <ofono/cell-info.h>

#include "sim-info.h"
#include "slot-manager-dbus.h"
#include "fake_watch.h"

#define OFONO_API_SUBJECT_TO_CHANGE
#include "ofono.h"

#include <gutil_log.h>
#include <gutil_strv.h>

#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>

#define TEST_TIMEOUT_SEC (20)
#define TEST_IDLE_WAIT_COUNT (10) /* Should be > SF_INIT_IDLE_COUNT */
#define TEST_PATH "/test_0"
#define TEST_PATH_1 "/test_1"
#define TEST_ICCID "1111111111111111111"
#define TEST_ICCID_1 "1111111111111111112"
#define TEST_IMEI "222222222222222"
#define TEST_IMEI_1 "222222222222223"
#define TEST_IMEISV "33"
#define TEST_IMSI "244120000000000"
#define TEST_IMSI_1 "244120000000001"
#define TEST_MCC "244"
#define TEST_MNC "12"
#define TEST_SPN "Test"
#define TEST_ERROR_KEY "Error"
#define TEST_SLOT_ERROR_KEY "SlotError"
#define TEST_CONFIG_DIR_TEMPLATE "test-saifish_manager-config-XXXXXX"

static GMainLoop *test_loop = NULL;
static GSList *test_drivers = NULL;
static struct ofono_slot_driver_reg *test_driver_reg = NULL;
static guint test_timeout_id = 0;

/* Recursive rmdir */

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

			buf = g_build_filename(path, p->d_name, NULL);
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

/* Fake ofono_modem */

struct ofono_modem {
	int unused;
};

/* Fake ofono_sim */

struct ofono_sim {
	const char *mcc;
	const char *mnc;
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

/* Fake ofono_netreg */

struct ofono_netreg {
	const char *mcc;
	const char *mnc;
	const char *name;
	int status;
};

int ofono_netreg_get_status(struct ofono_netreg *netreg)
{
	return netreg ? netreg->status : -1;
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
	ofono_netreg_status_notify_cb_t notify, void *data,
	ofono_destroy_func destroy)
{
	return 1;
}

gboolean __ofono_netreg_remove_status_watch(struct ofono_netreg *netreg,
						unsigned int id)
{
	return TRUE;
}

/* Fake slot_manager_dbus */

static struct slot_manager_dbus {
	struct ofono_slot_manager *m;
	struct slot_manager_dbus_cb cb;
	enum slot_manager_dbus_block block;
	void (*fn_block_changed)(struct slot_manager_dbus *d);
	void (*fn_signal)(struct slot_manager_dbus *d,
		enum slot_manager_dbus_signal mask);
	int signals;
} fake_slot_manager_dbus;

struct slot_manager_dbus *slot_manager_dbus_new(struct ofono_slot_manager *m,
	const struct slot_manager_dbus_cb *cb)
{
	memset(&fake_slot_manager_dbus, 0, sizeof(fake_slot_manager_dbus));
	fake_slot_manager_dbus.m = m;
	fake_slot_manager_dbus.cb = *cb;
	return &fake_slot_manager_dbus;
}

void slot_manager_dbus_free(struct slot_manager_dbus *d)
{
	g_assert(d == &fake_slot_manager_dbus);
	g_assert(fake_slot_manager_dbus.m);
	memset(&fake_slot_manager_dbus, 0, sizeof(fake_slot_manager_dbus));
}

void slot_manager_dbus_set_block(struct slot_manager_dbus *d,
	enum slot_manager_dbus_block b)
{
	if (d->block != b) {
		DBG("0x%02x", (int)b);
		d->block = b;
		if (d->fn_block_changed) {
			d->fn_block_changed(d);
		}
	}
}
void slot_manager_dbus_signal(struct slot_manager_dbus *d,
	enum slot_manager_dbus_signal m)
{
	d->signals |= m;
	if (d->fn_signal) {
		d->fn_signal(d, m);
	}
}

void slot_manager_dbus_signal_sim(struct slot_manager_dbus *d,
	int index, enum slot_manager_dbus_slot_signal mask) {}
void slot_manager_dbus_signal_error(struct slot_manager_dbus *d,
	const char *id, const char *message) {}
void slot_manager_dbus_signal_modem_error(struct slot_manager_dbus *d,
	int index, const char *id, const char *msg) {}

/* Fake sim_info */

struct sim_info_dbus {
	int unused;
};

struct sim_info_dbus *sim_info_dbus_new(struct sim_info *info)
{
	static struct sim_info_dbus fake_sim_info_dbus;
	return &fake_sim_info_dbus;
}

void sim_info_dbus_free(struct sim_info_dbus *dbus) {}

/* Fake ofono_cell_info */

static int fake_ofono_cell_info_ref_count = 0;

static void fake_ofono_cell_info_ref(struct ofono_cell_info *info)
{
	g_assert(fake_ofono_cell_info_ref_count >= 0);
	fake_ofono_cell_info_ref_count++;
}

static void fake_ofono_cell_info_unref(struct ofono_cell_info *info)
{
	g_assert(fake_ofono_cell_info_ref_count > 0);
	fake_ofono_cell_info_ref_count--;
}

static gulong fake_ofono_cell_info_add_cells_changed_handler
	(struct ofono_cell_info *info, ofono_cell_info_cb_t cb, void *arg)
{
	return 1;
}

static void fake_ofono_cell_info_remove_handler(struct ofono_cell_info *info,
	gulong id)
{
	g_assert(id == 1);
}

static const struct ofono_cell_info_proc fake_ofono_cell_info_proc = {
	fake_ofono_cell_info_ref,
	fake_ofono_cell_info_unref,
	fake_ofono_cell_info_add_cells_changed_handler,
	fake_ofono_cell_info_remove_handler
};

static struct ofono_cell_info fake_ofono_cell_info = {
	&fake_ofono_cell_info_proc,
	NULL
};

/* cell_info_dbus */

struct cell_info_dbus {
	int unused;
};

struct cell_info_dbus *cell_info_dbus_new(struct ofono_modem *modem,
	struct ofono_cell_info *info)
{
	static struct cell_info_dbus fake_ofono_cell_info_dbus;
	return &fake_ofono_cell_info_dbus;
}

void cell_info_dbus_free(struct cell_info_dbus *dbus) {}

/* Code shared by all tests */

typedef struct ofono_slot_driver_data {
	struct ofono_slot_manager *manager;
	gulong property_change_id;
	GSList *slot_data; /* TestSlotData* */
	int counter;
} TestDriverData;

typedef struct test_slot_data {
	struct ofono_slot *slot;
	TestDriverData *driver;
	gulong property_change_id;
	int slot_property_changed[OFONO_SLOT_PROPERTY_LAST + 1];
} TestSlotData;

static gboolean test_timeout_cb(gpointer user_data)
{
	ofono_error("Timeout!");
	g_main_loop_quit(test_loop);
	test_timeout_id = 0;
	return G_SOURCE_REMOVE;
}

static void test_quit_loop_when_unblocked(struct slot_manager_dbus *d)
{
	if (d->block == SLOT_MANAGER_DBUS_BLOCK_NONE) {
		g_main_loop_quit(test_loop);
	}
}

static void test_common_init()
{
	rmdir_r(STORAGEDIR);
	g_assert(!test_loop);
	g_assert(!test_drivers);
	g_assert(!test_timeout_id);
	g_assert(!test_driver_reg);
	__ofono_slot_manager_init();
	test_loop = g_main_loop_new(NULL, FALSE);
	test_timeout_id = g_timeout_add_seconds(TEST_TIMEOUT_SEC,
						test_timeout_cb, NULL);
}

static void test_common_deinit()
{
	__ofono_slot_manager_cleanup();
	g_assert(test_timeout_id);
	g_source_remove(test_timeout_id);
	g_main_loop_unref(test_loop);
	g_assert(!test_drivers);
	test_timeout_id = 0;
	test_loop = NULL;
}

static gboolean test_done_cb(gpointer user_data)
{
	g_main_loop_quit(test_loop);
	return G_SOURCE_REMOVE;
}

static gboolean test_done_when_zero(gpointer user_data)
{
	int* count = user_data;

	if (*count > 0) {
		(*count)--;
		return G_SOURCE_CONTINUE;
	} else {
		g_main_loop_quit(test_loop);
		return G_SOURCE_REMOVE;
	}
}

static gboolean test_unregister_later(void *unused)
{
	ofono_slot_driver_unregister(test_driver_reg);
	test_driver_reg = NULL;
	return G_SOURCE_REMOVE;
}

static  void test_slot_manager_unreachable_handler(struct ofono_slot_manager *m,
	enum ofono_slot_property property, void* user_data)
{
	g_assert_not_reached();
}

static  void test_slot_manager_exit_when_ready_cb(struct ofono_slot_manager *m,
	enum ofono_slot_property property, void* unused)
{
	DBG("%d", m->ready);
	if (m->ready) {
		DBG("Ready!");
		g_main_loop_quit(test_loop);
	}
}

static void test_slot_property_change_cb(struct ofono_slot *slot,
	enum ofono_slot_property property, void* user_data)
{
	TestSlotData *sd = user_data;

	g_assert(property <= OFONO_SLOT_PROPERTY_LAST);
	sd->slot_property_changed[OFONO_SLOT_PROPERTY_ANY]++;
	sd->slot_property_changed[property]++;
}

static TestSlotData *test_slot_data_new2(TestDriverData *dd,
	const char *path, const char *imei, const char *imeisv,
	enum ofono_slot_sim_presence presence)
{
	TestSlotData *sd = NULL;
	struct ofono_slot *slot = ofono_slot_add(dd->manager, path,
		OFONO_RADIO_ACCESS_MODE_GSM, imei, imeisv, presence,
		OFONO_SLOT_NO_FLAGS);

	if (slot) {
		sd = g_new0(TestSlotData, 1);
		sd->slot = slot;
		sd->driver = dd;
		sd->property_change_id = ofono_slot_add_property_handler(slot,
			OFONO_SLOT_PROPERTY_ANY, test_slot_property_change_cb,
			sd);
		dd->slot_data = g_slist_append(dd->slot_data, sd);
	}
	return sd;
}

static TestSlotData *test_slot_data_new(TestDriverData *dd,
	const char *path, const char *imei, const char *imeisv)
{
	return test_slot_data_new2(dd, path, imei, imeisv,
		OFONO_SLOT_SIM_UNKNOWN);
}

static void test_slot_data_free(gpointer data)
{
	TestSlotData *sd = data;

	ofono_slot_remove_handler(sd->slot, sd->property_change_id);
	ofono_slot_unref(sd->slot);
	g_free(sd);
}

static TestDriverData *test_driver_init(struct ofono_slot_manager *m)
{
	TestDriverData *dd = g_new0(TestDriverData, 1);

	DBG("%p", dd);
	dd->manager = m;
	test_drivers = g_slist_append(test_drivers, dd);
	return dd;
}

static void test_driver_cleanup(TestDriverData *dd)
{
	DBG("%p", dd);
	test_drivers = g_slist_remove(test_drivers, dd);
	ofono_slot_manager_remove_handler(dd->manager, dd->property_change_id);
	g_slist_free_full(dd->slot_data, test_slot_data_free);
	g_free(dd);
}

static void test_driver_cancel_unreachable(TestDriverData *dd, unsigned int id)
{
	g_assert_not_reached();
}

static void test_driver_cancel_source(TestDriverData *dd, unsigned int id)
{
	g_assert(id);
	g_source_remove(id);
}

/* Test cases */

/* ==== basic ==== */

static TestDriverData *test_basic_driver_init(struct ofono_slot_manager *m)
{
	TestDriverData *dd;

	DBG("");
	dd = test_driver_init(m);
	/* This ref is not necessary but is allowed */
	g_assert(ofono_slot_manager_ref(m) == m);
	return dd;
}

static void test_basic_driver_cleanup(TestDriverData *dd)
{
	/* Undo the ref */
	ofono_slot_manager_unref(dd->manager);
	test_driver_cleanup(dd);
}

static void test_basic(void)
{
	static const struct ofono_slot_driver dummy1 = {
		.name = "Dummy1",
		.api_version = OFONO_SLOT_API_VERSION,
		.init = test_basic_driver_init,
		.cleanup = test_basic_driver_cleanup
	};
	static const struct ofono_slot_driver dummy2 = { .name = "Dummy2" };
	static const struct ofono_slot_driver dummy3 = { .name = "Dummy3" };
	static const struct ofono_slot_driver dummy4 = { .name = "Dummy4" };
	struct ofono_slot_driver_reg *r1, *r2, *r3, *r4;
	TestDriverData *dd;
	int count;

	test_common_init();

	/* NULL resistance */
	g_assert(!ofono_slot_driver_register(NULL));
	ofono_slot_driver_unregister(NULL);
	ofono_slot_driver_started(NULL);
	g_assert(!ofono_slot_driver_get_data(NULL));
	g_assert(!ofono_slot_manager_ref(NULL));
	ofono_slot_manager_unref(NULL);
	ofono_slot_manager_error(NULL, NULL, NULL);
	g_assert(!ofono_slot_manager_add_property_handler(NULL, 0, NULL, NULL));
	ofono_slot_manager_remove_handler(NULL, 0);
	ofono_slot_manager_remove_handler(NULL, 1);
	ofono_slot_manager_remove_handlers(NULL, NULL, 0);
	g_assert(!ofono_slot_ref(NULL));
	ofono_slot_unref(NULL);
	ofono_slot_set_cell_info(NULL, NULL);
	ofono_slot_error(NULL, NULL, NULL);
	g_assert(!ofono_slot_add_property_handler(NULL, 0, NULL, NULL));
	ofono_slot_remove_handler(NULL, 0);
	ofono_slot_remove_handlers(NULL, NULL, 0);
	ofono_slot_set_sim_presence(NULL, 0);

	/* Register dummy driver */
	g_assert((r2 = ofono_slot_driver_register(&dummy2)));
	g_assert((r1 = ofono_slot_driver_register(&dummy1)));
	g_assert((r4 = ofono_slot_driver_register(&dummy4)));
	g_assert((r3 = ofono_slot_driver_register(&dummy3)));

	/*
	 * Run the main loop more than SM_INIT_IDLE_COUNT times to make
	 * sure that slot_manager handles drivers without init and start
	 * callbacks (even though it makes little or no sense).
	 */
	count = 10;
	g_idle_add(test_done_when_zero, &count);
	g_main_loop_run(test_loop);

	/* Only r1 has init callback */
	g_assert_cmpuint(g_slist_length(test_drivers), == ,1);
	g_assert(test_drivers->data == ofono_slot_driver_get_data(r1));

	/* Handlers for invalid properties don't get registered */
	g_assert_cmpuint(g_slist_length(test_drivers), == ,1);
	dd = test_drivers->data;
	g_assert(!ofono_slot_manager_add_property_handler(dd->manager,
		(enum ofono_slot_manager_property)(-1),
		test_slot_manager_unreachable_handler, NULL));
	g_assert(!ofono_slot_manager_add_property_handler(dd->manager,
		(enum ofono_slot_manager_property)
		(OFONO_SLOT_MANAGER_PROPERTY_LAST + 1),
		test_slot_manager_unreachable_handler, NULL));

	ofono_slot_driver_unregister(r3);
	ofono_slot_driver_unregister(r4);
	ofono_slot_driver_unregister(r2);
	ofono_slot_driver_unregister(r1);
	ofono_slot_driver_unregister(r1); /* Does nothing */

	/* Double cleanup is fine */
	test_common_deinit();
	__ofono_slot_manager_cleanup();

	/* These are ignored too */
	ofono_slot_driver_unregister(NULL);
	ofono_slot_driver_unregister(r1);
}

/* ==== early_init ==== */

static unsigned int test_early_init_start(TestDriverData *dd)
{
	struct ofono_slot_manager *mgr = dd->manager;

	DBG("");
	g_assert(!dd->property_change_id);
	dd->property_change_id = ofono_slot_manager_add_property_handler(mgr,
		OFONO_SLOT_MANAGER_PROPERTY_READY,
		test_slot_manager_exit_when_ready_cb, dd);
	return 0;
}

static void test_early_init(void)
{
	static const struct ofono_slot_driver early_init_driver = {
		.name = "early_init",
		.api_version = OFONO_SLOT_API_VERSION,
		.init = test_driver_init,
		.start = test_early_init_start,
		.cancel = test_driver_cancel_unreachable,
		.cleanup = test_driver_cleanup
	};

	/* Register before __ofono_slot_manager_init */
	g_assert(ofono_slot_driver_register(&early_init_driver));

	test_common_init();

	g_main_loop_run(test_loop);
	g_assert_cmpuint(g_slist_length(test_drivers), == ,1);

	test_common_deinit();
}

/* ==== too_late ==== */

static gboolean test_too_late_cb(gpointer user_data)
{
	guint* counter = user_data;

	(*counter)--;
	DBG("%u", *counter);
	if (!(*counter)) {
		static const struct ofono_slot_driver too_late_driver = {
			.name = "too_late",
			.api_version = OFONO_SLOT_API_VERSION,
			.init = test_driver_init,
			.cleanup = test_driver_cleanup
		};

		g_assert(!ofono_slot_driver_register(&too_late_driver));
		g_assert(fake_slot_manager_dbus.block ==
			 SLOT_MANAGER_DBUS_BLOCK_NONE);
		g_main_loop_quit(test_loop);
		return G_SOURCE_REMOVE;
	} else {
		return G_SOURCE_CONTINUE;
	}
}

static void test_too_late(void)
{
	guint counter = TEST_IDLE_WAIT_COUNT;

	test_common_init();

	g_idle_add(test_too_late_cb, &counter);
	g_main_loop_run(test_loop);
	g_assert(!counter);

	test_common_deinit();
}

/* ==== create_fail ==== */

static TestDriverData *test_create_fail_init(struct ofono_slot_manager *m)
{
	DBG("");
	g_main_loop_quit(test_loop);
	return NULL;
}

static void test_create_fail(void)
{
	static const struct ofono_slot_driver create_fail_driver = {
		.name = "create_fail",
		.api_version = OFONO_SLOT_API_VERSION,
		.init = test_create_fail_init
	};
	struct ofono_slot_driver_reg *reg;

	test_common_init();

	g_assert((reg = ofono_slot_driver_register(&create_fail_driver)));
	g_main_loop_run(test_loop);

	test_common_deinit();
}

/* ==== no_drivers ==== */

static void test_quit_when_ready(struct slot_manager_dbus *d,
					enum slot_manager_dbus_signal m)
{
	DBG("%d", m);
	if (d->m->ready) {
		DBG("Ready!");
		g_main_loop_quit(test_loop);
	}
}

static void test_no_drivers(void)
{
	test_common_init();

	fake_slot_manager_dbus.fn_signal = test_quit_when_ready;
	g_main_loop_run(test_loop);

	test_common_deinit();
}

/* ==== no_slots ==== */

static unsigned int test_no_slots_start(TestDriverData *dd)
{
	DBG("");
	g_main_loop_quit(test_loop);
	return 0;
}

static void test_no_slots(void)
{
	static const struct ofono_slot_driver no_slots_driver = {
		.name = "no_slots",
		.api_version = OFONO_SLOT_API_VERSION,
		.init = test_driver_init,
		.start = test_no_slots_start,
		.cancel = test_driver_cancel_unreachable,
		.cleanup = test_driver_cleanup
	};

	test_common_init();

	g_assert(ofono_slot_driver_register(&no_slots_driver));
	g_main_loop_run(test_loop);
	g_assert(fake_slot_manager_dbus.m);
	g_assert(fake_slot_manager_dbus.m->ready);

	test_common_deinit();
}

/* ==== sync_start ==== */

static gboolean test_sync_start_done(gpointer user_data)
{
	TestDriverData *dd = user_data;
	TestSlotData *sd;
	struct ofono_slot *s;
	struct ofono_slot_manager *mgr = dd->manager;
	struct ofono_watch *w = ofono_watch_new(TEST_PATH);
	struct ofono_slot_manager *m = fake_slot_manager_dbus.m;
	struct ofono_modem modem;
	char **slots;
	GHashTable *errors;

	g_assert_cmpuint(g_slist_length(dd->slot_data), == ,1);
	sd = dd->slot_data->data;
	s = sd->slot;

	/* Poke cell info API */
	ofono_slot_set_cell_info(s, NULL);
	ofono_slot_set_cell_info(s, &fake_ofono_cell_info);

	memset(&modem, 0, sizeof(modem));
	w->modem = &modem;
	w->online = TRUE;
	fake_watch_signal_queue(w, FAKE_WATCH_SIGNAL_ONLINE_CHANGED);
	fake_watch_signal_queue(w, FAKE_WATCH_SIGNAL_MODEM_CHANGED);
	fake_watch_emit_queued_signals(w);

	ofono_slot_set_cell_info(s, NULL);
	ofono_slot_set_cell_info(s, &fake_ofono_cell_info);

	w->modem = NULL;
	w->online = FALSE;
	fake_watch_signal_queue(w, FAKE_WATCH_SIGNAL_ONLINE_CHANGED);
	fake_watch_signal_queue(w, FAKE_WATCH_SIGNAL_MODEM_CHANGED);
	fake_watch_emit_queued_signals(w);

	ofono_slot_set_cell_info(s, NULL);
	g_assert(!fake_ofono_cell_info_ref_count);

	/* Poke error counters */
	ofono_slot_manager_error(mgr, TEST_ERROR_KEY, "Aaah!");
	ofono_slot_error(s, TEST_SLOT_ERROR_KEY, "Aaah!");

	errors = fake_slot_manager_dbus.cb.get_errors(m);
	g_assert(g_hash_table_size(errors) == 1);
	g_assert(GPOINTER_TO_INT(g_hash_table_lookup(errors,
            TEST_ERROR_KEY)) == 1);

	errors = fake_slot_manager_dbus.cb.get_slot_errors(s);
	g_assert(g_hash_table_size(errors) == 1);
	g_assert(GPOINTER_TO_INT(g_hash_table_lookup(errors,
            TEST_SLOT_ERROR_KEY)) == 1);

	ofono_slot_manager_error(mgr, TEST_ERROR_KEY, "Aaah!");
	ofono_slot_error(s, TEST_SLOT_ERROR_KEY, "Aaah!");

	errors = fake_slot_manager_dbus.cb.
		get_errors(fake_slot_manager_dbus.m);
	g_assert(g_hash_table_size(errors) == 1);
	g_assert(GPOINTER_TO_INT(g_hash_table_lookup(errors,
            TEST_ERROR_KEY)) == 2);

	errors = fake_slot_manager_dbus.cb.get_slot_errors(s);
	g_assert(g_hash_table_size(errors) == 1);
	g_assert(GPOINTER_TO_INT(g_hash_table_lookup(errors,
            TEST_SLOT_ERROR_KEY)) == 2);

	/* Enable/disable slots */
	g_assert(m->slots[0]);
	g_assert(!m->slots[1]);
	g_assert(m->slots[0] == s);
	g_assert_cmpstr(s->path, == ,TEST_PATH);
	g_assert_cmpstr(s->imei, == ,TEST_IMEI);
	g_assert_cmpstr(s->imeisv, == ,TEST_IMEISV);
	g_assert_cmpint(s->sim_presence, == ,OFONO_SLOT_SIM_UNKNOWN);
	g_assert(s->enabled);

	slots = gutil_strv_add(NULL, TEST_PATH);
	fake_slot_manager_dbus.cb.set_enabled_slots(m, slots);
	g_assert(s->enabled);
	g_assert_cmpint(sd->slot_property_changed
		[OFONO_SLOT_PROPERTY_ENABLED], == ,0);

	fake_slot_manager_dbus.cb.set_enabled_slots(m, NULL);
	g_assert(!s->enabled);
	g_assert_cmpint(sd->slot_property_changed
		[OFONO_SLOT_PROPERTY_ENABLED], == ,1);

	ofono_slot_set_sim_presence(s, OFONO_SLOT_SIM_PRESENT);
	g_assert_cmpint(s->sim_presence, == ,OFONO_SLOT_SIM_PRESENT);
	g_assert(!s->enabled);
	g_assert_cmpint(sd->slot_property_changed
		[OFONO_SLOT_PROPERTY_ENABLED], == ,1); /* Didn't change */
	g_assert_cmpint(sd->slot_property_changed
		[OFONO_SLOT_PROPERTY_SIM_PRESENCE], == ,1);
	g_strfreev(slots);

	ofono_slot_set_sim_presence(s, OFONO_SLOT_SIM_ABSENT);
	g_assert_cmpint(s->sim_presence, == ,OFONO_SLOT_SIM_ABSENT);
	g_assert_cmpint(sd->slot_property_changed
		[OFONO_SLOT_PROPERTY_SIM_PRESENCE], == ,2);

	ofono_slot_set_sim_presence(s, OFONO_SLOT_SIM_UNKNOWN);
	ofono_slot_set_sim_presence(s, OFONO_SLOT_SIM_UNKNOWN);
	g_assert_cmpint(s->sim_presence, == ,OFONO_SLOT_SIM_UNKNOWN);
	g_assert_cmpint(sd->slot_property_changed
		[OFONO_SLOT_PROPERTY_SIM_PRESENCE], == ,3);

	/* D-Bus interface must be unblocked by now */
	g_assert_cmpuint(fake_slot_manager_dbus.block, ==,
		 SLOT_MANAGER_DBUS_BLOCK_NONE);

	ofono_watch_unref(w);
	g_idle_add(test_done_cb, NULL);
	return G_SOURCE_REMOVE;
}

static unsigned int test_sync_start_start(TestDriverData *dd)
{
	TestSlotData *sd;

	DBG("");
	/* Create the slot */
	sd = test_slot_data_new(dd, TEST_PATH, TEST_IMEI, TEST_IMEISV);
	g_assert(sd);
	g_assert(ofono_slot_ref(sd->slot) == sd->slot);
	ofono_slot_unref(sd->slot);

	/* Can't create a second slot with the same name */
	g_assert(!test_slot_data_new(dd, TEST_PATH, TEST_IMEI, TEST_IMEISV));

	g_idle_add(test_sync_start_done, dd);
	return 0;
}

static void test_sync_start(void)
{
	static const struct ofono_slot_driver test_sync_start_driver = {
		.name = "sync_start",
		.api_version = OFONO_SLOT_API_VERSION,
		.init = test_driver_init,
		.start = test_sync_start_start,
		.cancel = test_driver_cancel_unreachable,
		.cleanup = test_driver_cleanup
	};

	struct ofono_slot_driver_reg *reg;
	TestDriverData *dd;

	test_common_init();
	reg = ofono_slot_driver_register(&test_sync_start_driver);
	g_assert(reg);

	g_main_loop_run(test_loop);

	g_assert_cmpuint(g_slist_length(test_drivers), == ,1);
	dd = test_drivers->data;

	/* Initialization is done, can't add any more slots */
	g_assert(!test_slot_data_new(dd, TEST_PATH, TEST_IMEI, TEST_IMEISV));

	ofono_slot_driver_unregister(reg);
	test_common_deinit();
}

/* ==== async_start ==== */

static void test_async_start_add_slot(TestDriverData *dd)
{
	struct ofono_slot_manager *m = fake_slot_manager_dbus.m;
	TestSlotData *sd;

	/* Create the slot */
	DBG("");
	g_assert(!m->ready);
	g_assert(fake_slot_manager_dbus.block ==
		SLOT_MANAGER_DBUS_BLOCK_ALL);
	sd = test_slot_data_new(dd, TEST_PATH, TEST_IMEI, TEST_IMEISV);
	g_assert(sd);
	g_assert(!m->ready);

	ofono_slot_set_sim_presence(sd->slot, OFONO_SLOT_SIM_ABSENT);
	ofono_slot_driver_started(test_driver_reg);
	g_assert(m->ready);
	ofono_slot_driver_started(test_driver_reg); /* Second one is a nop */
	g_assert(m->ready);

	/* D-Bus interface must be completely unblocked */
	g_assert(fake_slot_manager_dbus.block ==
		SLOT_MANAGER_DBUS_BLOCK_NONE);

	g_idle_add(test_done_cb, NULL);
}

static gboolean test_async_start_wait(gpointer user_data)
{
	TestDriverData *dd = user_data;

	dd->counter--;
	DBG("%d", dd->counter);
	if (dd->counter > 0) {
		return G_SOURCE_CONTINUE;
	} else {
		test_async_start_add_slot(dd);
		return G_SOURCE_REMOVE;
	}
}

static unsigned int test_async_start_start(TestDriverData *dd)
{
	DBG("");
	dd->counter = TEST_IDLE_WAIT_COUNT;
	return g_idle_add(test_async_start_wait, dd);
}

static void test_async_start(void)
{
	static const struct ofono_slot_driver test_async_start_driver = {
		.name = "async_start",
		.api_version = OFONO_SLOT_API_VERSION,
		.init = test_driver_init,
		.start = test_async_start_start,
		.cleanup = test_driver_cleanup
	};

	test_common_init();
	test_driver_reg = ofono_slot_driver_register(&test_async_start_driver);
	g_assert(test_driver_reg);

	g_main_loop_run(test_loop);

	ofono_slot_driver_unregister(test_driver_reg);
	test_driver_reg = NULL;
	test_common_deinit();
}

/* ==== cancel ==== */

static const guint test_cancel_id = 123;

static void test_cancel_driver_cancel(TestDriverData *dd, guint id)
{
	g_assert(id == test_cancel_id);
	g_idle_add(test_done_cb, NULL);
}

static unsigned int test_cancel_driver_start(TestDriverData *dd)
{
	/* Unregistration will cancel start */
	g_idle_add(test_unregister_later, NULL);
	return test_cancel_id;
}

static void test_cancel(void)
{
	static const struct ofono_slot_driver test_cancel_driver = {
		.name = "cancel_start",
		.api_version = OFONO_SLOT_API_VERSION,
		.init = test_driver_init,
		.start = test_cancel_driver_start,
		.cancel = test_cancel_driver_cancel,
		.cleanup = test_driver_cleanup
	};

	test_common_init();
	test_driver_reg = ofono_slot_driver_register(&test_cancel_driver);
	g_assert(test_driver_reg);
	g_main_loop_run(test_loop);
	g_assert(!test_driver_reg);
	test_common_deinit();
}

/* ==== no_cancel ==== */

static void test_no_cancel_driver_cleanup(TestDriverData *dd)
{
	g_idle_add(test_done_cb, NULL);
	test_driver_cleanup(dd);
}

static unsigned int test_no_cancel_driver_start(TestDriverData *dd)
{
	g_idle_add(test_unregister_later, NULL);
	return test_cancel_id;
}

static void test_no_cancel(void)
{
	static const struct ofono_slot_driver test_no_cancel_driver = {
		.name = "cancel_start",
		.api_version = OFONO_SLOT_API_VERSION,
		.init = test_driver_init,
		.start = test_no_cancel_driver_start,
		.cleanup = test_no_cancel_driver_cleanup
	};

	test_common_init();
	test_driver_reg = ofono_slot_driver_register(&test_no_cancel_driver);
	g_assert(test_driver_reg);
	g_main_loop_run(test_loop);
	g_assert(!test_driver_reg);
	test_common_deinit();
}

/* ==== voice_sim ==== */

static gboolean test_voice_sim_done(gpointer user_data)
{
	TestSlotData *sd = user_data;
	struct ofono_slot *s = sd->slot;
	struct ofono_slot_manager *m = fake_slot_manager_dbus.m;
	struct ofono_watch *w = ofono_watch_new(TEST_PATH);
	struct ofono_sim sim;
	gulong id;

	memset(&sim, 0, sizeof(sim));
	sim.mcc = TEST_MCC;
	sim.mnc = TEST_MNC;
	sim.state = OFONO_SIM_STATE_READY;

	/* No default voice modem yet */
	g_assert(m);
	g_assert(!m->default_voice_imsi);
	g_assert(!m->default_voice_path);

	/* Once IMSI is known, default voice modem will point to this slot */
	fake_watch_set_ofono_sim(w, &sim);
	fake_watch_set_ofono_iccid(w, TEST_ICCID);
	fake_watch_set_ofono_imsi(w, TEST_IMSI);
	fake_watch_emit_queued_signals(w);

	g_assert(!m->default_voice_imsi);
	g_assert_cmpstr(m->default_voice_path, == ,TEST_PATH);

	/* Point it to a non-existent SIM, it will still point to the
	 * existing one */
	fake_slot_manager_dbus.cb.set_default_voice_imsi(m, TEST_IMSI_1);
	g_assert_cmpstr(m->default_voice_imsi, == ,TEST_IMSI_1);
	g_assert_cmpstr(m->default_voice_path, == ,TEST_PATH);

	/*
	 * Settings the same IMSI again must have no effect and
	 * produce no signals
	 */
	id = ofono_slot_manager_add_property_handler(m,
		OFONO_SLOT_MANAGER_PROPERTY_ANY,
		test_slot_manager_unreachable_handler, NULL);
	g_assert(id);
	fake_slot_manager_dbus.cb.set_default_voice_imsi(m, TEST_IMSI_1);
	ofono_slot_manager_remove_handler(m, id);

	/* And back to the right SIM */
	fake_slot_manager_dbus.cb.set_default_voice_imsi(m, TEST_IMSI);
	g_assert_cmpstr(m->default_voice_imsi, == ,TEST_IMSI);
	g_assert_cmpstr(m->default_voice_path, == ,TEST_PATH);

	/* Remove the SIM */
	fake_watch_set_ofono_iccid(w, NULL);
	fake_watch_set_ofono_imsi(w, NULL);
	fake_watch_set_ofono_spn(w, NULL);
	ofono_slot_set_sim_presence(s, OFONO_SLOT_SIM_ABSENT);
	fake_watch_emit_queued_signals(w);
	g_assert_cmpint(m->slots[0]->sim_presence, == ,OFONO_SLOT_SIM_ABSENT);
	g_assert_cmpstr(m->default_voice_imsi, == ,TEST_IMSI);
	g_assert(!m->default_voice_path);

	ofono_watch_unref(w);
	g_main_loop_quit(test_loop);
	return G_SOURCE_REMOVE;
}

static unsigned int test_voice_sim_driver_start(TestDriverData *dd)
{
	TestSlotData *sd;

	DBG("");

	/* Create the slot */
	sd = test_slot_data_new(dd, TEST_PATH, TEST_IMEI, TEST_IMEISV);
	g_assert(sd);
	g_idle_add(test_voice_sim_done, sd);
	return 0;
}

static void test_voice_sim(void)
{
	static const struct ofono_slot_driver test_voice_sim_driver = {
		.name = "voice_sim",
		.api_version = OFONO_SLOT_API_VERSION,
		.init = test_driver_init,
		.start = test_voice_sim_driver_start,
		.cancel = test_driver_cancel_unreachable,
		.cleanup = test_driver_cleanup
	};
	struct ofono_slot_driver_reg *reg;

	test_common_init();
	reg = ofono_slot_driver_register(&test_voice_sim_driver);
	g_assert(reg);

	g_main_loop_run(test_loop);

	ofono_slot_driver_unregister(reg);
	test_common_deinit();
}

/* ==== data_sim ==== */

static gboolean test_data_sim_done(gpointer user_data)
{
	TestSlotData *sd = user_data;
	struct ofono_slot *s = sd->slot;
	struct ofono_slot_manager *m = fake_slot_manager_dbus.m;
	struct ofono_watch *w = ofono_watch_new(TEST_PATH);
	struct ofono_modem modem;
	struct ofono_sim sim;

	memset(&modem, 0, sizeof(modem));
	memset(&sim, 0, sizeof(sim));
	sim.mcc = TEST_MCC;
	sim.mnc = TEST_MNC;
	sim.state = OFONO_SIM_STATE_READY;

	/* No default voice or data modems yet */
	g_assert(m);
	g_assert(!m->default_voice_imsi);
	g_assert(!m->default_voice_path);
	g_assert(!m->default_data_imsi);
	g_assert(!m->default_data_path);

	/* Once IMSI is known, default voice modem will point to this slot */
	fake_watch_set_ofono_sim(w, &sim);
	fake_watch_set_ofono_iccid(w, TEST_ICCID);
	fake_watch_set_ofono_imsi(w, TEST_IMSI);
	fake_watch_emit_queued_signals(w);

	g_assert_cmpstr(m->default_voice_path, == ,TEST_PATH);
	g_assert(!m->default_data_path); /* No default data slot */

	/* Set data SIM IMSI */
	fake_slot_manager_dbus.cb.set_default_data_imsi(m, TEST_IMSI);
	g_assert_cmpstr(m->default_data_imsi, == ,TEST_IMSI);
	g_assert(!m->default_data_path); /* Modem is offline */

	/* Set modem online */
	w->modem = &modem;
	w->online = TRUE;
	fake_watch_signal_queue(w, FAKE_WATCH_SIGNAL_ONLINE_CHANGED);
	fake_watch_signal_queue(w, FAKE_WATCH_SIGNAL_MODEM_CHANGED);
	fake_watch_emit_queued_signals(w);
	/* Now is should point to our slot */
	g_assert_cmpstr(m->default_data_path, == ,TEST_PATH);

	/* Point it to a non-existent SIM */
	fake_slot_manager_dbus.cb.set_default_data_imsi(m, TEST_IMSI_1);
	g_assert_cmpstr(m->default_data_imsi, == ,TEST_IMSI_1);
	g_assert(!m->default_data_path);

	/* Switch the SIM */
	fake_watch_set_ofono_imsi(w, TEST_IMSI_1);
	fake_watch_emit_queued_signals(w);
	g_assert_cmpstr(m->default_data_path, == ,TEST_PATH);

	/* Remove the SIM */
	fake_watch_set_ofono_sim(w, NULL);
	fake_watch_emit_queued_signals(w);
	ofono_slot_set_sim_presence(s, OFONO_SLOT_SIM_ABSENT);
	g_assert_cmpint(m->slots[0]->sim_presence, == ,OFONO_SLOT_SIM_ABSENT);
	g_assert_cmpstr(m->default_data_imsi, == ,TEST_IMSI_1);
	g_assert(!m->default_data_path);

	ofono_watch_unref(w);
	g_main_loop_quit(test_loop);
	return G_SOURCE_REMOVE;
}

static unsigned int test_data_sim_start(TestDriverData *dd)
{
	TestSlotData *sd;

	DBG("");

	/* Create the slot */
	sd = test_slot_data_new2(dd, TEST_PATH, TEST_IMEI, TEST_IMEISV,
		OFONO_SLOT_SIM_PRESENT);
	g_assert(sd);
	g_idle_add(test_data_sim_done, sd);
	return 0;
}

static void test_data_sim(void)
{
	static const struct ofono_slot_driver test_data_sim_driver = {
		.name = "data_sim",
		.api_version = OFONO_SLOT_API_VERSION,
		.init = test_driver_init,
		.start = test_data_sim_start,
		.cancel = test_driver_cancel_unreachable,
		.cleanup = test_driver_cleanup
	};
	char *cfg_dir = g_dir_make_tmp(TEST_CONFIG_DIR_TEMPLATE, NULL);
	char *cfg_file = g_build_filename(cfg_dir, "main.conf", NULL);
	GKeyFile* cfg = g_key_file_new();
	struct ofono_slot_driver_reg *reg;

	/* Invalid AutoSelectDataSim option is treated as "off" */
	g_key_file_set_string(cfg, "ModemManager", "AutoSelectDataSim", "x");
	g_assert(g_key_file_save_to_file(cfg, cfg_file, NULL));
	g_key_file_unref(cfg);

	__ofono_set_config_dir(cfg_dir);
	test_common_init();
	reg = ofono_slot_driver_register(&test_data_sim_driver);
	g_assert(reg);

	g_main_loop_run(test_loop);

	ofono_slot_driver_unregister(reg);
	test_common_deinit();

	__ofono_set_config_dir(NULL);
	remove(cfg_file);
	remove(cfg_dir);
	g_free(cfg_file);
	g_free(cfg_dir);
}

/* ==== mms_sim ==== */

static gboolean test_mms_sim_done(gpointer user_data)
{
	TestSlotData *sd = user_data;
	struct ofono_slot *s = sd->slot;
	struct ofono_slot_manager *m = fake_slot_manager_dbus.m;
	struct ofono_watch *w = ofono_watch_new(TEST_PATH);
	struct ofono_modem modem;
	struct ofono_sim sim;

	memset(&modem, 0, sizeof(modem));
	memset(&sim, 0, sizeof(sim));
	sim.mcc = TEST_MCC;
	sim.mnc = TEST_MNC;
	sim.state = OFONO_SIM_STATE_READY;

	/* Nothing yet */
	g_assert(m);
	g_assert(!m->mms_imsi);
	g_assert(!m->mms_path);
	g_assert(!m->default_voice_imsi);
	g_assert(!m->default_voice_path);
	g_assert(!m->default_data_imsi);
	g_assert(!m->default_data_path);

	/* Make the test slot the default data modem */
	w->modem = &modem;
	w->online = TRUE;
	fake_watch_signal_queue(w, FAKE_WATCH_SIGNAL_ONLINE_CHANGED);
	fake_watch_signal_queue(w, FAKE_WATCH_SIGNAL_MODEM_CHANGED);
	fake_watch_set_ofono_sim(w, &sim);
	fake_watch_set_ofono_iccid(w, TEST_ICCID);
	fake_watch_set_ofono_imsi(w, TEST_IMSI);
	fake_watch_emit_queued_signals(w);

	/* Data SIM gets automatically selected on a single-SIM phone */
	g_assert_cmpstr(m->default_voice_path, == ,TEST_PATH);
	g_assert_cmpstr(m->default_data_path, == ,TEST_PATH);

	/* Set data SIM IMSI (second time is a noop */
	fake_slot_manager_dbus.cb.set_default_data_imsi(m, TEST_IMSI);
	g_assert_cmpstr(m->default_data_imsi, == ,TEST_IMSI);
	g_assert_cmpstr(m->default_data_path, == ,TEST_PATH);
	fake_slot_manager_dbus.cb.set_default_data_imsi(m, TEST_IMSI);
	g_assert_cmpstr(m->default_data_imsi, == ,TEST_IMSI);
	g_assert_cmpstr(m->default_data_path, == ,TEST_PATH);
	g_assert_cmpint(s->data_role, == ,OFONO_SLOT_DATA_INTERNET);

	/* Reserve it for MMS */
	g_assert(fake_slot_manager_dbus.cb.set_mms_imsi(m, TEST_IMSI));
	g_assert_cmpint(s->data_role,==,OFONO_SLOT_DATA_INTERNET); /*Not MMS!*/
	g_assert_cmpstr(m->default_data_path, == ,TEST_PATH);
	g_assert_cmpstr(m->mms_imsi, == ,TEST_IMSI);
	g_assert_cmpstr(m->mms_path, == ,TEST_PATH);

	/* Try to point MMS IMSI to a non-existent SIM */
	g_assert(!fake_slot_manager_dbus.cb.set_mms_imsi(m, TEST_IMSI_1));
	g_assert_cmpstr(m->default_data_path, == ,TEST_PATH);
	g_assert_cmpstr(m->mms_imsi, == ,TEST_IMSI);
	g_assert_cmpstr(m->mms_path, == ,TEST_PATH);
	g_assert_cmpint(s->data_role, == ,OFONO_SLOT_DATA_INTERNET);

	/* Reset MMS IMSI */
	g_assert(fake_slot_manager_dbus.cb.set_mms_imsi(m, NULL));
	g_assert(!m->mms_imsi);
	g_assert(!m->mms_path);

	/* Second time is a noop, empty IMSI is the same as NULL */
	g_assert(fake_slot_manager_dbus.cb.set_mms_imsi(m, ""));
	g_assert(!m->mms_imsi);
	g_assert(!m->mms_path);

	ofono_watch_unref(w);
	g_main_loop_quit(test_loop);
	return G_SOURCE_REMOVE;
}

static unsigned int test_mms_sim_start(TestDriverData *dd)
{
	TestSlotData *sd;

	DBG("");

	/* Create the slot */
	sd = test_slot_data_new2(dd, TEST_PATH, TEST_IMEI, TEST_IMEISV,
		OFONO_SLOT_SIM_PRESENT);
	g_assert(sd);
	g_idle_add(test_mms_sim_done, sd);
	return 0;
}

static void test_mms_sim(void)
{
	static const struct ofono_slot_driver test_mms_sim_driver = {
		.name = "mms_sim",
		.api_version = OFONO_SLOT_API_VERSION,
		.init = test_driver_init,
		.start = test_mms_sim_start,
		.cancel = test_driver_cancel_unreachable,
		.cleanup = test_driver_cleanup
	};
	struct ofono_slot_driver_reg *reg;

	test_common_init();
	reg = ofono_slot_driver_register(&test_mms_sim_driver);
	g_assert(reg);

	g_main_loop_run(test_loop);

	ofono_slot_driver_unregister(reg);
	test_common_deinit();
}

/* ==== auto_data_sim ==== */

static gboolean test_auto_data_sim_done(gpointer unused)
{
	struct ofono_slot_manager *m = fake_slot_manager_dbus.m;
	struct ofono_watch *w = ofono_watch_new(TEST_PATH);
	struct ofono_watch *w2 = ofono_watch_new(TEST_PATH_1);
	struct ofono_modem modem;
	struct ofono_sim sim;
	struct ofono_sim sim2;

	memset(&modem, 0, sizeof(modem));
	memset(&sim, 0, sizeof(sim));
	sim.mcc = TEST_MCC;
	sim.mnc = TEST_MNC;
	sim.state = OFONO_SIM_STATE_READY;
	sim2 = sim;

	/* Assign IMSI to the SIMs */
	w->modem = &modem;
	fake_watch_signal_queue(w, FAKE_WATCH_SIGNAL_MODEM_CHANGED);
	fake_watch_set_ofono_sim(w, &sim);
	fake_watch_set_ofono_iccid(w, TEST_ICCID);
	fake_watch_set_ofono_imsi(w, TEST_IMSI);
	fake_watch_emit_queued_signals(w);

	w2->modem = &modem;
	fake_watch_signal_queue(w2, FAKE_WATCH_SIGNAL_MODEM_CHANGED);
	fake_watch_set_ofono_sim(w2, &sim2);
	fake_watch_set_ofono_iccid(w2, TEST_ICCID_1);
	fake_watch_set_ofono_imsi(w2, TEST_IMSI_1);
	fake_watch_emit_queued_signals(w2);

	/* No data SIM yet, only voice SIM is assigned */
	g_assert_cmpint(m->slots[0]->data_role, == ,OFONO_SLOT_DATA_NONE);
	g_assert(!m->default_voice_imsi);
	g_assert_cmpstr(m->default_voice_path, == ,TEST_PATH);
	g_assert(!m->default_data_imsi);
	g_assert(!m->default_data_path);

	/* Set the first modem online */
	w->online = TRUE;
	fake_watch_signal_queue(w, FAKE_WATCH_SIGNAL_ONLINE_CHANGED);
	fake_watch_emit_queued_signals(w);

	/* Now data modem must point to the first slot */
	g_assert_cmpstr(m->default_data_path, == ,TEST_PATH);

	ofono_watch_unref(w);
	ofono_watch_unref(w2);
	g_main_loop_quit(test_loop);
	return G_SOURCE_REMOVE;
}

static guint test_auto_data_sim_start(TestDriverData *dd)
{
	/* Create the slots */
	struct ofono_slot_manager *m = fake_slot_manager_dbus.m;
	struct ofono_slot *s = ofono_slot_add(dd->manager, TEST_PATH,
			OFONO_RADIO_ACCESS_MODE_GSM, TEST_IMEI, TEST_IMEISV,
			OFONO_SLOT_SIM_PRESENT, OFONO_SLOT_NO_FLAGS);
	struct ofono_slot *s2 = ofono_slot_add(dd->manager, TEST_PATH_1,
			OFONO_RADIO_ACCESS_MODE_GSM, TEST_IMEI_1, TEST_IMEISV,
			OFONO_SLOT_SIM_PRESENT, OFONO_SLOT_NO_FLAGS);

	g_assert(s);
	g_assert(s2);
	g_assert(!m->ready);
	ofono_slot_driver_started(test_driver_reg);
	ofono_slot_unref(s);
	ofono_slot_unref(s2);
	g_assert(m->ready);

	g_idle_add(test_auto_data_sim_done, NULL);
	return 0;
}

static void test_auto_data_sim(gconstpointer option)
{
	static const struct ofono_slot_driver test_auto_data_sim_driver = {
		.name = "auto_data_sim",
		.api_version = OFONO_SLOT_API_VERSION,
		.init = test_driver_init,
		.start = test_auto_data_sim_start,
		.cancel = test_driver_cancel_unreachable,
		.cleanup = test_driver_cleanup
	};
	char *cfg_dir = g_dir_make_tmp(TEST_CONFIG_DIR_TEMPLATE, NULL);
	char *cfg_file = g_build_filename(cfg_dir, "main.conf", NULL);
	GKeyFile* cfg = g_key_file_new();

	g_key_file_set_string(cfg, "ModemManager", "AutoSelectDataSim", option);
	g_assert(g_key_file_save_to_file(cfg, cfg_file, NULL));
	g_key_file_unref(cfg);

	__ofono_set_config_dir(cfg_dir);
	test_common_init();
	test_driver_reg = ofono_slot_driver_register
		(&test_auto_data_sim_driver);
	g_assert(test_driver_reg);

	g_main_loop_run(test_loop);

	ofono_slot_driver_unregister(test_driver_reg);
	test_driver_reg = NULL;
	test_common_deinit();

	__ofono_set_config_dir(NULL);
	remove(cfg_file);
	remove(cfg_dir);
	g_free(cfg_file);
	g_free(cfg_dir);
}

/* ==== multisim ==== */

static gboolean test_multisim_done(gpointer user_data)
{
	TestDriverData *dd = user_data;
	TestSlotData *sd = dd->slot_data->data;
	TestSlotData *sd2 = dd->slot_data->next->data;
	struct ofono_slot *s = sd->slot;
	struct ofono_slot *s2 = sd2->slot;
	struct ofono_slot_manager *m = fake_slot_manager_dbus.m;
	struct ofono_watch *w = ofono_watch_new(TEST_PATH);
	struct ofono_watch *w2 = ofono_watch_new(TEST_PATH_1);
	struct ofono_modem modem;
	struct ofono_sim sim;
	struct ofono_sim sim2;

	memset(&modem, 0, sizeof(modem));
	memset(&sim, 0, sizeof(sim));
	sim.mcc = TEST_MCC;
	sim.mnc = TEST_MNC;
	sim.state = OFONO_SIM_STATE_READY;
	sim2 = sim;

	/* Assign IMSI to the SIMs */
	w->modem = &modem;
	w->online = TRUE;
	fake_watch_signal_queue(w, FAKE_WATCH_SIGNAL_ONLINE_CHANGED);
	fake_watch_signal_queue(w, FAKE_WATCH_SIGNAL_MODEM_CHANGED);
	fake_watch_set_ofono_sim(w, &sim);
	fake_watch_set_ofono_iccid(w, TEST_ICCID);
	fake_watch_set_ofono_imsi(w, TEST_IMSI);
	fake_watch_emit_queued_signals(w);

	w2->modem = &modem;
	w2->online = TRUE;
	fake_watch_signal_queue(w2, FAKE_WATCH_SIGNAL_ONLINE_CHANGED);
	fake_watch_signal_queue(w2, FAKE_WATCH_SIGNAL_MODEM_CHANGED);
	fake_watch_set_ofono_sim(w2, &sim2);
	fake_watch_set_ofono_iccid(w2, TEST_ICCID_1);
	fake_watch_set_ofono_imsi(w2, TEST_IMSI_1);
	fake_watch_emit_queued_signals(w2);

	/* No automatic data SIM selection on a multisim phone */
	g_assert_cmpint(s->data_role, == ,OFONO_SLOT_DATA_NONE);
	g_assert(!m->default_voice_imsi);
	g_assert_cmpstr(m->default_voice_path, == ,TEST_PATH);
	g_assert(!m->default_data_imsi);
	g_assert(!m->default_data_path);

	/* But there is automatic voice SIM selection */
	g_assert(!m->default_voice_imsi);
	g_assert_cmpstr(m->default_voice_path, == ,TEST_PATH);

	/* Switch the voice SIM back and forth */
	fake_slot_manager_dbus.cb.set_default_voice_imsi(m, TEST_IMSI);
	g_assert_cmpstr(m->default_voice_imsi, == ,TEST_IMSI);
	g_assert_cmpstr(m->default_voice_path, == ,TEST_PATH);

	fake_slot_manager_dbus.cb.set_default_voice_imsi(m, TEST_IMSI_1);
	g_assert_cmpstr(m->default_voice_imsi, == ,TEST_IMSI_1);
	g_assert_cmpstr(m->default_voice_path, == ,TEST_PATH_1);

	/* test_1 remains the current voice slot */
	fake_slot_manager_dbus.cb.set_default_voice_imsi(m, NULL);
	g_assert(!m->default_voice_imsi);
	g_assert_cmpstr(m->default_voice_path, == ,TEST_PATH_1);

	/* Reserve the first slot for data */
	fake_slot_manager_dbus.cb.set_default_data_imsi(m, TEST_IMSI);
	g_assert_cmpint(s->data_role, == ,OFONO_SLOT_DATA_INTERNET);
	g_assert_cmpstr(m->default_data_imsi, == ,TEST_IMSI);
	g_assert_cmpstr(m->default_data_path, == ,TEST_PATH);

	/* Second slot for MMS */
	g_assert(fake_slot_manager_dbus.cb.set_mms_imsi(m, TEST_IMSI_1));
	g_assert_cmpint(s->data_role, == ,OFONO_SLOT_DATA_NONE);
	g_assert_cmpint(s2->data_role, == ,OFONO_SLOT_DATA_MMS);
	g_assert_cmpstr(m->mms_path, == ,TEST_PATH_1);
	g_assert_cmpstr(m->mms_imsi, == ,TEST_IMSI_1);
	g_assert_cmpstr(m->default_data_imsi, == ,TEST_IMSI);
	g_assert(!m->default_data_path);

	/* Cancel MMS reservation */
	g_assert(fake_slot_manager_dbus.cb.set_mms_imsi(m, NULL));
	g_assert_cmpint(s->data_role, == ,OFONO_SLOT_DATA_INTERNET);
	g_assert_cmpint(s2->data_role, == ,OFONO_SLOT_DATA_NONE);
	g_assert_cmpstr(m->default_data_imsi, == ,TEST_IMSI);
	g_assert_cmpstr(m->default_data_path, == ,TEST_PATH);
	g_assert(!m->mms_path);
	g_assert(!m->mms_imsi);

	ofono_watch_unref(w);
	ofono_watch_unref(w2);
	g_main_loop_quit(test_loop);
	return G_SOURCE_REMOVE;
}

static gboolean test_multisim_add_slots(gpointer user_data)
{
	TestDriverData *dd = user_data;
	struct ofono_slot_manager *m = fake_slot_manager_dbus.m;

	DBG("");

	/* Create the slots */
	g_assert(test_slot_data_new2(dd, TEST_PATH, TEST_IMEI, TEST_IMEISV,
		OFONO_SLOT_SIM_PRESENT));
	g_assert(test_slot_data_new2(dd, TEST_PATH_1, TEST_IMEI_1, TEST_IMEISV,
		OFONO_SLOT_SIM_PRESENT));

	g_assert(!m->ready);
	ofono_slot_driver_started(test_driver_reg);
	g_assert(m->ready);

	g_idle_add(test_multisim_done, dd);
	return G_SOURCE_REMOVE;
}

static guint test_multisim_start(TestDriverData *dd)
{
	return g_idle_add(test_multisim_add_slots, dd);
}

static void test_multisim(void)
{
	static const struct ofono_slot_driver test_multisim_driver = {
		.name = "multisim",
		.api_version = OFONO_SLOT_API_VERSION,
		.init = test_driver_init,
		.start = test_multisim_start,
		.cancel = test_driver_cancel_source,
		.cleanup = test_driver_cleanup
	};

	test_common_init();
	test_driver_reg = ofono_slot_driver_register(&test_multisim_driver);
	g_assert(test_driver_reg);

	g_main_loop_run(test_loop);

	ofono_slot_driver_unregister(test_driver_reg);
	test_driver_reg = NULL;
	test_common_deinit();
}

/* ==== storage ==== */

static void test_storage_init()
{
	struct ofono_watch *w = ofono_watch_new(TEST_PATH);
	struct ofono_watch *w2 = ofono_watch_new(TEST_PATH_1);
	struct ofono_sim sim;
	struct ofono_sim sim2;

	memset(&sim, 0, sizeof(sim));
	sim.mcc = TEST_MCC;
	sim.mnc = TEST_MNC;
	sim.state = OFONO_SIM_STATE_READY;
	sim2 = sim;

	/* Assign IMSI to the SIMs */
	fake_watch_set_ofono_sim(w, &sim);
	fake_watch_set_ofono_iccid(w, TEST_ICCID);
	fake_watch_set_ofono_imsi(w, TEST_IMSI);
	fake_watch_emit_queued_signals(w);

	fake_watch_set_ofono_sim(w2, &sim2);
	fake_watch_set_ofono_iccid(w2, TEST_ICCID_1);
	fake_watch_set_ofono_imsi(w2, TEST_IMSI_1);
	fake_watch_emit_queued_signals(w2);

	ofono_watch_unref(w);
	ofono_watch_unref(w2);
}

static void test_storage_add_slots(TestDriverData *dd)
{
	/* Create the slots */
	struct ofono_slot_manager *m = fake_slot_manager_dbus.m;
	struct ofono_slot *s = ofono_slot_add(dd->manager, TEST_PATH,
		OFONO_RADIO_ACCESS_MODE_GSM, TEST_IMEI, TEST_IMEISV,
		OFONO_SLOT_SIM_PRESENT, OFONO_SLOT_NO_FLAGS);
	struct ofono_slot *s2 = ofono_slot_add(dd->manager, TEST_PATH_1,
		OFONO_RADIO_ACCESS_MODE_GSM, TEST_IMEI_1, TEST_IMEISV,
		OFONO_SLOT_SIM_PRESENT, OFONO_SLOT_NO_FLAGS);

	g_assert(s);
	g_assert(s2);
	g_assert(!m->ready);
	ofono_slot_driver_started(test_driver_reg);
	ofono_slot_unref(s);
	ofono_slot_unref(s2);
	g_assert(m->ready);
}

static gboolean test_storage_save_add_slots(gpointer user_data)
{
	TestDriverData *dd = user_data;
	struct ofono_slot_manager *m = fake_slot_manager_dbus.m;
	char **slots = gutil_strv_add(NULL, TEST_PATH);

	DBG("");

	/* Unblocking D-Bus clients will exit the loop */
	fake_slot_manager_dbus.fn_block_changed =
		test_quit_loop_when_unblocked;
	
	test_storage_add_slots(dd);

	fake_slot_manager_dbus.cb.set_enabled_slots(m, slots);
	g_assert(m->slots[0]->enabled);
	g_assert(!m->slots[1]->enabled);
	g_strfreev(slots);

	test_storage_init();
	return G_SOURCE_REMOVE;
}

static gboolean test_storage_restore_add_slots(gpointer user_data)
{
	TestDriverData *dd = user_data;
	struct ofono_slot_manager *m = fake_slot_manager_dbus.m;

	DBG("");

	/* Unblocking D-Bus clients will exit the loop */
	fake_slot_manager_dbus.fn_block_changed =
		test_quit_loop_when_unblocked;

	test_storage_add_slots(dd);

	/* These should get restored from the file */
	g_assert(m->slots[0]->enabled);
	g_assert(!m->slots[1]->enabled);
	return G_SOURCE_REMOVE;
}

static guint test_storage_save_start(TestDriverData *dd)
{
	return g_idle_add(test_storage_save_add_slots, dd);
}

static guint test_storage_restore_start(TestDriverData *dd)
{
	return g_idle_add(test_storage_restore_add_slots, dd);
}

static void test_storage(void)
{
	static const struct ofono_slot_driver test_storage_save_driver = {
		.name = "storage_save",
		.api_version = OFONO_SLOT_API_VERSION,
		.init = test_driver_init,
		.start = test_storage_save_start,
		.cleanup = test_driver_cleanup
	};

	static const struct ofono_slot_driver test_storage_restore_driver = {
		.name = "storage_restore",
		.api_version = OFONO_SLOT_API_VERSION,
		.init = test_driver_init,
		.start = test_storage_restore_start,
		.cancel = test_driver_cancel_source,
		.cleanup = test_driver_cleanup
	};

	test_common_init();

	test_driver_reg = ofono_slot_driver_register(&test_storage_save_driver);
	g_assert(test_driver_reg);
	g_main_loop_run(test_loop);
	g_assert(test_timeout_id);

	/* Reinitialize everything */
	__ofono_slot_manager_cleanup();
	__ofono_slot_manager_init();

	/* And restore settings from the file */
	test_driver_reg = ofono_slot_driver_register
		(&test_storage_restore_driver);
	g_assert(test_driver_reg);
	g_main_loop_run(test_loop);

	ofono_slot_driver_unregister(test_driver_reg);
	test_driver_reg = NULL;
	test_common_deinit();
}

#define TEST_(name) "/slot_manager/" name

int main(int argc, char *argv[])
{
	g_test_init(&argc, &argv, NULL);

	gutil_log_timestamp = FALSE;
	gutil_log_default.level = g_test_verbose() ?
		GLOG_LEVEL_VERBOSE : GLOG_LEVEL_NONE;
	__ofono_log_init("test-slot-manager",
				g_test_verbose() ? "*" : NULL,
				FALSE, FALSE);

	g_test_add_func(TEST_("basic"), test_basic);
	g_test_add_func(TEST_("early_init"), test_early_init);
	g_test_add_func(TEST_("too_late"), test_too_late);
	g_test_add_func(TEST_("create_fail"), test_create_fail);
	g_test_add_func(TEST_("no_drivers"), test_no_drivers);
	g_test_add_func(TEST_("no_slots"), test_no_slots);
	g_test_add_func(TEST_("sync_start"), test_sync_start);
	g_test_add_func(TEST_("async_start"), test_async_start);
	g_test_add_func(TEST_("cancel"), test_cancel);
	g_test_add_func(TEST_("no_cancel"), test_no_cancel);
	g_test_add_func(TEST_("voice_sim"), test_voice_sim);
	g_test_add_func(TEST_("data_sim"), test_data_sim);
	g_test_add_func(TEST_("mms_sim"), test_mms_sim);
	g_test_add_data_func(TEST_("auto_data_sim_on"), "on",
						test_auto_data_sim);
	g_test_add_data_func(TEST_("auto_data_sim_always"), "always",
						test_auto_data_sim);
	g_test_add_data_func(TEST_("auto_data_sim_once"), "once",
						test_auto_data_sim);
	g_test_add_func(TEST_("multisim"), test_multisim);
	g_test_add_func(TEST_("storage"), test_storage);
	return g_test_run();
}

/*
 * Local Variables:
 * mode: C
 * c-basic-offset: 8
 * indent-tabs-mode: t
 * End:
 */
