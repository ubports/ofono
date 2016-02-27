/*
 *  oFono - Open Source Telephony - RIL-based devices
 *
 *  Copyright (C) 2016 Jolla Ltd.
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License version 2 as
 *  published by the Free Software Foundation.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 */

#include "ril_sim_info.h"
#include "ril_log.h"

#include <ofono/sim.h>

#include "ofono.h"
#include "storage.h"

#define RIL_SIM_INFO_STORE          "cache"
#define RIL_SIM_INFO_STORE_GROUP    "sim"
#define RIL_SIM_INFO_STORE_SPN      "spn"

/* ICCID -> IMSI map */
#define RIL_SIM_ICCID_MAP           "iccidmap"
#define RIL_SIM_ICCID_MAP_IMSI      "imsi"

typedef GObjectClass RilSimInfoClass;
typedef struct ril_sim_info RilSimInfo;

typedef void (*ril_sim_info_remove_cb_t)(struct ofono_sim *sim, unsigned int id);
typedef void (*ril_sim_info_set_value_cb_t)(struct ril_sim_info *info,
							const char *value);

struct ril_sim_info_watch {
	ril_sim_info_set_value_cb_t set_value;
	ril_sim_info_remove_cb_t remove;
	struct ril_sim_info *info;
	unsigned int id;
};

struct ril_sim_info_priv {
	char *iccid;
	char *imsi;
	char *spn;
	struct ofono_sim *sim;
	struct ril_sim_info_watch state_watch;
	struct ril_sim_info_watch iccid_watch;
	struct ril_sim_info_watch imsi_watch;
	struct ril_sim_info_watch spn_watch;
	gboolean update_imsi_cache;
	gboolean update_iccid_map;
};

enum ril_sim_info_signal {
	SIGNAL_ICCID_CHANGED,
	SIGNAL_IMSI_CHANGED,
	SIGNAL_SPN_CHANGED,
	SIGNAL_COUNT
};

#define SIGNAL_ICCID_CHANGED_NAME   "ril-sim-info-iccid-changed"
#define SIGNAL_IMSI_CHANGED_NAME    "ril-sim-info-imsi-changed"
#define SIGNAL_SPN_CHANGED_NAME     "ril-sim-info-spn-changed"

static guint ril_sim_info_signals[SIGNAL_COUNT] = { 0 };

G_DEFINE_TYPE(RilSimInfo, ril_sim_info, G_TYPE_OBJECT)
#define RIL_SIMINFO_TYPE (ril_sim_info_get_type())
#define RIL_SIMINFO(obj) (G_TYPE_CHECK_INSTANCE_CAST((obj),\
	RIL_SIMINFO_TYPE, RilSimInfo))

