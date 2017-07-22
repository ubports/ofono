/*
 *  oFono - Open Source Telephony
 *
 *  Copyright (C) 2017 Jolla Ltd.
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

#include "fake_sailfish_watch.h"

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
	int queued_signals;
};

#define SIGNAL_MODEM_CHANGED_NAME       "sailfish-watch-modem-changed"
#define SIGNAL_ONLINE_CHANGED_NAME      "sailfish-watch-online-changed"
#define SIGNAL_SIM_CHANGED_NAME         "sailfish-watch-sim-changed"
#define SIGNAL_SIM_STATE_CHANGED_NAME   "sailfish-watch-sim-state-changed"
#define SIGNAL_ICCID_CHANGED_NAME       "sailfish-watch-iccid-changed"
#define SIGNAL_IMSI_CHANGED_NAME        "sailfish-watch-imsi-changed"
#define SIGNAL_SPN_CHANGED_NAME         "sailfish-watch-spn-changed"
#define SIGNAL_NETREG_CHANGED_NAME      "sailfish-watch-netreg-changed"

static guint sailfish_watch_signals[WATCH_SIGNAL_COUNT] = { 0 };
static GHashTable* sailfish_watch_table = NULL;

G_DEFINE_TYPE(SailfishWatch, sailfish_watch, G_TYPE_OBJECT)
#define SAILFISH_WATCH_TYPE (sailfish_watch_get_type())
#define SAILFISH_WATCH(obj) (G_TYPE_CHECK_INSTANCE_CAST((obj),\
	SAILFISH_WATCH_TYPE, SailfishWatch))

#define NEW_SIGNAL(klass,name) \
	sailfish_watch_signals[WATCH_SIGNAL_##name##_CHANGED] = \
		g_signal_new(SIGNAL_##name##_CHANGED_NAME, \
			G_OBJECT_CLASS_TYPE(klass), G_SIGNAL_RUN_FIRST, \
			0, NULL, NULL, NULL, G_TYPE_NONE, 0)

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

void fake_sailfish_watch_signal_queue(struct sailfish_watch *self,
					enum sailfish_watch_signal id)
{
	self->priv->queued_signals |= sailfish_watch_signal_bit(id);
}

void fake_sailfish_watch_emit_queued_signals(struct sailfish_watch *self)
{
	struct sailfish_watch_priv *priv = self->priv;
	int i;

	for (i = 0; priv->queued_signals && i < WATCH_SIGNAL_COUNT; i++) {
		if (priv->queued_signals & sailfish_watch_signal_bit(i)) {
			sailfish_watch_signal_emit(self, i);
		}
	}
}

void fake_sailfish_watch_set_ofono_iccid(struct sailfish_watch *self,
							const char *iccid)
{
	struct sailfish_watch_priv *priv = self->priv;

	if (g_strcmp0(priv->iccid, iccid)) {
		g_free(priv->iccid);
		self->iccid = priv->iccid = g_strdup(iccid);
		fake_sailfish_watch_signal_queue(self,
						WATCH_SIGNAL_ICCID_CHANGED);
	}
}

void fake_sailfish_watch_set_ofono_imsi(struct sailfish_watch *self,
							const char *imsi)
{
	struct sailfish_watch_priv *priv = self->priv;

	if (g_strcmp0(priv->imsi, imsi)) {
		g_free(priv->imsi);
		self->imsi = priv->imsi = g_strdup(imsi);
		fake_sailfish_watch_signal_queue(self,
						WATCH_SIGNAL_IMSI_CHANGED);
	}
}

void fake_sailfish_watch_set_ofono_spn(struct sailfish_watch *self,
							const char *spn)
{
	struct sailfish_watch_priv *priv = self->priv;

	if (g_strcmp0(priv->spn, spn)) {
		g_free(priv->spn);
		self->spn = priv->spn = g_strdup(spn);
		fake_sailfish_watch_signal_queue(self,
						WATCH_SIGNAL_SPN_CHANGED);
	}
}

void fake_sailfish_watch_set_ofono_sim(struct sailfish_watch *self,
						struct ofono_sim *sim)
{
	if (self->sim != sim) {
		self->sim = sim;
		fake_sailfish_watch_signal_queue(self,
						WATCH_SIGNAL_SIM_CHANGED);
		if (!sim) {
			fake_sailfish_watch_set_ofono_iccid(self, NULL);
			fake_sailfish_watch_set_ofono_imsi(self, NULL);
			fake_sailfish_watch_set_ofono_spn(self, NULL);
		}
	}
}

void fake_sailfish_watch_set_ofono_netreg(struct sailfish_watch *self,
						struct ofono_netreg *netreg)
{
	if (self->netreg != netreg) {
		self->netreg = netreg;
		fake_sailfish_watch_signal_queue(self,
						WATCH_SIGNAL_NETREG_CHANGED);
	}
}

static void sailfish_watch_initialize(struct sailfish_watch *self,
							const char *path)
{
	struct sailfish_watch_priv *priv = self->priv;

	self->path = priv->path = g_strdup(path);
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
	}
	return self;
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

	g_free(priv->path);
	g_free(priv->iccid);
	g_free(priv->imsi);
	g_free(priv->spn);
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
