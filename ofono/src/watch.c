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

#include <ofono/watch.h>

#include "ofono.h"

#include <glib-object.h>

typedef GObjectClass OfonoWatchObjectClass;
typedef struct ofono_watch_object OfonoWatchObject;

struct ofono_watch_object {
	GObject object;
	struct ofono_watch pub;
	char *path;
	char *iccid;
	char *imsi;
	char *spn;
	int signals_suspended;
	int queued_signals;
	guint modem_watch_id;
	guint online_watch_id;
	guint sim_watch_id;
	guint sim_state_watch_id;
	guint iccid_watch_id;
	guint imsi_watch_id;
	guint spn_watch_id;
	guint netreg_watch_id;
};

struct ofono_watch_closure {
	GCClosure cclosure;
	ofono_watch_cb_t cb;
	void *user_data;
};

enum ofono_watch_signal {
	SIGNAL_MODEM_CHANGED,
	SIGNAL_ONLINE_CHANGED,
	SIGNAL_SIM_CHANGED,
	SIGNAL_SIM_STATE_CHANGED,
	SIGNAL_ICCID_CHANGED,
	SIGNAL_IMSI_CHANGED,
	SIGNAL_SPN_CHANGED,
	SIGNAL_NETREG_CHANGED,
	SIGNAL_COUNT
};

#define SIGNAL_MODEM_CHANGED_NAME       "ofono-watch-modem-changed"
#define SIGNAL_ONLINE_CHANGED_NAME      "ofono-watch-online-changed"
#define SIGNAL_SIM_CHANGED_NAME         "ofono-watch-sim-changed"
#define SIGNAL_SIM_STATE_CHANGED_NAME   "ofono-watch-sim-state-changed"
#define SIGNAL_ICCID_CHANGED_NAME       "ofono-watch-iccid-changed"
#define SIGNAL_IMSI_CHANGED_NAME        "ofono-watch-imsi-changed"
#define SIGNAL_SPN_CHANGED_NAME         "ofono-watch-spn-changed"
#define SIGNAL_NETREG_CHANGED_NAME      "ofono-watch-netreg-changed"

static guint ofono_watch_signals[SIGNAL_COUNT] = { 0 };
static GHashTable *ofono_watch_table = NULL;

G_DEFINE_TYPE(OfonoWatchObject, ofono_watch_object, G_TYPE_OBJECT)
#define OFONO_WATCH_OBJECT_TYPE (ofono_watch_object_get_type())
#define OFONO_WATCH_OBJECT(obj) (G_TYPE_CHECK_INSTANCE_CAST((obj),\
	OFONO_WATCH_OBJECT_TYPE, OfonoWatchObject))

