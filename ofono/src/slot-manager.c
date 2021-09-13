/*
 *  oFono - Open Source Telephony
 *
 *  Copyright (C) 2017-2021 Jolla Ltd.
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

#include "ofono.h"
#include "sim-info.h"
#include "storage.h"
#include "slot-manager-dbus.h"
#include "cell-info-dbus.h"

#include <gutil_macros.h>
#include <gutil_misc.h>
#include <gutil_strv.h>
#include <gutil_log.h>

#include <glib-object.h>

enum slot_sim_auto_select {
	SLOT_SIM_AUTO_SELECT_OFF,
	SLOT_SIM_AUTO_SELECT_ON,
	SLOT_SIM_AUTO_SELECT_ONCE
};

enum watch_events {
	WATCH_EVENT_MODEM,
	WATCH_EVENT_ONLINE,
	WATCH_EVENT_IMSI,
	WATCH_EVENT_COUNT
};

#define SM_LOOP_CONTINUE (FALSE)
#define SM_LOOP_DONE (TRUE)

typedef struct ofono_slot_manager_object OfonoSlotManagerObject;
typedef struct ofono_slot_object OfonoSlotObject;
typedef struct ofono_slot_driver_reg OfonoSlotDriverReg;

/* This code assumes that public part immediately follows OfonoSlotBase */
typedef struct ofono_slot_base {
	GObject object;
	guint32 queued_signals;
} OfonoSlotBase;

/*
 * OfonoSlotManagerObject internally keeps a reference to each OfonoSlotObject
 * created by ofono_slot_add() function. In addition to that, ofono_slot_add()
 * returns another reference to the caller, which the caller must eventually
 * release with ofono_slot_unref().
 */
struct ofono_slot_object {
	OfonoSlotBase base;
	struct ofono_slot pub;
	OfonoSlotManagerObject *manager;  /* Not a ref, may be NULL */
	struct ofono_watch *watch;
	struct sim_info *siminfo;
	struct sim_info_dbus *siminfo_dbus;
	struct ofono_cell_info *cellinfo;
	struct cell_info_dbus *cellinfo_dbus;
	enum ofono_slot_flags flags;
	gulong watch_event_id[WATCH_EVENT_COUNT];
	char *imei;
	char *imeisv;
	GHashTable *errors;
	int index;
};

struct ofono_slot_manager_object {
	OfonoSlotBase base;
	struct ofono_slot_manager pub;
	struct slot_manager_dbus *dbus;
	GSList *drivers; /* OfonoSlotDriverReg* */
	GSList *slots; /* OfonoSlotObject* */
	ofono_slot_ptr *pslots;
	OfonoSlotObject *voice_slot;
	OfonoSlotObject *data_slot;
	OfonoSlotObject *mms_slot;
	enum slot_sim_auto_select auto_data_sim;
	gboolean auto_data_sim_done;
	guint init_countdown;
	guint init_id;
	char *default_voice_imsi;
	char *default_data_imsi;
	char *mms_imsi;
	GKeyFile *storage;
	GHashTable *errors;
	guint start_id;
};

struct ofono_slot_driver_reg {
	OfonoSlotManagerObject *manager;
	const struct ofono_slot_driver *driver;
	struct ofono_slot_driver_data *driver_data;
	guint start_id;
};

/* Path always starts with a slash, skip it */
#define slot_debug_prefix(s) ((s)->pub.path + 1)

/* How long we wait for all drivers to register (number of idle loops) */
#define SM_INIT_IDLE_COUNT (5)

/* Read-only config */
#define SM_CONFIG_FILE              "main.conf"
#define SM_CONFIG_GROUP             "ModemManager"
#define SM_CONFIG_KEY_AUTO_DATA_SIM "AutoSelectDataSim"

/* "ril" is used for historical reasons */
#define SM_STORE                    "ril"
#define SM_STORE_GROUP              "Settings"
#define SM_STORE_ENABLED_SLOTS      "EnabledSlots"
#define SM_STORE_DEFAULT_VOICE_SIM  "DefaultVoiceSim"
#define SM_STORE_DEFAULT_DATA_SIM   "DefaultDataSim"
#define SM_STORE_SLOTS_SEP          ","
#define SM_STORE_AUTO_DATA_SIM_DONE "AutoSelectDataSimDone"

/* The file where error statistics is stored. Again "rilerror" is historical */
#define SM_ERROR_STORAGE            "rilerror" /* File name */
#define SM_ERROR_COMMON_SECTION     "common"   /* Modem independent section */

typedef GObjectClass OfonoSlotBaseClass;
typedef OfonoSlotBaseClass OfonoSlotObjectClass;
typedef OfonoSlotBaseClass OfonoSlotManagerObjectClass;

#define OFONO_TYPE_BASE (ofono_slot_base_get_type())
#define OFONO_TYPE_SLOT (ofono_slot_object_get_type())
#define OFONO_TYPE_SLOT_MANAGER (ofono_slot_manager_object_get_type())
#define OFONO_SLOT_BASE(obj) (G_TYPE_CHECK_INSTANCE_CAST((obj), \
        OFONO_TYPE_BASE, OfonoSlotBase))
#define OFONO_SLOT_OBJECT(obj) (G_TYPE_CHECK_INSTANCE_CAST((obj), \
        OFONO_TYPE_SLOT, OfonoSlotObject))
#define OFONO_SLOT_MANAGER_OBJECT(obj) (G_TYPE_CHECK_INSTANCE_CAST((obj), \
        OFONO_TYPE_SLOT_MANAGER, OfonoSlotManagerObject))
G_DEFINE_TYPE(OfonoSlotBase,ofono_slot_base,G_TYPE_OBJECT)
G_DEFINE_TYPE(OfonoSlotObject,ofono_slot_object,OFONO_TYPE_BASE)
G_DEFINE_TYPE(OfonoSlotManagerObject,ofono_slot_manager_object,OFONO_TYPE_BASE)

typedef void (*slot_base_property_cb)(void *obj, int prop, void* data);

typedef struct ofono_slot_base_closure {
	GCClosure cclosure;
	slot_base_property_cb cb;
	gpointer user_data;
} OfonoSlotBaseClosure;

#define slot_base_closure_new() ((OfonoSlotBaseClosure *) \
    g_closure_new_simple(sizeof(OfonoSlotBaseClosure), NULL))

#define SIGNAL_BIT(property) (1 << ((property) - 1))
#define MAX_PROPERTIES \
	(MAX((int)OFONO_SLOT_PROPERTY_LAST, \
	     (int)OFONO_SLOT_MANAGER_PROPERTY_LAST) + 1)

#define SIGNAL_PROPERTY_CHANGED_NAME    "ofono-slot-base-property-changed"
#define SIGNAL_PROPERTY_DETAIL          "%x"
#define SIGNAL_PROPERTY_DETAIL_MAX_LEN  (8)

enum ofono_slot_base_signal {
    SIGNAL_PROPERTY_CHANGED,
    SIGNAL_COUNT
};

