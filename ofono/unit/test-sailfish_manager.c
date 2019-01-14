/*
 *  oFono - Open Source Telephony
 *
 *  Copyright (C) 2017-2019 Jolla Ltd.
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

#include <sailfish_manager.h>
#include <sailfish_cell_info.h>

#include "sailfish_sim_info.h"
#include "sailfish_manager_dbus.h"
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

extern struct ofono_plugin_desc __ofono_builtin_sailfish_manager;
static GMainLoop *test_loop = NULL;
static guint test_timeout_id = 0;

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
				ofono_netreg_status_notify_cb_t notify,
				void *data, ofono_destroy_func destroy)
{
	return 1;
}

gboolean __ofono_netreg_remove_status_watch(struct ofono_netreg *netreg,
						unsigned int id)
{
	return TRUE;
}

/* Fake sailfish_manager_dbus */

static struct sailfish_manager_dbus {
	struct sailfish_manager *m;
	struct sailfish_manager_dbus_cb cb;
	enum sailfish_manager_dbus_block block;
	void (*fn_block_changed)(struct sailfish_manager_dbus *d);
	void (*fn_signal)(struct sailfish_manager_dbus *d,
					enum sailfish_manager_dbus_signal m);
	int signals;
} fake_sailfish_manager_dbus;

struct sailfish_manager_dbus *sailfish_manager_dbus_new
		(struct sailfish_manager *m,
			const struct sailfish_manager_dbus_cb *cb)
{
	memset(&fake_sailfish_manager_dbus, 0,
		sizeof(fake_sailfish_manager_dbus));
	fake_sailfish_manager_dbus.m = m;
	fake_sailfish_manager_dbus.cb = *cb;
	return &fake_sailfish_manager_dbus;
}

void sailfish_manager_dbus_free(struct sailfish_manager_dbus *d)
{
	g_assert(d == &fake_sailfish_manager_dbus);
	g_assert(fake_sailfish_manager_dbus.m);
	memset(&fake_sailfish_manager_dbus, 0,
		sizeof(fake_sailfish_manager_dbus));
}

void sailfish_manager_dbus_set_block(struct sailfish_manager_dbus *d,
				enum sailfish_manager_dbus_block b)
{
	if (d->block != b) {
		DBG("0x%02x", (int)b);
		d->block = b;
		if (d->fn_block_changed) {
			d->fn_block_changed(d);
		}
	}
}
void sailfish_manager_dbus_signal(struct sailfish_manager_dbus *d,
				enum sailfish_manager_dbus_signal m)
{
	d->signals |= m;
	if (d->fn_signal) {
		d->fn_signal(d, m);
	}
}

void sailfish_manager_dbus_signal_sim(struct sailfish_manager_dbus *d,
				int index, gboolean present) {}
void sailfish_manager_dbus_signal_error(struct sailfish_manager_dbus *d,
				const char *id, const char *message) {}
void sailfish_manager_dbus_signal_modem_error(struct sailfish_manager_dbus *d,
				int index, const char *id, const char *msg) {}

/* Fake sailfish_sim_info */

struct sailfish_sim_info_dbus {
	int unused;
};

struct sailfish_sim_info_dbus *sailfish_sim_info_dbus_new
					(struct sailfish_sim_info *info)
{
	static struct sailfish_sim_info_dbus fake_sailfish_sim_info_dbus;
	return &fake_sailfish_sim_info_dbus;
}

void sailfish_sim_info_dbus_free(struct sailfish_sim_info_dbus *dbus) {}

/* Fake sailfish_cell_info */

static int fake_sailfish_cell_info_ref_count = 0;

static void fake_sailfish_cell_info_ref(struct sailfish_cell_info *info)
{
	g_assert(fake_sailfish_cell_info_ref_count >= 0);
	fake_sailfish_cell_info_ref_count++;
}

static void fake_sailfish_cell_info_unref(struct sailfish_cell_info *info)
{
	g_assert(fake_sailfish_cell_info_ref_count > 0);
	fake_sailfish_cell_info_ref_count--;
}

static gulong fake_sailfish_cell_info_add_cells_changed_handler
	(struct sailfish_cell_info *info, sailfish_cell_info_cb_t cb, void *arg)
{
	return 1;
}

static void fake_sailfish_cell_info_remove_handler
				(struct sailfish_cell_info *info, gulong id)
{
	g_assert(id == 1);
}

static const struct sailfish_cell_info_proc fake_sailfish_cell_info_proc = {
	fake_sailfish_cell_info_ref,
	fake_sailfish_cell_info_unref,
	fake_sailfish_cell_info_add_cells_changed_handler,
	fake_sailfish_cell_info_remove_handler
};

static struct sailfish_cell_info fake_sailfish_cell_info = {
	&fake_sailfish_cell_info_proc,
	NULL
};

/* Fake sailfish_cell_info_dbus */

struct sailfish_cell_info_dbus {
	int unused;
};

struct sailfish_cell_info_dbus *sailfish_cell_info_dbus_new
		(struct ofono_modem *modem, struct sailfish_cell_info *info)
{
	static struct sailfish_cell_info_dbus fake_sailfish_cell_info_dbus;
	return &fake_sailfish_cell_info_dbus;
}

void sailfish_cell_info_dbus_free(struct sailfish_cell_info_dbus *dbus) {}

/* Code shared by all tests */

typedef struct sailfish_slot_impl {
	struct sailfish_slot *handle;
	enum sailfish_data_role data_role;
	int enabled_changed;
} test_slot;

typedef struct sailfish_slot_manager_impl {
	struct sailfish_slot_manager *handle;
	test_slot *slot;
	test_slot *slot2;
	int counter;
} test_slot_manager;

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

