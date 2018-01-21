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
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 */

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include "sailfish_sim_info.h"
#include "sailfish_watch.h"

#include <gutil_misc.h>
#include <gutil_log.h>

#include "src/ofono.h"
#include "src/common.h"
#include "src/storage.h"

#define SAILFISH_SIM_INFO_STORE          "cache"
#define SAILFISH_SIM_INFO_STORE_GROUP    "sim"
#define SAILFISH_SIM_INFO_STORE_SPN      "spn"

/* ICCID -> IMSI map */
#define SAILFISH_SIM_ICCID_MAP           "iccidmap"
#define SAILFISH_SIM_ICCID_MAP_IMSI      "imsi"

#define DEFAULT_SPN_BUFSIZE 8
G_STATIC_ASSERT(DEFAULT_SPN_BUFSIZE >= \
	OFONO_MAX_MCC_LENGTH + OFONO_MAX_MNC_LENGTH + 1);

typedef GObjectClass SailfishSimInfoClass;
typedef struct sailfish_sim_info SailfishSimInfo;

enum sailfish_watch_events {
	WATCH_EVENT_SIM,
	WATCH_EVENT_SIM_STATE,
	WATCH_EVENT_ICCID,
	WATCH_EVENT_IMSI,
	WATCH_EVENT_SPN,
	WATCH_EVENT_NETREG,
	WATCH_EVENT_COUNT
};

struct sailfish_sim_info_priv {
	struct sailfish_watch *watch;
	struct ofono_netreg *netreg;
	char *iccid;
	char *imsi;
	char *cached_spn;
	char *sim_spn;
	char *public_spn;
	char default_spn[DEFAULT_SPN_BUFSIZE];
	gulong watch_event_id[WATCH_EVENT_COUNT];
	guint netreg_status_watch_id;
	gboolean update_imsi_cache;
	gboolean update_iccid_map;
	int queued_signals;
};

enum sailfish_sim_info_signal {
	SIGNAL_ICCID_CHANGED,
	SIGNAL_IMSI_CHANGED,
	SIGNAL_SPN_CHANGED,
	SIGNAL_COUNT
};

#define SIGNAL_ICCID_CHANGED_NAME   "sailfish-siminfo-iccid-changed"
#define SIGNAL_IMSI_CHANGED_NAME    "sailfish-siminfo-imsi-changed"
#define SIGNAL_SPN_CHANGED_NAME     "sailfish-siminfo-spn-changed"

static guint sailfish_sim_info_signals[SIGNAL_COUNT] = { 0 };

G_DEFINE_TYPE(SailfishSimInfo, sailfish_sim_info, G_TYPE_OBJECT)
#define SAILFISH_SIMINFO_TYPE (sailfish_sim_info_get_type())
#define SAILFISH_SIMINFO(obj) (G_TYPE_CHECK_INSTANCE_CAST((obj),\
	SAILFISH_SIMINFO_TYPE, SailfishSimInfo))

