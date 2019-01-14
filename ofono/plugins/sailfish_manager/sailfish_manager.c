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
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 */

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <gutil_log.h>
#include <gutil_strv.h>
#include <gutil_macros.h>
#include <string.h>

#include <ofono/watch.h>

#include "src/ofono.h"
#include "src/storage.h"

#include <sailfish_manager.h>
#include <sailfish_cell_info.h>

#include "sailfish_manager_dbus.h"
#include "sailfish_cell_info_dbus.h"
#include "sailfish_sim_info.h"

/* How long we wait for all drivers to register (number of idle loops) */
#define SF_INIT_IDLE_COUNT (5)

enum ofono_watch_events {
	WATCH_EVENT_MODEM,
	WATCH_EVENT_ONLINE,
	WATCH_EVENT_IMSI,
	WATCH_EVENT_COUNT
};

struct sailfish_manager_priv {
	struct sailfish_manager pub; /* Public part */
	struct sailfish_slot_driver_reg *drivers;
	struct sailfish_manager_dbus *dbus;
	struct sailfish_slot_priv *voice_slot;
	struct sailfish_slot_priv *data_slot;
	struct sailfish_slot_priv *mms_slot;
	sailfish_slot_ptr *slots;
	int slot_count;
	guint init_countdown;
	guint init_id;
	char *default_voice_imsi;
	char *default_data_imsi;
	char *mms_imsi;
	GKeyFile *storage;
	GHashTable *errors;
};

struct sailfish_slot_driver_reg {
	struct sailfish_slot_driver_reg *next;
	const struct sailfish_slot_driver *driver;
	struct sailfish_manager_priv *plugin;
	struct sailfish_slot_manager *manager;
	guint init_id;
};

struct sailfish_slot_manager {
	const struct sailfish_slot_driver *driver;
	struct sailfish_manager_priv *plugin;
	struct sailfish_slot_manager_impl *impl;
	struct sailfish_slot_priv *slots;
	gboolean started;
	guint start_id;
};

struct sailfish_slot_priv {
	struct sailfish_slot pub;
	struct sailfish_slot_priv *next;
	struct sailfish_slot_manager *manager;
	struct sailfish_slot_impl *impl;
	struct ofono_watch *watch;
	struct sailfish_sim_info *siminfo;
	struct sailfish_sim_info_dbus *siminfo_dbus;
	struct sailfish_cell_info *cellinfo;
	struct sailfish_cell_info_dbus *cellinfo_dbus;
	enum sailfish_sim_state sim_state;
	enum sailfish_slot_flags flags;
	gulong watch_event_id[WATCH_EVENT_COUNT];
	char *imei;
	char *imeisv;
	gboolean enabled_changed;
	GHashTable *errors;
	int index;
};

/* "ril" is used for historical reasons */
#define SF_STORE                    "ril"
#define SF_STORE_GROUP              "Settings"
#define SF_STORE_ENABLED_SLOTS      "EnabledSlots"
#define SF_STORE_DEFAULT_VOICE_SIM  "DefaultVoiceSim"
#define SF_STORE_DEFAULT_DATA_SIM   "DefaultDataSim"
#define SF_STORE_SLOTS_SEP          ","

/* The file where error statistics is stored. Again "rilerror" is historical */
#define SF_ERROR_STORAGE            "rilerror" /* File name */
#define SF_ERROR_COMMON_SECTION     "common"   /* Modem independent section */

/* Path always starts with a slash, skip it */
#define sailfish_slot_debug_prefix(s) ((s)->pub.path + 1)

static int sailfish_manager_update_modem_paths(struct sailfish_manager_priv *);
static gboolean sailfish_manager_update_ready(struct sailfish_manager_priv *p);

static inline struct sailfish_manager_priv *sailfish_manager_priv_cast
						(struct sailfish_manager *m)
{
	return G_CAST(m, struct sailfish_manager_priv, pub);
}

static inline struct sailfish_slot_priv *sailfish_slot_priv_cast
						(struct sailfish_slot *s)
{
	return G_CAST(s, struct sailfish_slot_priv, pub);
}

static inline const struct sailfish_slot_priv *sailfish_slot_priv_cast_const
						(const struct sailfish_slot *s)
{
	return G_CAST(s, struct sailfish_slot_priv, pub);
}

static inline void sailfish_slot_set_data_role(struct sailfish_slot_priv *s,
						enum sailfish_data_role role)
{
	const struct sailfish_slot_driver *d = s->manager->driver;

	if (d->slot_set_data_role) {
		d->slot_set_data_role(s->impl, role);
	}
}

/* Update modem paths and emit D-Bus signal if necessary */
static void sailfish_manager_update_modem_paths_full
					(struct sailfish_manager_priv *p)
{
	sailfish_manager_dbus_signal(p->dbus,
			sailfish_manager_update_modem_paths(p));
}

/*
 * sailfish_manager_foreach_driver() and sailfish_manager_foreach_slot()
 * terminate the loop and return TRUE if the callback returns TRUE. If all
 * callbacks return FALSE, they returns FALSE. It there are no drivers/slots,
 * they return FALSE too.
 */

#define SF_LOOP_CONTINUE (FALSE)
#define SF_LOOP_DONE (TRUE)

static gboolean sailfish_manager_foreach_driver(struct sailfish_manager_priv *p,
	gboolean (*fn)(struct sailfish_slot_driver_reg *r, void *user_data),
	void *user_data)
{
	struct sailfish_slot_driver_reg *r = p->drivers;
	gboolean done = FALSE;

	while (r && !done) {
		struct sailfish_slot_driver_reg *rnext = r->next;

		/* The callback returns TRUE to terminate the loop */
		done = fn(r, user_data);
		r = rnext;
	}

	return done;
}

