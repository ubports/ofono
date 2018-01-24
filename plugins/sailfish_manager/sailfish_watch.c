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

#include "sailfish_watch.h"

#include <gutil_misc.h>
#include <gutil_log.h>

#include "ofono.h"

typedef GObjectClass SailfishWatchClass;
typedef struct sailfish_watch SailfishWatch;

struct sailfish_watch_priv {
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

enum sailfish_watch_signal {
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

#define SIGNAL_MODEM_CHANGED_NAME       "sailfish-watch-modem-changed"
#define SIGNAL_ONLINE_CHANGED_NAME      "sailfish-watch-online-changed"
#define SIGNAL_SIM_CHANGED_NAME         "sailfish-watch-sim-changed"
#define SIGNAL_SIM_STATE_CHANGED_NAME   "sailfish-watch-sim-state-changed"
#define SIGNAL_ICCID_CHANGED_NAME       "sailfish-watch-iccid-changed"
#define SIGNAL_IMSI_CHANGED_NAME        "sailfish-watch-imsi-changed"
#define SIGNAL_SPN_CHANGED_NAME         "sailfish-watch-spn-changed"
#define SIGNAL_NETREG_CHANGED_NAME      "sailfish-watch-netreg-changed"

static guint sailfish_watch_signals[SIGNAL_COUNT] = { 0 };
static GHashTable* sailfish_watch_table = NULL;

G_DEFINE_TYPE(SailfishWatch, sailfish_watch, G_TYPE_OBJECT)
#define SAILFISH_WATCH_TYPE (sailfish_watch_get_type())
#define SAILFISH_WATCH(obj) (G_TYPE_CHECK_INSTANCE_CAST((obj),\
	SAILFISH_WATCH_TYPE, SailfishWatch))

