/*
 *  oFono - Open Source Telephony - RIL-based devices
 *
 *  Copyright (C) 2016-2019 Jolla Ltd.
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

#include <ofono/watch.h>

#include <gutil_misc.h>

#define RIL_PREF_MODE_DEFAULT(self)                     (\
	((self)->techs & OFONO_RADIO_ACCESS_MODE_LTE) ?  \
	OFONO_RADIO_ACCESS_MODE_LTE :                    \
	((self)->techs & OFONO_RADIO_ACCESS_MODE_UMTS) ? \
	OFONO_RADIO_ACCESS_MODE_UMTS :                   \
	OFONO_RADIO_ACCESS_MODE_GSM)

typedef GObjectClass RilSimSettingsClass;
typedef struct ril_sim_settings RilSimSettings;

enum ofono_watch_events {
	WATCH_EVENT_IMSI,
	WATCH_EVENT_COUNT
};

struct ril_sim_settings_priv {
	gulong watch_event_id[WATCH_EVENT_COUNT];
	struct ofono_watch *watch;
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

/* Skip the leading slash from the modem path: */
#define DBG_(obj,fmt,args...) DBG("%s " fmt, (obj)->path+1, ##args)

static void ril_sim_settings_signal_emit(struct ril_sim_settings *self,
					enum ril_sim_settings_signal id)
{
	g_signal_emit(self, ril_sim_settings_signals[id], 0);
}

void ril_sim_settings_set_pref_mode(struct ril_sim_settings *self,
					enum ofono_radio_access_mode mode)
{
	if (G_LIKELY(self) && self->pref_mode != mode) {
		self->pref_mode = mode;
		ril_sim_settings_signal_emit(self, SIGNAL_PREF_MODE_CHANGED);
	}
}

static void ril_sim_settings_imsi_changed(struct ofono_watch *watch,
							void *user_data)
{
	struct ril_sim_settings *self = RIL_SIM_SETTINGS(user_data);
	struct ril_sim_settings_priv *priv = self->priv;

	if (g_strcmp0(priv->imsi, watch->imsi)) {
		g_free(priv->imsi);
		self->imsi = priv->imsi = g_strdup(watch->imsi);
		ril_sim_settings_signal_emit(self, SIGNAL_IMSI_CHANGED);
	}
}

struct ril_sim_settings *ril_sim_settings_new(const char *path,
					enum ofono_radio_access_mode techs)
{
	struct ril_sim_settings *self = NULL;

	if (G_LIKELY(path)) {
		struct ril_sim_settings_priv *priv;

		self = g_object_new(RIL_SIM_SETTINGS_TYPE, NULL);
		priv = self->priv;
		self->techs = techs;
		self->pref_mode = RIL_PREF_MODE_DEFAULT(self);
		priv->watch = ofono_watch_new(path);
		priv->watch_event_id[WATCH_EVENT_IMSI] =
			ofono_watch_add_imsi_changed_handler(priv->watch,
				ril_sim_settings_imsi_changed, self);
		self->imsi = priv->imsi = g_strdup(priv->watch->imsi);
	}

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

static void ril_sim_settings_init(struct ril_sim_settings *self)
{
	self->priv =  G_TYPE_INSTANCE_GET_PRIVATE(self, RIL_SIM_SETTINGS_TYPE,
						struct ril_sim_settings_priv);
}

static void ril_sim_settings_finalize(GObject *object)
{
	struct ril_sim_settings *self = RIL_SIM_SETTINGS(object);
	struct ril_sim_settings_priv *priv = self->priv;

	ofono_watch_remove_all_handlers(priv->watch, priv->watch_event_id);
	ofono_watch_unref(priv->watch);
	g_free(priv->imsi);
	G_OBJECT_CLASS(ril_sim_settings_parent_class)->finalize(object);
}

static void ril_sim_settings_class_init(RilSimSettingsClass *klass)
{
	G_OBJECT_CLASS(klass)->finalize = ril_sim_settings_finalize;
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