static gboolean sailfish_manager_foreach_slot
		(struct sailfish_manager_priv *p,
		gboolean (*fn)(struct sailfish_slot_priv *s, void *user_data),
		void *user_data)
{
	struct sailfish_slot_driver_reg *r = p->drivers;
	gboolean done = FALSE;

	while (r && !done) {
		struct sailfish_slot_manager *m = r->manager;
		struct sailfish_slot_driver_reg *rnext = r->next;

		if (m) {
			struct sailfish_slot_priv *s = m->slots;

			while (s) {
				struct sailfish_slot_priv *snext = s->next;

				/* The callback returns TRUE to terminate
				 * the loop */
				if (fn(s, user_data)) {
					done = TRUE;
					break;
				}
				s = snext;
			}
		}
		r = rnext;
	}

	return done;
}

static void sailfish_manager_slot_update_cell_info_dbus
					(struct sailfish_slot_priv *s)
{
	struct ofono_modem *modem = s->watch->modem;

	if (modem && s->cellinfo) {
		if (!s->cellinfo_dbus) {
			s->cellinfo_dbus = sailfish_cell_info_dbus_new(modem,
								s->cellinfo);
		}
	} else {
		if (s->cellinfo_dbus) {
			sailfish_cell_info_dbus_free(s->cellinfo_dbus);
			s->cellinfo_dbus = NULL;
		}
	}
}

static void sailfish_manager_slot_modem_changed(struct ofono_watch *w,
							void *user_data)
{
	struct sailfish_slot_priv *s = user_data;
	struct sailfish_manager_priv *p = s->manager->plugin;

	sailfish_manager_slot_update_cell_info_dbus(s);
	sailfish_manager_update_modem_paths_full(p);
	sailfish_manager_update_ready(p);
}

static void sailfish_manager_slot_imsi_changed(struct ofono_watch *w,
							void *user_data)
{
	struct sailfish_slot_priv *slot = user_data;
	struct sailfish_manager_priv *plugin = slot->manager->plugin;
	struct sailfish_slot_priv *voice_slot = plugin->voice_slot;
	struct sailfish_slot_priv *data_slot = plugin->data_slot;
	int signal_mask;

	/*
	 * We want the first slot to be selected by default.
	 * However, things may become available in pretty much
	 * any order, so reset the slot pointers to NULL and let
	 * sailfish_manager_update_modem_paths() to pick them again.
	 */
	plugin->voice_slot = NULL;
	plugin->data_slot = NULL;
	plugin->pub.default_voice_path = NULL;
	plugin->pub.default_data_path = NULL;
	signal_mask = sailfish_manager_update_modem_paths(plugin);
	if (voice_slot != plugin->voice_slot) {
		if (!plugin->voice_slot) {
			DBG("No default voice SIM");
		}
		signal_mask |= SAILFISH_MANAGER_SIGNAL_VOICE_PATH;
	}
	if (data_slot != plugin->data_slot) {
		if (!plugin->data_slot) {
			DBG("No default data SIM");
		}
		signal_mask |= SAILFISH_MANAGER_SIGNAL_DATA_PATH;
	}
	sailfish_manager_dbus_signal(plugin->dbus, signal_mask);
}

static gboolean sailfish_manager_count_slot(struct sailfish_slot_priv *s,
							void *user_data)
{
	(*((int *)user_data))++;
	return SF_LOOP_CONTINUE;
}

static gboolean sailfish_manager_index_slot(struct sailfish_slot_priv *s,
							void *user_data)
{
	struct sailfish_manager_priv *p = user_data;

	s->index = p->slot_count;
	p->slots[p->slot_count++] = &s->pub;
	return SF_LOOP_CONTINUE;
}

static void sailfish_manager_reindex_slots(struct sailfish_manager_priv *p)
{
	int count = 0;

	sailfish_manager_foreach_slot(p, sailfish_manager_count_slot, &count);

	g_free(p->slots);
	p->pub.slots = p->slots = g_new0(sailfish_slot_ptr, count + 1);

	/* p->slot_count is the index for sailfish_manager_index_slot */
	p->slot_count = 0;
	sailfish_manager_foreach_slot(p, sailfish_manager_index_slot, p);
	p->slots[p->slot_count] = NULL;
	GASSERT(p->slot_count == count);
}

static gboolean sailfish_manager_check_slot_name(struct sailfish_slot_priv *s,
							void *path)
{
	return strcmp(s->pub.path, path) ? SF_LOOP_CONTINUE : SF_LOOP_DONE;
}

struct sailfish_slot *sailfish_manager_slot_add
	(struct sailfish_slot_manager *m, struct sailfish_slot_impl *impl,
		const char *path, enum ofono_radio_access_mode techs,
		const char *imei, const char *imeisv,
		enum sailfish_sim_state sim_state)
{
	return sailfish_manager_slot_add2(m, impl, path, techs, imei, imeisv,
					sim_state, SAILFISH_SLOT_NO_FLAGS);
}