#define NEW_SIGNAL(klass,name) \
	sailfish_sim_info_signals[SIGNAL_##name##_CHANGED] = \
		g_signal_new(SIGNAL_##name##_CHANGED_NAME, \
			G_OBJECT_CLASS_TYPE(klass), G_SIGNAL_RUN_FIRST, \
			0, NULL, NULL, NULL, G_TYPE_NONE, 0)

/* Skip the leading slash from the modem path: */
#define DBG_(obj,fmt,args...) DBG("%s " fmt, (obj)->path+1, ##args)

static int sailfish_sim_info_signal_bit(enum sailfish_sim_info_signal id)
{
	return (1 << id);
}

static void sailfish_sim_info_signal_emit(struct sailfish_sim_info *self,
					enum sailfish_sim_info_signal id)
{
	self->priv->queued_signals &= ~sailfish_sim_info_signal_bit(id);
	g_signal_emit(self, sailfish_sim_info_signals[id], 0);
}

static void sailfish_sim_info_signal_queue(struct sailfish_sim_info *self,
					enum sailfish_sim_info_signal id)
{
	self->priv->queued_signals |= sailfish_sim_info_signal_bit(id);
}

static void sailfish_sim_info_emit_queued_signals
					(struct sailfish_sim_info *self)
{
	struct sailfish_sim_info_priv *priv = self->priv;
	int i;

	for (i = 0; priv->queued_signals && i < SIGNAL_COUNT; i++) {
		if (priv->queued_signals & sailfish_sim_info_signal_bit(i)) {
			sailfish_sim_info_signal_emit(self, i);
		}
	}
}

static void sailfish_sim_info_update_imsi_cache(struct sailfish_sim_info *self)
{
	struct sailfish_sim_info_priv *priv = self->priv;

	if (priv->update_imsi_cache && priv->imsi && priv->imsi[0] &&
				priv->cached_spn && priv->cached_spn[0]) {
		gboolean save = FALSE;
		const char *store = SAILFISH_SIM_INFO_STORE;
		GKeyFile *cache = storage_open(priv->imsi, store);
		char *spn = g_key_file_get_string(cache,
					SAILFISH_SIM_INFO_STORE_GROUP,
					SAILFISH_SIM_INFO_STORE_SPN, NULL);

		if (g_strcmp0(priv->cached_spn, spn)) {
			save = TRUE;
			g_key_file_set_string(cache,
					SAILFISH_SIM_INFO_STORE_GROUP,
					SAILFISH_SIM_INFO_STORE_SPN,
					priv->cached_spn);
		}

		/*
		 * Since we are most likely running on flash which
		 * supports a limited number of writes, don't overwrite
		 * the file unless something has actually changed.
		 */
		if (save) {
			DBG_(self, "updating " STORAGEDIR "/%s/%s",
							priv->imsi, store);
			storage_close(priv->imsi, store, cache, TRUE);
		} else {
			g_key_file_free(cache);
		}

		g_free(spn);
		priv->update_imsi_cache = FALSE;
	}
}

static void sailfish_sim_info_update_iccid_map(struct sailfish_sim_info *self)
{
	struct sailfish_sim_info_priv *priv = self->priv;

	if (priv->update_iccid_map && priv->iccid && priv->iccid[0] &&
						priv->imsi && priv->imsi[0]) {
		const char *store = SAILFISH_SIM_ICCID_MAP;
		GKeyFile *map = storage_open(NULL, store);
		char *imsi = g_key_file_get_string(map,
			SAILFISH_SIM_ICCID_MAP_IMSI, priv->iccid, NULL);

		/*
		 * Since we are most likely running on flash which
		 * supports a limited number of writes, don't overwrite
		 * the file unless something has actually changed.
		 */
		if (g_strcmp0(imsi, priv->imsi)) {
			DBG_(self, "updating " STORAGEDIR "/%s", store);
			g_key_file_set_string(map, SAILFISH_SIM_ICCID_MAP_IMSI,
						priv->iccid, priv->imsi);
			storage_close(NULL, store, map, TRUE);
		} else {
			g_key_file_free(map);
		}

		g_free(imsi);
		priv->update_iccid_map = FALSE;
	}
}

static void sailfish_sim_info_update_public_spn(struct sailfish_sim_info *self)
{
	struct sailfish_sim_info_priv *priv = self->priv;
	const char *spn = priv->sim_spn ? priv->sim_spn :
				priv->cached_spn ? priv->cached_spn :
				priv->default_spn;

	if (g_strcmp0(priv->public_spn, spn)) {
		g_free(priv->public_spn);
		if (spn && spn[0]) {
			DBG_(self, "public spn \"%s\"", spn);
			priv->public_spn = g_strdup(spn);
		} else {
			DBG_(self, "no public spn");
			priv->public_spn = NULL;
		}
		self->spn = priv->public_spn;
		sailfish_sim_info_signal_queue(self, SIGNAL_SPN_CHANGED);
	}
}

static void sailfish_sim_info_set_cached_spn(struct sailfish_sim_info *self,
							const char *spn)
{
	struct sailfish_sim_info_priv *priv = self->priv;

	GASSERT(spn);
	if (g_strcmp0(priv->cached_spn, spn)) {
		DBG_(self, "%s", spn);
		g_free(priv->cached_spn);
		priv->cached_spn = g_strdup(spn);
		priv->update_imsi_cache = TRUE;
		sailfish_sim_info_update_imsi_cache(self);
		sailfish_sim_info_update_public_spn(self);
	}
}

static void sailfish_sim_info_set_spn(struct sailfish_sim_info *self,
							const char *spn)
{
	struct sailfish_sim_info_priv *priv = self->priv;

	GASSERT(spn);
	if (g_strcmp0(priv->sim_spn, spn)) {
		DBG_(self, "%s", spn);
		g_free(priv->sim_spn);
		priv->sim_spn = g_strdup(spn);
		priv->update_imsi_cache = TRUE;
		sailfish_sim_info_set_cached_spn(self, spn);
		sailfish_sim_info_update_imsi_cache(self);
		sailfish_sim_info_update_public_spn(self);
	}
}

static void sailfish_sim_info_update_spn(struct sailfish_sim_info *self)
{
	struct sailfish_watch *watch = self->priv->watch;

	if (watch->spn && watch->spn[0]) {
		sailfish_sim_info_set_spn(self, watch->spn);
	}
}

static void sailfish_sim_info_update_default_spn(struct sailfish_sim_info *self)
{
	struct sailfish_sim_info_priv *priv = self->priv;
	struct ofono_sim *sim = priv->watch->sim;
	char buf[DEFAULT_SPN_BUFSIZE];
	const char *mcc = NULL;
	const char *mnc = NULL;

	if (sim && ofono_sim_get_state(sim) == OFONO_SIM_STATE_READY) {
		mcc = ofono_sim_get_mcc(sim);
		mnc = ofono_sim_get_mnc(sim);
	}

	if (mcc && mnc) {
		snprintf(buf, DEFAULT_SPN_BUFSIZE, "%s%s", mcc, mnc);
		buf[DEFAULT_SPN_BUFSIZE - 1] = 0;
	} else {
		buf[0] = 0;
	}

	if (strcmp(buf, priv->default_spn)) {
		strncpy(priv->default_spn, buf, DEFAULT_SPN_BUFSIZE);
		DBG_(self, "default spn \"%s\"", priv->default_spn);
		sailfish_sim_info_update_public_spn(self);
	}
}

static void sailfish_sim_info_update_imsi(struct sailfish_sim_info *self)
{
	struct sailfish_sim_info_priv *priv = self->priv;
	const char *imsi = priv->watch->imsi;

	/* IMSI only gets reset when ICCID disappears, ignore NULL IMSI here */
	if (imsi && imsi[0] && g_strcmp0(priv->imsi, imsi)) {
		DBG_(self, "%s", imsi);
		g_free(priv->imsi);
		self->imsi = priv->imsi = g_strdup(imsi);
		priv->update_iccid_map = TRUE;
		sailfish_sim_info_update_iccid_map(self);
		sailfish_sim_info_update_imsi_cache(self);
		sailfish_sim_info_signal_queue(self, SIGNAL_IMSI_CHANGED);
	}

	/* Check if MCC/MNC have changed */
	sailfish_sim_info_update_default_spn(self);
}

static void sailfish_sim_info_network_check(struct sailfish_sim_info *self)
{
	struct sailfish_sim_info_priv *priv = self->priv;
	struct ofono_sim *sim = priv->watch->sim;
	enum network_registration_status reg_status =
		ofono_netreg_get_status(priv->netreg);

	if (sim && ofono_sim_get_state(sim) == OFONO_SIM_STATE_READY &&
		(reg_status == NETWORK_REGISTRATION_STATUS_REGISTERED ||
			reg_status == NETWORK_REGISTRATION_STATUS_ROAMING)) {
		const char *sim_mcc = ofono_sim_get_mcc(sim);
		const char *sim_mnc = ofono_sim_get_mnc(sim);
		const char *net_mcc = ofono_netreg_get_mcc(priv->netreg);
		const char *net_mnc = ofono_netreg_get_mnc(priv->netreg);
		const char *name = ofono_netreg_get_name(priv->netreg);

		if (sim_mcc && sim_mcc[0] && sim_mnc && sim_mnc[0] &&
			net_mcc && net_mcc[0] && net_mnc && net_mnc[0] &&
			name && name[0] && !strcmp(sim_mcc, net_mcc) &&
			!strcmp(sim_mnc, net_mnc)) {

			/*
			 * If EFspn is present then sim_spn should be set
			 * before we get registered with the network.
			 */
			DBG_(self, "home network \"%s\"", name);
			if (!priv->sim_spn) {
				sailfish_sim_info_set_cached_spn(self, name);
			}
		}
	}
}

static void sailfish_sim_info_load_cache(struct sailfish_sim_info *self)
{
	struct sailfish_sim_info_priv *priv = self->priv;

	if (priv->iccid && priv->iccid[0]) {
		GKeyFile *map = storage_open(NULL, SAILFISH_SIM_ICCID_MAP);
		char *imsi = g_key_file_get_string(map,
			SAILFISH_SIM_ICCID_MAP_IMSI, priv->iccid, NULL);
		g_key_file_free(map);

		if (imsi && imsi[0] && g_strcmp0(priv->imsi, imsi)) {
			if (priv->imsi && priv->imsi[0]) {
				/* Need to update ICCID -> IMSI map */
				DBG_(self, "IMSI changed %s -> %s",
							priv->imsi, imsi);
				priv->update_imsi_cache = TRUE;
			}
			g_free(priv->imsi);
			self->imsi = priv->imsi = imsi;
			DBG_(self, "imsi[%s] = %s", priv->iccid, imsi);
			sailfish_sim_info_update_iccid_map(self);
			sailfish_sim_info_update_default_spn(self);
			sailfish_sim_info_signal_queue(self,
						SIGNAL_IMSI_CHANGED);
		} else if (imsi) {
			g_free(imsi);
		} else {
			DBG_(self, "no imsi for iccid %s", priv->iccid);
		}
	}

	if (priv->imsi && priv->imsi[0]) {
		GKeyFile *cache = storage_open(priv->imsi,
					SAILFISH_SIM_INFO_STORE);
		char *spn = g_key_file_get_string(cache,
					SAILFISH_SIM_INFO_STORE_GROUP,
					SAILFISH_SIM_INFO_STORE_SPN, NULL);
		g_key_file_free(cache);

		if (spn && spn[0] && g_strcmp0(priv->cached_spn, spn)) {
			if (priv->cached_spn && priv->cached_spn[0]) {
				/* Need to update the cache file */
				DBG_(self, "spn changing %s -> %s",
						priv->cached_spn, spn);
				priv->update_imsi_cache = TRUE;
			}
			g_free(priv->cached_spn);
			priv->cached_spn = spn;
			DBG_(self, "spn[%s] = \"%s\"", priv->imsi, spn);
			sailfish_sim_info_update_imsi_cache(self);
			sailfish_sim_info_update_public_spn(self);
		} else if (spn) {
			g_free(spn);
		} else {
			DBG_(self, "no spn for imsi %s", priv->imsi);
		}
	}
}

static void sailfish_sim_info_set_iccid(struct sailfish_sim_info *self,
							const char *iccid)
{
	struct sailfish_sim_info_priv *priv = self->priv;

	if (g_strcmp0(priv->iccid, iccid)) {
		g_free(priv->iccid);
		self->iccid = priv->iccid = g_strdup(iccid);
		sailfish_sim_info_signal_queue(self, SIGNAL_ICCID_CHANGED);
		if (iccid) {
			sailfish_sim_info_load_cache(self);
		} else {
			DBG_(self, "no more iccid");
			if (priv->imsi) {
				g_free(priv->imsi);
				self->imsi = priv->imsi = NULL;
				sailfish_sim_info_signal_queue(self,
							SIGNAL_IMSI_CHANGED);
			}
			if (priv->sim_spn) {
				g_free(priv->sim_spn);
				priv->sim_spn = NULL;
			}
			if (priv->cached_spn) {
				g_free(priv->cached_spn);
				priv->cached_spn = NULL;
			}
			/* No more default SPN too */
			priv->default_spn[0] = 0;
			sailfish_sim_info_update_public_spn(self);
		}
	}
}

static void sailfish_sim_info_iccid_watch_cb(struct sailfish_watch *watch,
								void *data)
{
	struct sailfish_sim_info *self = SAILFISH_SIMINFO(data);

	DBG_(self, "%s", watch->iccid);
	sailfish_sim_info_set_iccid(self, watch->iccid);
	sailfish_sim_info_emit_queued_signals(self);
}

static void sailfish_sim_info_imsi_watch_cb(struct sailfish_watch *watch,
								void *data)
{
	struct sailfish_sim_info *self = SAILFISH_SIMINFO(data);

	sailfish_sim_info_update_imsi(self);
	sailfish_sim_info_emit_queued_signals(self);
}

static void sailfish_sim_info_spn_watch_cb(struct sailfish_watch *watch,
								void *data)
{
	struct sailfish_sim_info *self = SAILFISH_SIMINFO(data);

	sailfish_sim_info_update_spn(self);
	sailfish_sim_info_emit_queued_signals(self);
}

static void sailfish_sim_info_netreg_watch(int status, int lac, int ci,
		int tech, const char *mcc, const char *mnc, void *data)
{
	struct sailfish_sim_info *self = SAILFISH_SIMINFO(data);

	sailfish_sim_info_network_check(self);
	sailfish_sim_info_emit_queued_signals(self);
}

static void sailfish_sim_info_netreg_watch_done(void *data)
{
	struct sailfish_sim_info *self = SAILFISH_SIMINFO(data);
	struct sailfish_sim_info_priv *priv = self->priv;

	GASSERT(priv->netreg_status_watch_id);
	priv->netreg_status_watch_id = 0;
}

static void sailfish_sim_info_set_netreg(struct sailfish_sim_info *self,
						struct ofono_netreg *netreg)
{
	struct sailfish_sim_info_priv *priv = self->priv;

	if (priv->netreg != netreg) {
		if (netreg) {
			DBG_(self, "netreg attached");
			priv->netreg = netreg;
			priv->netreg_status_watch_id =
				__ofono_netreg_add_status_watch(netreg,
					sailfish_sim_info_netreg_watch, self,
					sailfish_sim_info_netreg_watch_done);
			sailfish_sim_info_network_check(self);
		} else if (priv->netreg) {
			if (priv->netreg_status_watch_id) {
				__ofono_netreg_remove_status_watch(priv->netreg,
						priv->netreg_status_watch_id);
				GASSERT(!priv->netreg_status_watch_id);
			}
			DBG_(self, "netreg detached");
			priv->netreg = NULL;
		}
	}
}

static void sailfish_sim_info_netreg_changed(struct sailfish_watch *watch,
								void *data)
{
	struct sailfish_sim_info *self = SAILFISH_SIMINFO(data);

	sailfish_sim_info_set_netreg(self, watch->netreg);
	sailfish_sim_info_emit_queued_signals(self);
}

struct sailfish_sim_info *sailfish_sim_info_new(const char *path)
{
	struct sailfish_sim_info *self = NULL;

	if (path) {
		struct sailfish_watch *watch = sailfish_watch_new(path);
		struct sailfish_sim_info_priv *priv;

		self = g_object_new(SAILFISH_SIMINFO_TYPE, NULL);
		priv = self->priv;
		priv->watch = watch;
		self->path = watch->path;
		priv->watch_event_id[WATCH_EVENT_ICCID] =
			sailfish_watch_add_iccid_changed_handler(watch,
				sailfish_sim_info_iccid_watch_cb, self);
		priv->watch_event_id[WATCH_EVENT_IMSI] =
			sailfish_watch_add_imsi_changed_handler(watch,
				sailfish_sim_info_imsi_watch_cb, self);
		priv->watch_event_id[WATCH_EVENT_SPN] =
			sailfish_watch_add_spn_changed_handler(watch,
				sailfish_sim_info_spn_watch_cb, self);
		priv->watch_event_id[WATCH_EVENT_NETREG] =
			sailfish_watch_add_netreg_changed_handler(watch,
				sailfish_sim_info_netreg_changed, self);
		sailfish_sim_info_set_iccid(self, watch->iccid);
		sailfish_sim_info_set_netreg(self, watch->netreg);
		sailfish_sim_info_update_imsi(self);
		sailfish_sim_info_update_spn(self);
		sailfish_sim_info_network_check(self);

		/* Clear queued events, if any */
		priv->queued_signals = 0;
	}
	return self;
}

struct sailfish_sim_info *sailfish_sim_info_ref(struct sailfish_sim_info *self)
{
	if (self) {
		g_object_ref(SAILFISH_SIMINFO(self));
		return self;
	} else {
		return NULL;
	}
}

void sailfish_sim_info_unref(struct sailfish_sim_info *self)
{
	if (self) {
		g_object_unref(SAILFISH_SIMINFO(self));
	}
}

gulong sailfish_sim_info_add_iccid_changed_handler(struct sailfish_sim_info *s,
					sailfish_sim_info_cb_t cb, void *arg)
{
	return (s && cb) ? g_signal_connect(s, SIGNAL_ICCID_CHANGED_NAME,
						G_CALLBACK(cb), arg) : 0;
}

gulong sailfish_sim_info_add_imsi_changed_handler(struct sailfish_sim_info *s,
					sailfish_sim_info_cb_t cb, void *arg)
{
	return (s && cb) ? g_signal_connect(s, SIGNAL_IMSI_CHANGED_NAME,
						G_CALLBACK(cb), arg) : 0;
}

gulong sailfish_sim_info_add_spn_changed_handler(struct sailfish_sim_info *s,
					sailfish_sim_info_cb_t cb, void *arg)
{
	return (s && cb) ? g_signal_connect(s, SIGNAL_SPN_CHANGED_NAME,
						G_CALLBACK(cb), arg) : 0;
}

void sailfish_sim_info_remove_handler(struct sailfish_sim_info *s, gulong id)
{
	if (s && id) {
		g_signal_handler_disconnect(s, id);
	}
}

void sailfish_sim_info_remove_handlers(struct sailfish_sim_info *self,
						gulong *ids, int count)
{
	gutil_disconnect_handlers(self, ids, count);
}

static void sailfish_sim_info_init(struct sailfish_sim_info *self)
{
	self->priv = G_TYPE_INSTANCE_GET_PRIVATE(self, SAILFISH_SIMINFO_TYPE,
					struct sailfish_sim_info_priv);
}

static void sailfish_sim_info_finalize(GObject *object)
{
	struct sailfish_sim_info *self = SAILFISH_SIMINFO(object);
	struct sailfish_sim_info_priv *priv = self->priv;

	sailfish_watch_remove_all_handlers(priv->watch, priv->watch_event_id);
	sailfish_watch_unref(priv->watch);
	g_free(priv->iccid);
	g_free(priv->imsi);
	g_free(priv->sim_spn);
	g_free(priv->cached_spn);
	g_free(priv->public_spn);
	G_OBJECT_CLASS(sailfish_sim_info_parent_class)->finalize(object);
}

static void sailfish_sim_info_class_init(SailfishSimInfoClass *klass)
{
	G_OBJECT_CLASS(klass)->finalize = sailfish_sim_info_finalize;
	g_type_class_add_private(klass, sizeof(struct sailfish_sim_info_priv));
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
