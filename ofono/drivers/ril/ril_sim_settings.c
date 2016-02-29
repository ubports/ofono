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
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 *  GNU General Public License for more details.
 */

#include "ril_sim_settings.h"
#include "ril_log.h"

#include <gutil_misc.h>

#include <ofono/sim.h>

#include "storage.h"

#define RIL_SIM_STORE                   "ril"
#define RIL_SIM_STORE_GROUP             "Settings"
#define RIL_SIM_STORE_PREF_MODE         "TechnologyPreference"

#define RIL_SIM_STORE_PREF_MODE_DEFAULT(self) ((self)->enable_4g ? \
	OFONO_RADIO_ACCESS_MODE_LTE : OFONO_RADIO_ACCESS_MODE_UMTS)

typedef GObjectClass RilSimSettingsClass;
typedef struct ril_sim_settings RilSimSettings;

struct ril_sim_settings_priv {
	struct ofono_sim *sim;
	guint imsi_watch_id;
	guint state_watch_id;
	GKeyFile *storage;
	char *imsi;
};

enum ril_sim_settings_signal {
	SIGNAL_IMSI_CHANGED,
	SIGNAL_PREF_MODE_CHANGED,
	SIGNAL_COUNT
};

#define SIGNAL_IMSI_CHANGED_NAME        "ril-sim-settings-imsi-changed"
#define SIGNAL_PREF_MODE_CHANGED_NAME   "ril-sim-settings-pref-mode-changed"

static guint ril_sim_settings_signals[SIGNAL_COUNT] = { 0 };

G_DEFINE_TYPE(RilSimSettings, ril_sim_settings, G_TYPE_OBJECT)
#define RIL_SIM_SETTINGS_TYPE (ril_sim_settings_get_type())
#define RIL_SIM_SETTINGS(obj) (G_TYPE_CHECK_INSTANCE_CAST((obj),\
	RIL_SIM_SETTINGS_TYPE, RilSimSettings))