struct sailfish_slot *sailfish_manager_slot_add2
	(struct sailfish_slot_manager *m, struct sailfish_slot_impl *impl,
		const char *path, enum ofono_radio_access_mode techs,
		const char *imei, const char *imeisv,
		enum sailfish_sim_state sim_state,
		enum sailfish_slot_flags flags)
{
	/* Only accept these calls when we are starting! We have been
	 * assuming all along that the number of slots is known right
	 * from startup. Perhaps it wasn't a super bright idea because
	 * there are USB modems which can appear (and disappear) pretty
	 * much at any time. This has to be dealt with somehow at some
	 * point but for now let's leave it as is. */
	if (path && m && !m->started && !sailfish_manager_foreach_slot
				(m->plugin, sailfish_manager_check_slot_name,
							(char*)path)) {
		char *enabled_slots;
		struct sailfish_manager_priv *p = m->plugin;
		struct sailfish_slot_priv *s =
			g_slice_new0(struct sailfish_slot_priv);

		DBG("%s", path);
		s->impl = impl;
		s->manager = m;
		s->sim_state = sim_state;
		s->flags = flags;
		s->watch = ofono_watch_new(path);
		s->siminfo = sailfish_sim_info_new(path);
		s->siminfo_dbus = sailfish_sim_info_dbus_new(s->siminfo);
		s->pub.path = s->watch->path;
		s->pub.imei = s->imei = g_strdup(imei);
		s->pub.imeisv = s->imeisv = g_strdup(imeisv);
		s->pub.sim_present = (sim_state == SAILFISH_SIM_STATE_PRESENT);

		/* Check if it's enabled */
		enabled_slots = g_key_file_get_string(p->storage,
				SF_STORE_GROUP, SF_STORE_ENABLED_SLOTS, NULL);
		if (enabled_slots) {
			char **strv = g_strsplit(enabled_slots,
						SF_STORE_SLOTS_SEP, 0);

			DBG("Enabled slots: %s", enabled_slots);
			s->pub.enabled = gutil_strv_contains(strv, path);
			g_strfreev(strv);
			g_free(enabled_slots);
		} else {
			/* All slots are enabled by default */
			s->pub.enabled = TRUE;
		}

		/* Add it to the list */
		if (!m->slots) {
			/* The first one */
			m->slots = s;
		} else if (strcmp(m->slots->pub.path, path) > 0) {
			/* This one becomes the head of the list */
			s->next = m->slots;
			m->slots = s;
		} else {
			/* Need to do some sorting */
			struct sailfish_slot_priv *prev = m->slots;
			struct sailfish_slot_priv *slot = m->slots->next;

			while (slot && strcmp(slot->pub.path, path) < 0) {
				prev = slot;
				slot = prev->next;
			}

			s->next = prev->next;
			prev->next = s;
		}

		sailfish_manager_reindex_slots(m->plugin);

		/* Register for events */
		s->watch_event_id[WATCH_EVENT_MODEM] =
			ofono_watch_add_modem_changed_handler(s->watch,
				sailfish_manager_slot_modem_changed, s);
		s->watch_event_id[WATCH_EVENT_ONLINE] =
			ofono_watch_add_online_changed_handler(s->watch,
				sailfish_manager_slot_modem_changed, s);
		s->watch_event_id[WATCH_EVENT_IMSI] =
			ofono_watch_add_imsi_changed_handler(s->watch,
				sailfish_manager_slot_imsi_changed, s);

		return &s->pub;
	} else {
		ofono_error("Refusing to register slot %s", path);
	}

	return NULL;
}

static void sailfish_slot_free(struct sailfish_slot_priv *s)
{
	struct sailfish_slot_manager *m = s->manager;
	struct sailfish_manager_priv *p = m->plugin;

	if (s->impl) {
		const struct sailfish_slot_driver *d = m->driver;

		if (d->slot_free) {
			d->slot_free(s->impl);
			s->impl = NULL;
		}
	}
	if (s->errors) {
		g_hash_table_destroy(s->errors);
	}
	sailfish_sim_info_unref(s->siminfo);
	sailfish_sim_info_dbus_free(s->siminfo_dbus);
	sailfish_cell_info_dbus_free(s->cellinfo_dbus);
	sailfish_cell_info_unref(s->cellinfo);
	ofono_watch_remove_all_handlers(s->watch, s->watch_event_id);
	ofono_watch_unref(s->watch);
	g_free(s->imei);
	g_free(s->imeisv);
	s->next = NULL;
	s->manager = NULL;
	g_slice_free(struct sailfish_slot_priv, s);
	sailfish_manager_reindex_slots(p);
}

void sailfish_manager_set_cell_info(struct sailfish_slot *s,
					struct sailfish_cell_info *info)
{
	if (s) {
		struct sailfish_slot_priv *slot = sailfish_slot_priv_cast(s);

		if (slot->cellinfo != info) {
			sailfish_cell_info_dbus_free(slot->cellinfo_dbus);
			sailfish_cell_info_unref(slot->cellinfo);
			slot->cellinfo = sailfish_cell_info_ref(info);
			slot->cellinfo_dbus = NULL;
			sailfish_manager_slot_update_cell_info_dbus(slot);
		}
	}
}

static gboolean sailfish_manager_update_dbus_block_proc
			(struct sailfish_slot_driver_reg *r, void *data)
{
	enum sailfish_manager_dbus_block *block = data;
	struct sailfish_slot_manager *m;
	struct sailfish_slot_priv *s;

	if (r->init_id) {
		/* Driver is being initialized */
		(*block) |= SAILFISH_MANAGER_DBUS_BLOCK_ALL;
		return SF_LOOP_DONE;
	}

	m = r->manager;
	if (!m) {
		return SF_LOOP_CONTINUE;
	}

	if (!m->started) {
		/* Slots are being initialized */
		(*block) |= SAILFISH_MANAGER_DBUS_BLOCK_ALL;
		return SF_LOOP_DONE;
	}

	for (s = m->slots; s && s->imei; s = s->next);
	if (s) {
		/* IMEI is not available (yet) */
		(*block) |= SAILFISH_MANAGER_DBUS_BLOCK_IMEI;
	}

	return SF_LOOP_CONTINUE;
}

static void sailfish_manager_update_dbus_block(struct sailfish_manager_priv *p)
{
	enum sailfish_manager_dbus_block block =
		SAILFISH_MANAGER_DBUS_BLOCK_NONE;

	if (p->init_countdown) {
		/* Plugin is being initialized */
		block |= SAILFISH_MANAGER_DBUS_BLOCK_ALL;
	} else {
		sailfish_manager_foreach_driver(p,
			sailfish_manager_update_dbus_block_proc, &block);
	}

	sailfish_manager_dbus_set_block(p->dbus, block);
}