static guint slot_base_signals[SIGNAL_COUNT];
static GQuark slot_base_property_quarks[MAX_PROPERTIES];

static void slot_manager_reindex_slots(OfonoSlotManagerObject *mgr);
static int slot_manager_update_modem_paths(OfonoSlotManagerObject *mgr);
static void slot_manager_emit_all_queued_signals(OfonoSlotManagerObject *mgr);
static void slot_manager_update_ready(OfonoSlotManagerObject *mgr);

static inline OfonoSlotBase *slot_base_cast(gpointer p)
{
	return G_LIKELY(p) ? OFONO_SLOT_BASE((OfonoSlotBase*)p - 1) : NULL;
}

static inline OfonoSlotObject *slot_object_cast(const struct ofono_slot *s)
{
	return G_LIKELY(s) ? OFONO_SLOT_OBJECT(G_CAST(s,
					OfonoSlotObject, pub)) : NULL;
}

static inline OfonoSlotManagerObject *slot_manager_object_cast
	(const struct ofono_slot_manager *m)
{
	return G_LIKELY(m) ? OFONO_SLOT_MANAGER_OBJECT(G_CAST(m,
					OfonoSlotManagerObject, pub)) : NULL;
}

static GQuark slot_base_property_quark(guint p)
{
	/* For ANY property (zero) this function is expected to return zero */
	if (p > 0 && G_LIKELY(p < MAX_PROPERTIES)) {
		const int i = p - 1;

		if (G_UNLIKELY(!slot_base_property_quarks[i])) {
			char s[SIGNAL_PROPERTY_DETAIL_MAX_LEN + 1];

			snprintf(s, sizeof(s), SIGNAL_PROPERTY_DETAIL, p);
			s[sizeof(s) - 1] = 0;
			slot_base_property_quarks[i] = g_quark_from_string(s);
		}
		return slot_base_property_quarks[i];
	}
	return 0;
}

static void slot_base_property_changed(OfonoSlotBase* base,
	int property, OfonoSlotBaseClosure* closure)
{
	closure->cb(((void*)(base + 1)), property, closure->user_data);
}

static void slot_base_emit_property_change(OfonoSlotBase* obj, int p)
{
	obj->queued_signals &= ~SIGNAL_BIT(p);
	g_signal_emit(obj, slot_base_signals[SIGNAL_PROPERTY_CHANGED],
		slot_base_property_quark(p), p);
}

static void slot_base_emit_queued_signals(OfonoSlotBase* obj)
{
	if (obj->queued_signals) {
		int p;

		/* Handlers could drop their references to us */
		g_object_ref(obj);

		/* Emit the signals */
		for (p = 0; obj->queued_signals && p < MAX_PROPERTIES; p++) {
			if (obj->queued_signals & SIGNAL_BIT(p)) {
				slot_base_emit_property_change(obj, p);
			}
		}

		/* And release the temporary reference */
		g_object_unref(obj);
	}
}

static gulong slot_base_add_property_handler(OfonoSlotBase* obj,
	guint property, slot_base_property_cb cb, gpointer user_data)
{
	if (G_LIKELY(cb)) {
		/*
		 * We can't directly connect the provided callback because
		 * it expects the first parameter to point to public part
		 * of the object but glib will call it with OfonoSlotBase
		 * as the first parameter. slot_base_property_changed()
		 * will do the conversion.
		 */
		OfonoSlotBaseClosure* closure = slot_base_closure_new();
		GCClosure* cc = &closure->cclosure;

		cc->closure.data = closure;
		cc->callback = G_CALLBACK(slot_base_property_changed);
		closure->cb = cb;
		closure->user_data = user_data;

		return g_signal_connect_closure_by_id(obj,
			slot_base_signals[SIGNAL_PROPERTY_CHANGED],
			slot_base_property_quark(property), &cc->closure,
			FALSE);
	}
	return 0;
}

static void slot_base_remove_handler(gpointer obj, gulong id)
{
	if (obj && id) {
            g_signal_handler_disconnect(obj, id);
	}
}

static gboolean config_get_enum(GKeyFile *file, const char *group,
					const char *key, int *result,
					const char *name, int value, ...)
{
	char *str = g_key_file_get_string(file, group, key, NULL);

	if (str) {
		/*
		 * Some people are thinking that # is a comment
		 * anywhere on the line, not just at the beginning
		 */
		char *comment = strchr(str, '#');

		if (comment) *comment = 0;
		g_strstrip(str);
		if (strcasecmp(str, name)) {
			va_list args;
			va_start(args, value);
			while ((name = va_arg(args, char*)) != NULL) {
				value = va_arg(args, int);
				if (!strcasecmp(str, name)) {
					break;
				}
			}
			va_end(args);
		}

		if (!name) {
			ofono_error("Invalid %s config value (%s)", key, str);
		}

		g_free(str);

		if (name) {
			if (result) {
				*result = value;
			}
			return TRUE;
		}
	}

	return FALSE;
}

static GHashTable *slot_manager_inc_error_count(GHashTable *errors,
				const char *group, const char *key)
{
	GKeyFile *storage = storage_open(NULL, SM_ERROR_STORAGE);
	int n;

	/* Update life-time statistics */
	if (storage) {
		g_key_file_set_integer(storage, group, key,
			g_key_file_get_integer(storage, group, key, NULL) + 1);
		storage_close(NULL, SM_ERROR_STORAGE, storage, TRUE);
	}

	/* Update run-time error counts. The key is the error id which
	 * is always a static string */
	if (!errors) {
		errors = g_hash_table_new_full(g_str_hash, g_str_equal,
			g_free, NULL);
	}
	n = GPOINTER_TO_INT(g_hash_table_lookup(errors, key));
	g_hash_table_insert(errors, g_strdup(key), GINT_TO_POINTER(n + 1));
	return errors;
}

/*==========================================================================*
 * OfonoSlotObject
 *==========================================================================*/

static inline void slot_queue_property_change(OfonoSlotObject* slot,
	enum ofono_slot_property p)
{
	slot->base.queued_signals |= SIGNAL_BIT(p);
}

static inline void slot_emit_queued_signals(OfonoSlotObject *slot)
{
	slot_base_emit_queued_signals(&slot->base);
}

static void slot_update_data_role(OfonoSlotObject *slot,
					enum ofono_slot_data_role role)
{
	if (slot->pub.data_role != role) {
		slot->pub.data_role = role;
		slot_queue_property_change(slot,
			OFONO_SLOT_PROPERTY_DATA_ROLE);
	}
}

static void slot_update_cell_info_dbus(OfonoSlotObject *slot)
{
	struct ofono_modem *modem = slot->watch->modem;

	if (modem && slot->cellinfo) {
		if (!slot->cellinfo_dbus) {
			slot->cellinfo_dbus = cell_info_dbus_new(modem,
				slot->cellinfo);
		}
	} else {
		if (slot->cellinfo_dbus) {
			cell_info_dbus_free(slot->cellinfo_dbus);
			slot->cellinfo_dbus = NULL;
		}
	}
}

