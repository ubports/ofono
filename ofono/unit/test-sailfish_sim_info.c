/*
 *  oFono - Open Source Telephony
 *
 *  Copyright (C) 2017-2018 Jolla Ltd.
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

#include "sailfish_sim_info.h"
#include "fake_sailfish_watch.h"

#define OFONO_API_SUBJECT_TO_CHANGE
#include "ofono.h"
#include "common.h"

#include <gutil_log.h>

#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>

#define TEST_PATH "/test"
#define TEST_ICCID "0000000000000000000"
#define TEST_IMSI "244120000000000"
#define TEST_ICCID_1 "1111111111111111111"
#define TEST_IMSI_1 "244120000000001"
#define TEST_MCC "244"
#define TEST_MNC "12"
#define TEST_DEFAULT_SPN TEST_MCC TEST_MNC
#define TEST_SPN "Test"

#define ICCID_MAP STORAGEDIR "/iccidmap"
#define SIM_CACHE STORAGEDIR "/" TEST_IMSI "/cache"

enum sim_info_signals {
	SIM_INFO_SIGNAL_ICCID_CHANGED,
	SIM_INFO_SIGNAL_IMSI_CHANGED,
	SIM_INFO_SIGNAL_SPN_CHANGED,
	SIM_INFO_SIGNAL_COUNT
};

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

/* Fake ofono_netreg */