#define NEW_SIGNAL(klass,name) \
	ril_sim_info_signals[SIGNAL_##name##_CHANGED] = \
		g_signal_new(SIGNAL_##name##_CHANGED_NAME, \
			G_OBJECT_CLASS_TYPE(klass), G_SIGNAL_RUN_FIRST, \
			0, NULL, NULL, NULL, G_TYPE_NONE, 0)

static void ril_sim_info_signal_emit(struct ril_sim_info *self,
						enum ril_sim_info_signal id)
{
	g_signal_emit(self, ril_sim_info_signals[id], 0);
}

static void ril_sim_info_watch_remove(struct ril_sim_info_watch *watch)
{
	if (watch->id) {
		struct ril_sim_info_priv *priv = watch->info->priv;

		GASSERT(priv->sim);
		if (priv->sim) {
			watch->remove(priv->sim, watch->id);
			GASSERT(!watch->id);
		}

		watch->id = 0;
	}

	if (watch->set_value) {
		watch->set_value(watch->info, NULL);
	}
}

static void ril_sim_info_remove_spn_watch(struct ofono_sim *sim, unsigned int id)
{
	ofono_sim_remove_spn_watch(sim, &id);
}

static void ril_sim_info_update_imsi_cache(struct ril_sim_info *self)
{
	struct ril_sim_info_priv *priv = self->priv;

	if (priv->update_imsi_cache && priv->imsi && priv->imsi[0] &&
					priv->spn && priv->spn[0]) {
		const char *store = RIL_SIM_INFO_STORE;
		GKeyFile *cache = storage_open(priv->imsi, store);

		DBG("Updating " STORAGEDIR "/%s/%s", priv->imsi, store);
		g_key_file_set_string(cache, RIL_SIM_INFO_STORE_GROUP,
					RIL_SIM_INFO_STORE_SPN, priv->spn);
		storage_close(priv->imsi, store, cache, TRUE);
		priv->update_imsi_cache = FALSE;
	}
}

static void ril_sim_info_update_iccid_map(struct ril_sim_info *self)
{
	struct ril_sim_info_priv *priv = self->priv;

	if (priv->update_iccid_map && priv->iccid && priv->iccid[0] &&
						priv->imsi && priv->imsi[0]) {
		const char *store = RIL_SIM_ICCID_MAP;
		GKeyFile *map = storage_open(NULL, store);

		DBG("Updating " STORAGEDIR "/%s", store);
		g_key_file_set_string(map, RIL_SIM_ICCID_MAP_IMSI,
						priv->iccid, priv->imsi);
		storage_close(NULL, store, map, TRUE);
		priv->update_iccid_map = FALSE;
	}
}

static void ril_sim_info_set_imsi(struct ril_sim_info *self, const char *imsi)
{
	struct ril_sim_info_priv *priv = self->priv;

	if (g_strcmp0(priv->imsi, imsi)) {
		g_free(priv->imsi);
		self->imsi = priv->imsi = g_strdup(imsi);
		priv->update_iccid_map = TRUE;
		ril_sim_info_update_iccid_map(self);
		ril_sim_info_update_imsi_cache(self);
		ril_sim_info_signal_emit(self, SIGNAL_IMSI_CHANGED);
	}
}

static void ril_sim_info_set_spn(struct ril_sim_info *self, const char *spn)
{
	struct ril_sim_info_priv *priv = self->priv;

	if (g_strcmp0(priv->spn, spn)) {
		g_free(priv->spn);
		self->spn = priv->spn = g_strdup(spn);
		priv->update_imsi_cache = TRUE;
		ril_sim_info_update_imsi_cache(self);
		ril_sim_info_signal_emit(self, SIGNAL_SPN_CHANGED);
	}
}

static void ril_sim_info_load_cache(struct ril_sim_info *self)
{
	struct ril_sim_info_priv *priv = self->priv;

	if (priv->iccid && priv->iccid[0]) {
		GKeyFile *map = storage_open(NULL, RIL_SIM_ICCID_MAP);
		char *imsi = g_key_file_get_string(map, RIL_SIM_ICCID_MAP_IMSI,
							priv->iccid, NULL);
		g_key_file_free(map);

		if (imsi && imsi[0] && g_strcmp0(priv->imsi, imsi)) {
			if (priv->imsi && priv->imsi[0]) {
				/* Need to update ICCID -> IMSI map */
				DBG("IMSI changed %s -> %s", priv->imsi, imsi);
				priv->update_imsi_cache = TRUE;
			}
			g_free(priv->imsi);
			self->imsi = priv->imsi = imsi;
			DBG("imsi[%s] = %s", priv->iccid, imsi);
			ril_sim_info_update_iccid_map(self);
			ril_sim_info_signal_emit(self, SIGNAL_IMSI_CHANGED);
		} else if (imsi) {
			g_free(imsi);
		} else {
			DBG("No imsi for iccid %s", priv->iccid);
		}
	}

	if (priv->imsi && priv->imsi[0]) {
		GKeyFile *cache = storage_open(priv->imsi, RIL_SIM_INFO_STORE);
		char *spn = g_key_file_get_string(cache,
					RIL_SIM_INFO_STORE_GROUP,
					RIL_SIM_INFO_STORE_SPN, NULL);
		g_key_file_free(cache);

		if (spn && spn[0] && g_strcmp0(priv->spn, spn)) {
			if (priv->spn && priv->spn[0]) {
				/* Need to update the cache file */
				DBG("spn changing %s -> %s", priv->spn, spn);
				priv->update_imsi_cache = TRUE;
			}
			g_free(priv->spn);
			self->spn = priv->spn = spn;
			DBG("spn[%s] = \"%s\"", priv->imsi, spn);
			ril_sim_info_update_imsi_cache(self);
			ril_sim_info_signal_emit(self, SIGNAL_SPN_CHANGED);
		} else if (spn) {
			g_free(spn);
		} else {
			DBG("No spn for imsi %s", priv->imsi);
		}
	}
}

static void ril_sim_info_set_iccid(struct ril_sim_info *self, const char *iccid)
{
	struct ril_sim_info_priv *priv = self->priv;

	if (g_strcmp0(priv->iccid, iccid)) {
		g_free(priv->iccid);
		self->iccid = priv->iccid = g_strdup(iccid);
		ril_sim_info_signal_emit(self, SIGNAL_ICCID_CHANGED);
		if (iccid) {
			ril_sim_info_load_cache(self);
		}
	}
}

static void ril_sim_info_imsi_watch_cb(const char *imsi, void *data)
{
	struct ril_sim_info_watch *watch = data;
	DBG("%s", imsi);
	ril_sim_info_set_imsi(watch->info, imsi);
}

static void ril_sim_info_spn_watch_cb(const char *spn, const char *dc,
								void *data)
{
	struct ril_sim_info_watch *watch = data;
	DBG("%s", spn);
	ril_sim_info_set_spn(watch->info, spn);
}

static void ril_sim_info_iccid_watch_cb(const char *iccid, void *data)
{
	struct ril_sim_info_watch *watch = data;
	DBG("%s", iccid);
	ril_sim_info_set_iccid(watch->info, iccid);
}

static void ril_sim_info_watch_done(void *data)
{
	struct ril_sim_info_watch *watch = data;

	GASSERT(watch->id);
	watch->id = 0;
}

static void ril_sim_info_handle_sim_state(struct ril_sim_info *self,
						enum ofono_sim_state state)
{
	struct ril_sim_info_priv *priv = self->priv;
	struct ril_sim_info_watch *watch;

	DBG("%d", state);

	switch (state) {
	case OFONO_SIM_STATE_READY:
		/* SPN */
		watch = &priv->spn_watch;
		if (!watch->id) {
			ofono_sim_add_spn_watch(priv->sim, &watch->id,
					ril_sim_info_spn_watch_cb, watch,
					ril_sim_info_watch_done);
			GASSERT(priv->spn_watch.id);
		}
		/* IMSI */
		watch = &priv->imsi_watch;
		if (!watch->id) {
			watch->id = ofono_sim_add_imsi_watch(priv->sim,
					ril_sim_info_imsi_watch_cb, watch,
					ril_sim_info_watch_done);
			GASSERT(watch->id);
		}
		/* no break */
	case OFONO_SIM_STATE_INSERTED:
	case OFONO_SIM_STATE_LOCKED_OUT:
		/* ICCID */
		watch = &priv->iccid_watch;
		if (!watch->id) {
			watch->id = ofono_sim_add_iccid_watch(priv->sim,
					ril_sim_info_iccid_watch_cb, watch,
					ril_sim_info_watch_done);
			GASSERT(watch->id);
		}
		break;
	case OFONO_SIM_STATE_NOT_PRESENT:
	case OFONO_SIM_STATE_RESETTING:
		ril_sim_info_watch_remove(&priv->spn_watch);
		ril_sim_info_watch_remove(&priv->imsi_watch);
		ril_sim_info_watch_remove(&priv->iccid_watch);
		break;
	}
}

static void ril_sim_info_state_watch_cb(enum ofono_sim_state new_state,
								void *data)
{
	struct ril_sim_info_watch *watch = data;
	ril_sim_info_handle_sim_state(watch->info, new_state);
}

struct ril_sim_info *ril_sim_info_new(struct ofono_sim *sim)
{
	struct ril_sim_info *self = g_object_new(RIL_SIMINFO_TYPE, NULL);

	ril_sim_info_set_ofono_sim(self, sim);
	return self;
}

struct ril_sim_info *ril_sim_info_ref(struct ril_sim_info *self)
{
	if (G_LIKELY(self)) {
		g_object_ref(RIL_SIMINFO(self));
		return self;
	} else {
		return NULL;
	}
}

void ril_sim_info_unref(struct ril_sim_info *self)
{
	if (G_LIKELY(self)) {
		g_object_unref(RIL_SIMINFO(self));
	}
}

void ril_sim_info_set_ofono_sim(struct ril_sim_info *self, struct ofono_sim *sim)
{
	if (G_LIKELY(self)) {
		struct ril_sim_info_priv *priv = self->priv;

		if (priv->sim != sim) {
			ril_sim_info_watch_remove(&priv->state_watch);
			ril_sim_info_watch_remove(&priv->iccid_watch);
			ril_sim_info_watch_remove(&priv->imsi_watch);
			ril_sim_info_watch_remove(&priv->spn_watch);

			priv->update_imsi_cache = FALSE;
			priv->update_iccid_map = FALSE;
			priv->sim = sim;

			if (sim) {
				priv->state_watch.id =
					ofono_sim_add_state_watch(sim,
						ril_sim_info_state_watch_cb,
						&priv->state_watch,
						ril_sim_info_watch_done);
				GASSERT(priv->state_watch.id);
				DBG("Attached to sim");
				ril_sim_info_handle_sim_state(self,
						ofono_sim_get_state(sim));
			}
		}
	}
}

gulong ril_sim_info_add_iccid_changed_handler(struct ril_sim_info *self,
					ril_sim_info_cb_t cb, void *arg)
{
	return (G_LIKELY(self) && G_LIKELY(cb)) ? g_signal_connect(self,
		SIGNAL_ICCID_CHANGED_NAME, G_CALLBACK(cb), arg) : 0;
}

gulong ril_sim_info_add_imsi_changed_handler(struct ril_sim_info *self,
					ril_sim_info_cb_t cb, void *arg)
{
	return (G_LIKELY(self) && G_LIKELY(cb)) ? g_signal_connect(self,
		SIGNAL_IMSI_CHANGED_NAME, G_CALLBACK(cb), arg) : 0;
}

gulong ril_sim_info_add_spn_changed_handler(struct ril_sim_info *self,
					ril_sim_info_cb_t cb, void *arg)
{
	return (G_LIKELY(self) && G_LIKELY(cb)) ? g_signal_connect(self,
		SIGNAL_SPN_CHANGED_NAME, G_CALLBACK(cb), arg) : 0;
}

void ril_sim_info_remove_handler(struct ril_sim_info *self, gulong id)
{
	if (G_LIKELY(self) && G_LIKELY(id)) {
		g_signal_handler_disconnect(self, id);
	}
}

static void ril_sim_info_watch_init(struct ril_sim_info *self,
				    struct ril_sim_info_watch *watch,
				    ril_sim_info_set_value_cb_t set_value,
				    ril_sim_info_remove_cb_t remove)
{
	watch->info = self;
	watch->set_value = set_value;
	watch->remove = remove;
}

static void ril_sim_info_init(struct ril_sim_info *self)
{
	struct ril_sim_info_priv *priv = G_TYPE_INSTANCE_GET_PRIVATE(self,
				RIL_SIMINFO_TYPE, struct ril_sim_info_priv);

	self->priv = priv;
	ril_sim_info_watch_init(self, &priv->state_watch, NULL,
					ofono_sim_remove_state_watch);
	ril_sim_info_watch_init(self, &priv->iccid_watch, ril_sim_info_set_iccid,
					ofono_sim_remove_iccid_watch);
	ril_sim_info_watch_init(self, &priv->imsi_watch, ril_sim_info_set_imsi,
					ofono_sim_remove_imsi_watch);
	ril_sim_info_watch_init(self, &priv->spn_watch, ril_sim_info_set_spn,
					ril_sim_info_remove_spn_watch);
}

static void ril_sim_info_dispose(GObject *object)
{
	struct ril_sim_info *self = RIL_SIMINFO(object);

	ril_sim_info_set_ofono_sim(self, NULL);
	G_OBJECT_CLASS(ril_sim_info_parent_class)->dispose(object);
}

static void ril_sim_info_class_init(RilSimInfoClass *klass)
{
	GObjectClass *object_class = G_OBJECT_CLASS(klass);

	object_class->dispose = ril_sim_info_dispose;
	g_type_class_add_private(klass, sizeof(struct ril_sim_info_priv));
	NEW_SIGNAL(klass, ICCID);
	NEW_SIGNAL(klass, IMSI);
	NEW_SIGNAL(klass, SPN);
}

/*
 * Local Variables:
 * mode: C
 * c-basic-offset: 8
 * indent-tabs-mode: t
 * End:
 */