static void slot_manager_slot_modem_changed(struct ofono_watch *w, void *data)
{
	OfonoSlotObject *slot = OFONO_SLOT_OBJECT(data);
	OfonoSlotManagerObject *mgr = slot->manager;

	slot_update_cell_info_dbus(slot);
	slot_manager_update_modem_paths(mgr);
	slot_manager_update_ready(mgr);
	slot_manager_emit_all_queued_signals(mgr);
}

static void slot_manager_slot_imsi_changed(struct ofono_watch *w, void *data)
{
	OfonoSlotObject *slot = OFONO_SLOT_OBJECT(data);
	OfonoSlotManagerObject *mgr = slot->manager;
	OfonoSlotObject *voice_slot = mgr->voice_slot;
	OfonoSlotObject *data_slot = mgr->data_slot;
	int signal_mask;

	/*
	 * We want the first slot to be selected by default.
	 * However, things may become available in pretty much
	 * any order, so reset the slot pointers to NULL and let
	 * slot_manager_update_modem_paths() to pick them again.
	 */
	mgr->voice_slot = NULL;
	mgr->data_slot = NULL;
	mgr->pub.default_voice_path = NULL;
	mgr->pub.default_data_path = NULL;
	signal_mask = slot_manager_update_modem_paths(mgr);
	if (voice_slot != mgr->voice_slot) {
		if (!mgr->voice_slot) {
			DBG("No default voice SIM");
		}
		signal_mask |= SLOT_MANAGER_DBUS_SIGNAL_VOICE_PATH;
	}
	if (data_slot != mgr->data_slot) {
		if (!mgr->data_slot) {
			DBG("No default data SIM");
		}
		signal_mask |= SLOT_MANAGER_DBUS_SIGNAL_DATA_PATH;
	}
	slot_manager_dbus_signal(mgr->dbus, signal_mask);
	slot_manager_emit_all_queued_signals(mgr);
}

static gboolean slot_check_slot_name(OfonoSlotObject *s, void *path)
{
	return strcmp(s->pub.path, path) ? SM_LOOP_CONTINUE : SM_LOOP_DONE;
}

static gint slot_compare_path(gconstpointer p1, gconstpointer  p2)
{
	OfonoSlotObject *s1 = OFONO_SLOT_OBJECT(p1);
	OfonoSlotObject *s2 = OFONO_SLOT_OBJECT(p2);

	return strcmp(s1->pub.path, s2->pub.path);
}

static void slot_object_finalize(GObject* obj)
{
	OfonoSlotObject *s = OFONO_SLOT_OBJECT(obj);

	if (s->errors) {
		g_hash_table_destroy(s->errors);
	}
	sim_info_unref(s->siminfo);
	sim_info_dbus_free(s->siminfo_dbus);
	cell_info_dbus_free(s->cellinfo_dbus);
	ofono_cell_info_unref(s->cellinfo);
	ofono_watch_remove_all_handlers(s->watch, s->watch_event_id);
	ofono_watch_unref(s->watch);
	g_free(s->imei);
	g_free(s->imeisv);
	G_OBJECT_CLASS(ofono_slot_object_parent_class)->finalize(obj);
}

static struct ofono_slot *slot_add_internal(OfonoSlotManagerObject *mgr,
	const char *path, enum ofono_radio_access_mode techs, const char *imei,
	const char *imeisv, enum ofono_slot_sim_presence sim_presence,
	enum ofono_slot_flags flags)
{
	char *enabled_slots;
	/* Extra ref for the caller */
	OfonoSlotObject *s = g_object_ref(g_object_new(OFONO_TYPE_SLOT, NULL));
	struct ofono_slot *pub = &s->pub;
	struct ofono_watch *w = ofono_watch_new(path);

	s->manager = mgr; /* Not a ref */
	s->flags = flags;
	s->watch = w;
	s->siminfo = sim_info_new(path);
	s->siminfo_dbus = sim_info_dbus_new(s->siminfo);
	pub->path = w->path;
	pub->imei = s->imei = g_strdup(imei);
	pub->imeisv = s->imeisv = g_strdup(imeisv);
	pub->sim_presence = sim_presence;
	DBG("%s", slot_debug_prefix(s));

	/* Check if it's enabled */
	enabled_slots = g_key_file_get_string(mgr->storage,
		SM_STORE_GROUP, SM_STORE_ENABLED_SLOTS, NULL);
	if (enabled_slots) {
		char **strv = g_strsplit(enabled_slots, SM_STORE_SLOTS_SEP, 0);

		DBG("Enabled slots: %s", enabled_slots);
		pub->enabled = gutil_strv_contains(strv, path);
		g_strfreev(strv);
		g_free(enabled_slots);
	}

	/* Add it to the list */
	mgr->slots = g_slist_insert_sorted(mgr->slots, s, slot_compare_path);
	slot_manager_reindex_slots(mgr);

	/* Register for events */
	s->watch_event_id[WATCH_EVENT_MODEM] =
		ofono_watch_add_modem_changed_handler(w,
			slot_manager_slot_modem_changed, s);
	s->watch_event_id[WATCH_EVENT_ONLINE] =
		ofono_watch_add_online_changed_handler(w,
			slot_manager_slot_modem_changed, s);
	s->watch_event_id[WATCH_EVENT_IMSI] =
		ofono_watch_add_imsi_changed_handler(w,
			slot_manager_slot_imsi_changed, s);

	/* Clear queued signals */
	mgr->base.queued_signals = 0;
	return pub;
}

/*==========================================================================*
 * OfonoSlotManagerObject
 *==========================================================================*/

/*
 * slot_manager_foreach_driver() and slot_manager_foreach_slot() terminate
 * the loop and return TRUE if the callback returns TRUE. If all callbacks
 * return FALSE, they returns FALSE. It there are no drivers/slots, they
 * return FALSE too.
 */

static gboolean slot_manager_foreach_driver(OfonoSlotManagerObject *mgr,
	gboolean (*fn)(OfonoSlotDriverReg *reg, void *user_data),
	gconstpointer user_data)
{
	GSList *l = mgr->drivers;
	gboolean done = FALSE;

	while (l && !done) {
		GSList *next = l->next;

		/* The callback returns TRUE to terminate the loop */
		done = fn((OfonoSlotDriverReg*)l->data, (void*) user_data);
		l = next;
	}

	return done;
}

static gboolean slot_manager_foreach_slot(OfonoSlotManagerObject *mgr,
	gboolean (*fn)(OfonoSlotObject *slot, void *user_data),
	gconstpointer user_data)
{
	GSList *l = mgr->slots;
	gboolean done = FALSE;

	while (l && !done) {
		GSList *next = l->next;
		OfonoSlotObject *s = OFONO_SLOT_OBJECT(l->data);

		/* The callback returns TRUE to terminate the loop */
		done = fn(s, (void*) user_data);
		l = next;
	}

	return done;
}