static void sailfish_manager_set_config_string
			(struct sailfish_manager_priv *p, const char *key,
					const char *value)
{
	if (value) {
		g_key_file_set_string(p->storage, SF_STORE_GROUP, key, value);
	} else {
		g_key_file_remove_key(p->storage, SF_STORE_GROUP, key, NULL);
	}
	storage_sync(NULL, SF_STORE, p->storage);
}

struct sailfish_manager_slot_imsi_data {
	struct sailfish_slot_priv *slot;
	const char *imsi;
};

static gboolean sailfish_manager_find_slot_imsi_proc
			(struct sailfish_slot_priv *s, void *user_data)
{
	struct sailfish_manager_slot_imsi_data *data = user_data;
	const char *slot_imsi = s->watch->imsi;

	if (slot_imsi && !strcmp(slot_imsi, data->imsi)) {
		data->slot = s;
		return SF_LOOP_DONE;
	} else {
		return SF_LOOP_CONTINUE;
	}
}

struct sailfish_manager_any_slot_data {
	struct sailfish_slot_priv *slot;
};

static gboolean sailfish_manager_find_any_slot_proc
			(struct sailfish_slot_priv *s, void *user_data)
{
	struct sailfish_manager_any_slot_data *data = user_data;
	const char *slot_imsi = s->watch->imsi;

	if (slot_imsi) {
		data->slot = s;
		return SF_LOOP_DONE;
	} else {
		return SF_LOOP_CONTINUE;
	}
}

static struct sailfish_slot_priv *sailfish_manager_find_slot_imsi
					(struct sailfish_manager_priv *p,
							const char *imsi)
{
	if (imsi) {
		/* We are looking for the specific sim */
		struct sailfish_manager_slot_imsi_data data;

		memset(&data, 0, sizeof(data));
		data.imsi = imsi;
		sailfish_manager_foreach_slot(p,
			sailfish_manager_find_slot_imsi_proc, &data);
		return data.slot;
	} else {
		/* We are looking for any slot with a sim */
		struct sailfish_manager_any_slot_data data;

		memset(&data, 0, sizeof(data));
		sailfish_manager_foreach_slot(p,
			sailfish_manager_find_any_slot_proc, &data);
		return data.slot;
	}
}

/* Returns the event mask to be passed to sailfish_manager_dbus_signal.
 * The caller has a chance to OR it with other bits */
static int sailfish_manager_update_modem_paths(struct sailfish_manager_priv *p)
{
	int mask = 0;
	struct sailfish_slot_priv *slot = NULL;
	struct sailfish_slot_priv *mms_slot = NULL;
	struct sailfish_slot_priv *old_data_slot = NULL;
	struct sailfish_slot_priv *new_data_slot = NULL;

	/* Voice */
	if (p->default_voice_imsi) {
		slot = sailfish_manager_find_slot_imsi(p,
				p->default_voice_imsi);
	} else if (p->voice_slot) {
		/* Make sure that the slot is enabled and SIM is in */
		slot = sailfish_manager_find_slot_imsi(p,
						p->voice_slot->watch->imsi);
	}

	/*
	 * If there's no default voice SIM, we will find any SIM instead.
	 * One should always be able to make and receive a phone call
	 * if there's a working SIM in the phone. However if the
	 * previously selected voice SIM is inserted, we will switch
	 * back to it.
	 *
	 * There is no such fallback for the data.
	 */
	if (!slot) {
		slot = sailfish_manager_find_slot_imsi(p, NULL);
	}

	if (p->voice_slot != slot) {
		mask |= SAILFISH_MANAGER_SIGNAL_VOICE_PATH;
		p->voice_slot = slot;
		if (slot) {
			const char *path = slot->pub.path;
			DBG("Default voice SIM at %s", path);
			p->pub.default_voice_path = path;
		} else {
			DBG("No default voice SIM");
			p->pub.default_voice_path = NULL;
		}
	}

	/* Data */
	if (p->default_data_imsi) {
		slot = sailfish_manager_find_slot_imsi(p,
						p->default_data_imsi);
	} else if (p->slot_count < 2) {
		if (p->data_slot) {
			/* Make sure that the slot is enabled and SIM is in */
			slot = sailfish_manager_find_slot_imsi(p,
						p->data_slot->watch->imsi);
		} else {
			/* Check if anything is available */
			slot = sailfish_manager_find_slot_imsi(p, NULL);
		}
	} else {
		/*
		 * Should we automatically select the default data sim
		 * on a multisim phone that has only one sim inserted?
		 */
		slot = NULL;
	}

	if (slot && !slot->watch->online) {
		slot = NULL;
	}

	if (p->mms_imsi) {
		mms_slot = sailfish_manager_find_slot_imsi(p, p->mms_imsi);
	}

	if (mms_slot && (mms_slot != slot ||
			(slot->flags & SAILFISH_SLOT_SINGLE_CONTEXT))) {
		/*
		 * Reset default data SIM if
		 * a) another SIM is temporarily selected for MMS; or
		 * b) this slot can't have more than one context active.
		 */
		slot = NULL;
	}

	/* Are we actually switching data SIMs? */
	old_data_slot = p->mms_slot ? p->mms_slot : p->data_slot;
	new_data_slot = mms_slot ? mms_slot : slot;

	if (p->data_slot != slot) {
		mask |= SAILFISH_MANAGER_SIGNAL_DATA_PATH;
		p->data_slot = slot;
		if (slot) {
			const char *path = slot->pub.path;
			DBG("Default data SIM at %s", path);
			p->pub.default_data_path = path;
		} else {
			DBG("No default data SIM");
			p->pub.default_data_path = NULL;
		}
	}

	if (p->mms_slot != mms_slot) {
		mask |= SAILFISH_MANAGER_SIGNAL_MMS_PATH;
		p->mms_slot = mms_slot;
		if (mms_slot) {
			const char *path = mms_slot->pub.path;
			DBG("MMS data SIM at %s", path);
			p->pub.mms_path = path;
		} else {
			DBG("No MMS data SIM");
			p->pub.mms_path = NULL;
		}
	}

	if (old_data_slot != new_data_slot) {
		/* Yes we are switching data SIMs */
		if (old_data_slot) {
			sailfish_slot_set_data_role(old_data_slot,
						SAILFISH_DATA_ROLE_NONE);
		}
		if (new_data_slot) {
			sailfish_slot_set_data_role(new_data_slot,
					(new_data_slot == p->data_slot) ?
					SAILFISH_DATA_ROLE_INTERNET :
					SAILFISH_DATA_ROLE_MMS);
		}
	}
	
	return mask;
}