#define NEW_SIGNAL(klass,name) \
	ofono_watch_signals[SIGNAL_##name##_CHANGED] = \
		g_signal_new(SIGNAL_##name##_CHANGED_NAME, \
			G_OBJECT_CLASS_TYPE(klass), G_SIGNAL_RUN_FIRST, \
			0, NULL, NULL, NULL, G_TYPE_NONE, 0)

/* Skip the leading slash from the modem path: */
#define DBG_(obj,fmt,args...) DBG("%s " fmt, (obj)->path+1, ##args)
#define ASSERT(expr) ((void)(expr))

static inline struct ofono_watch_object *ofono_watch_object_cast
						(struct ofono_watch *watch)
{
	return watch ? OFONO_WATCH_OBJECT(G_STRUCT_MEMBER_P(watch,
		- G_STRUCT_OFFSET(struct ofono_watch_object, pub))) : NULL;
}

static inline int ofono_watch_signal_bit(enum ofono_watch_signal id)
{
	return (1 << id);
}

static inline void ofono_watch_signal_emit(struct ofono_watch_object *self,
					enum ofono_watch_signal id)
{
	self->queued_signals &= ~ofono_watch_signal_bit(id);
	g_signal_emit(self, ofono_watch_signals[id], 0);
}

static inline void ofono_watch_signal_queue(struct ofono_watch_object *self,
					enum ofono_watch_signal id)
{
	self->queued_signals |= ofono_watch_signal_bit(id);
}

static void ofono_watch_emit_queued_signals(struct ofono_watch_object *self)
{
	if (self->signals_suspended < 1) {
		int i;

		for (i = 0; self->queued_signals && i < SIGNAL_COUNT; i++) {
			if (self->queued_signals & ofono_watch_signal_bit(i)) {
				ofono_watch_signal_emit(self, i);
			}
		}
	}
}

static inline void ofono_watch_suspend_signals(struct ofono_watch_object *self)
{
	self->signals_suspended++;
}

static inline void ofono_watch_resume_signals(struct ofono_watch_object *self)
{
	ASSERT(self->signals_suspended > 0);
	self->signals_suspended--;
	ofono_watch_emit_queued_signals(self);
}

static void ofono_watch_iccid_update(struct ofono_watch_object *self,
							const char *iccid)
{
	if (g_strcmp0(self->iccid, iccid)) {
		g_free(self->iccid);
		self->pub.iccid = self->iccid = g_strdup(iccid);
		ofono_watch_signal_queue(self, SIGNAL_ICCID_CHANGED);
	}
}

static void ofono_watch_iccid_notify(const char *iccid, void *user_data)
{
	struct ofono_watch_object *self = OFONO_WATCH_OBJECT(user_data);

	ofono_watch_iccid_update(self, iccid);
	ofono_watch_emit_queued_signals(self);
}

static void ofono_watch_iccid_destroy(void *user_data)
{
	struct ofono_watch_object *self = OFONO_WATCH_OBJECT(user_data);

	ASSERT(self->iccid_watch_id);
	self->iccid_watch_id = 0;
}

static void ofono_watch_spn_update(struct ofono_watch_object *self,
							const char *spn)
{
	if (g_strcmp0(self->spn, spn)) {
		g_free(self->spn);
		self->pub.spn = self->spn = g_strdup(spn);
		ofono_watch_signal_queue(self, SIGNAL_SPN_CHANGED);
	}
}

static void ofono_watch_spn_notify(const char *spn, const char *dc,
							void *user_data)
{
	struct ofono_watch_object *self = OFONO_WATCH_OBJECT(user_data);

	ofono_watch_spn_update(self, spn);
	ofono_watch_emit_queued_signals(self);
}

static void ofono_watch_spn_destroy(void *user_data)
{
	struct ofono_watch_object *self = OFONO_WATCH_OBJECT(user_data);

	ASSERT(self->spn_watch_id);
	self->spn_watch_id = 0;
}

static void ofono_watch_imsi_update(struct ofono_watch_object *self,
							const char *imsi)
{
	if (g_strcmp0(self->imsi, imsi)) {
		struct ofono_watch *watch = &self->pub;

		g_free(self->imsi);
		watch->imsi = self->imsi = g_strdup(imsi);
		ofono_watch_signal_queue(self, SIGNAL_IMSI_CHANGED);
		/* ofono core crashes if we add spn watch too early */
		if (imsi) {
			ofono_sim_add_spn_watch(watch->sim,
					&self->spn_watch_id,
					ofono_watch_spn_notify, self,
					ofono_watch_spn_destroy);
		}
	}
}

static void ofono_watch_imsi_notify(const char *imsi, void *user_data)
{
	struct ofono_watch_object *self = OFONO_WATCH_OBJECT(user_data);

	ofono_watch_imsi_update(self, imsi);
	ofono_watch_emit_queued_signals(self);
}

static void ofono_watch_imsi_destroy(void *user_data)
{
	struct ofono_watch_object *self = OFONO_WATCH_OBJECT(user_data);

	ASSERT(self->imsi_watch_id);
	self->imsi_watch_id = 0;
}

static void ofono_watch_sim_state_notify(enum ofono_sim_state new_state,
							void *user_data)
{
	struct ofono_watch_object *self = OFONO_WATCH_OBJECT(user_data);

	/*
	 * ofono core doesn't notify SIM watches when SIM card gets removed.
	 * So we have to reset things here based on the SIM state.
	 */
	if (new_state == OFONO_SIM_STATE_NOT_PRESENT) {
		ofono_watch_iccid_update(self, NULL);
	}
	if (new_state != OFONO_SIM_STATE_READY) {
		ofono_watch_imsi_update(self, NULL);
		ofono_watch_spn_update(self, NULL);
	}
	ofono_watch_signal_queue(self, SIGNAL_SIM_STATE_CHANGED);
	ofono_watch_emit_queued_signals(self);
}

static void ofono_watch_sim_state_destroy(void *user_data)
{
	struct ofono_watch_object *self = OFONO_WATCH_OBJECT(user_data);

	ASSERT(self->sim_state_watch_id);
	self->sim_state_watch_id = 0;
}

static void ofono_watch_set_sim(struct ofono_watch_object *self,
						struct ofono_sim *sim)
{
	struct ofono_watch *watch = &self->pub;

	if (watch->sim != sim) {
		if (self->sim_state_watch_id) {
			ofono_sim_remove_state_watch(watch->sim,
						self->sim_state_watch_id);
			/* The destroy callback clears it */
			ASSERT(!self->sim_state_watch_id);
		}
		if (self->iccid_watch_id) {
			ofono_sim_remove_iccid_watch(watch->sim,
						self->iccid_watch_id);
			/* The destroy callback clears it */
			ASSERT(!self->iccid_watch_id);
		}
		if (self->imsi_watch_id) {
			ofono_sim_remove_imsi_watch(watch->sim,
						self->imsi_watch_id);
			/* The destroy callback clears it */
			ASSERT(!self->imsi_watch_id);
		}
		if (self->spn_watch_id) {
			ofono_sim_remove_spn_watch(watch->sim,
						&self->spn_watch_id);
			/* The destroy callback clears it */
			ASSERT(!self->spn_watch_id);
		}
		watch->sim = sim;
		ofono_watch_signal_queue(self, SIGNAL_SIM_CHANGED);
		ofono_watch_suspend_signals(self);

		/* Reset the current state */
		ofono_watch_iccid_update(self, NULL);
		ofono_watch_imsi_update(self, NULL);
		ofono_watch_spn_update(self, NULL);
		if (sim) {
			self->sim_state_watch_id =
				ofono_sim_add_state_watch(sim,
					ofono_watch_sim_state_notify, self,
					ofono_watch_sim_state_destroy);
			/*
			 * Unlike ofono_sim_add_state_watch, the rest
			 * of ofono_sim_add_xxx_watch functions call the
			 * notify callback if the value is already known
			 * to the ofono core.
			 *
			 * Also note that ofono core crashes if we add
			 * spn watch too early.
			 */
			self->iccid_watch_id =
				ofono_sim_add_iccid_watch(sim,
					ofono_watch_iccid_notify, self,
					ofono_watch_iccid_destroy);
			self->imsi_watch_id =
				ofono_sim_add_imsi_watch(sim,
					ofono_watch_imsi_notify, self,
					ofono_watch_imsi_destroy);
		}

		/* Emit the pending signals. */
		ofono_watch_resume_signals(self);
	}
}

static void ofono_watch_sim_notify(struct ofono_atom *atom,
			enum ofono_atom_watch_condition cond, void *user_data)
{
	struct ofono_watch_object *self = OFONO_WATCH_OBJECT(user_data);

	if (cond == OFONO_ATOM_WATCH_CONDITION_REGISTERED) {
		struct ofono_sim *sim = __ofono_atom_get_data(atom);

		DBG_(self, "sim registered");
		ofono_watch_set_sim(self, sim);
	} else if (cond == OFONO_ATOM_WATCH_CONDITION_UNREGISTERED) {
		DBG_(self, "sim unregistered");
		ofono_watch_set_sim(self, NULL);
	}
}

static void ofono_watch_sim_destroy(void *user_data)
{
	struct ofono_watch_object *self = OFONO_WATCH_OBJECT(user_data);

	self->sim_watch_id = 0;
}

static void ofono_watch_set_netreg(struct ofono_watch_object *self,
						struct ofono_netreg *netreg)
{
	struct ofono_watch *watch = &self->pub;

	if (watch->netreg != netreg) {
		watch->netreg = netreg;
		ofono_watch_signal_emit(self, SIGNAL_NETREG_CHANGED);
	}
}

static void ofono_watch_netreg_notify(struct ofono_atom *atom,
			enum ofono_atom_watch_condition cond, void *user_data)
{
	struct ofono_watch_object *self = OFONO_WATCH_OBJECT(user_data);

	if (cond == OFONO_ATOM_WATCH_CONDITION_REGISTERED) {
		struct ofono_netreg *netreg = __ofono_atom_get_data(atom);

		DBG_(self, "netreg registered");
		ofono_watch_set_netreg(self, netreg);
	} else if (cond == OFONO_ATOM_WATCH_CONDITION_UNREGISTERED) {
		DBG_(self, "netreg unregistered");
		ofono_watch_set_netreg(self, NULL);
	}
}

static void ofono_watch_netreg_destroy(void *user_data)
{
	struct ofono_watch_object *self = OFONO_WATCH_OBJECT(user_data);

	self->netreg_watch_id = 0;
}

static void ofono_watch_online_update(struct ofono_watch_object *self,
							gboolean online)
{
	struct ofono_watch *watch = &self->pub;

	if (watch->online != online) {
		watch->online = online;
		ofono_watch_signal_queue(self, SIGNAL_ONLINE_CHANGED);
	}
}

static void ofono_watch_online_notify(struct ofono_modem *modem,
					ofono_bool_t online, void *user_data)
{
	struct ofono_watch_object *self = OFONO_WATCH_OBJECT(user_data);

	ASSERT(self->pub.modem == modem);
	ASSERT(online == ofono_modem_get_online(modem));
	ofono_watch_online_update(self, online);
	ofono_watch_emit_queued_signals(self);
}

static void ofono_watch_online_destroy(void *user_data)
{
	struct ofono_watch_object *self = OFONO_WATCH_OBJECT(user_data);

	self->online_watch_id = 0;
}

static void ofono_watch_setup_modem(struct ofono_watch_object *self)
{
	struct ofono_watch *watch = &self->pub;

	ASSERT(!self->online_watch_id);
	self->online_watch_id =
		__ofono_modem_add_online_watch(watch->modem,
				ofono_watch_online_notify, self,
				ofono_watch_online_destroy);

	/* __ofono_modem_add_atom_watch() calls the notify callback if the
	 * atom is already registered */
	ASSERT(!self->sim_watch_id);
	self->sim_watch_id = __ofono_modem_add_atom_watch(watch->modem,
		OFONO_ATOM_TYPE_SIM, ofono_watch_sim_notify,
		self, ofono_watch_sim_destroy);

	ASSERT(!self->netreg_watch_id);
	self->netreg_watch_id = __ofono_modem_add_atom_watch(watch->modem,
		OFONO_ATOM_TYPE_NETREG, ofono_watch_netreg_notify,
		self, ofono_watch_netreg_destroy);
}

static void ofono_watch_cleanup_modem(struct ofono_watch_object *self,
						struct ofono_modem *modem)
{
	/* Caller checks the self->modem isn't NULL */
	if (self->online_watch_id) {
		__ofono_modem_remove_online_watch(modem, self->online_watch_id);
		ASSERT(!self->online_watch_id);
	}

	if (self->sim_watch_id) {
		__ofono_modem_remove_atom_watch(modem, self->sim_watch_id);
		ASSERT(!self->sim_watch_id);
	}

	if (self->netreg_watch_id) {
		__ofono_modem_remove_atom_watch(modem, self->netreg_watch_id);
		ASSERT(!self->netreg_watch_id);
	}

	ofono_watch_set_sim(self, NULL);
	ofono_watch_set_netreg(self, NULL);
}

static void ofono_watch_set_modem(struct ofono_watch_object *self,
						struct ofono_modem *modem)
{
	struct ofono_watch *watch = &self->pub;

	if (watch->modem != modem) {
		struct ofono_modem *old_modem = watch->modem;

		watch->modem = modem;
		ofono_watch_signal_queue(self, SIGNAL_MODEM_CHANGED);
		if (old_modem) {
			ofono_watch_cleanup_modem(self, old_modem);
		}
		if (modem) {
			ofono_watch_setup_modem(self);
		}
		ofono_watch_online_update(self, ofono_modem_get_online(modem));
		ofono_watch_emit_queued_signals(self);
	}
}

static void ofono_watch_modem_notify(struct ofono_modem *modem,
					gboolean added, void *user_data)
{
	struct ofono_watch_object *self = OFONO_WATCH_OBJECT(user_data);

	if (added) {
		if (!g_strcmp0(self->path, ofono_modem_get_path(modem))) {
			ofono_watch_set_modem(self, modem);
		}
	} else if (self->pub.modem == modem) {
		ofono_watch_set_modem(self, NULL);
	}
}

static void ofono_watch_modem_destroy(void *user_data)
{
	struct ofono_watch_object *self = OFONO_WATCH_OBJECT(user_data);

	self->modem_watch_id = 0;
}

static ofono_bool_t ofono_watch_modem_find(struct ofono_modem *modem,
							void *user_data)
{
	struct ofono_watch_object *self = OFONO_WATCH_OBJECT(user_data);

	if (!g_strcmp0(self->path, ofono_modem_get_path(modem))) {
		self->pub.modem = modem;
		ofono_watch_setup_modem(self);
		return TRUE;
	} else {
		return FALSE;
	}
}

static void ofono_watch_initialize(struct ofono_watch_object *self,
							const char *path)
{
	struct ofono_watch *watch = &self->pub;

	watch->path = self->path = g_strdup(path);
	ofono_modem_find(ofono_watch_modem_find, self);
	watch->online = ofono_modem_get_online(watch->modem);
	self->modem_watch_id =
		__ofono_modemwatch_add(ofono_watch_modem_notify, self,
					ofono_watch_modem_destroy);
}

static void ofono_watch_destroyed(gpointer key, GObject *obj)
{
	ASSERT(ofono_watch_table);
	DBG("%s", (char*)key);
	if (ofono_watch_table) {
		ASSERT(g_hash_table_lookup(ofono_watch_table, key) == obj);
		g_hash_table_remove(ofono_watch_table, key);
		if (g_hash_table_size(ofono_watch_table) == 0) {
			g_hash_table_unref(ofono_watch_table);
			ofono_watch_table = NULL;
		}
	}
}

struct ofono_watch *ofono_watch_new(const char *path)
{
	if (path) {
		struct ofono_watch_object *self = NULL;

		if (ofono_watch_table) {
			self = g_hash_table_lookup(ofono_watch_table, path);
		}
		if (self) {
			g_object_ref(self);
		} else {
			char *key = g_strdup(path);

			self = g_object_new(OFONO_WATCH_OBJECT_TYPE, NULL);
			ofono_watch_initialize(self, path);
			if (!ofono_watch_table) {
				/* Create the table on demand */
				ofono_watch_table =
					g_hash_table_new_full(g_str_hash,
						g_str_equal, g_free, NULL);
			}
			g_hash_table_replace(ofono_watch_table, key, self);
			g_object_weak_ref(G_OBJECT(self),
					ofono_watch_destroyed, key);
			DBG_(self, "created");
		}
		return &self->pub;
	}
	return NULL;
}

struct ofono_watch *ofono_watch_ref(struct ofono_watch *watch)
{
	if (watch) {
		g_object_ref(ofono_watch_object_cast(watch));
		return watch;
	} else {
		return NULL;
	}
}

void ofono_watch_unref(struct ofono_watch *watch)
{
	if (watch) {
		g_object_unref(ofono_watch_object_cast(watch));
	}
}

static void ofono_watch_signal_cb(struct ofono_watch_object *source,
					struct ofono_watch_closure *closure)
{
	closure->cb(&source->pub, closure->user_data);
}

static unsigned long ofono_watch_add_signal_handler(struct ofono_watch *watch,
	enum ofono_watch_signal signal, ofono_watch_cb_t cb, void *user_data)
{
	struct ofono_watch_object *self = ofono_watch_object_cast(watch);

	if (self && cb) {
		/*
		 * We can't directly connect the provided callback because
		 * it expects the first parameter to point to the public
		 * part of ofono_watch_object (i.e. ofono_watch) but glib
		 * will invoke it with ofono_watch_object as the first
		 * parameter. ofono_watch_signal_cb() will do the conversion.
		 */
		struct ofono_watch_closure *closure =
			(struct ofono_watch_closure *)g_closure_new_simple
				(sizeof(struct ofono_watch_closure), NULL);

		closure->cclosure.closure.data = closure;
		closure->cclosure.callback = G_CALLBACK(ofono_watch_signal_cb);
		closure->cb = cb;
		closure->user_data = user_data;

		return g_signal_connect_closure_by_id(self,
					ofono_watch_signals[signal], 0,
					&closure->cclosure.closure, FALSE);
	}
	return 0;
}

unsigned long ofono_watch_add_modem_changed_handler(struct ofono_watch *watch,
				ofono_watch_cb_t cb, void *user_data)
{
	return ofono_watch_add_signal_handler(watch, SIGNAL_MODEM_CHANGED,
							cb, user_data);
}

unsigned long ofono_watch_add_online_changed_handler(struct ofono_watch *watch,
				ofono_watch_cb_t cb, void *user_data)
{
	return ofono_watch_add_signal_handler(watch, SIGNAL_ONLINE_CHANGED,
							cb, user_data);
}

unsigned long ofono_watch_add_sim_changed_handler(struct ofono_watch *watch,
				ofono_watch_cb_t cb, void *user_data)
{
	return ofono_watch_add_signal_handler(watch, SIGNAL_SIM_CHANGED,
							cb, user_data);
}

unsigned long ofono_watch_add_sim_state_changed_handler
	(struct ofono_watch *watch, ofono_watch_cb_t cb, void *user_data)
{
	return ofono_watch_add_signal_handler(watch, SIGNAL_SIM_STATE_CHANGED,
							cb, user_data);
}

unsigned long ofono_watch_add_iccid_changed_handler(struct ofono_watch *watch,
				ofono_watch_cb_t cb, void *user_data)
{
	return ofono_watch_add_signal_handler(watch, SIGNAL_ICCID_CHANGED,
							cb, user_data);
}

unsigned long ofono_watch_add_imsi_changed_handler(struct ofono_watch *watch,
				ofono_watch_cb_t cb, void *user_data)
{
	return ofono_watch_add_signal_handler(watch, SIGNAL_IMSI_CHANGED,
							cb, user_data);
}

unsigned long ofono_watch_add_spn_changed_handler(struct ofono_watch *watch,
				ofono_watch_cb_t cb, void *user_data)
{
	return ofono_watch_add_signal_handler(watch, SIGNAL_SPN_CHANGED,
							cb, user_data);
}

unsigned long ofono_watch_add_netreg_changed_handler(struct ofono_watch *watch,
				ofono_watch_cb_t cb, void *user_data)
{
	return ofono_watch_add_signal_handler(watch, SIGNAL_NETREG_CHANGED,
							cb, user_data);
}

void ofono_watch_remove_handler(struct ofono_watch *watch, unsigned long id)
{
	if (watch && id) {
		g_signal_handler_disconnect(ofono_watch_object_cast(watch),
									id);
	}
}

void ofono_watch_remove_handlers(struct ofono_watch *watch, unsigned long *ids,
							unsigned int count)
{
	struct ofono_watch_object *self = ofono_watch_object_cast(watch);

	if (self && ids && count) {
		unsigned int i;

		for (i = 0; i < count; i++) {
			if (ids[i]) {
				g_signal_handler_disconnect(self, ids[i]);
				ids[i] = 0;
			}
		}
	}
}

static void ofono_watch_object_init(struct ofono_watch_object *self)
{
}

static void ofono_watch_object_finalize(GObject *object)
{
	struct ofono_watch_object *self = OFONO_WATCH_OBJECT(object);
	struct ofono_watch *watch = &self->pub;

	if (watch->modem) {
		struct ofono_modem *modem = watch->modem;

		watch->modem = NULL;
		ofono_watch_cleanup_modem(self, modem);
	}
	if (self->modem_watch_id) {
		__ofono_modemwatch_remove(self->modem_watch_id);
		ASSERT(!self->modem_watch_id);
	}
	g_free(self->path);
	G_OBJECT_CLASS(ofono_watch_object_parent_class)->finalize(object);
}

static void ofono_watch_object_class_init(OfonoWatchObjectClass *klass)
{
	G_OBJECT_CLASS(klass)->finalize = ofono_watch_object_finalize;
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