static inline void slot_manager_queue_property_change
	(OfonoSlotManagerObject* mgr, enum ofono_slot_manager_property p)
{
	mgr->base.queued_signals |= SIGNAL_BIT(p);
}

static inline void slot_manager_emit_queued_signals(OfonoSlotManagerObject *mgr)
{
	slot_base_emit_queued_signals(&mgr->base);
}

static gboolean slot_manager_emit_all_queued_signals_cb
	(OfonoSlotObject *slot, void *unused)
{
	slot_emit_queued_signals(slot);
	return SM_LOOP_CONTINUE;
}

static void slot_manager_emit_all_queued_signals(OfonoSlotManagerObject *mgr)
{
	slot_manager_emit_queued_signals(mgr);
	slot_manager_foreach_slot(mgr, slot_manager_emit_all_queued_signals_cb,
		NULL);
}

static void slot_manager_reindex_slots(OfonoSlotManagerObject *mgr)
{
	const int n = g_slist_length(mgr->slots);
	ofono_slot_ptr *ptr = g_new0(ofono_slot_ptr, n + 1);
	int i = 0;
	GSList *l;

	g_free(mgr->pslots);
	mgr->pub.slots = mgr->pslots = ptr;

	for (l = mgr->slots; l; l = l->next) {
		OfonoSlotObject *slot = OFONO_SLOT_OBJECT(l->data);

		slot->index = i++;
		*ptr++ = &slot->pub;
	}

	*ptr = NULL;
}

static void slot_manager_update_dbus_block(OfonoSlotManagerObject *mgr)
{
	slot_manager_dbus_set_block(mgr->dbus, mgr->pub.ready ?
		SLOT_MANAGER_DBUS_BLOCK_NONE :
		SLOT_MANAGER_DBUS_BLOCK_ALL);
}

static void slot_manager_set_config_string(OfonoSlotManagerObject *mgr,
	const char *key, const char *value)
{
	if (value) {
		g_key_file_set_string(mgr->storage, SM_STORE_GROUP, key, value);
	} else {
		g_key_file_remove_key(mgr->storage, SM_STORE_GROUP, key, NULL);
	}
	storage_sync(NULL, SM_STORE, mgr->storage);
}

struct slot_manager_imsi_slot_data {
	OfonoSlotObject *slot;
	const char *imsi; /* NULL if we are looking for any slot with IMSI */
};

static gboolean slot_manager_find_slot_imsi_cb(OfonoSlotObject *slot,
	void *user_data)
{
	struct slot_manager_imsi_slot_data *data = user_data;
	const char *slot_imsi = slot->watch->imsi;

	if (slot_imsi && (!data->imsi || !strcmp(slot_imsi, data->imsi))) {
		data->slot = slot;
		return SM_LOOP_DONE;
	} else {
		return SM_LOOP_CONTINUE;
	}
}

static OfonoSlotObject *slot_manager_find_slot_imsi(OfonoSlotManagerObject *mgr,
	const char *imsi)
{
	struct slot_manager_imsi_slot_data data;

	memset(&data, 0, sizeof(data));
	data.imsi = imsi;
	slot_manager_foreach_slot(mgr, slot_manager_find_slot_imsi_cb, &data);
	return data.slot;
}

static gboolean slot_manager_all_sims_are_initialized_cb(OfonoSlotObject *slot,
	void *result)
{
	/* Not initialized if present and enabled but no IMSI yet */
	if (slot->pub.sim_presence == OFONO_SLOT_SIM_PRESENT &&
		slot->pub.enabled && !slot->watch->imsi) {
		*((gboolean*)result) = FALSE;
		return SM_LOOP_DONE;
	} else {
		return SM_LOOP_CONTINUE;
	}
}

static gboolean slot_manager_all_sims_are_initialized
	(OfonoSlotManagerObject *mgr)
{
	gboolean result = TRUE;

	slot_manager_foreach_slot(mgr,
		slot_manager_all_sims_are_initialized_cb, &result);
	return result;
}

/*
 * Returns the event mask to be passed to slot_manager_dbus_signal.
 * The caller has a chance to OR it with other bits. Also updates the
 * queued signals mask but doesn't actually emit any signals.
 */