#define NEW_SIGNAL(klass,name) \
	sailfish_watch_signals[SIGNAL_##name##_CHANGED] = \
		g_signal_new(SIGNAL_##name##_CHANGED_NAME, \
			G_OBJECT_CLASS_TYPE(klass), G_SIGNAL_RUN_FIRST, \
			0, NULL, NULL, NULL, G_TYPE_NONE, 0)

/* Skip the leading slash from the modem path: */
#define DBG_(obj,fmt,args...) DBG("%s " fmt, (obj)->path+1, ##args)

static inline int sailfish_watch_signal_bit(enum sailfish_watch_signal id)
{
	return (1 << id);
}

static inline void sailfish_watch_signal_emit(struct sailfish_watch *self,
					enum sailfish_watch_signal id)
{
	self->priv->queued_signals &= ~sailfish_watch_signal_bit(id);
	g_signal_emit(self, sailfish_watch_signals[id], 0);
}

static inline void sailfish_watch_signal_queue(struct sailfish_watch *self,
					enum sailfish_watch_signal id)
{
	self->priv->queued_signals |= sailfish_watch_signal_bit(id);
}

static void sailfish_watch_emit_queued_signals(struct sailfish_watch *self)
{
	struct sailfish_watch_priv *priv = self->priv;

	if (priv->signals_suspended < 1) {
		int i;

		for (i = 0; priv->queued_signals && i < SIGNAL_COUNT; i++) {
			if (priv->queued_signals &
					sailfish_watch_signal_bit(i)) {
				sailfish_watch_signal_emit(self, i);
			}
		}
	}
}

static inline void sailfish_watch_suspend_signals(struct sailfish_watch *self)
{
	self->priv->signals_suspended++;
}

static inline void sailfish_watch_resume_signals(struct sailfish_watch *self)
{
	struct sailfish_watch_priv *priv = self->priv;

	GASSERT(priv->signals_suspended > 0);
	priv->signals_suspended--;
	sailfish_watch_emit_queued_signals(self);
}

static void sailfish_watch_iccid_update(struct sailfish_watch *self,
							const char *iccid)
{
	struct sailfish_watch_priv *priv = self->priv;

	if (g_strcmp0(priv->iccid, iccid)) {
		g_free(priv->iccid);
		self->iccid = priv->iccid = g_strdup(iccid);
		sailfish_watch_signal_queue(self, SIGNAL_ICCID_CHANGED);
	}
}

static void sailfish_watch_iccid_notify(const char *iccid, void *user_data)
{
	struct sailfish_watch *self = SAILFISH_WATCH(user_data);

	sailfish_watch_iccid_update(self, iccid);
	sailfish_watch_emit_queued_signals(self);
}

static void sailfish_watch_iccid_destroy(void *user_data)
{
	struct sailfish_watch *self = SAILFISH_WATCH(user_data);
	struct sailfish_watch_priv *priv = self->priv;

	GASSERT(priv->iccid_watch_id);
	priv->iccid_watch_id = 0;
}

static void sailfish_watch_spn_update(struct sailfish_watch *self,
							const char *spn)
{
	struct sailfish_watch_priv *priv = self->priv;

	if (g_strcmp0(priv->spn, spn)) {
		g_free(priv->spn);
		self->spn = priv->spn = g_strdup(spn);
		sailfish_watch_signal_queue(self, SIGNAL_SPN_CHANGED);
	}
}

static void sailfish_watch_spn_notify(const char *spn, const char *dc,
							void *user_data)
{
	struct sailfish_watch *self = SAILFISH_WATCH(user_data);

	sailfish_watch_spn_update(self, spn);
	sailfish_watch_emit_queued_signals(self);
}

static void sailfish_watch_spn_destroy(void *user_data)
{
	struct sailfish_watch *self = SAILFISH_WATCH(user_data);
	struct sailfish_watch_priv *priv = self->priv;

	GASSERT(priv->spn_watch_id);
	priv->spn_watch_id = 0;
}

static void sailfish_watch_imsi_update(struct sailfish_watch *self,
							const char *imsi)
{
	struct sailfish_watch_priv *priv = self->priv;

	if (g_strcmp0(priv->imsi, imsi)) {
		g_free(priv->imsi);
		self->imsi = priv->imsi = g_strdup(imsi);
		sailfish_watch_signal_queue(self, SIGNAL_IMSI_CHANGED);
		/* ofono core crashes if we add spn watch too early */
		if (imsi) {
			ofono_sim_add_spn_watch(self->sim, &priv->spn_watch_id,
					sailfish_watch_spn_notify, self,
					sailfish_watch_spn_destroy);
		}
	}
}

static void sailfish_watch_imsi_notify(const char *imsi, void *user_data)
{
	struct sailfish_watch *self = SAILFISH_WATCH(user_data);

	sailfish_watch_imsi_update(self, imsi);
	sailfish_watch_emit_queued_signals(self);
}

static void sailfish_watch_imsi_destroy(void *user_data)
{
	struct sailfish_watch *self = SAILFISH_WATCH(user_data);
	struct sailfish_watch_priv *priv = self->priv;

	GASSERT(priv->imsi_watch_id);
	priv->imsi_watch_id = 0;
}

static void sailfish_watch_sim_state_notify(enum ofono_sim_state new_state,
							void *user_data)
{
	struct sailfish_watch *self = SAILFISH_WATCH(user_data);

	/*
	 * ofono core doesn't notify SIM watches when SIM card gets removed.
	 * So we have to reset things here based on the SIM state.
	 */
	if (new_state == OFONO_SIM_STATE_NOT_PRESENT) {
		sailfish_watch_iccid_update(self, NULL);
	}
	if (new_state != OFONO_SIM_STATE_READY) {
		sailfish_watch_imsi_update(self, NULL);
		sailfish_watch_spn_update(self, NULL);
	}
	sailfish_watch_signal_queue(self, SIGNAL_SIM_STATE_CHANGED);
	sailfish_watch_emit_queued_signals(self);
}

static void sailfish_watch_sim_state_destroy(void *user_data)
{
	struct sailfish_watch *self = SAILFISH_WATCH(user_data);
	struct sailfish_watch_priv *priv = self->priv;

	GASSERT(priv->sim_state_watch_id);
	priv->sim_state_watch_id = 0;
}

static void sailfish_watch_set_sim(struct sailfish_watch *self,
						struct ofono_sim *sim)
{
	if (self->sim != sim) {
		struct sailfish_watch_priv *priv = self->priv;

		if (priv->sim_state_watch_id) {
			ofono_sim_remove_state_watch(self->sim,
						priv->sim_state_watch_id);
			/* The destroy callback clears it */
			GASSERT(!priv->sim_state_watch_id);
		}
		if (priv->iccid_watch_id) {
			ofono_sim_remove_iccid_watch(self->sim,
						priv->iccid_watch_id);
			/* The destroy callback clears it */
			GASSERT(!priv->iccid_watch_id);
		}
		if (priv->imsi_watch_id) {
			ofono_sim_remove_imsi_watch(self->sim,
						priv->imsi_watch_id);
			/* The destroy callback clears it */
			GASSERT(!priv->imsi_watch_id);
		}
		if (priv->spn_watch_id) {
			ofono_sim_remove_spn_watch(self->sim,
						&priv->spn_watch_id);
			/* The destroy callback clears it */
			GASSERT(!priv->spn_watch_id);
		}
		self->sim = sim;
		sailfish_watch_signal_queue(self, SIGNAL_SIM_CHANGED);
		sailfish_watch_suspend_signals(self);

		/* Reset the current state */
		sailfish_watch_iccid_update(self, NULL);
		sailfish_watch_imsi_update(self, NULL);
		sailfish_watch_spn_update(self, NULL);
		if (sim) {
			priv->sim_state_watch_id =
				ofono_sim_add_state_watch(sim,
					sailfish_watch_sim_state_notify, self,
					sailfish_watch_sim_state_destroy);
			/*
			 * Unlike ofono_sim_add_state_watch, the rest
			 * of ofono_sim_add_xxx_watch functions call the
			 * notify callback if the value is already known
			 * to the ofono core.
			 *
			 * Also note that ofono core crashes if we add
			 * spn watch too early.
			 */
			priv->iccid_watch_id =
				ofono_sim_add_iccid_watch(self->sim,
					sailfish_watch_iccid_notify, self,
					sailfish_watch_iccid_destroy);
			priv->imsi_watch_id =
				ofono_sim_add_imsi_watch(self->sim,
					sailfish_watch_imsi_notify, self,
					sailfish_watch_imsi_destroy);
		}

		/* Emit the pending signals. */
		sailfish_watch_resume_signals(self);
	}
}

static void sailfish_watch_sim_notify(struct ofono_atom *atom,
			enum ofono_atom_watch_condition cond, void *user_data)
{
	struct sailfish_watch *self = SAILFISH_WATCH(user_data);

	if (cond == OFONO_ATOM_WATCH_CONDITION_REGISTERED) {
		struct ofono_sim *sim = __ofono_atom_get_data(atom);

		DBG_(self, "sim registered");
		sailfish_watch_set_sim(self, sim);
	} else if (cond == OFONO_ATOM_WATCH_CONDITION_UNREGISTERED) {
		DBG_(self, "sim unregistered");
		sailfish_watch_set_sim(self, NULL);
	}
}

static void sailfish_watch_sim_destroy(void *user_data)
{
	struct sailfish_watch *self = SAILFISH_WATCH(user_data);

	self->priv->sim_watch_id = 0;
}

static void sailfish_watch_set_netreg(struct sailfish_watch *self,
						struct ofono_netreg *netreg)
{
	if (self->netreg != netreg) {
		self->netreg = netreg;
		sailfish_watch_signal_emit(self, SIGNAL_NETREG_CHANGED);
	}
}

static void sailfish_watch_netreg_notify(struct ofono_atom *atom,
			enum ofono_atom_watch_condition cond, void *user_data)
{
	struct sailfish_watch *self = SAILFISH_WATCH(user_data);

	if (cond == OFONO_ATOM_WATCH_CONDITION_REGISTERED) {
		struct ofono_netreg *netreg = __ofono_atom_get_data(atom);

		DBG_(self, "netreg registered");
		sailfish_watch_set_netreg(self, netreg);
	} else if (cond == OFONO_ATOM_WATCH_CONDITION_UNREGISTERED) {
		DBG_(self, "netreg unregistered");
		sailfish_watch_set_netreg(self, NULL);
	}
}

static void sailfish_watch_netreg_destroy(void *user_data)
{
	struct sailfish_watch *self = SAILFISH_WATCH(user_data);

	self->priv->netreg_watch_id = 0;
}

static void sailfish_watch_online_update(struct sailfish_watch *self,
							gboolean online)
{
	if (self->online != online) {
		self->online = online;
		sailfish_watch_signal_queue(self, SIGNAL_ONLINE_CHANGED);
	}
}

static void sailfish_watch_online_notify(struct ofono_modem *modem,
					ofono_bool_t online, void *user_data)
{
	struct sailfish_watch *self = SAILFISH_WATCH(user_data);

	GASSERT(self->modem == modem);
	GASSERT(online == ofono_modem_get_online(modem));
	sailfish_watch_online_update(self, online);
	sailfish_watch_emit_queued_signals(self);
}

static void sailfish_watch_online_destroy(void *user_data)
{
	struct sailfish_watch *self = SAILFISH_WATCH(user_data);

	self->priv->online_watch_id = 0;
}

static void sailfish_watch_setup_modem(struct sailfish_watch *self)
{
	struct sailfish_watch_priv *priv = self->priv;

	GASSERT(!priv->online_watch_id);
	priv->online_watch_id =
		__ofono_modem_add_online_watch(self->modem,
				sailfish_watch_online_notify, self,
				sailfish_watch_online_destroy);

	/* __ofono_modem_add_atom_watch() calls the notify callback if the
	 * atom is already registered */
	GASSERT(!priv->sim_watch_id);
	priv->sim_watch_id = __ofono_modem_add_atom_watch(self->modem,
		OFONO_ATOM_TYPE_SIM, sailfish_watch_sim_notify,
		self, sailfish_watch_sim_destroy);

	GASSERT(!priv->netreg_watch_id);
	priv->netreg_watch_id = __ofono_modem_add_atom_watch(self->modem,
		OFONO_ATOM_TYPE_NETREG, sailfish_watch_netreg_notify,
		self, sailfish_watch_netreg_destroy);
}

static void sailfish_watch_cleanup_modem(struct sailfish_watch *self,
						struct ofono_modem *modem)
{
	/* Caller checks the self->modem isn't NULL */
	struct sailfish_watch_priv *priv = self->priv;

	if (priv->online_watch_id) {
		__ofono_modem_remove_online_watch(modem,
						priv->online_watch_id);
		GASSERT(!priv->online_watch_id);
	}

	if (priv->sim_watch_id) {
		__ofono_modem_remove_atom_watch(modem, priv->sim_watch_id);
		GASSERT(!priv->sim_watch_id);
	}

	if (priv->netreg_watch_id) {
		__ofono_modem_remove_atom_watch(modem, priv->netreg_watch_id);
		GASSERT(!priv->netreg_watch_id);
	}

	sailfish_watch_set_sim(self, NULL);
	sailfish_watch_set_netreg(self, NULL);
}

static void sailfish_watch_set_modem(struct sailfish_watch *self,
						struct ofono_modem *modem)
{
	if (self->modem != modem) {
		struct ofono_modem *old_modem = self->modem;

		self->modem = modem;
		sailfish_watch_signal_queue(self, SIGNAL_MODEM_CHANGED);
		if (old_modem) {
			sailfish_watch_cleanup_modem(self, old_modem);
		}
		if (modem) {
			sailfish_watch_setup_modem(self);
		}
		sailfish_watch_online_update(self,
					ofono_modem_get_online(self->modem));
		sailfish_watch_emit_queued_signals(self);
	}
}

static void sailfish_watch_modem_notify(struct ofono_modem *modem,
					gboolean added, void *user_data)
{
	struct sailfish_watch *self = SAILFISH_WATCH(user_data);

	if (added) {
		if (!g_strcmp0(self->path, ofono_modem_get_path(modem))) {
			sailfish_watch_set_modem(self, modem);
		}
	} else if (self->modem == modem) {
		sailfish_watch_set_modem(self, NULL);
	}
}

static void sailfish_watch_modem_destroy(void *user_data)
{
	struct sailfish_watch *self = SAILFISH_WATCH(user_data);

	self->priv->modem_watch_id = 0;
}

static ofono_bool_t sailfish_watch_modem_find(struct ofono_modem *modem,
							void *user_data)
{
	struct sailfish_watch *self = SAILFISH_WATCH(user_data);

	if (!g_strcmp0(self->path, ofono_modem_get_path(modem))) {
		self->modem = modem;
		sailfish_watch_setup_modem(self);
		return TRUE;
	} else {
		return FALSE;
	}
}

static void sailfish_watch_initialize(struct sailfish_watch *self,
							const char *path)
{
	struct sailfish_watch_priv *priv = self->priv;

	self->path = priv->path = g_strdup(path);
	ofono_modem_find(sailfish_watch_modem_find, self);
	self->online = ofono_modem_get_online(self->modem);
	priv->modem_watch_id =
		__ofono_modemwatch_add(sailfish_watch_modem_notify, self,
					sailfish_watch_modem_destroy);
}

static void sailfish_watch_destroyed(gpointer key, GObject* obj)
{
	GASSERT(sailfish_watch_table);
	DBG("%s", (char*)key);
	if (sailfish_watch_table) {
		GASSERT(g_hash_table_lookup(sailfish_watch_table, key) == obj);
		g_hash_table_remove(sailfish_watch_table, key);
		if (g_hash_table_size(sailfish_watch_table) == 0) {
			g_hash_table_unref(sailfish_watch_table);
			sailfish_watch_table = NULL;
		}
	}
}

struct sailfish_watch *sailfish_watch_new(const char *path)
{
	struct sailfish_watch *watch = NULL;

	if (path) {
		if (sailfish_watch_table) {
			watch = sailfish_watch_ref(g_hash_table_lookup(
						sailfish_watch_table, path));
		}
		if (!watch) {
			char* key = g_strdup(path);

			watch = g_object_new(SAILFISH_WATCH_TYPE, NULL);
			sailfish_watch_initialize(watch, path);
			if (!sailfish_watch_table) {
				/* Create the table on demand */
				sailfish_watch_table =
					g_hash_table_new_full(g_str_hash,
						g_str_equal, g_free, NULL);
			}
			g_hash_table_replace(sailfish_watch_table, key, watch);
			g_object_weak_ref(G_OBJECT(watch),
					sailfish_watch_destroyed, key);
			DBG_(watch, "created");
		}
	}
	return watch;
}

struct sailfish_watch *sailfish_watch_ref(struct sailfish_watch *self)
{
	if (self) {
		g_object_ref(SAILFISH_WATCH(self));
		return self;
	} else {
		return NULL;
	}
}

void sailfish_watch_unref(struct sailfish_watch *self)
{
	if (self) {
		g_object_unref(SAILFISH_WATCH(self));
	}
}

gulong sailfish_watch_add_modem_changed_handler(struct sailfish_watch *self,
				sailfish_watch_cb_t cb, void *user_data)
{
	return (self && cb) ? g_signal_connect(self,
		SIGNAL_MODEM_CHANGED_NAME, G_CALLBACK(cb), user_data) : 0;
}

gulong sailfish_watch_add_online_changed_handler(struct sailfish_watch *self,
				sailfish_watch_cb_t cb, void *user_data)
{
	return (self && cb) ? g_signal_connect(self,
		SIGNAL_ONLINE_CHANGED_NAME, G_CALLBACK(cb), user_data) : 0;
}

gulong sailfish_watch_add_sim_changed_handler(struct sailfish_watch *self,
				sailfish_watch_cb_t cb, void *user_data)
{
	return (self && cb) ? g_signal_connect(self,
		SIGNAL_SIM_CHANGED_NAME, G_CALLBACK(cb), user_data) : 0;
}

gulong sailfish_watch_add_sim_state_changed_handler(struct sailfish_watch *self,
				sailfish_watch_cb_t cb, void *user_data)
{
	return (self && cb) ? g_signal_connect(self,
		SIGNAL_SIM_STATE_CHANGED_NAME, G_CALLBACK(cb), user_data) : 0;
}

gulong sailfish_watch_add_iccid_changed_handler(struct sailfish_watch *self,
				sailfish_watch_cb_t cb, void *user_data)
{
	return (self && cb) ? g_signal_connect(self,
		SIGNAL_ICCID_CHANGED_NAME, G_CALLBACK(cb), user_data) : 0;
}

gulong sailfish_watch_add_imsi_changed_handler(struct sailfish_watch *self,
				sailfish_watch_cb_t cb, void *user_data)
{
	return (self && cb) ? g_signal_connect(self,
		SIGNAL_IMSI_CHANGED_NAME, G_CALLBACK(cb), user_data) : 0;
}

gulong sailfish_watch_add_spn_changed_handler(struct sailfish_watch *self,
				sailfish_watch_cb_t cb, void *user_data)
{
	return (self && cb) ? g_signal_connect(self,
		SIGNAL_SPN_CHANGED_NAME, G_CALLBACK(cb), user_data) : 0;
}

gulong sailfish_watch_add_netreg_changed_handler(struct sailfish_watch *self,
				sailfish_watch_cb_t cb, void *user_data)
{
	return (self && cb) ? g_signal_connect(self,
		SIGNAL_NETREG_CHANGED_NAME, G_CALLBACK(cb), user_data) : 0;
}

void sailfish_watch_remove_handler(struct sailfish_watch *self, gulong id)
{
	if (self && id) {
		g_signal_handler_disconnect(self, id);
	}
}

void sailfish_watch_remove_handlers(struct sailfish_watch *self, gulong *ids,
								int count)
{
	gutil_disconnect_handlers(self, ids, count);
}

static void sailfish_watch_init(struct sailfish_watch *self)
{
	self->priv = G_TYPE_INSTANCE_GET_PRIVATE(self, SAILFISH_WATCH_TYPE,
						struct sailfish_watch_priv);
}

static void sailfish_watch_finalize(GObject *object)
{
	struct sailfish_watch *self = SAILFISH_WATCH(object);
	struct sailfish_watch_priv *priv = self->priv;

	if (self->modem) {
		struct ofono_modem *modem = self->modem;

		self->modem = NULL;
		sailfish_watch_cleanup_modem(self, modem);
	}
	if (priv->modem_watch_id) {
		__ofono_modemwatch_remove(priv->modem_watch_id);
		GASSERT(!priv->modem_watch_id);
	}
	g_free(priv->path);
	G_OBJECT_CLASS(sailfish_watch_parent_class)->finalize(object);
}

static void sailfish_watch_class_init(SailfishWatchClass *klass)
{
	G_OBJECT_CLASS(klass)->finalize = sailfish_watch_finalize;
	g_type_class_add_private(klass, sizeof(struct sailfish_watch_priv));
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
