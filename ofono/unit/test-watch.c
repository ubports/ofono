/*
 *  oFono - Open Source Telephony
 *
 *  Copyright (C) 2018-2022 Jolla Ltd.
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

#include "watch_p.h"

#include "ofono.h"

#include <gutil_log.h>

static GSList *g_modem_list = NULL;
static struct ofono_watchlist *g_modemwatches = NULL;

#define TEST_PATH "/test_0"
#define TEST_PATH_1 "/test_1"
#define TEST_ICCID "0000000000000000000"
#define TEST_IMSI "244120000000000"
#define TEST_SPN "Test"
#define TEST_MCC "244"
#define TEST_MNC "12"
#define TEST_NAME "Test"

/* Fake ofono_atom */

struct ofono_atom {
	enum ofono_atom_type type;
	gboolean registered;
	void *data;
	struct ofono_modem *modem;
};

void *__ofono_atom_get_data(struct ofono_atom *atom)
{
	return atom->data;
}

/* Fake ofono_gprs */

struct ofono_gprs {
	struct ofono_atom atom;
	enum ofono_gprs_context_type type;
	const struct ofono_gprs_primary_context *settings;
};

/* Fake ofono_netreg */

struct ofono_netreg {
	struct ofono_atom atom;
	struct ofono_watchlist *status_watches;
	enum ofono_netreg_status status;
	enum ofono_access_technology tech;
	const char *mcc;
	const char *mnc;
	const char *name;
};

enum ofono_netreg_status ofono_netreg_get_status(struct ofono_netreg *netreg)
{
	return netreg ? netreg->status : OFONO_NETREG_STATUS_NONE;
}