static int slot_manager_update_modem_paths(OfonoSlotManagerObject *mgr)
{
	int mask = 0;
	OfonoSlotObject *slot = NULL;
	OfonoSlotObject *mms_slot = NULL;
	OfonoSlotObject *old_data_slot = NULL;
	OfonoSlotObject *new_data_slot = NULL;

	/* Voice */
	if (mgr->default_voice_imsi) {
		slot = slot_manager_find_slot_imsi(mgr,
			mgr->default_voice_imsi);
	} else if (mgr->voice_slot) {
		/* Make sure that the slot is enabled and SIM is in */
		slot = slot_manager_find_slot_imsi(mgr,
			mgr->voice_slot->watch->imsi);
	}

	/*
	 * If there's no default voice SIM, we will find any SIM instead.
	 * One should always be able to make and receive a phone call
	 * if there's a working SIM in the phone. However if the
	 * previously selected voice SIM is inserted, we will switch
	 * back to it.
	 *
	 * A similar behavior can be configured for data SIM too.
	 */
	if (!slot) {
		slot = slot_manager_find_slot_imsi(mgr, NULL);
	}

	if (mgr->voice_slot != slot) {
		slot_manager_queue_property_change(mgr,
			OFONO_SLOT_MANAGER_PROPERTY_DEFAULT_VOICE_PATH);
		mask |= SLOT_MANAGER_DBUS_SIGNAL_VOICE_PATH;
		mgr->voice_slot = slot;
		if (slot) {
			const char *path = slot->pub.path;
			DBG("Default voice SIM at %s", path);
			mgr->pub.default_voice_path = path;
		} else {
			DBG("No default voice SIM");
			mgr->pub.default_voice_path = NULL;
		}
	}

	/* Data */
	if (mgr->default_data_imsi) {
		slot = slot_manager_find_slot_imsi(mgr,
			mgr->default_data_imsi);
	} else if (g_slist_length(mgr->slots) < 2) {
		if (mgr->data_slot) {
			/* Make sure that the slot is enabled and SIM is in */
			slot = slot_manager_find_slot_imsi(mgr,
				mgr->data_slot->watch->imsi);
		} else {
			/* Check if anything is available */
			slot = slot_manager_find_slot_imsi(mgr, NULL);
		}
	} else {
		slot = NULL;
	}

	/* Check if we need to auto-select data SIM (always or once) */
	if (!slot && (mgr->auto_data_sim == SLOT_SIM_AUTO_SELECT_ON ||
			(mgr->auto_data_sim == SLOT_SIM_AUTO_SELECT_ONCE &&
				!mgr->auto_data_sim_done))) {
		/*
		 * To actually make a selection we need all present SIMs
		 * to be initialized. Otherwise we may end up endlessly
		 * switching data SIMs back and forth.
		 */
		if (slot_manager_all_sims_are_initialized(mgr)) {
			slot = slot_manager_find_slot_imsi(mgr, NULL);
			if (slot && slot->watch->imsi && slot->watch->online &&
				mgr->auto_data_sim ==
					SLOT_SIM_AUTO_SELECT_ONCE) {
				const char *imsi = slot->watch->imsi;

				/*
				 * Data SIM only needs to be auto-selected
				 * once and it's done. Write that down.
				 */
				DBG("Default data sim set to %s once", imsi);
				mgr->auto_data_sim_done = TRUE;
				g_key_file_set_boolean(mgr->storage,
						SM_STORE_GROUP,
						SM_STORE_AUTO_DATA_SIM_DONE,
						mgr->auto_data_sim_done);

				g_free(mgr->default_data_imsi);
				mgr->pub.default_data_imsi =
				mgr->default_data_imsi = g_strdup(imsi);
				g_key_file_set_string(mgr->storage,
						SM_STORE_GROUP,
						SM_STORE_DEFAULT_DATA_SIM,
						imsi);

				storage_sync(NULL, SM_STORE, mgr->storage);
				slot_manager_queue_property_change(mgr,
				OFONO_SLOT_MANAGER_PROPERTY_DEFAULT_DATA_IMSI);
				mask |= SLOT_MANAGER_DBUS_SIGNAL_DATA_IMSI;
			}
		} else {
			DBG("Skipping auto-selection of data SIM");
		}
	}

	if (slot && !slot->watch->online) {
		slot = NULL;
	}

	if (mgr->mms_imsi) {
		mms_slot = slot_manager_find_slot_imsi(mgr, mgr->mms_imsi);
	}

	if (mms_slot && (mms_slot != slot ||
			(slot->flags & OFONO_SLOT_FLAG_SINGLE_CONTEXT))) {
		/*
		 * Reset default data SIM if
		 * a) another SIM is temporarily selected for MMS; or
		 * b) this slot can't have more than one context active.
		 */
		slot = NULL;
	}

	/* Are we actually switching data SIMs? */
	old_data_slot = mgr->mms_slot ? mgr->mms_slot : mgr->data_slot;
	new_data_slot = mms_slot ? mms_slot : slot;

	if (mgr->data_slot != slot) {
		slot_manager_queue_property_change(mgr,
			OFONO_SLOT_MANAGER_PROPERTY_DEFAULT_DATA_PATH);
		mask |= SLOT_MANAGER_DBUS_SIGNAL_DATA_PATH;
		mgr->data_slot = slot;
		if (slot) {
			const char *path = slot->pub.path;
			DBG("Default data SIM at %s", path);
			mgr->pub.default_data_path = path;
		} else {
			DBG("No default data SIM");
			mgr->pub.default_data_path = NULL;
		}
	}

	if (mgr->mms_slot != mms_slot) {
		slot_manager_queue_property_change(mgr,
			OFONO_SLOT_MANAGER_PROPERTY_MMS_PATH);
		mask |= SLOT_MANAGER_DBUS_SIGNAL_MMS_PATH;
		mgr->mms_slot = mms_slot;
		if (mms_slot) {
			const char *path = mms_slot->pub.path;
			DBG("MMS data SIM at %s", path);
			mgr->pub.mms_path = path;
		} else {
			DBG("No MMS data SIM");
			mgr->pub.mms_path = NULL;
		}
	}

	if (old_data_slot != new_data_slot) {
		/* Yes we are switching data SIMs */
		if (old_data_slot) {
			slot_update_data_role(old_data_slot,
				OFONO_SLOT_DATA_NONE);
		}
		if (new_data_slot) {
			slot_update_data_role(new_data_slot,
				(new_data_slot == mgr->data_slot) ?
					OFONO_SLOT_DATA_INTERNET :
					OFONO_SLOT_DATA_MMS);
		}
	}
	
	return mask;
}

static gboolean slot_manager_update_ready_driver_cb(OfonoSlotDriverReg *reg,
	void *unused)
{
	return reg->start_id ? SM_LOOP_DONE : SM_LOOP_CONTINUE;
}

static gboolean slot_manager_update_ready_slot_cb(OfonoSlotObject *slot,
	void *unused)
{
	return (slot->pub.sim_presence == OFONO_SLOT_SIM_UNKNOWN) ?
		SM_LOOP_DONE : SM_LOOP_CONTINUE;
}

static void slot_manager_update_ready(OfonoSlotManagerObject *mgr)
{
	struct ofono_slot_manager *m = &mgr->pub;

	/* ready is a one-way flag */
	if (!m->ready &&
		/*
		 * slot_manager_foreach_xxx return FALSE if either all
		 * callbacks returned SM_LOOP_CONTINUE or there are no
		 * drivers/slots. In either case we are ready.
		 */
		!slot_manager_foreach_driver(mgr,
			slot_manager_update_ready_driver_cb, NULL) &&
		!slot_manager_foreach_slot(mgr,
			slot_manager_update_ready_slot_cb, NULL)) {
		m->ready = TRUE;
		DBG("ready");
		slot_manager_update_dbus_block(mgr);
		slot_manager_queue_property_change(mgr,
			OFONO_SLOT_MANAGER_PROPERTY_READY);
		slot_manager_dbus_signal(mgr->dbus,
			SLOT_MANAGER_DBUS_SIGNAL_READY);
	}
}

static gboolean slot_manager_start_driver_cb(OfonoSlotDriverReg *reg,
	void *unused)
{
	const struct ofono_slot_driver *d = reg->driver;
	OfonoSlotManagerObject *mgr = reg->manager;

	if (d->init) {
		reg->driver_data = d->init(&mgr->pub);
	}
	if (d->start) {
		reg->start_id = d->start(reg->driver_data);
	}
	return SM_LOOP_CONTINUE;
}

static gboolean slot_manager_init_countdown_cb(gpointer user_data)
{
	OfonoSlotManagerObject *mgr = OFONO_SLOT_MANAGER_OBJECT(user_data);

	mgr->init_countdown--;
	if (!mgr->init_countdown) {
		mgr->init_id = 0;
		DBG("done with registrations");
		slot_manager_foreach_driver(mgr,
			slot_manager_start_driver_cb, NULL);
		slot_manager_update_ready(mgr);
		slot_manager_emit_queued_signals(mgr);
		return G_SOURCE_REMOVE;
	} else {
		/* Keep on waiting */
		return G_SOURCE_CONTINUE;
	}
}

static void slot_driver_reg_free(OfonoSlotDriverReg *r)
{
	const struct ofono_slot_driver *d = r->driver;

	if (r->start_id && d->cancel) {
		d->cancel(r->driver_data, r->start_id);
	}
	if (d->cleanup) {
		d->cleanup(r->driver_data);
	}
	g_free(r);
}

