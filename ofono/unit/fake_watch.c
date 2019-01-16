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

#include "fake_watch.h"

#include "ofono.h"

#include <gutil_log.h>
#include <gutil_macros.h>
#include <gutil_misc.h>

#include <glib-object.h>

typedef GObjectClass FakeOfonoWatchClass;
typedef struct fake_ofono_watch FakeOfonoWatch;

struct fake_ofono_watch {
	GObject object;
	struct ofono_watch pub;
	char *path;
	char *iccid;
	char *imsi;
	char *spn;
	int queued_signals;
};

struct fake_ofono_watch_closure {
	GCClosure cclosure;
	ofono_watch_cb_t cb;
	void *user_data;
};

#define SIGNAL_MODEM_CHANGED_NAME       "ofono-watch-modem-changed"
#define SIGNAL_ONLINE_CHANGED_NAME      "ofono-watch-online-changed"
#define SIGNAL_SIM_CHANGED_NAME         "ofono-watch-sim-changed"
#define SIGNAL_SIM_STATE_CHANGED_NAME   "ofono-watch-sim-state-changed"
#define SIGNAL_ICCID_CHANGED_NAME       "ofono-watch-iccid-changed"
#define SIGNAL_IMSI_CHANGED_NAME        "ofono-watch-imsi-changed"
#define SIGNAL_SPN_CHANGED_NAME         "ofono-watch-spn-changed"
#define SIGNAL_NETREG_CHANGED_NAME      "ofono-watch-netreg-changed"

static guint fake_ofono_watch_signals[FAKE_WATCH_SIGNAL_COUNT] = { 0 };
static GHashTable *fake_ofono_watch_table = NULL;

G_DEFINE_TYPE(FakeOfonoWatch, fake_ofono_watch, G_TYPE_OBJECT)
#define FAKE_OFONO_WATCH_TYPE (fake_ofono_watch_get_type())
#define FAKE_OFONO_WATCH(obj) (G_TYPE_CHECK_INSTANCE_CAST((obj),\
	FAKE_OFONO_WATCH_TYPE, FakeOfonoWatch))