static gboolean test_timeout_cb(gpointer user_data)
{
	ofono_error("Timeout!");
	g_main_loop_quit(test_loop);
	test_timeout_id = 0;

	return G_SOURCE_REMOVE;
}

static void test_quit_loop_when_unblocked(struct sailfish_manager_dbus *d)
{
	if (d->block == SAILFISH_MANAGER_DBUS_BLOCK_NONE) {
		g_main_loop_quit(test_loop);
	}
}

static void test_common_init()
{
	rmdir_r(STORAGEDIR);
	__ofono_builtin_sailfish_manager.init();
	test_loop = g_main_loop_new(NULL, FALSE);
	test_timeout_id = g_timeout_add_seconds(TEST_TIMEOUT_SEC,
						test_timeout_cb, NULL);
}

static void test_common_deinit()
{
	__ofono_builtin_sailfish_manager.exit();
	g_assert(test_timeout_id);
	g_source_remove(test_timeout_id);
	g_main_loop_unref(test_loop);
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

static test_slot_manager *test_slot_manager_create
					(struct sailfish_slot_manager *handle)
{
	test_slot_manager *sm = g_new0(test_slot_manager, 1);

	DBG("");
	sm->handle = handle;
	return sm;
}

static void test_slot_manager_free(test_slot_manager *sm)
{
	g_free(sm);
}

static void test_slot_enabled_changed(test_slot *s)
{
	s->enabled_changed++;
}

static void test_slot_set_data_role(test_slot *s, enum sailfish_data_role role)
{
	s->data_role = role;
}

static void test_slot_free(test_slot *s)
{
	g_free(s);
}

static void test_slot_manager_count_cb(test_slot_manager *sm, void *user_data)
{
	(*((int *)user_data))++;
}

/* Test cases */

/* ==== basic ==== */

static void test_basic(void)
{
	static const struct sailfish_slot_driver dummy1 = {
		.name = "Dummy1",
		.priority = 1
	};
	static const struct sailfish_slot_driver dummy2 = { .name = "Dummy2" };
	static const struct sailfish_slot_driver dummy3 = { .name = "Dummy3" };
	static const struct sailfish_slot_driver dummy4 = { .name = "Dummy4" };
	struct sailfish_slot_driver_reg *r1, *r2, *r3, *r4;
	int count = 0;

	test_common_init();

	/* NULL resistance */
	g_assert(!sailfish_slot_driver_register(NULL));
	sailfish_slot_driver_unregister(NULL);
	sailfish_manager_foreach_slot_manager(NULL, NULL, NULL);
	sailfish_manager_imei_obtained(NULL, NULL);
	sailfish_manager_imeisv_obtained(NULL, NULL);
	sailfish_manager_set_cell_info(NULL, NULL);
	sailfish_manager_set_sim_state(NULL, SAILFISH_SIM_STATE_UNKNOWN);
	sailfish_manager_slot_error(NULL, NULL, NULL);
	sailfish_manager_error(NULL, NULL, NULL);

	/* Register dummy driver */
	g_assert((r2 = sailfish_slot_driver_register(&dummy2)));
	g_assert((r1 = sailfish_slot_driver_register(&dummy1)));
	g_assert((r4 = sailfish_slot_driver_register(&dummy4)));
	g_assert((r3 = sailfish_slot_driver_register(&dummy3)));
	sailfish_manager_foreach_slot_manager(r1, NULL, &count);
	g_assert(!count);
	sailfish_manager_foreach_slot_manager(r1,
					test_slot_manager_count_cb, &count);
	g_assert(!count);

	/* Run the main loop to make sure that sailfish_manager handles
	 * drivers without manager_start callback (even though it makes
	 * little or no sense). */
	count = 1;
	g_idle_add(test_done_when_zero, &count);
	g_main_loop_run(test_loop);

	sailfish_slot_driver_unregister(r3);
	sailfish_slot_driver_unregister(r4);
	sailfish_slot_driver_unregister(r2);
	sailfish_slot_driver_unregister(r1);

	/* This one will get destroyed by sailfish_manager_exit */
	g_assert(sailfish_slot_driver_register(&dummy1));
	test_common_deinit();

	/* Double exit is fine */
	__ofono_builtin_sailfish_manager.exit();
}

/* ==== early_init ==== */

static guint test_early_init_start(test_slot_manager *sm)
{
	DBG("");
	g_main_loop_quit(test_loop);
	return 0;
}

static void test_early_init(void)
{
	static const struct sailfish_slot_driver early_init_driver = {
		.name = "early_init",
		.manager_create = test_slot_manager_create,
		.manager_start = test_early_init_start,
		.manager_free = test_slot_manager_free
	};
	struct sailfish_slot_driver_reg *reg;
	int count = 0;

	/* Register before sailfish_manager_init */
	g_assert((reg = sailfish_slot_driver_register(&early_init_driver)));

	test_common_init();
	g_main_loop_run(test_loop);
	sailfish_manager_foreach_slot_manager(reg,
					test_slot_manager_count_cb, &count);
	g_assert(count == 1);
	test_common_deinit();
}

/* ==== too_late ==== */

static gboolean test_too_late_cb(gpointer user_data)
{
	guint* counter = user_data;

	(*counter)--;
	DBG("%u", *counter);
	if (!(*counter)) {
		static const struct sailfish_slot_driver too_late_driver = {
			.name = "too_late",
			.manager_create = test_slot_manager_create,
		};

		g_assert(!sailfish_slot_driver_register(&too_late_driver));
		g_assert(fake_sailfish_manager_dbus.block ==
			 SAILFISH_MANAGER_DBUS_BLOCK_NONE);
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

static test_slot_manager *test_create_fail_manager_create
					(struct sailfish_slot_manager *m)
{
	DBG("");
	g_main_loop_quit(test_loop);
	return NULL;
}

static void test_create_fail(void)
{
	static const struct sailfish_slot_driver create_fail_driver = {
		.name = "create_fail",
		.manager_create = test_create_fail_manager_create,
	};
	struct sailfish_slot_driver_reg *reg;
	int count = 0;

	test_common_init();
	g_assert((reg = sailfish_slot_driver_register(&create_fail_driver)));
	g_main_loop_run(test_loop);
	sailfish_manager_foreach_slot_manager(reg,
					test_slot_manager_count_cb, &count);
	g_assert(!count);
	test_common_deinit();
}

/* ==== no_plugins ==== */

static void test_quit_when_ready(struct sailfish_manager_dbus *d,
					enum sailfish_manager_dbus_signal m)
{
	DBG("%d", m);
	if (d->m->ready) {
		DBG("Ready!");
		g_main_loop_quit(test_loop);
	}
}

static void test_no_plugins(void)
{
	test_common_init();
	fake_sailfish_manager_dbus.fn_signal = test_quit_when_ready;
	g_main_loop_run(test_loop);
	test_common_deinit();
}

/* ==== no_manager ==== */

static void test_no_manager(void)
{
	static const struct sailfish_slot_driver no_manager_driver = {
		.name = "no_manager",
	};

	test_common_init();
	g_assert(sailfish_slot_driver_register(&no_manager_driver));
	fake_sailfish_manager_dbus.fn_signal = test_quit_when_ready;
	g_main_loop_run(test_loop);
	g_assert(fake_sailfish_manager_dbus.m->ready);

	test_common_deinit();
}

/* ==== no_slots ==== */

static guint test_no_slots_start(test_slot_manager *sm)
{
	DBG("");
	g_main_loop_quit(test_loop);
	return 0;
}

static void test_no_slots(void)
{
	static const struct sailfish_slot_driver no_slots_driver = {
		.name = "no_slots",
		.manager_create = test_slot_manager_create,
		.manager_start = test_no_slots_start,
		.manager_free = test_slot_manager_free
	};

	test_common_init();
	g_assert(sailfish_slot_driver_register(&no_slots_driver));
	g_main_loop_run(test_loop);
	g_assert(fake_sailfish_manager_dbus.m);
	g_assert(fake_sailfish_manager_dbus.m->ready);

	test_common_deinit();
}

/* ==== sync_start ==== */

static gboolean test_sync_start_done(gpointer user_data)
{
	test_slot_manager *sm = user_data;
	test_slot *s = sm->slot;
	struct ofono_watch *w = ofono_watch_new(TEST_PATH);
	struct sailfish_manager *m = fake_sailfish_manager_dbus.m;
	struct ofono_modem modem;
	char **slots;
	GHashTable *errors;

	g_assert(m);

	/* Poke cell info API */
	sailfish_manager_set_cell_info(s->handle, NULL);
	sailfish_manager_set_cell_info(s->handle, &fake_sailfish_cell_info);

	memset(&modem, 0, sizeof(modem));
	w->modem = &modem;
	w->online = TRUE;
	fake_watch_signal_queue(w, FAKE_WATCH_SIGNAL_ONLINE_CHANGED);
	fake_watch_signal_queue(w, FAKE_WATCH_SIGNAL_MODEM_CHANGED);
	fake_watch_emit_queued_signals(w);

	sailfish_manager_set_cell_info(s->handle, NULL);
	sailfish_manager_set_cell_info(s->handle, &fake_sailfish_cell_info);

	w->modem = NULL;
	w->online = FALSE;
	fake_watch_signal_queue(w, FAKE_WATCH_SIGNAL_ONLINE_CHANGED);
	fake_watch_signal_queue(w, FAKE_WATCH_SIGNAL_MODEM_CHANGED);
	fake_watch_emit_queued_signals(w);

	sailfish_manager_set_cell_info(s->handle, NULL);
	g_assert(!fake_sailfish_cell_info_ref_count);

	/* Poke error counters */
	sailfish_manager_error(sm->handle, TEST_ERROR_KEY, "Aaah!");
	sailfish_manager_slot_error(s->handle, TEST_SLOT_ERROR_KEY, "Aaah!");

	errors = fake_sailfish_manager_dbus.cb.get_errors(m);
	g_assert(g_hash_table_size(errors) == 1);
	g_assert(GPOINTER_TO_INT(g_hash_table_lookup(errors,
            TEST_ERROR_KEY)) == 1);

	errors = fake_sailfish_manager_dbus.cb.get_slot_errors(s->handle);
	g_assert(g_hash_table_size(errors) == 1);
	g_assert(GPOINTER_TO_INT(g_hash_table_lookup(errors,
            TEST_SLOT_ERROR_KEY)) == 1);

	sailfish_manager_error(sm->handle, TEST_ERROR_KEY, "Aaah!");
	sailfish_manager_slot_error(s->handle, TEST_SLOT_ERROR_KEY, "Aaah!");

	errors = fake_sailfish_manager_dbus.cb.
		get_errors(fake_sailfish_manager_dbus.m);
	g_assert(g_hash_table_size(errors) == 1);
	g_assert(GPOINTER_TO_INT(g_hash_table_lookup(errors,
            TEST_ERROR_KEY)) == 2);

	errors = fake_sailfish_manager_dbus.cb.get_slot_errors(s->handle);
	g_assert(g_hash_table_size(errors) == 1);
	g_assert(GPOINTER_TO_INT(g_hash_table_lookup(errors,
            TEST_SLOT_ERROR_KEY)) == 2);

	/* Enable/disable slots */
	g_assert(m->slots[0]);
	g_assert(!g_strcmp0(m->slots[0]->path, TEST_PATH));
	g_assert(!g_strcmp0(m->slots[0]->imei, TEST_IMEI));
	g_assert(!g_strcmp0(m->slots[0]->imeisv, TEST_IMEISV));
	g_assert(!m->slots[0]->sim_present);
	g_assert(m->slots[0]->enabled);
	g_assert(!m->slots[1]);

	slots = gutil_strv_add(NULL, TEST_PATH);
	fake_sailfish_manager_dbus.cb.set_enabled_slots(m, slots);
	g_assert(m->slots[0]->enabled);
	g_assert(!s->enabled_changed);

	fake_sailfish_manager_dbus.cb.set_enabled_slots(m, NULL);
	g_assert(!m->slots[0]->enabled);
	g_assert(s->enabled_changed == 1);
	s->enabled_changed = 0;

	sailfish_manager_set_sim_state(s->handle, SAILFISH_SIM_STATE_PRESENT);
	fake_sailfish_manager_dbus.cb.set_enabled_slots(m, slots);
	g_assert(m->slots[0]->sim_present);
	g_assert(m->slots[0]->enabled);
	g_assert(s->enabled_changed == 1);
	s->enabled_changed = 0;
	g_strfreev(slots);

	sailfish_manager_set_sim_state(s->handle, SAILFISH_SIM_STATE_ABSENT);
	g_assert(!m->slots[0]->sim_present);
	sailfish_manager_set_sim_state(s->handle, SAILFISH_SIM_STATE_ERROR);
	sailfish_manager_set_sim_state(s->handle, SAILFISH_SIM_STATE_ERROR);
	g_assert(!m->slots[0]->sim_present);

	/* D-Bus interface is still blocked, wait for it to get unblocked */
	g_assert(fake_sailfish_manager_dbus.block ==
		 SAILFISH_MANAGER_DBUS_BLOCK_ALL);
	fake_sailfish_manager_dbus.fn_block_changed =
		test_quit_loop_when_unblocked;

	ofono_watch_unref(w);
	return G_SOURCE_REMOVE;
}

static guint test_sync_start_start(test_slot_manager *sm)
{
	test_slot *slot = g_new0(test_slot, 1);

	DBG("");

	/* Create the slot */
	slot->handle = sailfish_manager_slot_add(sm->handle, slot, TEST_PATH,
			OFONO_RADIO_ACCESS_MODE_GSM, NULL, NULL,
			SAILFISH_SIM_STATE_UNKNOWN);
	sailfish_manager_imei_obtained(slot->handle, TEST_IMEI);
	sailfish_manager_imeisv_obtained(slot->handle, TEST_IMEISV);

	sm->slot = slot;
	g_idle_add(test_sync_start_done, sm);
	return 0;
}

static void test_sync_start_slot_manager_cb(test_slot_manager *sm, void *data)
{
	/* Initialization is done, can't add any more slots */
	g_assert(!sailfish_manager_slot_add(sm->handle, NULL, TEST_PATH,
			OFONO_RADIO_ACCESS_MODE_GSM, NULL, NULL,
			SAILFISH_SIM_STATE_UNKNOWN));
}

static void test_sync_start(void)
{
	static const struct sailfish_slot_driver test_sync_start_driver = {
		.name = "sync_start",
		.manager_create = test_slot_manager_create,
		.manager_start = test_sync_start_start,
		.manager_free = test_slot_manager_free,
		.slot_enabled_changed = test_slot_enabled_changed,
		.slot_free = test_slot_free
	};

	struct sailfish_slot_driver_reg *reg;

	test_common_init();
	reg = sailfish_slot_driver_register(&test_sync_start_driver);
	g_assert(reg);

	g_main_loop_run(test_loop);

	sailfish_manager_foreach_slot_manager(reg, NULL, NULL); /* nop */
	sailfish_manager_foreach_slot_manager(reg,
				test_sync_start_slot_manager_cb, NULL);
	sailfish_slot_driver_unregister(reg);
	test_common_deinit();
}

/* ==== async_start ==== */

static void test_async_start_add_slot(test_slot_manager *sm)
{
	struct sailfish_manager *m = fake_sailfish_manager_dbus.m;
	test_slot *s = g_new0(test_slot, 1);

	/* Create the slot */
	DBG("");

	g_assert(fake_sailfish_manager_dbus.block ==
		SAILFISH_MANAGER_DBUS_BLOCK_ALL);

	s->handle = sailfish_manager_slot_add(sm->handle, s, TEST_PATH,
			OFONO_RADIO_ACCESS_MODE_GSM, NULL, NULL,
			SAILFISH_SIM_STATE_UNKNOWN);
	sm->slot = s;

	g_assert(!m->ready);
	sailfish_manager_set_sim_state(s->handle, SAILFISH_SIM_STATE_ABSENT);
	sailfish_slot_manager_started(sm->handle);
	sailfish_slot_manager_started(sm->handle); /* Second one is a nop */

	/* D-Bus interface is still blocked because IMEI is not yet known */
	g_assert(fake_sailfish_manager_dbus.block ==
		SAILFISH_MANAGER_DBUS_BLOCK_IMEI);

	g_assert(!m->ready);
	sailfish_manager_imei_obtained(s->handle, TEST_IMEI);
	sailfish_manager_imeisv_obtained(s->handle, TEST_IMEISV);
	g_assert(m->ready);

	/* Now D-Bus interface is completely unblocked */
	g_assert(fake_sailfish_manager_dbus.block ==
		SAILFISH_MANAGER_DBUS_BLOCK_NONE);

	g_idle_add(test_done_cb, NULL);
}

static gboolean test_async_start_wait(gpointer user_data)
{
	test_slot_manager *sm = user_data;

	sm->counter--;
	if (sm->counter > 0) {
		return G_SOURCE_CONTINUE;
	} else {
		test_async_start_add_slot(sm);
		return G_SOURCE_REMOVE;
	}
}

static guint test_async_start_start(test_slot_manager *sm)
{
	sm->counter = TEST_IDLE_WAIT_COUNT;
	return g_idle_add(test_async_start_wait, sm);
}

static void test_async_start(void)
{
	static const struct sailfish_slot_driver test_async_start_driver = {
		.name = "async_start",
		.manager_create = test_slot_manager_create,
		.manager_start = test_async_start_start,
		.manager_free = test_slot_manager_free,
		.slot_free = test_slot_free
	};
	struct sailfish_slot_driver_reg *reg;

	test_common_init();
	reg = sailfish_slot_driver_register(&test_async_start_driver);
	g_assert(reg);

	g_main_loop_run(test_loop);

	sailfish_slot_driver_unregister(reg);
	test_common_deinit();
}

/* ==== cancel_start ==== */

static gboolean test_cancel_ok;
static guint test_cancel_id = 123;

static void test_cancel_start_cancel(test_slot_manager *sm, guint id)
{
	g_assert(id == test_cancel_id);
	test_cancel_ok = TRUE;
}

static guint test_cancel_start_start(test_slot_manager *sm)
{
	g_main_loop_quit(test_loop);
	return test_cancel_id;
}

static void test_cancel_start(void)
{
	static const struct sailfish_slot_driver test_cancel_start_driver = {
		.name = "cancel_start",
		.manager_create = test_slot_manager_create,
		.manager_start = test_cancel_start_start,
		.manager_cancel_start = test_cancel_start_cancel,
		.manager_free = test_slot_manager_free,
	};

	test_cancel_ok = FALSE;
	test_common_init();
	g_assert(sailfish_slot_driver_register(&test_cancel_start_driver));
	g_main_loop_run(test_loop);
	test_common_deinit();
	g_assert(test_cancel_ok);
}

/* ==== voice_sim ==== */

static gboolean test_voice_sim_done(gpointer user_data)
{
	test_slot_manager *sm = user_data;
	test_slot *s = sm->slot;
	struct sailfish_manager *m = fake_sailfish_manager_dbus.m;
	struct ofono_watch *w = ofono_watch_new(TEST_PATH);
	struct ofono_sim sim;

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
	g_assert(!g_strcmp0(m->default_voice_path, TEST_PATH));

	/* Point it to a non-existent SIM, it will still point to the
	 * existing one */
	fake_sailfish_manager_dbus.cb.set_default_voice_imsi(m, TEST_IMSI_1);
	g_assert(!g_strcmp0(m->default_voice_imsi, TEST_IMSI_1));
	g_assert(!g_strcmp0(m->default_voice_path, TEST_PATH));

	/* And back to the right SIM */
	fake_sailfish_manager_dbus.cb.set_default_voice_imsi(m, TEST_IMSI);
	g_assert(!g_strcmp0(m->default_voice_imsi, TEST_IMSI));
	g_assert(!g_strcmp0(m->default_voice_path, TEST_PATH));

	/* Remove the SIM */
	fake_watch_set_ofono_iccid(w, NULL);
	fake_watch_set_ofono_imsi(w, NULL);
	fake_watch_set_ofono_spn(w, NULL);
	sailfish_manager_set_sim_state(s->handle, SAILFISH_SIM_STATE_ABSENT);
	fake_watch_emit_queued_signals(w);
	g_assert(!m->slots[0]->sim_present);
	g_assert(!g_strcmp0(m->default_voice_imsi, TEST_IMSI));
	g_assert(!m->default_voice_path);

	ofono_watch_unref(w);
	g_main_loop_quit(test_loop);
	return G_SOURCE_REMOVE;
}

static guint test_voice_sim_start(test_slot_manager *sm)
{
	test_slot *slot = g_new0(test_slot, 1);

	DBG("");

	/* Create the slot */
	slot->handle = sailfish_manager_slot_add(sm->handle, slot, TEST_PATH,
			OFONO_RADIO_ACCESS_MODE_GSM, TEST_IMEI, TEST_IMEISV,
			SAILFISH_SIM_STATE_PRESENT);

	sm->slot = slot;
	g_idle_add(test_voice_sim_done, sm);
	return 0;
}

static void test_voice_sim(void)
{
	static const struct sailfish_slot_driver test_voice_sim_driver = {
		.name = "voice_sim",
		.manager_create = test_slot_manager_create,
		.manager_start = test_voice_sim_start,
		.manager_free = test_slot_manager_free,
		.slot_free = test_slot_free
	};
	struct sailfish_slot_driver_reg *reg;

	test_common_init();
	reg = sailfish_slot_driver_register(&test_voice_sim_driver);
	g_assert(reg);

	g_main_loop_run(test_loop);

	sailfish_slot_driver_unregister(reg);
	test_common_deinit();
}

/* ==== data_sim ==== */

static gboolean test_data_sim_done(gpointer user_data)
{
	test_slot_manager *sm = user_data;
	test_slot *s = sm->slot;
	struct sailfish_manager *m = fake_sailfish_manager_dbus.m;
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

	g_assert(!g_strcmp0(m->default_voice_path, TEST_PATH));
	g_assert(!m->default_data_path); /* No default data slot */

	/* Set data SIM IMSI */
	fake_sailfish_manager_dbus.cb.set_default_data_imsi(m, TEST_IMSI);
	g_assert(!g_strcmp0(m->default_data_imsi, TEST_IMSI));
	g_assert(!m->default_data_path); /* Modem is offline */

	/* Set modem online */
	w->modem = &modem;
	w->online = TRUE;
	fake_watch_signal_queue(w, FAKE_WATCH_SIGNAL_ONLINE_CHANGED);
	fake_watch_signal_queue(w, FAKE_WATCH_SIGNAL_MODEM_CHANGED);
	fake_watch_emit_queued_signals(w);
	/* Now is should point to our slot */
	g_assert(!g_strcmp0(m->default_data_path, TEST_PATH));

	/* Point it to a non-existent SIM */
	fake_sailfish_manager_dbus.cb.set_default_data_imsi(m, TEST_IMSI_1);
	g_assert(!g_strcmp0(m->default_data_imsi, TEST_IMSI_1));
	g_assert(!m->default_data_path);

	/* Switch the SIM */
	fake_watch_set_ofono_imsi(w, TEST_IMSI_1);
	fake_watch_emit_queued_signals(w);
	g_assert(!g_strcmp0(m->default_data_path, TEST_PATH));

	/* Remove the SIM */
	fake_watch_set_ofono_sim(w, NULL);
	fake_watch_emit_queued_signals(w);
	sailfish_manager_set_sim_state(s->handle, SAILFISH_SIM_STATE_ABSENT);
	g_assert(!m->slots[0]->sim_present);
	g_assert(!g_strcmp0(m->default_data_imsi, TEST_IMSI_1));
	g_assert(!m->default_data_path);

	ofono_watch_unref(w);
	g_main_loop_quit(test_loop);
	return G_SOURCE_REMOVE;
}

static guint test_data_sim_start(test_slot_manager *sm)
{
	test_slot *slot = g_new0(test_slot, 1);

	DBG("");

	/* Create the slot */
	slot->handle = sailfish_manager_slot_add(sm->handle, slot, TEST_PATH,
			OFONO_RADIO_ACCESS_MODE_GSM, TEST_IMEI, TEST_IMEISV,
			SAILFISH_SIM_STATE_PRESENT);

	sm->slot = slot;
	g_idle_add(test_data_sim_done, sm);
	return 0;
}

static void test_data_sim(void)
{
	static const struct sailfish_slot_driver test_data_sim_driver = {
		.name = "data_sim",
		.manager_create = test_slot_manager_create,
		.manager_start = test_data_sim_start,
		.manager_free = test_slot_manager_free,
		.slot_enabled_changed = test_slot_enabled_changed,
		.slot_free = test_slot_free
	};
	struct sailfish_slot_driver_reg *reg;

	test_common_init();
	reg = sailfish_slot_driver_register(&test_data_sim_driver);
	g_assert(reg);

	g_main_loop_run(test_loop);

	sailfish_slot_driver_unregister(reg);
	test_common_deinit();
}

/* ==== mms_sim ==== */

static gboolean test_mms_sim_done(gpointer user_data)
{
	test_slot_manager *sm = user_data;
	test_slot *s = sm->slot;
	struct sailfish_manager *m = fake_sailfish_manager_dbus.m;
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
	g_assert(!g_strcmp0(m->default_voice_path, TEST_PATH));
	g_assert(!g_strcmp0(m->default_data_path, TEST_PATH));

	/* Set data SIM IMSI (second time is a noop */
	fake_sailfish_manager_dbus.cb.set_default_data_imsi(m, TEST_IMSI);
	g_assert(!g_strcmp0(m->default_data_imsi, TEST_IMSI));
	g_assert(!g_strcmp0(m->default_data_path, TEST_PATH));
	fake_sailfish_manager_dbus.cb.set_default_data_imsi(m, TEST_IMSI);
	g_assert(!g_strcmp0(m->default_data_imsi, TEST_IMSI));
	g_assert(!g_strcmp0(m->default_data_path, TEST_PATH));
	g_assert(s->data_role == SAILFISH_DATA_ROLE_INTERNET);

	/* Reserve it for MMS */
	g_assert(fake_sailfish_manager_dbus.cb.set_mms_imsi(m, TEST_IMSI));
	g_assert(s->data_role == SAILFISH_DATA_ROLE_INTERNET); /* Not MMS! */
	g_assert(!g_strcmp0(m->default_data_path, TEST_PATH));
	g_assert(!g_strcmp0(m->mms_imsi, TEST_IMSI));
	g_assert(!g_strcmp0(m->mms_path, TEST_PATH));

	/* Try to point MMS IMSI to a non-existent SIM */
	g_assert(!fake_sailfish_manager_dbus.cb.set_mms_imsi(m, TEST_IMSI_1));
	g_assert(!g_strcmp0(m->default_data_path, TEST_PATH));
	g_assert(!g_strcmp0(m->mms_imsi, TEST_IMSI));
	g_assert(!g_strcmp0(m->mms_path, TEST_PATH));
	g_assert(s->data_role == SAILFISH_DATA_ROLE_INTERNET);

	/* Reset MMS IMSI */
	g_assert(fake_sailfish_manager_dbus.cb.set_mms_imsi(m, NULL));
	g_assert(!m->mms_imsi);
	g_assert(!m->mms_path);

	/* Second time is a noop, empty IMSI is the same as NULL */
	g_assert(fake_sailfish_manager_dbus.cb.set_mms_imsi(m, ""));
	g_assert(!m->mms_imsi);
	g_assert(!m->mms_path);

	ofono_watch_unref(w);
	g_main_loop_quit(test_loop);
	return G_SOURCE_REMOVE;
}

static guint test_mms_sim_start(test_slot_manager *sm)
{
	test_slot *slot = g_new0(test_slot, 1);

	DBG("");

	/* Create the slot */
	slot->handle = sailfish_manager_slot_add(sm->handle, slot, TEST_PATH,
			OFONO_RADIO_ACCESS_MODE_GSM, TEST_IMEI, TEST_IMEISV,
			SAILFISH_SIM_STATE_PRESENT);

	sm->slot = slot;
	g_idle_add(test_mms_sim_done, sm);
	return 0;
}

static void test_mms_sim(void)
{
	static const struct sailfish_slot_driver test_mms_sim_driver = {
		.name = "mms_sim",
		.manager_create = test_slot_manager_create,
		.manager_start = test_mms_sim_start,
		.manager_free = test_slot_manager_free,
		.slot_enabled_changed = test_slot_enabled_changed,
		.slot_set_data_role = test_slot_set_data_role,
		.slot_free = test_slot_free
	};
	struct sailfish_slot_driver_reg *reg;

	test_common_init();
	reg = sailfish_slot_driver_register(&test_mms_sim_driver);
	g_assert(reg);

	g_main_loop_run(test_loop);

	sailfish_slot_driver_unregister(reg);
	test_common_deinit();
}

/* ==== multisim ==== */

static gboolean test_multisim_done(gpointer user_data)
{
	test_slot_manager *sm = user_data;
	test_slot *s = sm->slot;
	test_slot *s2 = sm->slot2;
	struct sailfish_manager *m = fake_sailfish_manager_dbus.m;
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
	g_assert(s->data_role == SAILFISH_DATA_ROLE_NONE);
	g_assert(!m->default_voice_imsi);
	g_assert(!g_strcmp0(m->default_voice_path, TEST_PATH));
	g_assert(!m->default_data_imsi);
	g_assert(!m->default_data_path);

	/* But there is automatic voice SIM selection */
	g_assert(!m->default_voice_imsi);
	g_assert(!g_strcmp0(m->default_voice_path, TEST_PATH));

	/* Switch the voice SIM back and forth */
	fake_sailfish_manager_dbus.cb.set_default_voice_imsi(m, TEST_IMSI);
	g_assert(!g_strcmp0(m->default_voice_imsi, TEST_IMSI));
	g_assert(!g_strcmp0(m->default_voice_path, TEST_PATH));

	fake_sailfish_manager_dbus.cb.set_default_voice_imsi(m, TEST_IMSI_1);
	g_assert(!g_strcmp0(m->default_voice_imsi, TEST_IMSI_1));
	g_assert(!g_strcmp0(m->default_voice_path, TEST_PATH_1));

	/* test_1 remains the current voice slot */
	fake_sailfish_manager_dbus.cb.set_default_voice_imsi(m, NULL);
	g_assert(!m->default_voice_imsi);
	g_assert(!g_strcmp0(m->default_voice_path, TEST_PATH_1));

	/* Reserve the first slot for data */
	fake_sailfish_manager_dbus.cb.set_default_data_imsi(m, TEST_IMSI);
	g_assert(s->data_role == SAILFISH_DATA_ROLE_INTERNET);
	g_assert(!g_strcmp0(m->default_data_imsi, TEST_IMSI));
	g_assert(!g_strcmp0(m->default_data_path, TEST_PATH));

	/* Second slot for MMS */
	g_assert(fake_sailfish_manager_dbus.cb.set_mms_imsi(m, TEST_IMSI_1));
	g_assert(s->data_role == SAILFISH_DATA_ROLE_NONE);
	g_assert(s2->data_role == SAILFISH_DATA_ROLE_MMS);
	g_assert(!g_strcmp0(m->mms_path, TEST_PATH_1));
	g_assert(!g_strcmp0(m->mms_imsi, TEST_IMSI_1));
	g_assert(!g_strcmp0(m->default_data_imsi, TEST_IMSI));
	g_assert(!m->default_data_path);

	/* Cancel MMS reservation */
	g_assert(fake_sailfish_manager_dbus.cb.set_mms_imsi(m, NULL));
	g_assert(s->data_role == SAILFISH_DATA_ROLE_INTERNET);
	g_assert(s2->data_role == SAILFISH_DATA_ROLE_NONE);
	g_assert(!g_strcmp0(m->default_data_imsi, TEST_IMSI));
	g_assert(!g_strcmp0(m->default_data_path, TEST_PATH));
	g_assert(!m->mms_path);
	g_assert(!m->mms_imsi);

	ofono_watch_unref(w);
	ofono_watch_unref(w2);
	g_main_loop_quit(test_loop);
	return G_SOURCE_REMOVE;
}

static gboolean test_multisim_add_slots(gpointer user_data)
{
	struct sailfish_manager *m = fake_sailfish_manager_dbus.m;
	test_slot_manager *sm = user_data;
	test_slot *s = g_new0(test_slot, 1);
	test_slot *s2 = g_new0(test_slot, 1);

	/* Create the slots */
	DBG("");
	s->handle = sailfish_manager_slot_add(sm->handle, s, TEST_PATH,
			OFONO_RADIO_ACCESS_MODE_GSM, NULL, TEST_IMEISV,
			SAILFISH_SIM_STATE_PRESENT);
	s2->handle = sailfish_manager_slot_add(sm->handle, s2, TEST_PATH_1,
			OFONO_RADIO_ACCESS_MODE_GSM, NULL, TEST_IMEISV,
			SAILFISH_SIM_STATE_PRESENT);
	sm->slot = s;
	sm->slot2 = s2;
	sailfish_slot_manager_started(sm->handle);

	g_assert(!m->ready);
	sailfish_manager_imei_obtained(s->handle, TEST_IMEI);
	g_assert(!m->ready);
	sailfish_manager_imei_obtained(s2->handle, TEST_IMEI_1);
	g_assert(m->ready);

	g_idle_add(test_multisim_done, sm);
	return G_SOURCE_REMOVE;
}

static guint test_multisim_start(test_slot_manager *sm)
{
	return g_idle_add(test_multisim_add_slots, sm);
}

static void test_multisim(void)
{
	static const struct sailfish_slot_driver test_multisim_driver = {
		.name = "multisim",
		.manager_create = test_slot_manager_create,
		.manager_start = test_multisim_start,
		.manager_free = test_slot_manager_free,
		.slot_enabled_changed = test_slot_enabled_changed,
		.slot_set_data_role = test_slot_set_data_role,
		.slot_free = test_slot_free
	};
	struct sailfish_slot_driver_reg *reg;

	test_common_init();
	reg = sailfish_slot_driver_register(&test_multisim_driver);
	g_assert(reg);

	g_main_loop_run(test_loop);

	sailfish_slot_driver_unregister(reg);
	test_common_deinit();
}

/* ==== storage ==== */

static void test_storage_init(test_slot_manager *sm)
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

static void test_storage_add_slots(test_slot_manager *sm)
{
	struct sailfish_manager *m = fake_sailfish_manager_dbus.m;
	test_slot *s = g_new0(test_slot, 1);
	test_slot *s2 = g_new0(test_slot, 1);

	/* Create the slots */
	DBG("");
	s->handle = sailfish_manager_slot_add(sm->handle, s, TEST_PATH,
			OFONO_RADIO_ACCESS_MODE_GSM, NULL, TEST_IMEISV,
			SAILFISH_SIM_STATE_PRESENT);
	s2->handle = sailfish_manager_slot_add(sm->handle, s2, TEST_PATH_1,
			OFONO_RADIO_ACCESS_MODE_GSM, NULL, TEST_IMEISV,
			SAILFISH_SIM_STATE_PRESENT);
	sm->slot = s;
	sm->slot2 = s2;
	sailfish_slot_manager_started(sm->handle);

	g_assert(!m->ready);
	sailfish_manager_imei_obtained(s->handle, TEST_IMEI);
	g_assert(!m->ready);
	sailfish_manager_imei_obtained(s2->handle, TEST_IMEI_1);
	g_assert(m->ready);
}

static gboolean test_storage_save_add_slots(gpointer user_data)
{
	test_slot_manager *sm = user_data;
	struct sailfish_manager *m = fake_sailfish_manager_dbus.m;
	char **slots = gutil_strv_add(NULL, TEST_PATH);

	test_storage_add_slots(sm);

	fake_sailfish_manager_dbus.cb.set_enabled_slots(m, slots);
	g_assert(m->slots[0]->enabled);
	g_assert(!m->slots[1]->enabled);
	g_strfreev(slots);

	test_storage_init(sm);

	/* Wait for D-Bus interface to get unblocked and exit the loop */
	fake_sailfish_manager_dbus.fn_block_changed =
		test_quit_loop_when_unblocked;
	return G_SOURCE_REMOVE;
}

static gboolean test_storage_restore_add_slots(gpointer user_data)
{
	test_slot_manager *sm = user_data;
	struct sailfish_manager *m = fake_sailfish_manager_dbus.m;

	test_storage_add_slots(sm);

	/* These should get restored from the file */
	g_assert(m->slots[0]->enabled);
	g_assert(!m->slots[1]->enabled);

	/* Wait for D-Bus interface to get unblocked and exit the loop */
	fake_sailfish_manager_dbus.fn_block_changed =
		test_quit_loop_when_unblocked;
	return G_SOURCE_REMOVE;
}

static guint test_storage_save_start(test_slot_manager *sm)
{
	return g_idle_add(test_storage_save_add_slots, sm);
}

static guint test_storage_restore_start(test_slot_manager *sm)
{
	return g_idle_add(test_storage_restore_add_slots, sm);
}

static void test_storage(void)
{
	static const struct sailfish_slot_driver test_storage_save_driver = {
		.name = "storage_save",
		.manager_create = test_slot_manager_create,
		.manager_start = test_storage_save_start,
		.manager_free = test_slot_manager_free,
		.slot_enabled_changed = test_slot_enabled_changed,
		.slot_free = test_slot_free
	};

	static const struct sailfish_slot_driver test_storage_restore_driver = {
		.name = "storage_restore",
		.manager_create = test_slot_manager_create,
		.manager_start = test_storage_restore_start,
		.manager_free = test_slot_manager_free,
		.slot_enabled_changed = test_slot_enabled_changed,
		.slot_free = test_slot_free
	};

	test_common_init();

	g_assert(sailfish_slot_driver_register(&test_storage_save_driver));
	g_main_loop_run(test_loop);

	/* Reinitialize everything */
	__ofono_builtin_sailfish_manager.exit();
	__ofono_builtin_sailfish_manager.init();

	/* And restore settings from the file */
	g_assert(sailfish_slot_driver_register(&test_storage_restore_driver));
	g_main_loop_run(test_loop);

	test_common_deinit();
}

#define TEST_(name) "/sailfish_manager/" name

int main(int argc, char *argv[])
{
	g_test_init(&argc, &argv, NULL);

	gutil_log_timestamp = FALSE;
	gutil_log_default.level = g_test_verbose() ?
		GLOG_LEVEL_VERBOSE : GLOG_LEVEL_NONE;
	__ofono_log_init("test-sailfish_manager",
				g_test_verbose() ? "*" : NULL,
				FALSE, FALSE);

	g_test_add_func(TEST_("basic"), test_basic);
	g_test_add_func(TEST_("early_init"), test_early_init);
	g_test_add_func(TEST_("too_late"), test_too_late);
	g_test_add_func(TEST_("create_fail"), test_create_fail);
	g_test_add_func(TEST_("no_plugins"), test_no_plugins);
	g_test_add_func(TEST_("no_slots"), test_no_slots);
	g_test_add_func(TEST_("no_manager"), test_no_manager);
	g_test_add_func(TEST_("sync_start"), test_sync_start);
	g_test_add_func(TEST_("async_start"), test_async_start);
	g_test_add_func(TEST_("cancel_start"), test_cancel_start);
	g_test_add_func(TEST_("voice_sim"), test_voice_sim);
	g_test_add_func(TEST_("data_sim"), test_data_sim);
	g_test_add_func(TEST_("mms_sim"), test_mms_sim);
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