static OfonoSlotDriverReg *slot_manager_register_driver
	(OfonoSlotManagerObject *mgr, const struct ofono_slot_driver *d)
{
	/* Only allow registrations at startup */
	if (mgr->init_countdown) {
		OfonoSlotDriverReg *reg = g_new0(OfonoSlotDriverReg, 1);

		reg->manager = mgr;
		reg->driver = d;
		mgr->drivers = g_slist_append(mgr->drivers, reg);
		return reg;
	} else {
		ofono_error("Refusing to register slot driver %s", d->name);
		return NULL;
	}
}

static gboolean ofono_slot_manager_detach(OfonoSlotObject *s, void *p)
{
	s->manager = NULL;
	return SM_LOOP_CONTINUE;
}

static void ofono_slot_manager_object_finalize(GObject* obj)
{
	OfonoSlotManagerObject *mgr = OFONO_SLOT_MANAGER_OBJECT(obj);

	/* Drivers are unregistered by __ofono_slot_manager_cleanup */ 
	GASSERT(!mgr->drivers);
	g_slist_free_full(mgr->slots, g_object_unref);
	g_free(mgr->pslots);
	slot_manager_dbus_free(mgr->dbus);
	if (mgr->init_id) {
		g_source_remove(mgr->init_id);
	}
	if (mgr->errors) {
		g_hash_table_destroy(mgr->errors);
	}
	g_key_file_free(mgr->storage);
	g_free(mgr->default_voice_imsi);
	g_free(mgr->default_data_imsi);
	g_free(mgr->mms_imsi);
	G_OBJECT_CLASS(ofono_slot_manager_object_parent_class)->finalize(obj);
}

static OfonoSlotManagerObject *ofono_slot_manager_new()
{
	return g_object_new(OFONO_TYPE_SLOT_MANAGER, NULL);
}

/*==========================================================================*
 * slot_manager_dbus callbacks
 *==========================================================================*/

struct slot_manager_set_enabled_slots_data {
	gchar * const * enabled;
	gboolean all_enabled;
	gboolean changed;
};

static gboolean slot_manager_set_enabled_slots_cb(OfonoSlotObject *slot,
	void *user_data)
{
	struct slot_manager_set_enabled_slots_data *data = user_data;
	struct ofono_slot *s = &slot->pub;
	const gboolean was_enabled = s->enabled;

	s->enabled = gutil_strv_contains(data->enabled, s->path);
	if ((was_enabled && !s->enabled) || (!was_enabled && s->enabled)) {
		DBG("%s %s", slot_debug_prefix(slot), s->enabled ?
			"enabled" : "disabled");
		slot_queue_property_change(slot, OFONO_SLOT_PROPERTY_ENABLED);
		data->changed = TRUE;
	}

	if (!s->enabled) {
		data->all_enabled = FALSE;
	}

	return SM_LOOP_CONTINUE;
}

static gboolean slot_manager_enabled_slots_cb(OfonoSlotObject *slot,
	void *user_data)
{
	struct ofono_slot *s = &slot->pub;

	if (s->enabled) {
		char ***list = user_data;

		*list = gutil_strv_add(*list, s->path);
	}

	return SM_LOOP_CONTINUE;
}

static GHashTable *slot_manager_dbus_get_errors
	(const struct ofono_slot_manager *m)
{
	return slot_manager_object_cast(m)->errors;
}

static GHashTable *slot_manager_dbus_get_slot_errors
	(const struct ofono_slot *s)
{
	return slot_object_cast(s)->errors;
}

static void slot_manager_dbus_set_enabled_slots(struct ofono_slot_manager *m,
	char **slots)
{
	OfonoSlotManagerObject *mgr = slot_manager_object_cast(m);
	struct slot_manager_set_enabled_slots_data cbd;

	cbd.enabled = slots;
	cbd.changed = FALSE;
	cbd.all_enabled = TRUE;
	slot_manager_foreach_slot(mgr, slot_manager_set_enabled_slots_cb, &cbd);
	if (cbd.changed) {
		char **new_slots = NULL;

		slot_manager_foreach_slot(mgr,slot_manager_enabled_slots_cb,
			&new_slots);

		/*
		 * Save the new config value. If it exactly matches the list
		 * of available modems, delete the setting because that's the
		 * default behavior.
		 */
		if (cbd.all_enabled) {
			slot_manager_set_config_string(mgr,
				SM_STORE_ENABLED_SLOTS, NULL);
		} else {
			const char *value;
			char *tmp;

			if (new_slots) {
				tmp = g_strjoinv(SM_STORE_SLOTS_SEP, new_slots);
				value = tmp;
			} else {
				tmp = NULL;
				value = "";
			}

			slot_manager_set_config_string(mgr,
				SM_STORE_ENABLED_SLOTS, value);
			g_free(tmp);
		}
		g_strfreev(new_slots);

		/* Update paths and emigt signals */
		slot_manager_dbus_signal(mgr->dbus,
			SLOT_MANAGER_DBUS_SIGNAL_ENABLED_SLOTS |
			slot_manager_update_modem_paths(mgr));
		slot_manager_emit_all_queued_signals(mgr);
	}
}

static gboolean slot_manager_dbus_set_mms_imsi(struct ofono_slot_manager *m,
	const char *imsi)
{
	OfonoSlotManagerObject *mgr = slot_manager_object_cast(m);

	if (imsi && imsi[0]) {
		if (g_strcmp0(mgr->mms_imsi, imsi)) {
			if (slot_manager_find_slot_imsi(mgr, imsi)) {
				DBG("MMS sim %s", imsi);
				g_free(mgr->mms_imsi);
				m->mms_imsi = mgr->mms_imsi = g_strdup(imsi);
				slot_manager_dbus_signal(mgr->dbus,
					SLOT_MANAGER_DBUS_SIGNAL_MMS_IMSI |
					slot_manager_update_modem_paths(mgr));
				slot_manager_emit_all_queued_signals(mgr);
			} else {
				DBG("IMSI not found: %s", imsi);
				return FALSE;
			}
		}
	} else {
		if (mgr->mms_imsi) {
			DBG("No MMS sim");
			g_free(mgr->mms_imsi);
			m->mms_imsi = mgr->mms_imsi = NULL;
			slot_manager_dbus_signal(mgr->dbus,
				SLOT_MANAGER_DBUS_SIGNAL_MMS_IMSI |
				slot_manager_update_modem_paths(mgr));
			slot_manager_emit_all_queued_signals(mgr);
		}
	}

	return TRUE;
}

static void slot_manager_dbus_set_default_voice_imsi
	(struct ofono_slot_manager *m, const char *imsi)
{
	OfonoSlotManagerObject *mgr = slot_manager_object_cast(m);

	if (g_strcmp0(mgr->default_voice_imsi, imsi)) {
		DBG("Default voice sim set to %s", imsi ? imsi : "(auto)");
		g_free(mgr->default_voice_imsi);
		m->default_voice_imsi =
		mgr->default_voice_imsi = g_strdup(imsi);
		slot_manager_set_config_string(mgr,
			SM_STORE_DEFAULT_VOICE_SIM, imsi);
		slot_manager_dbus_signal(mgr->dbus,
			SLOT_MANAGER_DBUS_SIGNAL_VOICE_IMSI |
			slot_manager_update_modem_paths(mgr));
		slot_manager_emit_all_queued_signals(mgr);
	}
}