#define NEW_SIGNAL(klass,name) \
	fake_ofono_watch_signals[FAKE_WATCH_SIGNAL_##name##_CHANGED] = \
		g_signal_new(SIGNAL_##name##_CHANGED_NAME, \
			G_OBJECT_CLASS_TYPE(klass), G_SIGNAL_RUN_FIRST, \
			0, NULL, NULL, NULL, G_TYPE_NONE, 0)

#define DBG_(obj,fmt,args...) DBG("%s " fmt, (obj)->path+1, ##args)

static inline struct fake_ofono_watch *fake_ofono_watch_cast
						(struct ofono_watch *watch)
{
	return watch ?
		FAKE_OFONO_WATCH(G_CAST(watch, struct fake_ofono_watch, pub)) :
		NULL;
}

static inline int fake_ofono_watch_signal_bit(enum fake_watch_signal id)
{
	return (1 << id);
}

static inline void fake_ofono_watch_signal_emit(struct fake_ofono_watch *self,
					enum fake_watch_signal id)
{
	self->queued_signals &= ~fake_ofono_watch_signal_bit(id);
	g_signal_emit(self, fake_ofono_watch_signals[id], 0);
}

void fake_watch_signal_queue(struct ofono_watch *watch,
					enum fake_watch_signal id)
{
	struct fake_ofono_watch *self = fake_ofono_watch_cast(watch);

	self->queued_signals |= fake_ofono_watch_signal_bit(id);
}

void fake_watch_emit_queued_signals(struct ofono_watch *watch)
{
	struct fake_ofono_watch *self = fake_ofono_watch_cast(watch);
	int i;

	for (i = 0; self->queued_signals && i < FAKE_WATCH_SIGNAL_COUNT; i++) {
		if (self->queued_signals & fake_ofono_watch_signal_bit(i)) {
			fake_ofono_watch_signal_emit(self, i);
		}
	}
}

void fake_watch_set_ofono_iccid(struct ofono_watch *watch, const char *iccid)
{
	struct fake_ofono_watch *self = fake_ofono_watch_cast(watch);

	if (g_strcmp0(self->iccid, iccid)) {
		g_free(self->iccid);
		watch->iccid = self->iccid = g_strdup(iccid);
		fake_watch_signal_queue(watch, FAKE_WATCH_SIGNAL_ICCID_CHANGED);
	}
}

void fake_watch_set_ofono_imsi(struct ofono_watch *watch, const char *imsi)
{
	struct fake_ofono_watch *self = fake_ofono_watch_cast(watch);

	if (g_strcmp0(self->imsi, imsi)) {
		g_free(self->imsi);
		watch->imsi = self->imsi = g_strdup(imsi);
		fake_watch_signal_queue(watch, FAKE_WATCH_SIGNAL_IMSI_CHANGED);
	}
}

void fake_watch_set_ofono_spn(struct ofono_watch *watch, const char *spn)
{
	struct fake_ofono_watch *self = fake_ofono_watch_cast(watch);

	if (g_strcmp0(self->spn, spn)) {
		g_free(self->spn);
		watch->spn = self->spn = g_strdup(spn);
		fake_watch_signal_queue(watch, FAKE_WATCH_SIGNAL_SPN_CHANGED);
	}
}

void fake_watch_set_ofono_sim(struct ofono_watch *watch,
						struct ofono_sim *sim)
{
	if (watch->sim != sim) {
		watch->sim = sim;
		fake_watch_signal_queue(watch, FAKE_WATCH_SIGNAL_SIM_CHANGED);
		if (!sim) {
			fake_watch_set_ofono_iccid(watch, NULL);
			fake_watch_set_ofono_imsi(watch, NULL);
			fake_watch_set_ofono_spn(watch, NULL);
		}
	}
}

void fake_watch_set_ofono_netreg(struct ofono_watch *watch,
						struct ofono_netreg *netreg)
{
	if (watch->netreg != netreg) {
		watch->netreg = netreg;
		fake_watch_signal_queue(watch,
					FAKE_WATCH_SIGNAL_NETREG_CHANGED);
	}
}

static void fake_ofono_watch_initialize(struct fake_ofono_watch *self,
							const char *path)
{
	self->pub.path = self->path = g_strdup(path);
}

static void fake_ofono_watch_destroyed(gpointer key, GObject *obj)
{
	GASSERT(fake_ofono_watch_table);
	DBG("%s", (char*)key);
	if (fake_ofono_watch_table) {
		GASSERT(g_hash_table_lookup(fake_ofono_watch_table,key) == obj);
		g_hash_table_remove(fake_ofono_watch_table, key);
		if (g_hash_table_size(fake_ofono_watch_table) == 0) {
			g_hash_table_unref(fake_ofono_watch_table);
			fake_ofono_watch_table = NULL;
		}
	}
}

struct ofono_watch *ofono_watch_new(const char *path)
{
	if (path) {
		struct fake_ofono_watch *self = NULL;

		if (fake_ofono_watch_table) {
			self = g_hash_table_lookup(fake_ofono_watch_table,
									path);
		}
		if (self) {
			g_object_ref(self);
		} else {
			char *key = g_strdup(path);

			self = g_object_new(FAKE_OFONO_WATCH_TYPE, NULL);
			fake_ofono_watch_initialize(self, path);
			if (!fake_ofono_watch_table) {
				/* Create the table on demand */
				fake_ofono_watch_table =
					g_hash_table_new_full(g_str_hash,
						g_str_equal, g_free, NULL);
			}
			g_hash_table_replace(fake_ofono_watch_table, key, self);
			g_object_weak_ref(G_OBJECT(self),
					fake_ofono_watch_destroyed, key);
			DBG_(self, "created");
		}
		return &self->pub;
	}
	return NULL;
}

struct ofono_watch *ofono_watch_ref(struct ofono_watch *self)
{
	if (self) {
		g_object_ref(fake_ofono_watch_cast(self));
	}
	return self;
}

void ofono_watch_unref(struct ofono_watch *self)
{
	if (self) {
		g_object_unref(fake_ofono_watch_cast(self));
	}
}

static void fake_watch_signal_cb(struct fake_ofono_watch *source,
				struct fake_ofono_watch_closure *closure)
{
	closure->cb(&source->pub, closure->user_data);
}

static unsigned long fake_watch_add_signal_handler(struct ofono_watch *watch,
	enum fake_watch_signal signal, ofono_watch_cb_t cb, void *user_data)
{
	if (watch && cb) {
		struct fake_ofono_watch *self = fake_ofono_watch_cast(watch);
		struct fake_ofono_watch_closure *closure =
			(struct fake_ofono_watch_closure *)g_closure_new_simple
				(sizeof(struct fake_ofono_watch_closure), NULL);

		closure->cclosure.closure.data = closure;
		closure->cclosure.callback = G_CALLBACK(fake_watch_signal_cb);
		closure->cb = cb;
		closure->user_data = user_data;

		return g_signal_connect_closure_by_id(self,
					fake_ofono_watch_signals[signal], 0,
					&closure->cclosure.closure, FALSE);
	}
	return 0;
}

unsigned long ofono_watch_add_modem_changed_handler(struct ofono_watch *watch,
				ofono_watch_cb_t cb, void *user_data)
{
	return fake_watch_add_signal_handler(watch,
			FAKE_WATCH_SIGNAL_MODEM_CHANGED, cb, user_data);
}

unsigned long ofono_watch_add_online_changed_handler(struct ofono_watch *watch,
				ofono_watch_cb_t cb, void *user_data)
{
	return fake_watch_add_signal_handler(watch,
			FAKE_WATCH_SIGNAL_ONLINE_CHANGED, cb, user_data);
}

unsigned long ofono_watch_add_sim_changed_handler(struct ofono_watch *watch,
				ofono_watch_cb_t cb, void *user_data)
{
	return fake_watch_add_signal_handler(watch,
			FAKE_WATCH_SIGNAL_SIM_CHANGED, cb, user_data);
}

unsigned long ofono_watch_add_sim_state_changed_handler
	(struct ofono_watch *watch, ofono_watch_cb_t cb, void *user_data)
{
	return fake_watch_add_signal_handler(watch,
			FAKE_WATCH_SIGNAL_SIM_STATE_CHANGED, cb, user_data);
}

unsigned long ofono_watch_add_iccid_changed_handler(struct ofono_watch *watch,
				ofono_watch_cb_t cb, void *user_data)
{
	return fake_watch_add_signal_handler(watch,
			FAKE_WATCH_SIGNAL_ICCID_CHANGED, cb, user_data);
}

unsigned long ofono_watch_add_imsi_changed_handler(struct ofono_watch *watch,
				ofono_watch_cb_t cb, void *user_data)
{
	return fake_watch_add_signal_handler(watch,
			FAKE_WATCH_SIGNAL_IMSI_CHANGED, cb, user_data);
}

unsigned long ofono_watch_add_spn_changed_handler(struct ofono_watch *watch,
				ofono_watch_cb_t cb, void *user_data)
{
	return fake_watch_add_signal_handler(watch,
			FAKE_WATCH_SIGNAL_SPN_CHANGED, cb, user_data);
}

unsigned long ofono_watch_add_netreg_changed_handler(struct ofono_watch *watch,
				ofono_watch_cb_t cb, void *user_data)
{
	return fake_watch_add_signal_handler(watch,
			FAKE_WATCH_SIGNAL_NETREG_CHANGED, cb, user_data);
}

void ofono_watch_remove_handler(struct ofono_watch *watch, unsigned long id)
{
	if (watch && id) {
		g_signal_handler_disconnect(fake_ofono_watch_cast(watch), id);
	}
}

void ofono_watch_remove_handlers(struct ofono_watch *watch, unsigned long *ids,
							unsigned int count)
{
	gutil_disconnect_handlers(fake_ofono_watch_cast(watch), ids, count);
}

static void fake_ofono_watch_init(struct fake_ofono_watch *self)
{
}

static void fake_ofono_watch_finalize(GObject *object)
{
	struct fake_ofono_watch *self = FAKE_OFONO_WATCH(object);

	g_free(self->path);
	g_free(self->iccid);
	g_free(self->imsi);
	g_free(self->spn);
	G_OBJECT_CLASS(fake_ofono_watch_parent_class)->finalize(object);
}

static void fake_ofono_watch_class_init(FakeOfonoWatchClass *klass)
{
	G_OBJECT_CLASS(klass)->finalize = fake_ofono_watch_finalize;
	NEW_SIGNAL(klass, MODEM);
	NEW_SIGNAL(klass, ONLINE);
	NEW_SIGNAL(klass, SIM);
	NEW_SIGNAL(klass, SIM_STATE);
	NEW_SIGNAL(klass, ICCID);
	NEW_SIGNAL(klass, IMSI);
	NEW_SIGNAL(klass, SPN);
	NEW_SIGNAL(klass, NETREG);
}

/*
 * Local Variables:
 * mode: C
 * c-basic-offset: 8
 * indent-tabs-mode: t
 * End:
 */