#define NEW_SIGNAL(klass,name) \
	ril_sim_settings_signals[SIGNAL_##name##_CHANGED] = \
		g_signal_new(SIGNAL_##name##_CHANGED_NAME, \
			G_OBJECT_CLASS_TYPE(klass), G_SIGNAL_RUN_FIRST, \
			0, NULL, NULL, NULL, G_TYPE_NONE, 0)

static void ril_sim_settings_signal_emit(struct ril_sim_settings *self,
					enum ril_sim_settings_signal id)
{
	g_signal_emit(self, ril_sim_settings_signals[id], 0);
}

static void ril_sim_settings_reload(struct ril_sim_settings *self)
{
	struct ril_sim_settings_priv *priv = self->priv;

	if (priv->storage) {
		g_key_file_free(priv->storage);
		priv->storage = NULL;
	}

	if (priv->imsi) {
		char *mode_str;
		enum ofono_radio_access_mode mode;
		priv->storage = storage_open(priv->imsi, RIL_SIM_STORE);
		mode_str = g_key_file_get_string(priv->storage,
			RIL_SIM_STORE_GROUP, RIL_SIM_STORE_PREF_MODE, NULL);
		if (ofono_radio_access_mode_from_string(mode_str, &mode)) {
			if (!self->enable_4g &&
					mode == OFONO_RADIO_ACCESS_MODE_LTE) {
				mode = OFONO_RADIO_ACCESS_MODE_ANY;
			}
		} else {
			mode = OFONO_RADIO_ACCESS_MODE_ANY;
		}
		if (mode == OFONO_RADIO_ACCESS_MODE_ANY) {
			self->pref_mode = RIL_SIM_STORE_PREF_MODE_DEFAULT(self);
		} else {
			self->pref_mode = mode;
		}
		g_free(mode_str);
	}
}

void ril_sim_settings_set_pref_mode(struct ril_sim_settings *self,
					enum ofono_radio_access_mode mode)
{
	if (G_LIKELY(self) && self->pref_mode != mode) {
		struct ril_sim_settings_priv *priv = self->priv;
		const char *mode_str = ofono_radio_access_mode_to_string(mode);

		GASSERT(priv->storage);
		if (mode_str) {
			if (priv->storage) {
				g_key_file_set_string(priv->storage,
					RIL_SIM_STORE_GROUP,
					RIL_SIM_STORE_PREF_MODE, mode_str);
				storage_sync(self->imsi, RIL_SIM_STORE,
							priv->storage);
			}
			self->pref_mode = mode;
			ril_sim_settings_signal_emit(self,
						SIGNAL_PREF_MODE_CHANGED);
		}
	}
}

static void ril_sim_settings_set_imsi(struct ril_sim_settings *self,
							const char *imsi)
{
	struct ril_sim_settings_priv *priv = self->priv;
	if (g_strcmp0(priv->imsi, imsi)) {
		enum ofono_radio_access_mode prev_mode = self->pref_mode;
		g_free(priv->imsi);
		self->imsi = priv->imsi = g_strdup(imsi);
		ril_sim_settings_reload(self);
		ril_sim_settings_signal_emit(self, SIGNAL_IMSI_CHANGED);
		if (prev_mode != self->pref_mode) {
			ril_sim_settings_signal_emit(self,
						SIGNAL_PREF_MODE_CHANGED);
		}
	}
}

static void ril_sim_settings_imsi_watch_cb(const char *imsi, void *user_data)
{
	ril_sim_settings_set_imsi(RIL_SIM_SETTINGS(user_data), imsi);
}

static void ril_sim_settings_imsi_watch_done(void *user_data)
{
	struct ril_sim_settings *self = RIL_SIM_SETTINGS(user_data);
	struct ril_sim_settings_priv *priv = self->priv;

	GASSERT(priv->imsi_watch_id);
	priv->imsi_watch_id = 0;
}

static void ril_sim_settings_ready(struct ril_sim_settings *self)
{
	struct ril_sim_settings_priv *priv = self->priv;

	GASSERT(!priv->imsi_watch_id);
	priv->imsi_watch_id = ofono_sim_add_imsi_watch(priv->sim,
				ril_sim_settings_imsi_watch_cb, self,
				ril_sim_settings_imsi_watch_done);
}

static void ril_sim_settings_state_watch(enum ofono_sim_state new_state,
							void *user_data)
{
	if (new_state == OFONO_SIM_STATE_READY) {
		ril_sim_settings_ready(RIL_SIM_SETTINGS(user_data));
	}
}

static void ril_sim_settings_state_watch_done(void *user_data)
{
	struct ril_sim_settings *self = RIL_SIM_SETTINGS(user_data);
	struct ril_sim_settings_priv *priv = self->priv;

	GASSERT(priv->state_watch_id);
	priv->state_watch_id = 0;
}

void ril_sim_settings_set_ofono_sim(struct ril_sim_settings *self,
						struct ofono_sim *sim)
{
	if (G_LIKELY(self)) {
		struct ril_sim_settings_priv *priv = self->priv;
		if (priv->sim != sim) {
			GASSERT(priv->sim || !priv->imsi_watch_id);
			if (priv->imsi_watch_id) {
				ofono_sim_remove_imsi_watch(priv->sim,
							priv->imsi_watch_id);
				/* ril_sim_settings_imsi_watch_done clears it */
				GASSERT(!priv->imsi_watch_id);
			}
			if (priv->state_watch_id) {
				ofono_sim_remove_state_watch(priv->sim,
						priv->state_watch_id);
				/* ril_sim_settings_state_watch_done clears it */
				GASSERT(!priv->state_watch_id);
			}
			priv->sim = sim;
			if (sim) {
				priv->state_watch_id =
					ofono_sim_add_state_watch(sim,
					ril_sim_settings_state_watch, self,
					ril_sim_settings_state_watch_done);
				GASSERT(priv->state_watch_id);
				if (ofono_sim_get_state(sim) ==
						OFONO_SIM_STATE_READY) {
					ril_sim_settings_ready(self);
				}
			} else {
				ril_sim_settings_set_imsi(self, NULL);
			}
		}
	}
}

gulong ril_sim_settings_add_imsi_changed_handler(struct ril_sim_settings *self,
					ril_sim_settings_cb_t cb, void *arg)
{
	return (G_LIKELY(self) && G_LIKELY(cb)) ? g_signal_connect(self,
		SIGNAL_IMSI_CHANGED_NAME, G_CALLBACK(cb), arg) : 0;
}

gulong ril_sim_settings_add_pref_mode_changed_handler(
					struct ril_sim_settings *self,
					ril_sim_settings_cb_t cb, void *arg)
{
	return (G_LIKELY(self) && G_LIKELY(cb)) ? g_signal_connect(self,
		SIGNAL_PREF_MODE_CHANGED_NAME, G_CALLBACK(cb), arg) : 0;
}

void ril_sim_settings_remove_handler(struct ril_sim_settings *self, gulong id)
{
	if (G_LIKELY(self) && G_LIKELY(id)) {
		g_signal_handler_disconnect(self, id);
	}
}

void ril_sim_settings_remove_handlers(struct ril_sim_settings *self,
						gulong *ids, int count)
{
	gutil_disconnect_handlers(self, ids, count);
}

struct ril_sim_settings *ril_sim_settings_new(const struct ril_slot_config *sc)
{
	struct ril_sim_settings *self = g_object_new(RIL_SIM_SETTINGS_TYPE, 0);
	self->enable_4g = sc->enable_4g;
	self->slot = sc->slot;
	self->pref_mode = RIL_SIM_STORE_PREF_MODE_DEFAULT(self);
	return self;
}

struct ril_sim_settings *ril_sim_settings_ref(struct ril_sim_settings *self)
{
	if (G_LIKELY(self)) {
		g_object_ref(RIL_SIM_SETTINGS(self));
		return self;
	} else {
		return NULL;
	}
}

void ril_sim_settings_unref(struct ril_sim_settings *self)
{
	if (G_LIKELY(self)) {
		g_object_unref(RIL_SIM_SETTINGS(self));
	}
}

static void ril_sim_settings_init(struct ril_sim_settings *self)
{
	self->priv =  G_TYPE_INSTANCE_GET_PRIVATE(self, RIL_SIM_SETTINGS_TYPE,
						struct ril_sim_settings_priv);
}

static void ril_sim_settings_dispose(GObject *object)
{
	struct ril_sim_settings *self = RIL_SIM_SETTINGS(object);

	ril_sim_settings_set_ofono_sim(self, NULL);
	G_OBJECT_CLASS(ril_sim_settings_parent_class)->dispose(object);
}

static void ril_sim_settings_class_init(RilSimSettingsClass *klass)
{
	GObjectClass *object_class = G_OBJECT_CLASS(klass);

	object_class->dispose = ril_sim_settings_dispose;
	g_type_class_add_private(klass, sizeof(struct ril_sim_settings_priv));
	NEW_SIGNAL(klass, IMSI);
	NEW_SIGNAL(klass, PREF_MODE);
}

/*
 * Local Variables:
 * mode: C
 * c-basic-offset: 8
 * indent-tabs-mode: t
 * End:
 */