enum ofono_access_technology
		ofono_netreg_get_technology (struct ofono_netreg *netreg)
{
	return netreg ? netreg->tech : OFONO_ACCESS_TECHNOLOGY_NONE;
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

static void netreg_notify(struct ofono_netreg *netreg)
{
	GSList *l;

	for (l = netreg->status_watches->items; l; l = l->next) {
		struct ofono_watchlist_item *item = l->data;
		ofono_netreg_status_notify_cb_t notify = item->notify;

		notify(netreg->status, -1, -1, netreg->tech, netreg->mcc,
					netreg->mnc, item->notify_data);
	}
}

static unsigned int add_watch_item(struct ofono_watchlist *list,
			void *notify, void *data, ofono_destroy_func destroy)
{
	struct ofono_watchlist_item *watch =
		g_new0(struct ofono_watchlist_item, 1);

	watch->notify = notify;
	watch->destroy = destroy;
	watch->notify_data = data;
	return __ofono_watchlist_add_item(list, watch);
}

unsigned int __ofono_netreg_add_status_watch(struct ofono_netreg *netreg,
				ofono_netreg_status_notify_cb_t notify,
				void *data, ofono_destroy_func destroy)
{
	return add_watch_item(netreg->status_watches, notify, data, destroy);
}

gboolean __ofono_netreg_remove_status_watch(struct ofono_netreg *netreg,
						unsigned int id)
{
	return __ofono_watchlist_remove_item(netreg->status_watches, id);
}

/* Fake ofono_sim */

struct ofono_sim {
	struct ofono_atom atom;
	const char *spn;
	const char *spn_dc;
	const char *imsi;
	const char *iccid;
	enum ofono_sim_state state;
	struct ofono_watchlist *spn_watches;
	struct ofono_watchlist *imsi_watches;
	struct ofono_watchlist *iccid_watches;
	struct ofono_watchlist *state_watches;
};

unsigned int ofono_sim_add_iccid_watch(struct ofono_sim *sim,
				ofono_sim_iccid_event_cb_t cb, void *data,
				ofono_destroy_func destroy)
{
	guint id = add_watch_item(sim->iccid_watches, cb, data, destroy);

	if (sim->iccid) {
		cb(sim->iccid, data);
	}

	return id;
}

void ofono_sim_remove_iccid_watch(struct ofono_sim *sim, unsigned int id)
{
	__ofono_watchlist_remove_item(sim->iccid_watches, id);
}

static void iccid_watches_cb(gpointer data, gpointer user_data)
{
	struct ofono_sim *sim = user_data;
	struct ofono_watchlist_item *item = data;
	ofono_sim_iccid_event_cb_t cb = item->notify;

	cb(sim->iccid, item->notify_data);
}

static inline void iccid_watches_notify(struct ofono_sim *sim)
{
	g_slist_foreach(sim->iccid_watches->items, iccid_watches_cb, sim);
}

unsigned int ofono_sim_add_imsi_watch(struct ofono_sim *sim,
				ofono_sim_imsi_event_cb_t cb, void *data,
				ofono_destroy_func destroy)
{
	guint id = add_watch_item(sim->imsi_watches, cb, data, destroy);

	if (sim->imsi) {
		cb(sim->imsi, data);
	}

	return id;
}

void ofono_sim_remove_imsi_watch(struct ofono_sim *sim, unsigned int id)
{
	__ofono_watchlist_remove_item(sim->imsi_watches, id);
}

static void imsi_watches_cb(gpointer data, gpointer user_data)
{
	struct ofono_sim *sim = user_data;
	struct ofono_watchlist_item *item = data;
	ofono_sim_imsi_event_cb_t cb = item->notify;

	cb(sim->imsi, item->notify_data);
}

static inline void imsi_watches_notify(struct ofono_sim *sim)
{
	g_slist_foreach(sim->imsi_watches->items, imsi_watches_cb, sim);
}

ofono_bool_t ofono_sim_add_spn_watch(struct ofono_sim *sim, unsigned int *id,
		ofono_sim_spn_cb_t cb, void *data, ofono_destroy_func destroy)
{
	*id = add_watch_item(sim->spn_watches, cb, data, destroy);
	if (sim->spn) {
		cb(sim->spn, sim->spn_dc, data);
	}
	return TRUE;
}

ofono_bool_t ofono_sim_remove_spn_watch(struct ofono_sim *sim, unsigned int *id)
{
	if (__ofono_watchlist_remove_item(sim->spn_watches, *id)) {
		*id = 0;
		return TRUE;
	} else {
		return FALSE;
	}
}

static void spn_watches_cb(gpointer data, gpointer user_data)
{
	struct ofono_sim *sim = user_data;
	struct ofono_watchlist_item *item = data;
	ofono_sim_spn_cb_t notify = item->notify;

	notify(sim->spn, sim->spn_dc, item->notify_data);
}

static inline void spn_watches_notify(struct ofono_sim *sim)
{
	g_slist_foreach(sim->spn_watches->items, spn_watches_cb, sim);
}

unsigned int ofono_sim_add_state_watch(struct ofono_sim *sim,
					ofono_sim_state_event_cb_t notify,
					void *data, ofono_destroy_func destroy)
{
	return add_watch_item(sim->state_watches, notify, data, destroy);
}

void ofono_sim_remove_state_watch(struct ofono_sim *sim, unsigned int id)
{
	__ofono_watchlist_remove_item(sim->state_watches, id);
}

static void state_watches_cb(gpointer data, gpointer user_data)
{
	struct ofono_sim *sim = user_data;
	struct ofono_watchlist_item *item = data;
	ofono_sim_state_event_cb_t notify = item->notify;

	notify(sim->state, item->notify_data);
}

static void state_watches_notify(struct ofono_sim *sim)
{
	g_slist_foreach(sim->state_watches->items, state_watches_cb, sim);
}

/* Fake modem */

struct ofono_modem {
	const char *path;
	gboolean online;
	GSList *atoms;
	struct ofono_watchlist *atom_watches;
	struct ofono_watchlist *online_watches;
	struct ofono_sim sim;
	struct ofono_netreg netreg;
	struct ofono_gprs gprs;
};

struct atom_watch {
	struct ofono_watchlist_item item;
	enum ofono_atom_type type;
};

void __ofono_modemwatch_init(void)
{
	g_assert(!g_modem_list);
	g_assert(!g_modemwatches);
	g_modemwatches = __ofono_watchlist_new(g_free);
}

void __ofono_modemwatch_cleanup(void)
{
	g_assert(!g_modem_list);
	__ofono_watchlist_free(g_modemwatches);
	g_modemwatches = NULL;
}

unsigned int __ofono_modemwatch_add(ofono_modemwatch_cb_t cb, void *data,
					ofono_destroy_func destroy)
{
	return add_watch_item(g_modemwatches, cb, data, destroy);
}

gboolean __ofono_modemwatch_remove(unsigned int id)
{
	return __ofono_watchlist_remove_item(g_modemwatches, id);
}

static void call_modemwatches(struct ofono_modem *modem, gboolean added)
{
	GSList *l;

	DBG("%p added:%d", modem, added);
	for (l = g_modemwatches->items; l; l = l->next) {
		struct ofono_watchlist_item *watch = l->data;
		ofono_modemwatch_cb_t notify = watch->notify;

		notify(modem, added, watch->notify_data);
	}
}

const char *ofono_modem_get_path(struct ofono_modem *modem)
{
	return modem->path;
}

ofono_bool_t ofono_modem_get_online(struct ofono_modem *modem)
{
	return modem && modem->online;
}

struct ofono_modem *ofono_modem_find(ofono_modem_compare_cb_t func,
					void *user_data)
{
	GSList *l;

	for (l = g_modem_list; l; l = l->next) {
		struct ofono_modem *modem = l->data;

		if (func(modem, user_data)) {
			return modem;
		}
	}

	return NULL;
}

unsigned int __ofono_modem_add_atom_watch(struct ofono_modem *modem,
		enum ofono_atom_type type, ofono_atom_watch_func notify,
		void *data, ofono_destroy_func destroy)
{
	GSList *l;
	unsigned int id;
	struct atom_watch *watch = g_new0(struct atom_watch, 1);

	watch->type = type;
	watch->item.notify = notify;
	watch->item.destroy = destroy;
	watch->item.notify_data = data;
	id = __ofono_watchlist_add_item(modem->atom_watches, &watch->item);

	for (l = modem->atoms; l; l = l->next) {
		struct ofono_atom *atom = l->data;

		if (atom->type == type && atom->registered) {
			notify(atom, OFONO_ATOM_WATCH_CONDITION_REGISTERED,
									data);
		}
	}

	return id;
}

static void atom_notify(struct ofono_atom *atom,
				enum ofono_atom_watch_condition cond)
{
	GSList *l;

	for (l = atom->modem->atom_watches->items; l; l = l->next) {
		struct atom_watch *watch = l->data;

		if (watch->type == atom->type) {
			ofono_atom_watch_func notify = watch->item.notify;

			notify(atom, cond, watch->item.notify_data);
		}
	}
}

gboolean __ofono_modem_remove_atom_watch(struct ofono_modem *modem,
						unsigned int id)
{
	return __ofono_watchlist_remove_item(modem->atom_watches, id);
}

unsigned int __ofono_modem_add_online_watch(struct ofono_modem *modem,
					ofono_modem_online_notify_func notify,
					void *data, ofono_destroy_func destroy)
{
	return add_watch_item(modem->online_watches, notify, data, destroy);
}

void __ofono_modem_remove_online_watch(struct ofono_modem *modem,
					unsigned int id)
{
	__ofono_watchlist_remove_item(modem->online_watches, id);
}

static void notify_online_watches_cb(gpointer data, gpointer user_data)
{
	struct ofono_modem *modem = user_data;
	struct ofono_watchlist_item *item = data;
	ofono_modem_online_notify_func  notify = item->notify;

	notify(modem, modem->online, item->notify_data);
}

static void notify_online_watches(struct ofono_modem *modem)
{
	g_slist_foreach(modem->online_watches->items,
					notify_online_watches_cb, modem);
}

/* Utilities */

static void test_modem_register_atom(struct ofono_modem *modem,
						struct ofono_atom *atom)
{
	if (!atom->registered) {
		atom->registered = TRUE;
		modem->atoms = g_slist_append(modem->atoms, atom);
		atom_notify(atom, OFONO_ATOM_WATCH_CONDITION_REGISTERED);
	}
}

static void test_modem_unregister_atom(struct ofono_modem *modem,
						struct ofono_atom *atom)
{
	if (atom->registered) {
		atom->registered = FALSE;
		atom_notify(atom, OFONO_ATOM_WATCH_CONDITION_UNREGISTERED);
		modem->atoms = g_slist_remove(modem->atoms, atom);
	}
}

static void test_modem_init1(struct ofono_modem *modem, const char *path)
{
	struct ofono_netreg *netreg = &modem->netreg;
	struct ofono_gprs *gprs = &modem->gprs;
	struct ofono_sim *sim = &modem->sim;

	/* Assume that the structure has been zero-initialized */
	modem->path = path;
	modem->atom_watches = __ofono_watchlist_new(g_free);
	modem->online_watches = __ofono_watchlist_new(g_free);

	netreg->atom.type = OFONO_ATOM_TYPE_NETREG;
	netreg->atom.modem = modem;
	netreg->atom.data = netreg;
	netreg->status = OFONO_NETREG_STATUS_NOT_REGISTERED;
	netreg->tech = OFONO_ACCESS_TECHNOLOGY_NONE;
	netreg->status_watches = __ofono_watchlist_new(g_free);

	gprs->atom.type = OFONO_ATOM_TYPE_GPRS;
	gprs->atom.modem = modem;
	gprs->atom.data = gprs;

	sim->atom.type = OFONO_ATOM_TYPE_SIM;
	sim->atom.modem = modem;
	sim->atom.data = sim;

	sim->iccid_watches = __ofono_watchlist_new(g_free);
	sim->imsi_watches = __ofono_watchlist_new(g_free);
	sim->state_watches = __ofono_watchlist_new(g_free);
	sim->spn_watches = __ofono_watchlist_new(g_free);
	sim->state = OFONO_SIM_STATE_NOT_PRESENT;

	g_modem_list = g_slist_prepend(g_modem_list, modem);
	call_modemwatches(modem, TRUE);
}

static void test_modem_init(struct ofono_modem *modem)
{
	test_modem_init1(modem, TEST_PATH);
}

static void test_modem_shutdown(struct ofono_modem *modem)
{
	struct ofono_sim *sim = &modem->sim;
	struct ofono_netreg *netreg = &modem->netreg;

	call_modemwatches(modem, FALSE);
	g_modem_list = g_slist_remove(g_modem_list, modem);
	g_slist_free(modem->atoms);

	__ofono_watchlist_free(sim->iccid_watches);
	__ofono_watchlist_free(sim->imsi_watches);
	__ofono_watchlist_free(sim->state_watches);
	__ofono_watchlist_free(sim->spn_watches);
	__ofono_watchlist_free(netreg->status_watches);
	__ofono_watchlist_free(modem->atom_watches);
	__ofono_watchlist_free(modem->online_watches);
}

static void test_inc_cb(struct ofono_watch *watch, void *user_data)
{
	(*((int *)user_data))++;
}

/* ==== basic ==== */

static void test_basic(void)
{
	struct ofono_watch *watch;
	struct ofono_watch *watch1;
	struct ofono_modem modem, modem1;
	unsigned long id = 0;

	/* NULL resistance */
	g_assert(!ofono_watch_new(NULL));
	g_assert(!ofono_watch_ref(NULL));
	ofono_watch_unref(NULL);
	g_assert(!ofono_watch_add_modem_changed_handler(NULL, NULL, NULL));
	g_assert(!ofono_watch_add_online_changed_handler(NULL, NULL, NULL));
	g_assert(!ofono_watch_add_sim_changed_handler(NULL, NULL, NULL));
	g_assert(!ofono_watch_add_sim_state_changed_handler(NULL, NULL,
									NULL));
	g_assert(!ofono_watch_add_iccid_changed_handler(NULL, NULL, NULL));
	g_assert(!ofono_watch_add_imsi_changed_handler(NULL, NULL, NULL));
	g_assert(!ofono_watch_add_spn_changed_handler(NULL, NULL, NULL));
	g_assert(!ofono_watch_add_netreg_changed_handler(NULL, NULL, NULL));
	g_assert(!ofono_watch_add_reg_status_changed_handler(NULL, NULL, NULL));
	g_assert(!ofono_watch_add_reg_mcc_changed_handler(NULL, NULL, NULL));
	g_assert(!ofono_watch_add_reg_mnc_changed_handler(NULL, NULL, NULL));
	g_assert(!ofono_watch_add_reg_name_changed_handler(NULL, NULL, NULL));
	g_assert(!ofono_watch_add_gprs_changed_handler(NULL, NULL, NULL));
	g_assert(!ofono_watch_add_gprs_settings_changed_handler(NULL,
								NULL, NULL));
	ofono_watch_remove_handler(NULL, 0);
	ofono_watch_remove_handlers(NULL, NULL, 0);
	__ofono_watch_gprs_settings_changed
		(NULL, OFONO_GPRS_CONTEXT_TYPE_ANY, NULL);
	__ofono_watch_gprs_settings_changed
		(TEST_PATH, OFONO_GPRS_CONTEXT_TYPE_ANY, NULL);

	/* Instance caching */
	memset(&modem, 0, sizeof(modem));
	memset(&modem1, 0, sizeof(modem1));
	__ofono_modemwatch_init();
	test_modem_init1(&modem, TEST_PATH);

	watch = ofono_watch_new(TEST_PATH);
	watch1 = ofono_watch_new(TEST_PATH_1);

	/* The second modem is added after the watch is created */
	test_modem_init1(&modem1, TEST_PATH_1);

	/* The second notification has no effect */
	call_modemwatches(&modem1, TRUE);

	g_assert(watch);
	g_assert(watch1);
	g_assert(watch->modem == &modem);
	g_assert(watch1->modem == &modem1);
	g_assert(ofono_watch_new(TEST_PATH) == watch);
	g_assert(ofono_watch_new(TEST_PATH_1) == watch1);
	g_assert(ofono_watch_ref(watch) == watch);
	ofono_watch_unref(watch);
	ofono_watch_unref(watch);
	ofono_watch_unref(watch1);

	/* More NULLs and zeros */
	g_assert(!ofono_watch_add_modem_changed_handler(watch, NULL, NULL));
	g_assert(!ofono_watch_add_online_changed_handler(watch, NULL, NULL));
	g_assert(!ofono_watch_add_sim_changed_handler(watch, NULL, NULL));
	g_assert(!ofono_watch_add_sim_state_changed_handler(watch, NULL,
									NULL));
	g_assert(!ofono_watch_add_iccid_changed_handler(watch, NULL, NULL));
	g_assert(!ofono_watch_add_imsi_changed_handler(watch, NULL, NULL));
	g_assert(!ofono_watch_add_spn_changed_handler(watch, NULL, NULL));
	g_assert(!ofono_watch_add_netreg_changed_handler(watch, NULL, NULL));
	ofono_watch_remove_handler(watch, 0);
	ofono_watch_remove_handlers(watch, NULL, 0);
	ofono_watch_remove_handlers(watch, &id, 0);
	ofono_watch_remove_handlers(watch, &id, 1);

	/* The first modem is removed when the watch is still alive */
	test_modem_shutdown(&modem);
	ofono_watch_unref(watch);
	ofono_watch_unref(watch1);
	test_modem_shutdown(&modem1);
	__ofono_modemwatch_cleanup();
}

/* ==== modem ==== */

static void test_modem(void)
{
	struct ofono_watch *watch;
	struct ofono_modem modem;
	gulong id;
	int n = 0;

	__ofono_modemwatch_init();
	watch = ofono_watch_new(TEST_PATH);

	id = ofono_watch_add_modem_changed_handler(watch, test_inc_cb, &n);
	g_assert(id);
	memset(&modem, 0, sizeof(modem));
	test_modem_init(&modem);
	g_assert(n == 1);

	ofono_watch_remove_handler(watch, id);
	ofono_watch_unref(watch);
	test_modem_shutdown(&modem);
	__ofono_modemwatch_cleanup();
}

/* ==== online ==== */

static void test_online(void)
{
	struct ofono_watch *watch;
	struct ofono_modem modem;
	gulong id;
	int n = 0;

	memset(&modem, 0, sizeof(modem));
	__ofono_modemwatch_init();
	test_modem_init(&modem);
	watch = ofono_watch_new(TEST_PATH);
	g_assert(!watch->online);

	modem.online = TRUE;
	id = ofono_watch_add_online_changed_handler(watch, test_inc_cb, &n);
	notify_online_watches(&modem);
	g_assert(watch->online);
	g_assert(n == 1);
	notify_online_watches(&modem); /* Second one has no effect */
	g_assert(n == 1);

	test_modem_shutdown(&modem);
	g_assert(!watch->online);
	g_assert(n == 2);

	ofono_watch_remove_handler(watch, id);
	ofono_watch_unref(watch);
	__ofono_modemwatch_cleanup();
}

/* ==== netreg ==== */

static void test_netreg(void)
{
	struct ofono_watch *watch;
	struct ofono_modem modem;
	struct ofono_netreg *netreg = &modem.netreg;
	gulong id[6];
	int n[G_N_ELEMENTS(id)];

#define NETREG 0
#define REG_STATUS 1
#define REG_MCC 2
#define REG_MNC 3
#define REG_NAME 4
#define REG_TECH 5

	memset(&modem, 0, sizeof(modem));
	__ofono_modemwatch_init();
	test_modem_init(&modem);
	watch = ofono_watch_new(TEST_PATH);
	g_assert(!watch->netreg);

	memset(id, 0, sizeof(id));
	memset(n, 0, sizeof(n));
	id[NETREG] = ofono_watch_add_netreg_changed_handler
		(watch, test_inc_cb, n + NETREG);
	id[REG_STATUS] = ofono_watch_add_reg_status_changed_handler
		(watch, test_inc_cb, n + REG_STATUS);
	id[REG_MCC] = ofono_watch_add_reg_mcc_changed_handler
		(watch, test_inc_cb, n + REG_MCC);
	id[REG_MNC] = ofono_watch_add_reg_mnc_changed_handler
		(watch, test_inc_cb, n + REG_MNC);
	id[REG_NAME] = ofono_watch_add_reg_name_changed_handler
		(watch, test_inc_cb, n + REG_NAME);
	id[REG_TECH] = ofono_watch_add_reg_tech_changed_handler
		(watch, test_inc_cb, n + REG_TECH);
	test_modem_register_atom(&modem, &netreg->atom);
	g_assert(watch->netreg == netreg);
	g_assert_cmpint(watch->reg_status, == ,netreg->status);
	g_assert_cmpint(watch->reg_tech, == ,netreg->tech);
	g_assert_cmpint(n[NETREG], == ,1);
	g_assert_cmpint(n[REG_STATUS], == ,1);
	g_assert_cmpint(n[REG_TECH], == ,0);
	n[NETREG] = 0;
	n[REG_STATUS] = 0;

	netreg->status++;
	netreg_notify(netreg);
	g_assert(watch->reg_status == netreg->status);
	g_assert(n[REG_STATUS] == 1);
	n[REG_STATUS] = 0;

	netreg->mcc = TEST_MCC;
	netreg->mnc = TEST_MNC;
	netreg->name = TEST_NAME;
	netreg->tech = OFONO_ACCESS_TECHNOLOGY_EUTRAN;
	netreg_notify(netreg);
	netreg_notify(netreg); /* This one has no effect */
	g_assert_cmpint(n[REG_STATUS], == ,0);
	g_assert_cmpint(n[REG_MCC], == ,1);
	g_assert_cmpint(n[REG_MNC], == ,1);
	g_assert_cmpint(n[REG_NAME], == ,1);
	g_assert_cmpint(n[REG_TECH], == ,1);
	g_assert_cmpstr(watch->reg_mcc, == ,netreg->mcc);
	g_assert_cmpstr(watch->reg_mnc, == ,netreg->mnc);
	g_assert_cmpstr(watch->reg_name, == ,netreg->name);
	n[REG_MCC] = 0;
	n[REG_MNC] = 0;
	n[REG_NAME] = 0;
	n[REG_TECH] = 0;

	test_modem_unregister_atom(&modem, &netreg->atom);
	g_assert(!watch->netreg);
	g_assert_cmpint(watch->reg_status, == ,OFONO_NETREG_STATUS_NONE);
	g_assert_cmpint(watch->reg_tech, == ,OFONO_ACCESS_TECHNOLOGY_NONE);
	g_assert(!watch->reg_mcc);
	g_assert(!watch->reg_mnc);
	g_assert(!watch->reg_name);
	g_assert_cmpint(n[NETREG], == ,1);
	g_assert_cmpint(n[REG_STATUS], == ,1);
	g_assert_cmpint(n[REG_MCC], == ,1);
	g_assert_cmpint(n[REG_MNC], == ,1);
	g_assert_cmpint(n[REG_NAME], == ,1);
	g_assert_cmpint(n[REG_TECH], == ,1);
	memset(n, 0, sizeof(n));

	netreg->mcc = NULL;
	netreg->mnc = NULL;
	netreg->name = NULL;

	test_modem_register_atom(&modem, &netreg->atom);
	g_assert(watch->netreg == netreg);
	g_assert_cmpint(watch->reg_status, == ,netreg->status);
	g_assert_cmpint(watch->reg_tech, == ,netreg->tech);
	g_assert_cmpint(n[NETREG], == ,1);
	g_assert_cmpint(n[REG_STATUS], == ,1);
	n[NETREG] = 0;
	n[REG_STATUS] = 0;
	n[REG_TECH] = 0;

	test_modem_shutdown(&modem);
	g_assert(!watch->netreg);
	g_assert_cmpint(watch->reg_status, == ,OFONO_NETREG_STATUS_NONE);
	g_assert_cmpint(watch->reg_tech, == ,OFONO_ACCESS_TECHNOLOGY_NONE);
	g_assert_cmpint(n[NETREG], == ,1);
	g_assert_cmpint(n[REG_STATUS], == ,1);
	g_assert_cmpint(n[REG_TECH], == ,1);
	g_assert_cmpint(n[REG_MCC], == ,0);
	g_assert_cmpint(n[REG_MNC], == ,0);
	g_assert_cmpint(n[REG_NAME], == ,0);

	ofono_watch_remove_all_handlers(watch, id);
	ofono_watch_unref(watch);
	__ofono_modemwatch_cleanup();
}

/* ==== gprs ==== */

static void test_gprs_settings_cb(struct ofono_watch *watch,
			enum ofono_gprs_context_type type,
			const struct ofono_gprs_primary_context *settings,
			void *user_data)
{
	struct ofono_gprs *gprs = user_data;

	g_assert(gprs == watch->gprs);
	gprs->type = type;
	gprs->settings = settings;
}

static void test_gprs(void)
{
	struct ofono_watch *watch;
	struct ofono_modem modem;
	struct ofono_gprs *gprs = &modem.gprs;
	struct ofono_gprs_primary_context settings;
	gulong ids[2];
	int n = 0;

	__ofono_modemwatch_init();
	memset(&modem, 0, sizeof(modem));
	test_modem_init(&modem);
	watch = ofono_watch_new(TEST_PATH);
	g_assert(!watch->gprs);

	ids[0] = ofono_watch_add_gprs_changed_handler(watch, test_inc_cb, &n);
	ids[1] = ofono_watch_add_gprs_settings_changed_handler(watch,
						test_gprs_settings_cb, gprs);

	test_modem_register_atom(&modem, &gprs->atom);
	g_assert(watch->gprs == gprs);
	g_assert(n == 1);
	test_modem_register_atom(&modem, &gprs->atom); /* No effect */
	g_assert(n == 1);

	test_modem_unregister_atom(&modem, &gprs->atom);
	g_assert(!watch->gprs);
	g_assert(n == 2);

	test_modem_register_atom(&modem, &gprs->atom);
	g_assert(watch->gprs == gprs);
	g_assert(n == 3);

	memset(&settings, 0, sizeof(settings));
	__ofono_watch_gprs_settings_changed(TEST_PATH,
				OFONO_GPRS_CONTEXT_TYPE_INTERNET, &settings);
	__ofono_watch_gprs_settings_changed(TEST_PATH_1, /* No effect */
				OFONO_GPRS_CONTEXT_TYPE_ANY, NULL);
	g_assert(gprs->type == OFONO_GPRS_CONTEXT_TYPE_INTERNET);
	g_assert(gprs->settings == &settings);

	test_modem_shutdown(&modem);
	g_assert(!watch->gprs);
	g_assert(n == 4);

	ofono_watch_remove_all_handlers(watch, ids);
	ofono_watch_unref(watch);
	__ofono_modemwatch_cleanup();
}

/* ==== sim ==== */

static void test_sim(void)
{
	struct ofono_watch *watch;
	struct ofono_modem modem;
	struct ofono_sim *sim = &modem.sim;
	gulong id[4];
	int n[G_N_ELEMENTS(id)];

#define SIM 0
#define ICCID 1
#define IMSI 2
#define SPN 3

	memset(&modem, 0, sizeof(modem));
	__ofono_modemwatch_init();
	test_modem_init(&modem);
	watch = ofono_watch_new(TEST_PATH);
	g_assert(!watch->iccid);
	g_assert(!watch->imsi);
	g_assert(!watch->spn);

	memset(id, 0, sizeof(id));
	memset(n, 0, sizeof(n));
	id[SIM] = ofono_watch_add_sim_changed_handler(watch,
						test_inc_cb, n + SIM);
	id[ICCID] = ofono_watch_add_iccid_changed_handler(watch,
						test_inc_cb, n + ICCID);
	id[IMSI] = ofono_watch_add_imsi_changed_handler(watch,
						test_inc_cb, n + IMSI);
	id[SPN] = ofono_watch_add_spn_changed_handler(watch,
						test_inc_cb, n + SPN);
	test_modem_register_atom(&modem, &modem.sim.atom);
	g_assert(watch->sim == &modem.sim);
	g_assert(n[SIM] == 1);

	/* Simulate insert */
	sim->state = OFONO_SIM_STATE_INSERTED;
	state_watches_notify(sim);

	/* ICCID retrieval */
	sim->iccid = TEST_ICCID;
	iccid_watches_notify(sim);
	g_assert(!g_strcmp0(watch->iccid, sim->iccid));
	g_assert(n[ICCID] == 1);

	/* EFspn retrieval */
	sim->spn = TEST_SPN;
	spn_watches_notify(sim);
	/* Not yet... We first expect IMSI */
	g_assert(!watch->spn);
	g_assert(n[SPN] == 0);

	sim->imsi = TEST_IMSI;
	imsi_watches_notify(sim);
	g_assert(!g_strcmp0(watch->imsi, sim->imsi));
	g_assert(!g_strcmp0(watch->spn, sim->spn));
	g_assert(n[IMSI] == 1);
	g_assert(n[SPN] == 1);

	/* Ready */
	sim->state = OFONO_SIM_STATE_READY;
	state_watches_notify(sim);

	/* And finally remove the SIM */
	sim->state = OFONO_SIM_STATE_NOT_PRESENT;
	state_watches_notify(sim);
	g_assert(!watch->iccid);
	g_assert(!watch->imsi);
	g_assert(!watch->spn);
	g_assert(n[ICCID] == 2);
	g_assert(n[IMSI] == 2);
	g_assert(n[SPN] == 2);

	test_modem_unregister_atom(&modem, &modem.sim.atom);
	g_assert(!watch->sim);
	g_assert(n[SIM] == 2);

	ofono_watch_remove_all_handlers(watch, id);
	ofono_watch_unref(watch);
	test_modem_shutdown(&modem);
	__ofono_modemwatch_cleanup();
}

#define TEST_(name) "/ofono_watch/" name

int main(int argc, char *argv[])
{
	g_test_init(&argc, &argv, NULL);

	gutil_log_timestamp = FALSE;
	gutil_log_default.level = g_test_verbose() ?
		GLOG_LEVEL_VERBOSE : GLOG_LEVEL_NONE;
	__ofono_log_init("test-ofono_watch", g_test_verbose() ? "*" : NULL,
		FALSE, FALSE);

	g_test_add_func(TEST_("basic"), test_basic);
	g_test_add_func(TEST_("modem"), test_modem);
	g_test_add_func(TEST_("online"), test_online);
	g_test_add_func(TEST_("netreg"), test_netreg);
	g_test_add_func(TEST_("gprs"), test_gprs);
	g_test_add_func(TEST_("sim"), test_sim);

	return g_test_run();
}

/*
 * Local Variables:
 * mode: C
 * c-basic-offset: 8
 * indent-tabs-mode: t
 * End:
 */