static gboolean sailfish_manager_update_ready_driver_proc
			(struct sailfish_slot_driver_reg *r, void *unused)
{
	struct sailfish_slot_manager *m = r->manager;

	if (!m || m->started) {
		/* This one is either missing or ready */
		return SF_LOOP_CONTINUE;
	} else {
		/* This one is not */
		return SF_LOOP_DONE;
	}
}

static gboolean sailfish_manager_update_ready_slot_proc
			(struct sailfish_slot_priv *s, void *unused)
{
	if (s->imei && s->sim_state != SAILFISH_SIM_STATE_UNKNOWN) {
		/* This one is ready */
		return SF_LOOP_CONTINUE;
	} else {
		/* This one is not */
		return SF_LOOP_DONE;
	}
}

static gboolean sailfish_manager_update_ready(struct sailfish_manager_priv *p)
{
	/*
	 * sailfish_manager_foreach_driver and sailfish_manager_foreach_slot
	 * return FALSE if either all callbacks returned SF_LOOP_CONTINUE or
	 * there are no drivers/slots. In either case we are ready. */
	const gboolean ready =
		!sailfish_manager_foreach_driver
			(p,sailfish_manager_update_ready_driver_proc, NULL) &&
		!sailfish_manager_foreach_slot
			(p, sailfish_manager_update_ready_slot_proc, NULL);
	
	if (p->pub.ready != ready) {
		p->pub.ready = ready;
		sailfish_manager_update_dbus_block(p);
		DBG("%sready", ready ? "" : "not ");
		sailfish_manager_dbus_signal(p->dbus,
					SAILFISH_MANAGER_SIGNAL_READY);
		return TRUE;
	} else {
		return FALSE;
	}
}

void sailfish_manager_imei_obtained(struct sailfish_slot *s, const char *imei)
{
	if (s) {
		struct sailfish_slot_priv *slot = sailfish_slot_priv_cast(s);

		/* We assume that IMEI never changes */
		GASSERT(imei);
		GASSERT(!slot->imei || !g_strcmp0(slot->imei, imei));
		g_free(slot->imei); /* Just in case */
		slot->pub.imei = slot->imei = g_strdup(imei);
		sailfish_manager_update_ready(slot->manager->plugin);
	}
}

void sailfish_manager_imeisv_obtained(struct sailfish_slot *s,
							const char *imeisv)
{
	if (s) {
		struct sailfish_slot_priv *slot = sailfish_slot_priv_cast(s);

		/* We assume that IMEISV never changes */
		GASSERT(imeisv);
		GASSERT(!slot->imeisv || !g_strcmp0(slot->imeisv, imeisv));
		g_free(slot->imeisv); /* Just in case */
		slot->pub.imeisv = slot->imeisv = g_strdup(imeisv);
		sailfish_manager_update_ready(slot->manager->plugin);
	}
}

void sailfish_manager_set_sim_state(struct sailfish_slot *s,
					enum sailfish_sim_state state)
{
	if (s) {
		struct sailfish_slot_priv *slot = sailfish_slot_priv_cast(s);
		struct sailfish_manager_priv *p = slot->manager->plugin;
		const gboolean present = (state == SAILFISH_SIM_STATE_PRESENT);

		if (slot->pub.sim_present != present) {
			slot->pub.sim_present = present;
			sailfish_manager_dbus_signal_sim(p->dbus,
						slot->index, present);
			sailfish_manager_update_modem_paths_full(p);
		}

		if (slot->sim_state != state) {
			slot->sim_state = state;
			sailfish_manager_update_ready(p);
		}
	}
}

static gboolean sailfish_manager_update_enabled_slot
				(struct sailfish_slot_priv *s, void *unused)
{
	if (s->pub.enabled && s->enabled_changed) {
		const struct sailfish_slot_driver *d = s->manager->driver;

		DBG("%s enabled", sailfish_slot_debug_prefix(s));
		s->enabled_changed = TRUE;
		if (d->slot_enabled_changed) {
			d->slot_enabled_changed(s->impl);
		}
	}
	return SF_LOOP_CONTINUE;
}

static gboolean sailfish_manager_update_disabled_slot
				(struct sailfish_slot_priv *s, void *unused)
{
	if (!s->pub.enabled && s->enabled_changed) {
		struct sailfish_slot_manager *m = s->manager;
		const struct sailfish_slot_driver *d = m->driver;

		DBG("%s disabled", sailfish_slot_debug_prefix(s));
		s->enabled_changed = FALSE;
		if (d->slot_enabled_changed) {
			d->slot_enabled_changed(s->impl);
		}
		sailfish_manager_update_modem_paths_full(m->plugin);
	}
	return SF_LOOP_CONTINUE;
}

static void sailfish_manager_update_slots(struct sailfish_manager_priv *p)
{
	sailfish_manager_foreach_slot(p, sailfish_manager_update_disabled_slot,
									NULL);
	sailfish_manager_foreach_slot(p, sailfish_manager_update_enabled_slot,
									NULL);
	sailfish_manager_update_modem_paths_full(p);
}