static void slot_manager_dbus_set_default_data_imsi
	(struct ofono_slot_manager *m, const char *imsi)
{
	OfonoSlotManagerObject *mgr = slot_manager_object_cast(m);

	if (g_strcmp0(mgr->default_data_imsi, imsi)) {
		DBG("Default data sim set to %s", imsi ? imsi : "(auto)");
		g_free(mgr->default_data_imsi);
		m->default_data_imsi =
		mgr->default_data_imsi = g_strdup(imsi);
		slot_manager_set_config_string(mgr,
			SM_STORE_DEFAULT_DATA_SIM, imsi);
		slot_manager_dbus_signal(mgr->dbus,
			SLOT_MANAGER_DBUS_SIGNAL_DATA_IMSI |
			slot_manager_update_modem_paths(mgr));
		slot_manager_emit_all_queued_signals(mgr);
	}
}

/*==========================================================================*
 * API
 *==========================================================================*/

struct ofono_slot_driver_data *ofono_slot_driver_get_data
	(struct ofono_slot_driver_reg *reg)
{
	return reg ? reg->driver_data : NULL;
}

struct ofono_slot_manager *ofono_slot_manager_ref(struct ofono_slot_manager *m)
{
	if (m) {
		g_object_ref(slot_manager_object_cast(m));
		return m;
	}
	return NULL;
}

void ofono_slot_manager_unref(struct ofono_slot_manager *m)
{
	if (m) {
		g_object_unref(slot_manager_object_cast(m));
	}
}

void ofono_slot_driver_started(OfonoSlotDriverReg *reg)
{
	if (reg) {
		OfonoSlotManagerObject *mgr = reg->manager;

		reg->start_id = 0;
		g_object_ref(mgr);
		slot_manager_update_ready(mgr);
		slot_manager_emit_all_queued_signals(mgr);
		g_object_unref(mgr);
	}
}

unsigned long ofono_slot_manager_add_property_handler(
	struct ofono_slot_manager *m, enum ofono_slot_manager_property p,
	ofono_slot_manager_property_cb cb, void* data)
{
	return (p >= OFONO_SLOT_MANAGER_PROPERTY_ANY &&
		p <= OFONO_SLOT_MANAGER_PROPERTY_LAST) ?
		slot_base_add_property_handler(slot_base_cast(m), p,
			(slot_base_property_cb)cb, data) : 0;
}

void ofono_slot_manager_remove_handler(struct ofono_slot_manager *m,
	unsigned long id)
{
	slot_base_remove_handler(slot_manager_object_cast(m), id);
}

void ofono_slot_manager_remove_handlers(struct ofono_slot_manager *m,
	unsigned long *ids, unsigned int n)
{
	gutil_disconnect_handlers(slot_manager_object_cast(m), ids, n);
}

void ofono_slot_manager_error(struct ofono_slot_manager *m, const char *key,
	const char *message)
{
	OfonoSlotManagerObject *mgr = slot_manager_object_cast(m);

	if (mgr) {
		mgr->errors = slot_manager_inc_error_count(mgr->errors,
			SM_ERROR_COMMON_SECTION, key);
		slot_manager_dbus_signal_error(mgr->dbus, key, message);
	}
}

struct ofono_slot *ofono_slot_add(struct ofono_slot_manager *m,
	const char *path, enum ofono_radio_access_mode techs, const char *imei,
	const char *imeisv, enum ofono_slot_sim_presence sim_presence,
	enum ofono_slot_flags flags)
{
	OfonoSlotManagerObject *mgr = slot_manager_object_cast(m);

	/*
	 * Only accept these calls when we are starting! We have been
	 * assuming all along that the number of slots is known right
	 * from startup. Perhaps it wasn't a super bright idea because
	 * there are USB modems which can appear (and disappear) pretty
	 * much at any time. This has to be dealt with somehow at some
	 * point but for now let's leave it as is.
	 */
	if (mgr && !mgr->pub.ready && path &&
		g_variant_is_object_path(path) && imei &&
		!slot_manager_foreach_slot(mgr,	slot_check_slot_name, path)) {
		return slot_add_internal(mgr, path, techs, imei, imeisv,
			sim_presence, flags);
	} else if (path) {
		ofono_error("Refusing to register slot %s", path);
	}
	return NULL;
}

struct ofono_slot *ofono_slot_ref(struct ofono_slot *s)
{
	if (s) {
		g_object_ref(slot_object_cast(s));
		return s;
	}
	return NULL;
}

void ofono_slot_unref(struct ofono_slot *s)
{
	if (s) {
		g_object_unref(slot_object_cast(s));
	}
}

void ofono_slot_error(struct ofono_slot *s, const char *key, const char *msg)
{
	OfonoSlotObject *slot = slot_object_cast(s);

	if (slot) {
		/* slot->path always starts with a slash, skip it */
		const char *section = s->path + 1;
		OfonoSlotManagerObject *mgr = slot->manager;

		slot->errors = slot_manager_inc_error_count(slot->errors,
			section, key);
		if (mgr) {
			slot_manager_dbus_signal_modem_error(mgr->dbus,
				slot->index, key, msg);
		}
	}
}

void ofono_slot_set_cell_info(struct ofono_slot *s, struct ofono_cell_info *ci)
{
	OfonoSlotObject *slot = slot_object_cast(s);

	if (slot && slot->cellinfo != ci) {
		cell_info_dbus_free(slot->cellinfo_dbus);
		ofono_cell_info_unref(slot->cellinfo);
		slot->cellinfo = ofono_cell_info_ref(ci);
		slot->cellinfo_dbus = NULL;
		slot_update_cell_info_dbus(slot);
	}
}

unsigned long ofono_slot_add_property_handler(struct ofono_slot *s,
	enum ofono_slot_property p, ofono_slot_property_cb cb, void* data)
{
	return (p >= OFONO_SLOT_PROPERTY_ANY && p <= OFONO_SLOT_PROPERTY_LAST) ?
		slot_base_add_property_handler(slot_base_cast(s), p,
			(slot_base_property_cb)cb, data) : 0;
}

void ofono_slot_remove_handler(struct ofono_slot *s, unsigned long id)
{
	slot_base_remove_handler(slot_object_cast(s), id);
}

void ofono_slot_remove_handlers(struct ofono_slot *s, unsigned long *ids,
	unsigned int n)
{
	gutil_disconnect_handlers(slot_object_cast(s), ids, n);
}