struct ofono_netreg {
	const char *mcc;
	const char *mnc;
	const char *name;
	int location;
	int cellid;
	enum ofono_radio_access_mode technology;
	enum network_registration_status status;
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

static void netreg_notify_status_watches(struct ofono_netreg *netreg)
{
	GSList *l;
	const char *mcc = netreg->mcc;
	const char *mnc = netreg->mnc;

	for (l = netreg->status_watches->items; l; l = l->next) {
		struct ofono_watchlist_item *item = l->data;
		ofono_netreg_status_notify_cb_t notify = item->notify;

		notify(netreg->status, netreg->location, netreg->cellid,
			netreg->technology, mcc, mnc, item->notify_data);
	}
}

static void test_remove_sim(struct ofono_sim* sim, struct sailfish_watch *watch)
{
	sim->mcc = NULL;
	sim->mnc = NULL;
	sim->state = OFONO_SIM_STATE_NOT_PRESENT;
	fake_sailfish_watch_signal_queue(watch, WATCH_SIGNAL_IMSI_CHANGED);
	fake_sailfish_watch_signal_queue(watch, WATCH_SIGNAL_SIM_STATE_CHANGED);
	fake_sailfish_watch_set_ofono_iccid(watch, NULL);
	fake_sailfish_watch_set_ofono_imsi(watch, NULL);
	fake_sailfish_watch_set_ofono_spn(watch, NULL);
	fake_sailfish_watch_emit_queued_signals(watch);
}

/* Test cases */

static void test_basic(void)
{
	struct sailfish_sim_info *si;

	/* NULL tolerance */
	g_assert(!sailfish_sim_info_new(NULL));
	g_assert(!sailfish_sim_info_ref(NULL));
	sailfish_sim_info_unref(NULL);
	g_assert(!sailfish_sim_info_add_iccid_changed_handler(NULL,NULL,NULL));
	g_assert(!sailfish_sim_info_add_imsi_changed_handler(NULL,NULL,NULL));
	g_assert(!sailfish_sim_info_add_spn_changed_handler(NULL,NULL,NULL));
	sailfish_sim_info_remove_handler(NULL, 0);
	sailfish_sim_info_remove_handlers(NULL, NULL, 0);

	/* Very basic things (mostly to improve code coverage) */
	si = sailfish_sim_info_new("/test");
	g_assert(si);
	g_assert(!sailfish_sim_info_add_iccid_changed_handler(si,NULL,NULL));
	g_assert(!sailfish_sim_info_add_imsi_changed_handler(si,NULL,NULL));
	g_assert(!sailfish_sim_info_add_spn_changed_handler(si,NULL,NULL));
	sailfish_sim_info_remove_handler(si, 0);
	sailfish_sim_info_remove_handlers(si, NULL, 0);
	sailfish_sim_info_unref(sailfish_sim_info_ref(si));
	sailfish_sim_info_unref(si);
}

static void test_signal_count_cb(struct sailfish_sim_info *si, void *data)
{
	(*((int*)data))++;
}

static void test_cache(void)
{
	struct sailfish_sim_info *si;
	struct sailfish_watch *w = sailfish_watch_new(TEST_PATH);
	struct ofono_sim sim;
	struct stat st;
	gulong id[SIM_INFO_SIGNAL_COUNT];
	int count[SIM_INFO_SIGNAL_COUNT];

	memset(id, 0, sizeof(id));
	memset(count, 0, sizeof(count));
	memset(&sim, 0, sizeof(sim));
	sim.state = OFONO_SIM_STATE_INSERTED;

	rmdir_r(STORAGEDIR);
	si = sailfish_sim_info_new(TEST_PATH);
	id[SIM_INFO_SIGNAL_ICCID_CHANGED] =
		sailfish_sim_info_add_iccid_changed_handler(si,
			test_signal_count_cb, count +
			SIM_INFO_SIGNAL_ICCID_CHANGED);
	id[SIM_INFO_SIGNAL_IMSI_CHANGED] =
		sailfish_sim_info_add_imsi_changed_handler(si,
			test_signal_count_cb, count +
			SIM_INFO_SIGNAL_IMSI_CHANGED);
	id[SIM_INFO_SIGNAL_SPN_CHANGED] =
		sailfish_sim_info_add_spn_changed_handler(si,
			test_signal_count_cb, count +
			SIM_INFO_SIGNAL_SPN_CHANGED);

	fake_sailfish_watch_set_ofono_sim(w, &sim);
	fake_sailfish_watch_emit_queued_signals(w);
	g_assert(!count[SIM_INFO_SIGNAL_ICCID_CHANGED]);
	g_assert(!count[SIM_INFO_SIGNAL_IMSI_CHANGED]);
	g_assert(!count[SIM_INFO_SIGNAL_SPN_CHANGED]);
	g_assert(!si->iccid);
	g_assert(!si->imsi);
	g_assert(!si->spn);

	fake_sailfish_watch_set_ofono_iccid(w, TEST_ICCID);
	fake_sailfish_watch_emit_queued_signals(w);
	g_assert(!g_strcmp0(si->iccid, TEST_ICCID));
	g_assert(count[SIM_INFO_SIGNAL_ICCID_CHANGED] == 1);
	g_assert(!count[SIM_INFO_SIGNAL_IMSI_CHANGED]);
	g_assert(!count[SIM_INFO_SIGNAL_SPN_CHANGED]);
	g_assert(stat(ICCID_MAP, &st) < 0);
	count[SIM_INFO_SIGNAL_ICCID_CHANGED] = 0;

	fake_sailfish_watch_set_ofono_imsi(w, TEST_IMSI);
	fake_sailfish_watch_emit_queued_signals(w);
	g_assert(!g_strcmp0(si->imsi, TEST_IMSI));
	g_assert(!count[SIM_INFO_SIGNAL_ICCID_CHANGED]);
	g_assert(!count[SIM_INFO_SIGNAL_SPN_CHANGED]);
	g_assert(count[SIM_INFO_SIGNAL_IMSI_CHANGED] == 1);
	count[SIM_INFO_SIGNAL_IMSI_CHANGED] = 0;
	/* ICCID map appears */
	g_assert(stat(ICCID_MAP, &st) == 0);
	g_assert(S_ISREG(st.st_mode));
	/* But no cache yet */
	g_assert(stat(SIM_CACHE, &st) < 0);

	/* This will generate default SPN out of MCC and MNC */
	sim.mcc = TEST_MCC;
	sim.mnc = TEST_MNC;
	sim.state = OFONO_SIM_STATE_READY;
	fake_sailfish_watch_signal_queue(w, WATCH_SIGNAL_IMSI_CHANGED);
	fake_sailfish_watch_signal_queue(w, WATCH_SIGNAL_SIM_STATE_CHANGED);
	fake_sailfish_watch_emit_queued_signals(w);
	g_assert(!g_strcmp0(si->spn, TEST_DEFAULT_SPN));
	g_assert(count[SIM_INFO_SIGNAL_SPN_CHANGED] == 1);
	count[SIM_INFO_SIGNAL_SPN_CHANGED] = 0;

	/* Remove the SIM and insert it again */
	test_remove_sim(&sim, w);
	g_assert(count[SIM_INFO_SIGNAL_ICCID_CHANGED] == 1);
	g_assert(count[SIM_INFO_SIGNAL_IMSI_CHANGED] == 1);
	g_assert(count[SIM_INFO_SIGNAL_SPN_CHANGED] == 1);
	g_assert(!si->iccid);
	g_assert(!si->imsi);
	g_assert(!si->spn);
	memset(count, 0, sizeof(count));

	sim.state = OFONO_SIM_STATE_INSERTED;
	fake_sailfish_watch_signal_queue(w, WATCH_SIGNAL_SIM_STATE_CHANGED);
	fake_sailfish_watch_emit_queued_signals(w);
	g_assert(!count[SIM_INFO_SIGNAL_ICCID_CHANGED]);
	g_assert(!count[SIM_INFO_SIGNAL_IMSI_CHANGED]);
	g_assert(!count[SIM_INFO_SIGNAL_SPN_CHANGED]);

	sim.mcc = TEST_MCC;
	sim.mnc = TEST_MNC;
	sim.state = OFONO_SIM_STATE_READY;
	fake_sailfish_watch_signal_queue(w, WATCH_SIGNAL_SIM_STATE_CHANGED);
	fake_sailfish_watch_set_ofono_iccid(w, TEST_ICCID);
	fake_sailfish_watch_emit_queued_signals(w);

	/* IMSI gets loaded from the cache file */
	g_assert(!g_strcmp0(si->iccid, TEST_ICCID));
	g_assert(!g_strcmp0(si->imsi, TEST_IMSI));
	g_assert(!g_strcmp0(si->spn, TEST_DEFAULT_SPN));
	g_assert(count[SIM_INFO_SIGNAL_ICCID_CHANGED] == 1);
	g_assert(count[SIM_INFO_SIGNAL_IMSI_CHANGED] == 1);
	g_assert(count[SIM_INFO_SIGNAL_SPN_CHANGED] == 1);
	memset(count, 0, sizeof(count));

	/* Replace default SPN with the real one */
	fake_sailfish_watch_set_ofono_spn(w, TEST_SPN);
	fake_sailfish_watch_emit_queued_signals(w);
	g_assert(!g_strcmp0(si->spn, TEST_SPN));
	g_assert(!count[SIM_INFO_SIGNAL_ICCID_CHANGED]);
	g_assert(!count[SIM_INFO_SIGNAL_IMSI_CHANGED]);
	g_assert(count[SIM_INFO_SIGNAL_SPN_CHANGED] == 1);
	count[SIM_INFO_SIGNAL_SPN_CHANGED] = 0;
	/* Cache file appears */
	g_assert(stat(SIM_CACHE, &st) == 0);
	g_assert(S_ISREG(st.st_mode));

	/* Stray events have no effect */
	fake_sailfish_watch_signal_queue(w, WATCH_SIGNAL_SPN_CHANGED);
	fake_sailfish_watch_signal_queue(w, WATCH_SIGNAL_IMSI_CHANGED);
	fake_sailfish_watch_signal_queue(w, WATCH_SIGNAL_ICCID_CHANGED);
	fake_sailfish_watch_signal_queue(w, WATCH_SIGNAL_SIM_STATE_CHANGED);
	fake_sailfish_watch_emit_queued_signals(w);
	g_assert(!count[SIM_INFO_SIGNAL_ICCID_CHANGED]);
	g_assert(!count[SIM_INFO_SIGNAL_IMSI_CHANGED]);
	g_assert(!count[SIM_INFO_SIGNAL_SPN_CHANGED]);

	/* Empty SPN and IMSI are ignored too */
	fake_sailfish_watch_set_ofono_imsi(w, "");
	fake_sailfish_watch_set_ofono_spn(w, "");
	fake_sailfish_watch_emit_queued_signals(w);
	g_assert(!count[SIM_INFO_SIGNAL_ICCID_CHANGED]);
	g_assert(!count[SIM_INFO_SIGNAL_IMSI_CHANGED]);
	g_assert(!count[SIM_INFO_SIGNAL_SPN_CHANGED]);

	/* Reset the information */
	test_remove_sim(&sim, w);
	g_assert(count[SIM_INFO_SIGNAL_ICCID_CHANGED] == 1);
	g_assert(count[SIM_INFO_SIGNAL_IMSI_CHANGED] == 1);
	g_assert(count[SIM_INFO_SIGNAL_SPN_CHANGED] == 1);
	g_assert(!si->iccid);
	g_assert(!si->imsi);
	g_assert(!si->spn);
	memset(count, 0, sizeof(count));

	/* Set ICCID again, that will load the cached information */
	sim.mcc = NULL;
	sim.mnc = NULL;
	sim.state = OFONO_SIM_STATE_INSERTED;
	fake_sailfish_watch_signal_queue(w, WATCH_SIGNAL_ICCID_CHANGED);
	fake_sailfish_watch_signal_queue(w, WATCH_SIGNAL_SIM_STATE_CHANGED);
	fake_sailfish_watch_set_ofono_iccid(w, TEST_ICCID);
	fake_sailfish_watch_emit_queued_signals(w);
	g_assert(!g_strcmp0(si->iccid, TEST_ICCID));
	g_assert(!g_strcmp0(si->imsi, TEST_IMSI));
	g_assert(!g_strcmp0(si->spn, TEST_SPN));
	g_assert(count[SIM_INFO_SIGNAL_ICCID_CHANGED] == 1);
	g_assert(count[SIM_INFO_SIGNAL_IMSI_CHANGED] == 1);
	g_assert(count[SIM_INFO_SIGNAL_SPN_CHANGED] == 1);
	memset(count, 0, sizeof(count));

	/* Replace the SIM with a different one */
	test_remove_sim(&sim, w);
	g_assert(count[SIM_INFO_SIGNAL_ICCID_CHANGED] == 1);
	g_assert(count[SIM_INFO_SIGNAL_IMSI_CHANGED] == 1);
	g_assert(count[SIM_INFO_SIGNAL_SPN_CHANGED] == 1);
	g_assert(!si->iccid);
	g_assert(!si->imsi);
	g_assert(!si->spn);
	memset(count, 0, sizeof(count));

	sim.state = OFONO_SIM_STATE_INSERTED;
	fake_sailfish_watch_signal_queue(w, WATCH_SIGNAL_SIM_STATE_CHANGED);
	fake_sailfish_watch_emit_queued_signals(w);
	g_assert(!count[SIM_INFO_SIGNAL_ICCID_CHANGED]);
	g_assert(!count[SIM_INFO_SIGNAL_IMSI_CHANGED]);
	g_assert(!count[SIM_INFO_SIGNAL_SPN_CHANGED]);

	sim.mcc = TEST_MCC;
	sim.mnc = TEST_MNC;
	sim.state = OFONO_SIM_STATE_READY;
	fake_sailfish_watch_signal_queue(w, WATCH_SIGNAL_SIM_STATE_CHANGED);
	fake_sailfish_watch_set_ofono_iccid(w, TEST_ICCID_1);
	fake_sailfish_watch_set_ofono_imsi(w, TEST_IMSI_1);

	fake_sailfish_watch_emit_queued_signals(w);
	g_assert(!g_strcmp0(si->iccid, TEST_ICCID_1));
	g_assert(!g_strcmp0(si->imsi, TEST_IMSI_1));
	g_assert(!g_strcmp0(si->spn, TEST_DEFAULT_SPN));
	g_assert(count[SIM_INFO_SIGNAL_ICCID_CHANGED] == 1);
	g_assert(count[SIM_INFO_SIGNAL_IMSI_CHANGED] == 1);
	g_assert(count[SIM_INFO_SIGNAL_SPN_CHANGED] == 1);
	memset(count, 0, sizeof(count));

	/* And then insert back the previous one */
	test_remove_sim(&sim, w);
	memset(count, 0, sizeof(count));

	sim.state = OFONO_SIM_STATE_INSERTED;
	fake_sailfish_watch_signal_queue(w, WATCH_SIGNAL_SIM_STATE_CHANGED);
	fake_sailfish_watch_emit_queued_signals(w);

	sim.mcc = TEST_MCC;
	sim.mnc = TEST_MNC;
	sim.state = OFONO_SIM_STATE_READY;
	fake_sailfish_watch_signal_queue(w, WATCH_SIGNAL_SIM_STATE_CHANGED);
	fake_sailfish_watch_set_ofono_iccid(w, TEST_ICCID);
	fake_sailfish_watch_set_ofono_imsi(w, TEST_IMSI);

	fake_sailfish_watch_emit_queued_signals(w);
	g_assert(!g_strcmp0(si->iccid, TEST_ICCID));
	g_assert(!g_strcmp0(si->imsi, TEST_IMSI));
	g_assert(!g_strcmp0(si->spn, TEST_SPN));
	g_assert(count[SIM_INFO_SIGNAL_ICCID_CHANGED] == 1);
	g_assert(count[SIM_INFO_SIGNAL_IMSI_CHANGED] == 1);
	g_assert(count[SIM_INFO_SIGNAL_SPN_CHANGED] == 1);
	memset(count, 0, sizeof(count));

	/* Make sure that removed handler doesn't get invoked */
	sailfish_sim_info_remove_handler(si, id[SIM_INFO_SIGNAL_SPN_CHANGED]);
	id[SIM_INFO_SIGNAL_SPN_CHANGED] = 0;
	sim.mcc = NULL;
	sim.mnc = NULL;
	sim.state = OFONO_SIM_STATE_NOT_PRESENT;
	fake_sailfish_watch_signal_queue(w, WATCH_SIGNAL_IMSI_CHANGED);
	fake_sailfish_watch_signal_queue(w, WATCH_SIGNAL_SIM_STATE_CHANGED);
	fake_sailfish_watch_set_ofono_iccid(w, NULL);
	fake_sailfish_watch_set_ofono_imsi(w, NULL);
	fake_sailfish_watch_set_ofono_spn(w, NULL);
	fake_sailfish_watch_emit_queued_signals(w);
	g_assert(count[SIM_INFO_SIGNAL_ICCID_CHANGED] == 1);
	g_assert(count[SIM_INFO_SIGNAL_IMSI_CHANGED] == 1);
	g_assert(!count[SIM_INFO_SIGNAL_SPN_CHANGED]); /* removed ^ */
	memset(count, 0, sizeof(count));

	sailfish_sim_info_remove_handlers(si, id, G_N_ELEMENTS(id));
	sailfish_sim_info_unref(si);
	sailfish_watch_unref(w);
}

static void test_netreg(void)
{
	struct sailfish_sim_info *si;
	struct sailfish_watch *w = sailfish_watch_new(TEST_PATH);
	struct ofono_sim sim;
	struct ofono_netreg netreg;
	struct stat st;
	gulong id[SIM_INFO_SIGNAL_COUNT];
	int count[SIM_INFO_SIGNAL_COUNT];

	memset(id, 0, sizeof(id));
	memset(count, 0, sizeof(count));

	memset(&netreg, 0, sizeof(netreg));
	netreg.technology = OFONO_RADIO_ACCESS_MODE_GSM;
	netreg.status = NETWORK_REGISTRATION_STATUS_NOT_REGISTERED;
	netreg.status_watches = __ofono_watchlist_new(g_free);

	memset(&sim, 0, sizeof(sim));
	sim.mcc = TEST_MCC;
	sim.mnc = TEST_MNC;
	sim.state = OFONO_SIM_STATE_READY;

	rmdir_r(STORAGEDIR);
	si = sailfish_sim_info_new(TEST_PATH);
	id[SIM_INFO_SIGNAL_ICCID_CHANGED] =
		sailfish_sim_info_add_iccid_changed_handler(si,
			test_signal_count_cb, count +
			SIM_INFO_SIGNAL_ICCID_CHANGED);
	id[SIM_INFO_SIGNAL_IMSI_CHANGED] =
		sailfish_sim_info_add_imsi_changed_handler(si,
			test_signal_count_cb, count +
			SIM_INFO_SIGNAL_IMSI_CHANGED);
	id[SIM_INFO_SIGNAL_SPN_CHANGED] =
		sailfish_sim_info_add_spn_changed_handler(si,
			test_signal_count_cb, count +
			SIM_INFO_SIGNAL_SPN_CHANGED);

	fake_sailfish_watch_set_ofono_sim(w, &sim);
	fake_sailfish_watch_set_ofono_iccid(w, TEST_ICCID);
	fake_sailfish_watch_set_ofono_imsi(w, TEST_IMSI);
	fake_sailfish_watch_emit_queued_signals(w);
	g_assert(!g_strcmp0(si->iccid, TEST_ICCID));
	g_assert(!g_strcmp0(si->imsi, TEST_IMSI));
	g_assert(!g_strcmp0(si->spn, TEST_DEFAULT_SPN));
	g_assert(count[SIM_INFO_SIGNAL_ICCID_CHANGED] == 1);
	g_assert(count[SIM_INFO_SIGNAL_IMSI_CHANGED] == 1);
	g_assert(count[SIM_INFO_SIGNAL_SPN_CHANGED] == 1);
	memset(count, 0, sizeof(count));

	g_assert(stat(ICCID_MAP, &st) == 0);
	g_assert(S_ISREG(st.st_mode));
	/* Default SPN doesn't get cached */
	g_assert(stat(SIM_CACHE, &st) < 0);

	fake_sailfish_watch_set_ofono_netreg(w, &netreg);
	fake_sailfish_watch_emit_queued_signals(w);
	g_assert(!count[SIM_INFO_SIGNAL_ICCID_CHANGED]);
	g_assert(!count[SIM_INFO_SIGNAL_IMSI_CHANGED]);
	g_assert(!count[SIM_INFO_SIGNAL_SPN_CHANGED]);

	/* Simulate home registation */
	netreg.mcc = TEST_MCC;
	netreg.mnc = TEST_MNC;
	netreg.name = TEST_SPN;
	netreg.status = NETWORK_REGISTRATION_STATUS_REGISTERED;
	netreg_notify_status_watches(&netreg);
	g_assert(!g_strcmp0(si->spn, TEST_SPN));
	g_assert(count[SIM_INFO_SIGNAL_SPN_CHANGED] == 1);
	/* This one does get cached */
	g_assert(stat(SIM_CACHE, &st) == 0);
	g_assert(S_ISREG(st.st_mode));

	fake_sailfish_watch_set_ofono_netreg(w, NULL);
	fake_sailfish_watch_emit_queued_signals(w);

	__ofono_watchlist_free(netreg.status_watches);
	sailfish_sim_info_remove_handlers(si, id, G_N_ELEMENTS(id));
	sailfish_sim_info_unref(si);
	sailfish_watch_unref(w);
}

#define TEST_(name) "/sailfish_sim_info/" name

int main(int argc, char *argv[])
{
	g_test_init(&argc, &argv, NULL);

	gutil_log_timestamp = FALSE;
	gutil_log_default.level = g_test_verbose() ?
		GLOG_LEVEL_VERBOSE : GLOG_LEVEL_NONE;
	__ofono_log_init("test-sailfish_sim_info",
				g_test_verbose() ? "*" : NULL,
				FALSE, FALSE);

	g_test_add_func(TEST_("basic"), test_basic);
	g_test_add_func(TEST_("cache"), test_cache);
	g_test_add_func(TEST_("netreg"), test_netreg);

	return g_test_run();
}

/*
 * Local Variables:
 * mode: C
 * c-basic-offset: 8
 * indent-tabs-mode: t
 * End:
 */