static gboolean sailfish_manager_enabled_slots_proc
			(struct sailfish_slot_priv *slot, void *user_data)
{
	if (slot->pub.enabled) {
		char ***list = user_data;
		*list = gutil_strv_add(*list, slot->pub.path);
	}

	return SF_LOOP_CONTINUE;
}

struct sailfish_manager_set_enabled_slots_data {
	gchar * const * enabled;
	gboolean all_enabled;
	gboolean changed;
};

static gboolean sailfish_manager_set_enabled_slots_proc
			(struct sailfish_slot_priv *slot, void *user_data)
{
	struct sailfish_manager_set_enabled_slots_data *data = user_data;
	struct sailfish_slot *s = &slot->pub;
	const gboolean was_enabled = s->enabled;

	s->enabled = gutil_strv_contains(data->enabled, s->path);
	if ((was_enabled && !s->enabled) || (!was_enabled && s->enabled)) {
		slot->enabled_changed = TRUE;
		data->changed = TRUE;
	}

	if (!s->enabled) {
		data->all_enabled = FALSE;
	}

	return SF_LOOP_CONTINUE;
}

static void sailfish_manager_set_enabled_slots(struct sailfish_manager *m,
							gchar **slots)
{
	struct sailfish_manager_priv *p = sailfish_manager_priv_cast(m);
	struct sailfish_manager_set_enabled_slots_data data;

	data.enabled = slots;
	data.changed = FALSE;
	data.all_enabled = TRUE;
	sailfish_manager_foreach_slot(p,
			sailfish_manager_set_enabled_slots_proc, &data);
	if (data.changed) {
		char **new_slots = NULL;

		sailfish_manager_foreach_slot(p,
			sailfish_manager_enabled_slots_proc, &new_slots);

		/* Save the new config value. If it exactly matches the list
		 * of available modems, delete the setting because that's the
		 * default behavior. */
		if (data.all_enabled) {
			sailfish_manager_set_config_string(p,
					SF_STORE_ENABLED_SLOTS, NULL);
		} else {
			const char *value;
			char *tmp;

			if (new_slots) {
				tmp = g_strjoinv(SF_STORE_SLOTS_SEP, new_slots);
				value = tmp;
			} else {
				tmp = NULL;
				value = "";
			}

			sailfish_manager_set_config_string(p,
					SF_STORE_ENABLED_SLOTS, value);
			g_free(tmp);
		}
		g_strfreev(new_slots);
		sailfish_manager_dbus_signal(p->dbus,
				SAILFISH_MANAGER_SIGNAL_ENABLED_SLOTS);

		/* Add and remove modems */
		sailfish_manager_update_slots(p);
	}
}

static void sailfish_manager_set_default_voice_imsi(struct sailfish_manager *m,
							const char *imsi)
{
	struct sailfish_manager_priv *p = sailfish_manager_priv_cast(m);

	if (g_strcmp0(p->default_voice_imsi, imsi)) {
		DBG("Default voice sim set to %s", imsi ? imsi : "(auto)");
		g_free(p->default_voice_imsi);
		m->default_voice_imsi =
		p->default_voice_imsi = g_strdup(imsi);
		sailfish_manager_set_config_string(p,
				SF_STORE_DEFAULT_VOICE_SIM, imsi);
		sailfish_manager_dbus_signal(p->dbus,
				SAILFISH_MANAGER_SIGNAL_VOICE_IMSI |
				sailfish_manager_update_modem_paths(p));
	}
}

static void sailfish_manager_set_default_data_imsi(struct sailfish_manager *m,
							const char *imsi)
{
	struct sailfish_manager_priv *p = sailfish_manager_priv_cast(m);

	if (g_strcmp0(p->default_data_imsi, imsi)) {
		DBG("Default data sim set to %s", imsi ? imsi : "(auto)");
		g_free(p->default_data_imsi);
		m->default_data_imsi =
		p->default_data_imsi = g_strdup(imsi);
		sailfish_manager_set_config_string(p,
				SF_STORE_DEFAULT_DATA_SIM, imsi);
		sailfish_manager_dbus_signal(p->dbus,
				SAILFISH_MANAGER_SIGNAL_DATA_IMSI |
				sailfish_manager_update_modem_paths(p));
	}
}

static gboolean sailfish_manager_set_mms_imsi(struct sailfish_manager *m,
							const char *imsi)
{
	struct sailfish_manager_priv *p = sailfish_manager_priv_cast(m);

	if (imsi && imsi[0]) {
		if (g_strcmp0(p->mms_imsi, imsi)) {
			if (sailfish_manager_find_slot_imsi(p, imsi)) {
				DBG("MMS sim %s", imsi);
				g_free(p->mms_imsi);
				m->mms_imsi = p->mms_imsi = g_strdup(imsi);
				sailfish_manager_dbus_signal(p->dbus,
					SAILFISH_MANAGER_SIGNAL_MMS_IMSI |
					sailfish_manager_update_modem_paths(p));
			} else {
				DBG("IMSI not found: %s", imsi);
				return FALSE;
			}
		}
	} else {
		if (p->mms_imsi) {
			DBG("No MMS sim");
			g_free(p->mms_imsi);
			m->mms_imsi = p->mms_imsi = NULL;
			sailfish_manager_dbus_signal(p->dbus,
				SAILFISH_MANAGER_SIGNAL_MMS_IMSI |
				sailfish_manager_update_modem_paths(p));
		}
	}

	return TRUE;
}