void ofono_slot_set_sim_presence(struct ofono_slot *s,
	enum ofono_slot_sim_presence sim_presence)
{
	if (s && s->sim_presence != sim_presence) {
		OfonoSlotObject *slot = slot_object_cast(s);
		OfonoSlotManagerObject *mgr = slot->manager;

		s->sim_presence = sim_presence;
		slot_queue_property_change(slot,
			OFONO_SLOT_PROPERTY_SIM_PRESENCE);
		slot_manager_dbus_signal_sim(mgr->dbus, slot->index,
			SLOT_MANAGER_DBUS_SLOT_SIGNAL_PRESENT);
		slot_manager_update_modem_paths(mgr);
		slot_manager_update_ready(mgr);
		slot_manager_emit_all_queued_signals(mgr);
	}
}

/*==========================================================================*
 * GObject boilerplate
 *==========================================================================*/

static void ofono_slot_base_init(OfonoSlotBase *base)
{
}

static void ofono_slot_object_init(OfonoSlotObject *slot)
{
	slot->pub.enabled = TRUE; /* Enabled by default */
}

static void ofono_slot_manager_object_init(OfonoSlotManagerObject *mgr)
{
	static const struct slot_manager_dbus_cb dbus_cb = {
		slot_manager_dbus_get_errors,
		slot_manager_dbus_get_slot_errors,
		slot_manager_dbus_set_enabled_slots,
		slot_manager_dbus_set_mms_imsi,
		slot_manager_dbus_set_default_voice_imsi,
		slot_manager_dbus_set_default_data_imsi
	};

	GKeyFile *conf = g_key_file_new();
	char* fn = g_build_filename(ofono_config_dir(), SM_CONFIG_FILE, NULL);

	/* Load config */
	if (g_key_file_load_from_file(conf, fn, 0, NULL)) {
		int ival;

		DBG("Loading configuration file %s", fn);
		if (config_get_enum(conf, SM_CONFIG_GROUP,
				SM_CONFIG_KEY_AUTO_DATA_SIM, &ival,
				"off", SLOT_SIM_AUTO_SELECT_OFF,
				"once", SLOT_SIM_AUTO_SELECT_ONCE,
				"always", SLOT_SIM_AUTO_SELECT_ON,
				"on", SLOT_SIM_AUTO_SELECT_ON, NULL)) {
			DBG("Automatic data SIM selection: %s",
				ival == SLOT_SIM_AUTO_SELECT_ONCE ? "once":
				ival == SLOT_SIM_AUTO_SELECT_ON ? "on":
				"off");
			mgr->auto_data_sim = ival;
		}
	}
	g_key_file_free(conf);
	g_free(fn);

	/* Load settings */
	mgr->storage = storage_open(NULL, SM_STORE);
	mgr->pub.default_voice_imsi = mgr->default_voice_imsi =
		g_key_file_get_string(mgr->storage, SM_STORE_GROUP,
					SM_STORE_DEFAULT_VOICE_SIM, NULL);
	mgr->pub.default_data_imsi = mgr->default_data_imsi =
		g_key_file_get_string(mgr->storage, SM_STORE_GROUP,
					SM_STORE_DEFAULT_DATA_SIM, NULL);
	mgr->auto_data_sim_done = g_key_file_get_boolean(mgr->storage,
			SM_STORE_GROUP, SM_STORE_AUTO_DATA_SIM_DONE, NULL);

	DBG("Default voice sim is %s",  mgr->default_voice_imsi ?
				mgr->default_voice_imsi : "(auto)");
	DBG("Default data sim is %s",  mgr->default_data_imsi ?
				mgr->default_data_imsi : "(auto)");

	/* Delay the initialization until after all drivers get registered */
	mgr->init_countdown = SM_INIT_IDLE_COUNT;
	mgr->init_id = g_idle_add(slot_manager_init_countdown_cb, mgr);

	/* And block all requests until that happens */
	mgr->dbus = slot_manager_dbus_new(&mgr->pub, &dbus_cb);
	slot_manager_dbus_set_block(mgr->dbus, SLOT_MANAGER_DBUS_BLOCK_ALL);
}

static void ofono_slot_base_class_init(OfonoSlotBaseClass *c)
{
	GType type = G_OBJECT_CLASS_TYPE(c);

	slot_base_signals[SIGNAL_PROPERTY_CHANGED] =
		g_signal_new(SIGNAL_PROPERTY_CHANGED_NAME, type,
			G_SIGNAL_RUN_FIRST | G_SIGNAL_DETAILED, 0,
			NULL, NULL, NULL, G_TYPE_NONE, 1, G_TYPE_UINT);
}

static void ofono_slot_object_class_init(OfonoSlotObjectClass *c)
{
	G_OBJECT_CLASS(c)->finalize = slot_object_finalize;
}

static void ofono_slot_manager_object_class_init(OfonoSlotManagerObjectClass *c)
{
	G_OBJECT_CLASS(c)->finalize = ofono_slot_manager_object_finalize;
}

/*==========================================================================*
 * Internal API
 *==========================================================================*/

static OfonoSlotManagerObject *slot_manager = NULL;

void __ofono_slot_manager_init(void)
{
	/*
	 * Let's not assume what's called first, ofono_slot_driver_register()
	 * or __ofono_slot_manager_init()
	 */
	if (!slot_manager) {
		slot_manager = ofono_slot_manager_new();
	}
}

void __ofono_slot_manager_cleanup(void)
{
	if (slot_manager) {
		OfonoSlotManagerObject *mgr = slot_manager;
		GSList *drivers = mgr->drivers;

		/*
		 * This cleanup needs to be done before dropping the ref.
		 * Drivers may keep a ref to slot_manager which would keep
		 * OfonoSlotManagerObject alive even after we drop our ref.
		 */
		slot_manager = NULL;
		slot_manager_foreach_slot(mgr, ofono_slot_manager_detach, NULL);
		mgr->drivers = NULL;
		g_slist_free_full(drivers,(GDestroyNotify)slot_driver_reg_free);
		g_object_unref(mgr);
	}
}

/*==========================================================================*
 * Driver registration
 * Requires access to slot_manager variable
 *==========================================================================*/

OfonoSlotDriverReg *ofono_slot_driver_register
	(const struct ofono_slot_driver *d)
{
	if (d) {
		/*
		 * Let's not assume what's called first,
		 * ofono_slot_driver_register() or __ofono_slot_manager_init()
		 */
		if (!slot_manager) {
			slot_manager = ofono_slot_manager_new();
		}

		return slot_manager_register_driver(slot_manager, d);
	}
	return NULL;
}

void ofono_slot_driver_unregister(OfonoSlotDriverReg *reg)
{
	if (reg && slot_manager) {
		GSList* l = g_slist_find(slot_manager->drivers, reg);

		if (l) {
			slot_manager->drivers = g_slist_delete_link
				(slot_manager->drivers, l);
			slot_driver_reg_free(reg);
		}
	}
}

/*
 * Local Variables:
 * mode: C
 * c-basic-offset: 8
 * indent-tabs-mode: t
 * End:
 */