static GHashTable *sailfish_manager_inc_error_count(GHashTable *errors,
				const char *group, const char *key)
{
	GKeyFile *storage = storage_open(NULL, SF_ERROR_STORAGE);

	/* Update life-time statistics */
	if (storage) {
		g_key_file_set_integer(storage, group, key,
			g_key_file_get_integer(storage, group, key, NULL) + 1);
		storage_close(NULL, SF_ERROR_STORAGE, storage, TRUE);
	}

	/* Update run-time error counts. The key is the error id which
	 * is always a static string */
	if (!errors) {
		errors = g_hash_table_new_full(g_str_hash, g_str_equal,
								g_free, NULL);
	}
	g_hash_table_insert(errors, g_strdup(key), GINT_TO_POINTER(
		GPOINTER_TO_INT(g_hash_table_lookup(errors, key)) + 1));
	return errors;
}

void sailfish_manager_error(struct sailfish_slot_manager *m, const char *key,
							const char *message)
{
	if (m) {
		struct sailfish_manager_priv *p = m->plugin;

		p->errors = sailfish_manager_inc_error_count(p->errors,
						SF_ERROR_COMMON_SECTION, key);
		sailfish_manager_dbus_signal_error(p->dbus, key, message);
	}
}

void sailfish_manager_slot_error(struct sailfish_slot *s, const char *key,
							const char *msg)
{
	if (s) {
		struct sailfish_slot_priv *priv = sailfish_slot_priv_cast(s);
		/* slot->path always starts with a slash, skip it */
		const char *section = s->path + 1;

		priv->errors = sailfish_manager_inc_error_count(priv->errors,
								section, key);
		sailfish_manager_dbus_signal_modem_error
			(priv->manager->plugin->dbus, priv->index, key, msg);
	}
}

static GHashTable *sailfish_manager_get_errors(struct sailfish_manager *m)
{
	return sailfish_manager_priv_cast(m)->errors;
}

static GHashTable *sailfish_manager_get_slot_errors
					(const struct sailfish_slot *s)
{
	return sailfish_slot_priv_cast_const(s)->errors;
}

static void sailfish_slot_manager_has_started(struct sailfish_slot_manager *m)
{
	if (!m->started) {
		DBG("%s", m->driver->name);
		m->started = TRUE;
		if (!sailfish_manager_update_ready(m->plugin)) {
			sailfish_manager_update_dbus_block(m->plugin);
		}
	}
}

void sailfish_slot_manager_started(struct sailfish_slot_manager *m)
{
	DBG("%s", m->driver->name);
	m->start_id = 0;
	sailfish_slot_manager_has_started(m);
}

static void sailfish_slot_manager_start(struct sailfish_slot_manager *m)
{
	const struct sailfish_slot_driver *d = m->driver;

	if (d->manager_start) {
		m->start_id = d->manager_start(m->impl);
		if (!m->start_id) {
			sailfish_slot_manager_has_started(m);
		}
	}
}

static struct sailfish_slot_manager *sailfish_slot_manager_new
				(struct sailfish_slot_driver_reg *r)
{
	const struct sailfish_slot_driver *d = r->driver;

	if (d->manager_create) {
		struct sailfish_slot_manager *m =
			g_slice_new0(struct sailfish_slot_manager);

		m->driver = d;
		m->plugin = r->plugin;
		m->impl = d->manager_create(m);
		if (m->impl) {
			return m;
		}
		g_slice_free(struct sailfish_slot_manager, m);
	}
	return NULL;
}

static void sailfish_slot_manager_free(struct sailfish_slot_manager *m)
{
	/* Ignore nested sailfish_slot_manager_free calls */
	if (m && m->impl) {
		const struct sailfish_slot_driver *driver = m->driver;

		if (m->start_id && driver->manager_cancel_start) {
			driver->manager_cancel_start(m->impl, m->start_id);
		}
		while (m->slots) {
			struct sailfish_slot_priv *s = m->slots;

			m->slots = s->next;
			s->next = NULL;
			sailfish_slot_free(s);
		}
		if (driver->manager_free) {
			struct sailfish_slot_manager_impl *impl = m->impl;

			m->impl = NULL;
			driver->manager_free(impl);
		}
		g_slice_free(struct sailfish_slot_manager, m);
	}
}

static int sailfish_slot_driver_compare(const struct sailfish_slot_driver *a,
					const struct sailfish_slot_driver *b)
{
	if (a->priority != b->priority) {
		return a->priority - b->priority;
	} else {
		return -g_strcmp0(a->name, b->name);
	}
}

static gboolean sailfish_slot_driver_init(gpointer user_data)
{
	struct sailfish_slot_driver_reg *r = user_data;

	r->init_id = 0;
	r->manager = sailfish_slot_manager_new(r);
	if (r->manager) {
		sailfish_slot_manager_start(r->manager);
	}

	return G_SOURCE_REMOVE;
}

static struct sailfish_slot_driver_reg *sailfish_manager_priv_reg_new
				(struct sailfish_manager_priv *p,
				const struct sailfish_slot_driver *d)
{
	struct sailfish_slot_driver_reg *r = NULL;

	if (p) {
		r = g_slice_new0(struct sailfish_slot_driver_reg);
		r->plugin = p;
		r->driver = d;
		r->init_id = g_idle_add(sailfish_slot_driver_init, r);
		if (!p->drivers || sailfish_slot_driver_compare
					(p->drivers->driver, d) < 0) {
			r->next = p->drivers;
			p->drivers = r;
		} else {
			struct sailfish_slot_driver_reg *prev = p->drivers;

			/* Keep the list sorted */
			while (prev->next && sailfish_slot_driver_compare
					(prev->next->driver, d) >= 0) {
				prev = prev->next;
			}

			r->next = prev->next;
			prev->next = r;
		}
	}

	return r;
}

static void sailfish_slot_driver_free(struct sailfish_slot_driver_reg *r)
{
	if (r->init_id) {
		g_source_remove(r->init_id);
	}
	if (r->manager) {
		sailfish_slot_manager_free(r->manager);
		r->manager = NULL;
	}
	r->next = NULL;
	g_slice_free(struct sailfish_slot_driver_reg, r);
}

static void sailfish_manager_priv_unreg(struct sailfish_manager_priv *p,
					struct sailfish_slot_driver_reg *r)
{
	if (r == p->drivers) {
		p->drivers = r->next;
		sailfish_slot_driver_free(r);
	} else if (p->drivers) {
		struct sailfish_slot_driver_reg *prev = p->drivers;

		while (prev && prev->next != r) {
			prev = prev->next;
		}

		if (prev) {
			prev->next = r->next;
			sailfish_slot_driver_free(r);
		}
	}
}

static gboolean sailfish_manager_priv_init(gpointer user_data)
{
	struct sailfish_manager_priv *p = user_data;

	p->init_countdown--;
	if (!p->init_countdown) {
		p->init_id = 0;
		DBG("done with registrations");
		if (!sailfish_manager_update_ready(p)) {
			sailfish_manager_update_dbus_block(p);
		}
		return G_SOURCE_REMOVE;
	} else {
		/* Keep on waiting */
		return G_SOURCE_CONTINUE;
	}
}

static struct sailfish_manager_priv *sailfish_manager_priv_new()
{
	static const struct sailfish_manager_dbus_cb dbus_cb = {
		.get_errors = sailfish_manager_get_errors,
		.get_slot_errors = sailfish_manager_get_slot_errors,
		.set_enabled_slots = sailfish_manager_set_enabled_slots,
		.set_mms_imsi = sailfish_manager_set_mms_imsi,
		.set_default_voice_imsi =
				sailfish_manager_set_default_voice_imsi,
		.set_default_data_imsi =
				sailfish_manager_set_default_data_imsi
	};

	struct sailfish_manager_priv *p =
		g_slice_new0(struct sailfish_manager_priv);

	/* Load settings */
	p->storage = storage_open(NULL, SF_STORE);
	p->pub.default_voice_imsi = p->default_voice_imsi =
		g_key_file_get_string(p->storage, SF_STORE_GROUP,
					SF_STORE_DEFAULT_VOICE_SIM, NULL);
	p->pub.default_data_imsi = p->default_data_imsi =
		g_key_file_get_string(p->storage, SF_STORE_GROUP,
					SF_STORE_DEFAULT_DATA_SIM, NULL);

	DBG("Default voice sim is %s",  p->default_voice_imsi ?
				p->default_voice_imsi : "(auto)");
	DBG("Default data sim is %s",  p->default_data_imsi ?
				p->default_data_imsi : "(auto)");

	/* Delay the initialization until after all drivers get registered */
	p->init_countdown = SF_INIT_IDLE_COUNT;
	p->init_id = g_idle_add(sailfish_manager_priv_init, p);

	/* And block all requests until that happens */
	p->dbus = sailfish_manager_dbus_new(&p->pub, &dbus_cb);
	sailfish_manager_dbus_set_block(p->dbus,
					SAILFISH_MANAGER_DBUS_BLOCK_ALL);
	return p;
}

static void sailfish_manager_priv_free(struct sailfish_manager_priv *p)
{
	if (p) {
		while (p->drivers) {
			sailfish_manager_priv_unreg(p, p->drivers);
		}
		if (p->init_id) {
			g_source_remove(p->init_id);
		}
		if (p->errors) {
			g_hash_table_destroy(p->errors);
		}
		sailfish_manager_dbus_free(p->dbus);
		g_key_file_free(p->storage);
		g_free(p->default_voice_imsi);
		g_free(p->default_data_imsi);
		g_free(p->mms_imsi);
		g_free(p->slots);
		g_slice_free(struct sailfish_manager_priv, p);
	}
}

void sailfish_manager_foreach_slot_manager
		(struct sailfish_slot_driver_reg *r,
			sailfish_slot_manager_impl_cb_t cb, void *user_data)
{
	if (r && r->manager && cb) {
		/* Yes, it's just one to one mapping but let's keep the API
		 * generic and allow many slot_manager instances. */
		cb(r->manager->impl, user_data);
	}
}

/* Global part (that requires access to sfos_manager_plugin variable) */

static struct sailfish_manager_priv *sfos_manager_plugin;

struct sailfish_slot_driver_reg *sailfish_slot_driver_register
				(const struct sailfish_slot_driver *d)
{
	if (d) {
		DBG("%s", d->name);

		/* This function can be invoked before sailfish_manager_init */
		if (!sfos_manager_plugin) {
			sfos_manager_plugin = sailfish_manager_priv_new();
		}

		/* Only allow registrations at startup */
		if (sfos_manager_plugin->init_countdown) {
			return sailfish_manager_priv_reg_new
						(sfos_manager_plugin, d);
		} else {
			ofono_error("Refusing to register driver %s", d->name);
		}
	}
	return NULL;
}

void sailfish_slot_driver_unregister(struct sailfish_slot_driver_reg *r)
{
	if (r) {
		DBG("%s", r->driver->name);
		sailfish_manager_priv_unreg(sfos_manager_plugin, r);
	}
}

static int sailfish_manager_init(void)
{
	DBG("");
	if (!sfos_manager_plugin) {
		sfos_manager_plugin = sailfish_manager_priv_new();
	}
	return 0;
}

static void sailfish_manager_exit(void)
{
	DBG("");
	if (sfos_manager_plugin) {
		sailfish_manager_priv_free(sfos_manager_plugin);
		sfos_manager_plugin = NULL;
	}
}

OFONO_PLUGIN_DEFINE(sailfish_manager, "Sailfish OS modem manager plugin",
	VERSION, OFONO_PLUGIN_PRIORITY_DEFAULT,
	sailfish_manager_init, sailfish_manager_exit)

/*
 * Local Variables:
 * mode: C
 * c-basic-offset: 8
 * indent-tabs-mode: t
 * End:
 */
