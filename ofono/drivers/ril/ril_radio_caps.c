/*
 *  oFono - Open Source Telephony - RIL-based devices
 *
 *  Copyright (C) 2017-2020 Jolla Ltd.
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

#include "ril_radio_caps.h"
#include "ril_radio.h"
#include "ril_sim_card.h"
#include "ril_sim_settings.h"
#include "ril_data.h"
#include "ril_log.h"

#include <grilio_queue.h>
#include <grilio_channel.h>
#include <grilio_parser.h>
#include <grilio_request.h>

#include <gutil_macros.h>
#include <gutil_idlepool.h>
#include <gutil_misc.h>

#include <ofono/watch.h>

#define SET_CAPS_TIMEOUT_MS (30*1000)
#define GET_CAPS_TIMEOUT_MS (5*1000)
#define DATA_OFF_TIMEOUT_MS (10*1000)
#define DEACTIVATE_TIMEOUT_MS (10*1000)
#define CHECK_LATER_TIMEOUT_SEC (5)

#define GET_CAPS_RETRIES 60

/*
 * This code is doing something similar to what
 * com.android.internal.telephony.ProxyController
 * is doing.
 */

enum ril_radio_caps_watch_events {
	WATCH_EVENT_IMSI,
	WATCH_EVENT_MODEM,
	WATCH_EVENT_COUNT
};

enum ril_radio_caps_sim_events {
	SIM_EVENT_STATE_CHANGED,
	SIM_EVENT_IO_ACTIVE_CHANGED,
	SIM_EVENT_COUNT
};

enum ril_radio_caps_settings_events {
	SETTINGS_EVENT_PREF_MODE,
	SETTINGS_EVENT_COUNT
};

enum ril_radio_caps_io_events {
	IO_EVENT_UNSOL_RADIO_CAPABILITY,
	IO_EVENT_PENDING,
	IO_EVENT_OWNER,
	IO_EVENT_COUNT
};

enum ril_radio_events {
	RADIO_EVENT_STATE,
	RADIO_EVENT_ONLINE,
	RADIO_EVENT_COUNT
};

typedef struct ril_radio_caps_object {
	GObject object;
	struct ril_radio_caps pub;
	enum ofono_radio_access_mode requested_modes;
	guint slot;
	char *log_prefix;
	GRilIoQueue *q;
	GRilIoChannel *io;
	GUtilIdlePool *idle_pool;
	gulong watch_event_id[WATCH_EVENT_COUNT];
	gulong settings_event_id[SETTINGS_EVENT_COUNT];
	gulong simcard_event_id[SIM_EVENT_COUNT];
	gulong io_event_id[IO_EVENT_COUNT];
	gulong radio_event_id[RADIO_EVENT_COUNT];
	int tx_id;
	int tx_pending;
	struct ofono_watch *watch;
	struct ril_data *data;
	struct ril_radio *radio;
	struct ril_sim_settings *settings;
	struct ril_sim_card *simcard;
	struct ril_radio_capability cap;
	struct ril_radio_capability old_cap;
	struct ril_radio_capability new_cap;
} RilRadioCaps;

typedef struct ril_radio_caps_manager {
	GObject object;
	GUtilIdlePool *idle_pool;
	GPtrArray *caps_list;
	GPtrArray *order_list;
	GPtrArray *requests;
	guint check_id;
	int tx_id;
	int tx_phase_index;
	gboolean tx_failed;
	struct ril_data_manager *data_manager;
} RilRadioCapsManager;

typedef struct ril_radio_caps_closure {
	GCClosure cclosure;
	ril_radio_caps_cb_t cb;
	void *user_data;
} RilRadioCapsClosure;

#define ril_radio_caps_closure_new() ((RilRadioCapsClosure *) \
	g_closure_new_simple(sizeof(RilRadioCapsClosure), NULL))

struct ril_radio_caps_request {
	RilRadioCaps *caps;
	enum ofono_radio_access_mode mode;
	enum ril_data_role role;
};

struct ril_radio_caps_check_data {
	ril_radio_caps_check_cb_t cb;
	void *data;
};

struct ril_radio_caps_request_tx_phase {
	const char *name;
	enum ril_radio_capability_phase phase;
	enum ril_radio_capability_status status;
	gboolean send_new_cap;
};

typedef void (*ril_radio_caps_enum_cb_t)(RilRadioCapsManager *self,
						RilRadioCaps *caps);

typedef GObjectClass RilRadioCapsClass;
G_DEFINE_TYPE(RilRadioCaps, ril_radio_caps, G_TYPE_OBJECT)
#define RADIO_CAPS_TYPE (ril_radio_caps_get_type())
#define RADIO_CAPS(obj) (G_TYPE_CHECK_INSTANCE_CAST((obj), \
        RADIO_CAPS_TYPE, RilRadioCaps))

enum ril_radio_caps_signal {
	CAPS_SIGNAL_MODES_CHANGED,
	CAPS_SIGNAL_COUNT
};

#define CAPS_SIGNAL_MODES_CHANGED_NAME    "ril-modes-changed"
static guint ril_radio_caps_signals[CAPS_SIGNAL_COUNT] = { 0 };

typedef GObjectClass RilRadioCapsManagerClass;
G_DEFINE_TYPE(RilRadioCapsManager, ril_radio_caps_manager, G_TYPE_OBJECT)
#define RADIO_CAPS_MANAGER_TYPE (ril_radio_caps_manager_get_type())
#define RADIO_CAPS_MANAGER(obj) (G_TYPE_CHECK_INSTANCE_CAST((obj), \
        RADIO_CAPS_MANAGER_TYPE, RilRadioCapsManager))

enum ril_radio_caps_manager_signal {
	CAPS_MANAGER_SIGNAL_ABORTED,
	CAPS_MANAGER_SIGNAL_TX_DONE,
	CAPS_MANAGER_SIGNAL_COUNT
};

#define CAPS_MANAGER_SIGNAL_ABORTED_NAME  "ril-capsmgr-aborted"
#define CAPS_MANAGER_SIGNAL_TX_DONE_NAME  "ril-capsmgr-tx-done"
static guint ril_radio_caps_manager_signals[CAPS_MANAGER_SIGNAL_COUNT] = { 0 };

static const struct ril_access_mode_raf {
	enum ofono_radio_access_mode mode;
	enum ril_radio_access_family raf;
} ril_access_mode_raf_map[] = {
	{ OFONO_RADIO_ACCESS_MODE_GSM,  RAF_EDGE | RAF_GPRS | RAF_GSM },
	{ OFONO_RADIO_ACCESS_MODE_UMTS, RAF_UMTS },
	{ OFONO_RADIO_ACCESS_MODE_LTE,  RAF_LTE | RAF_LTE_CA }
};

static const struct ril_radio_caps_request_tx_phase
					ril_radio_caps_tx_phase[] = {
	{ "START", RC_PHASE_START, RC_STATUS_NONE, FALSE },
	{ "APPLY", RC_PHASE_APPLY, RC_STATUS_NONE, TRUE },
	{ "FINISH", RC_PHASE_FINISH, RC_STATUS_SUCCESS, TRUE }
};

static const struct ril_radio_caps_request_tx_phase
					ril_radio_caps_fail_phase =
	{ "ABORT", RC_PHASE_FINISH, RC_STATUS_FAIL, FALSE };

static GUtilIdlePool *ril_radio_caps_shared_pool = NULL;

#define DBG_(caps, fmt, args...) DBG("%s" fmt, (caps)->log_prefix, ##args)

static void ril_radio_caps_manager_next_phase(RilRadioCapsManager *mgr);
static void ril_radio_caps_manager_consider_requests(RilRadioCapsManager *mgr);
static void ril_radio_caps_manager_schedule_check(RilRadioCapsManager *mgr);
static void ril_radio_caps_manager_recheck_later(RilRadioCapsManager *mgr);
static void ril_radio_caps_manager_add(RilRadioCapsManager *mgr,
							RilRadioCaps *caps);
static void ril_radio_caps_manager_remove(RilRadioCapsManager *mgr,
							RilRadioCaps *caps);

static void ril_radio_caps_permutate(GPtrArray *list, const guint *sample,
							guint off, guint n)
{
	if (off < n) {
		guint i;

		ril_radio_caps_permutate(list, sample, off + 1, n);
		for (i = off + 1; i < n; i++) {
			guint *resample = g_memdup(sample, sizeof(guint) * n);

			resample[off] = sample[i];
			resample[i] = sample[off];
			g_ptr_array_add(list, resample);
			ril_radio_caps_permutate(list, resample, off + 1, n);
		}
	}
}

static void ril_radio_caps_generate_permutations(GPtrArray *list, guint n)
{
	g_ptr_array_set_size(list, 0);

	if (n > 0) {
		guint i;
		guint *order = g_new(guint, n);

		/*
		 * In a general case this gives n! of permutations (1, 2,
		 * 6, 24, ...) but typically no more than 2
		 */
		for (i = 0; i < n; i++) order[i] = i;
		g_ptr_array_set_free_func(list, g_free);
		g_ptr_array_add(list, order);
		ril_radio_caps_permutate(list, order, 0, n);
	}
}

static gboolean ril_radio_caps_parse(const char *log_prefix,
		const void *data, guint len, struct ril_radio_capability *cap)
{
	GRilIoParser rilp;
	guint32 version, tx, phase, rat;

	memset(cap, 0, sizeof(*cap));
	grilio_parser_init(&rilp, data, len);

	if (grilio_parser_get_uint32(&rilp, &version) &&
			grilio_parser_get_uint32(&rilp, &tx) &&
			grilio_parser_get_uint32(&rilp, &phase) &&
			grilio_parser_get_uint32(&rilp, &rat)) {
		guint32 status;
		char *uuid = grilio_parser_get_utf8(&rilp);

		if (grilio_parser_get_uint32(&rilp, &status) &&
				grilio_parser_at_end(&rilp)) {
			DBG("%sversion=%d,tx=%d,phase=%d,rat=0x%x,"
				"uuid=%s,status=%d", log_prefix, version,
				tx, phase, rat, uuid, status);
			cap->version = version;
			cap->session = tx;
			cap->phase = phase;
			cap->rat = rat;
			cap->status = status;
			if (uuid) {
				strncpy(cap->logicalModemUuid, uuid,
					G_N_ELEMENTS(cap->logicalModemUuid));
				g_free(uuid);
			}
			return TRUE;
		}

		g_free(uuid);
	}

	return FALSE;
}

static void ril_radio_caps_check_done(GRilIoChannel *io, int ril_status,
				const void *data, guint len, void *user_data)
{
	struct ril_radio_caps_check_data *check = user_data;
	const struct ril_radio_capability *result = NULL;
	struct ril_radio_capability cap;

	if (ril_status == RIL_E_SUCCESS &&
				ril_radio_caps_parse("", data, len, &cap)) {
		GASSERT(cap.rat);
		if (cap.rat) {
			result = &cap;
		}
	}

	check->cb(result, check->data);
}

static gboolean ril_radio_caps_check_retry(GRilIoRequest *request,
		int ril_status, const void *resp, guint len, void *user_data)
{
	/*
	 * RIL_E_REQUEST_NOT_SUPPORTED is not listed among the valid
	 * RIL_REQUEST_GET_RADIO_CAPABILITY errors in ril.h but some
	 * RILs (e.g. Jolla C) return is anyway.
	 */
	switch (ril_status) {
	case RIL_E_SUCCESS:
	case RIL_E_REQUEST_NOT_SUPPORTED:
	case RIL_E_OPERATION_NOT_ALLOWED:
		return FALSE;
	default:
		return TRUE;
	}
}

guint ril_radio_caps_check(GRilIoChannel *io, ril_radio_caps_check_cb_t cb,
								void *data)
{
	guint id;
	GRilIoRequest *req = grilio_request_new();
	struct ril_radio_caps_check_data *check =
		g_new0(struct ril_radio_caps_check_data, 1);

	check->cb = cb;
	check->data = data;

	/* Make is blocking because this is typically happening at startup
	 * when there are lots of things happening at the same time which
	 * makes some RILs unhappy. Slow things down a bit by not letting
	 * to submit any other requests while this one is pending. */
	grilio_request_set_blocking(req, TRUE);
	grilio_request_set_retry(req, GET_CAPS_TIMEOUT_MS, GET_CAPS_RETRIES);
	grilio_request_set_retry_func(req, ril_radio_caps_check_retry);
	id = grilio_channel_send_request_full(io, req,
				RIL_REQUEST_GET_RADIO_CAPABILITY,
				ril_radio_caps_check_done, g_free, check);
	grilio_request_unref(req);
	return id;
}

/*==========================================================================*
 * ril_radio_caps
 *==========================================================================*/

static inline RilRadioCaps *ril_radio_caps_cast(struct ril_radio_caps *caps)
{
	return caps ? RADIO_CAPS(G_CAST(caps,RilRadioCaps,pub)) : NULL;
}

static enum ofono_radio_access_mode ril_radio_caps_access_mode
						(const RilRadioCaps *self)
{
	int i;

	/* Returns the highest matched mode */
	for (i = G_N_ELEMENTS(ril_access_mode_raf_map); i >= 0; i--) {
		if (self->cap.rat & ril_access_mode_raf_map[i].raf) {
			return ril_access_mode_raf_map[i].mode;
		}
	}

	return OFONO_RADIO_ACCESS_MODE_ANY;
}

static enum ofono_radio_access_mode ril_radio_caps_modes
				(const struct ril_radio_capability *cap)
{
	const enum ril_radio_access_family raf = cap->rat;
	enum ofono_radio_access_mode modes = 0;
	int i;

	/* Bitwise-OR all matched modes */
	for (i = 0; i < G_N_ELEMENTS(ril_access_mode_raf_map); i++) {
		if (raf & ril_access_mode_raf_map[i].raf) {
			modes |= ril_access_mode_raf_map[i].mode;
		}
	}
	return modes;
}

static void ril_radio_caps_update_modes(RilRadioCaps *self)
{
	struct ril_radio_caps *caps = &self->pub;
	const struct ril_radio_capability *cap = &self->cap;
	const enum ofono_radio_access_mode modes = ril_radio_caps_modes(cap);

	if (caps->supported_modes != modes) {
		caps->supported_modes = modes;
		ril_radio_caps_manager_schedule_check(caps->mgr);
		g_signal_emit(self, ril_radio_caps_signals
					[CAPS_SIGNAL_MODES_CHANGED], 0);
	}
}

static int ril_radio_caps_score(const RilRadioCaps *self,
					const struct ril_radio_capability *cap)
{
	if (!self->radio->online || !self->simcard->status ||
		self->simcard->status->card_state != RIL_CARDSTATE_PRESENT) {
		/* Unusable slot */
		return -(int)ril_radio_caps_modes(cap);
	} else if (self->requested_modes) {
		if (ril_radio_caps_modes(cap) >= self->requested_modes) {
			/* Happy slot (upgrade not required) */
			return self->requested_modes;
		} else {
			/* Unhappy slot (wants upgrade) */
			return -(int)self->requested_modes;
		}
	} else {
		/* Whatever */
		return 0;
	}
}

static void ril_radio_caps_radio_event(struct ril_radio *radio, void *arg)
{
	RilRadioCaps *self = RADIO_CAPS(arg);

	DBG_(self, "");
	ril_radio_caps_manager_schedule_check(self->pub.mgr);
}

static void ril_radio_caps_simcard_event(struct ril_sim_card *sim,
							void *arg)
{
	RilRadioCaps *self = RADIO_CAPS(arg);

	DBG_(self, "");
	ril_radio_caps_manager_schedule_check(self->pub.mgr);
}

static void ril_radio_caps_watch_event(struct ofono_watch *w, void *arg)
{
	RilRadioCaps *self = RADIO_CAPS(arg);

	DBG_(self, "");
	ril_radio_caps_manager_schedule_check(self->pub.mgr);
}

static void ril_radio_caps_settings_event(struct ril_sim_settings *settings,
							void *arg)
{
	RilRadioCaps *self = RADIO_CAPS(arg);
	RilRadioCapsManager *mgr = self->pub.mgr;

	DBG_(self, "");
	ril_radio_caps_manager_consider_requests(mgr);
	ril_radio_caps_manager_schedule_check(mgr);
}

static void ril_radio_caps_changed_cb(GRilIoChannel *io, guint code,
				const void *data, guint len, void *arg)
{
	RilRadioCaps *self = RADIO_CAPS(arg);

	DBG_(self, "");
	GASSERT(code == RIL_UNSOL_RADIO_CAPABILITY);
	if (ril_radio_caps_parse(self->log_prefix, data, len, &self->cap)) {
		ril_radio_caps_update_modes(self);
		ril_radio_caps_manager_schedule_check(self->pub.mgr);
	}
}

static void ril_radio_caps_finish_init(RilRadioCaps *self)
{
	GASSERT(ril_radio_caps_access_mode(self));

	/* Register for update notifications */
	self->io_event_id[IO_EVENT_UNSOL_RADIO_CAPABILITY] =
		grilio_channel_add_unsol_event_handler(self->io,
			ril_radio_caps_changed_cb, RIL_UNSOL_RADIO_CAPABILITY,
			self);

	/* Schedule capability check */
	ril_radio_caps_manager_schedule_check(self->pub.mgr);
}

static void ril_radio_caps_initial_query_cb(GRilIoChannel *io, int ril_status,
				const void *data, guint len, void *user_data)
{
	RilRadioCaps *self = RADIO_CAPS(user_data);

	if (ril_status == RIL_E_SUCCESS) {
		ril_radio_caps_parse(self->log_prefix, data, len, &self->cap);
	}

	if (self->cap.rat) {
		ril_radio_caps_update_modes(self);
		ril_radio_caps_finish_init(self);
	} else {
		DBG_(self, "failed to query radio capabilities");
	}
}

static void ril_radio_caps_finalize(GObject *object)
{
	RilRadioCaps *self = RADIO_CAPS(object);
	RilRadioCapsManager *mgr = self->pub.mgr;

	ril_radio_remove_all_handlers(self->radio, self->radio_event_id);
	ril_sim_settings_remove_handlers(self->settings,
		self->settings_event_id, G_N_ELEMENTS(self->settings_event_id));
	ril_sim_card_remove_all_handlers(self->simcard, self->simcard_event_id);
	grilio_channel_remove_all_handlers(self->io, self->io_event_id);
	ofono_watch_remove_all_handlers(self->watch, self->watch_event_id);
	ofono_watch_unref(self->watch);
	ril_radio_caps_manager_remove(mgr, self);
	ril_radio_caps_manager_unref(mgr);
	grilio_queue_cancel_all(self->q, FALSE);
	grilio_queue_unref(self->q);
	grilio_channel_unref(self->io);
	ril_data_unref(self->data);
	ril_radio_unref(self->radio);
	ril_sim_card_unref(self->simcard);
	ril_sim_settings_unref(self->settings);
	gutil_idle_pool_unref(self->idle_pool);
	g_free(self->log_prefix);
	G_OBJECT_CLASS(ril_radio_caps_parent_class)->finalize(object);
}

struct ril_radio_caps *ril_radio_caps_new(RilRadioCapsManager *mgr,
		const char *log_prefix, GRilIoChannel *io,
		struct ofono_watch *watch,
		struct ril_data *data, struct ril_radio *radio,
		struct ril_sim_card *sim, struct ril_sim_settings *settings,
		const struct ril_slot_config *config,
		const struct ril_radio_capability *cap)
{
	GASSERT(mgr);
	if (G_LIKELY(mgr)) {
		RilRadioCaps *self = g_object_new(RADIO_CAPS_TYPE, 0);
		struct ril_radio_caps *caps = &self->pub;

		self->slot = config->slot;
		self->log_prefix = (log_prefix && log_prefix[0]) ?
			g_strconcat(log_prefix, " ", NULL) : g_strdup("");

		self->q = grilio_queue_new(io);
		self->io = grilio_channel_ref(io);
		self->data = ril_data_ref(data);
		caps->mgr = ril_radio_caps_manager_ref(mgr);

		self->radio = ril_radio_ref(radio);
		self->radio_event_id[RADIO_EVENT_STATE] =
			ril_radio_add_state_changed_handler(radio,
				ril_radio_caps_radio_event, self);
		self->radio_event_id[RADIO_EVENT_ONLINE] =
			ril_radio_add_online_changed_handler(radio,
				ril_radio_caps_radio_event, self);

		self->simcard = ril_sim_card_ref(sim);
		self->simcard_event_id[SIM_EVENT_STATE_CHANGED] =
			ril_sim_card_add_state_changed_handler(sim,
				ril_radio_caps_simcard_event, self);
		self->simcard_event_id[SIM_EVENT_IO_ACTIVE_CHANGED] =
			ril_sim_card_add_sim_io_active_changed_handler(sim,
				ril_radio_caps_simcard_event, self);

		self->watch = ofono_watch_ref(watch);
		self->watch_event_id[WATCH_EVENT_IMSI] =
			ofono_watch_add_imsi_changed_handler(watch,
				ril_radio_caps_watch_event, self);
		self->watch_event_id[WATCH_EVENT_MODEM] =
			ofono_watch_add_modem_changed_handler(watch,
				ril_radio_caps_watch_event, self);

		self->settings = ril_sim_settings_ref(settings);
		self->settings_event_id[SETTINGS_EVENT_PREF_MODE] =
			ril_sim_settings_add_pref_mode_changed_handler(
				settings, ril_radio_caps_settings_event, self);

		ril_radio_caps_manager_add(mgr, self);
		if (cap) {
			/* Current capabilities are provided by the caller */
			self->cap = *cap;
			caps->supported_modes = ril_radio_caps_modes(cap);
			ril_radio_caps_finish_init(self);
		} else {
			/* Need to query current capabilities */
			GRilIoRequest *req = grilio_request_new();
			grilio_request_set_retry(req, GET_CAPS_TIMEOUT_MS,
					GET_CAPS_RETRIES);
			grilio_queue_send_request_full(self->q, req,
					RIL_REQUEST_GET_RADIO_CAPABILITY,
					ril_radio_caps_initial_query_cb,
					NULL, self);
			grilio_request_unref(req);
		}

		return caps;
	}
	return NULL;
}

struct ril_radio_caps *ril_radio_caps_ref(struct ril_radio_caps *caps)
{
	RilRadioCaps *self = ril_radio_caps_cast(caps);

	if (G_LIKELY(self)) {
		g_object_ref(self);
	}
	return caps;
}

void ril_radio_caps_unref(struct ril_radio_caps *caps)
{
	RilRadioCaps *self = ril_radio_caps_cast(caps);

	if (G_LIKELY(self)) {
		g_object_unref(self);
	}
}

void ril_radio_caps_drop(struct ril_radio_caps *caps)
{
	RilRadioCaps *self = ril_radio_caps_cast(caps);

	if (G_LIKELY(self)) {
		ril_radio_caps_manager_remove(self->pub.mgr, self);
		g_object_unref(self);
	}
}

static void ril_radio_caps_signal_cb(RilRadioCaps *object,
					RilRadioCapsClosure *closure)
{
	closure->cb(&object->pub, closure->user_data);
}

gulong ril_radio_caps_add_supported_modes_handler(struct ril_radio_caps *caps,
					ril_radio_caps_cb_t cb, void *arg)
{
	RilRadioCaps *self = ril_radio_caps_cast(caps);

	if (G_LIKELY(self) && G_LIKELY(cb)) {
		RilRadioCapsClosure *closure = ril_radio_caps_closure_new();
		GCClosure *cc = &closure->cclosure;

		cc->closure.data = closure;
		cc->callback = G_CALLBACK(ril_radio_caps_signal_cb);
		closure->cb = cb;
		closure->user_data = arg;

		return g_signal_connect_closure_by_id(self,
			ril_radio_caps_signals[CAPS_SIGNAL_MODES_CHANGED],
			0, &cc->closure, FALSE);
	}
	return 0;
}

void ril_radio_caps_remove_handler(struct ril_radio_caps *caps, gulong id)
{
	if (G_LIKELY(id)) {
		RilRadioCaps *self = ril_radio_caps_cast(caps);

		if (G_LIKELY(self)) {
			g_signal_handler_disconnect(self, id);
		}
	}
}

static void ril_radio_caps_init(RilRadioCaps *self)
{
	self->idle_pool = gutil_idle_pool_ref
		(gutil_idle_pool_get(&ril_radio_caps_shared_pool));
}

static void ril_radio_caps_class_init(RilRadioCapsClass *klass)
{
	G_OBJECT_CLASS(klass)->finalize = ril_radio_caps_finalize;
	ril_radio_caps_signals[CAPS_SIGNAL_MODES_CHANGED] =
		g_signal_new(CAPS_SIGNAL_MODES_CHANGED_NAME,
			G_OBJECT_CLASS_TYPE(klass), G_SIGNAL_RUN_FIRST,
			0, NULL, NULL, NULL, G_TYPE_NONE, 0);
}

/*==========================================================================*
 * ril_radio_caps_manager
 *==========================================================================*/

static const char *ril_radio_caps_manager_order_str(RilRadioCapsManager *self,
							const guint *order)
{
	const guint n = self->caps_list->len;

	if (n > 0) {
		guint i;
		char *str;
		GString *buf = g_string_sized_new(2*n + 2 /* roughly */);

		g_string_append_printf(buf, "(%u", order[0]);
		for (i = 1; i < n; i++) {
			g_string_append_printf(buf, ",%u", order[i]);
		}
		g_string_append_c(buf, ')');
		str = g_string_free(buf, FALSE);
		gutil_idle_pool_add(self->idle_pool, str, g_free);
		return str;
	} else {
		return "()";
	}
}

static const char *ril_radio_caps_manager_role_str(RilRadioCapsManager *self,
						enum ril_data_role role)
{
	char *str;

	switch (role) {
	case RIL_DATA_ROLE_NONE:
		return "none";
	case RIL_DATA_ROLE_MMS:
		return "mms";
	case RIL_DATA_ROLE_INTERNET:
		return "internet";
	}

	str = g_strdup_printf("%d", (int)role);
	gutil_idle_pool_add(self->idle_pool, str, g_free);
	return str;
}

static void ril_radio_caps_manager_foreach(RilRadioCapsManager *self,
						ril_radio_caps_enum_cb_t cb)
{
	guint i;
	const GPtrArray *list = self->caps_list;

	for (i = 0; i < list->len; i++) {
		cb(self, (RilRadioCaps *)(list->pdata[i]));
	}
}

static void ril_radio_caps_manager_foreach_tx(RilRadioCapsManager *self,
						ril_radio_caps_enum_cb_t cb)
{
	guint i;
	const GPtrArray *list = self->caps_list;

	for (i = 0; i < list->len; i++) {
		RilRadioCaps *caps = list->pdata[i];

		/* Ignore the modems not associated with this transaction */
		if (caps->tx_id == self->tx_id) {
			cb(self, caps);
		}
	}
}

static gboolean ril_radio_caps_manager_tx_pending(RilRadioCapsManager *self)
{
	guint i;
	const GPtrArray *list = self->caps_list;

	for (i = 0; i < list->len; i++) {
		RilRadioCaps *caps = list->pdata[i];

		/* Ignore the modems not associated with this transaction */
		if (caps->tx_id == self->tx_id && caps->tx_pending > 0) {
			return TRUE;
		}
	}

	return FALSE;
}

/**
 * Checks that all radio caps have been initialized (i.e. all the initial
 * GET_RADIO_CAPABILITY requests have completed) and there's no transaction
 * in progress.
 */
static gboolean ril_radio_caps_manager_can_check(RilRadioCapsManager *self)
{
	if (self->caps_list && !ril_radio_caps_manager_tx_pending(self)) {
		const GPtrArray *list = self->caps_list;
		const RilRadioCaps *prev_caps = NULL;
		gboolean all_modes_equal = TRUE;
		guint i;

		for (i = 0; i < list->len; i++) {
			const RilRadioCaps *caps = list->pdata[i];
			const struct ril_radio *radio = caps->radio;
			const struct ril_sim_card_status *status =
				caps->simcard->status;
			const gboolean slot_enabled =
				(caps->watch->modem != NULL);
			const gboolean sim_present = status &&
				(status->card_state == RIL_CARDSTATE_PRESENT);

			if (slot_enabled &&
				((radio->online &&
					(radio->state != RADIO_STATE_ON ||
					!caps->cap.rat)) || (sim_present &&
						!caps->settings->imsi))) {
				DBG_(caps, "not ready");
				return FALSE;
			}

			if (!prev_caps) {
				prev_caps = caps;
			} else if (ril_radio_caps_access_mode(prev_caps) !=
					ril_radio_caps_access_mode(caps)) {
				all_modes_equal = FALSE;
			}

			DBG_(caps, "enabled=%s,online=%s,sim=%s,imsi=%s,"
				"raf=0x%x(%s),uuid=%s,req=%s,score=%d",
				slot_enabled ? "yes" : "no",
				radio->online ? "yes" : "no", status ?
				(status->card_state == RIL_CARDSTATE_PRESENT) ?
				"yes" : "no" : "?", caps->settings->imsi ?
				caps->settings->imsi : "", caps->cap.rat,
				ofono_radio_access_mode_to_string
				(ril_radio_caps_access_mode(caps)),
				caps->cap.logicalModemUuid,
				ofono_radio_access_mode_to_string
				(caps->requested_modes),
				ril_radio_caps_score(caps, &caps->cap));
		}
		return !all_modes_equal;
	}
	return FALSE;
}

static void ril_radio_caps_manager_issue_requests(RilRadioCapsManager *self,
		const struct ril_radio_caps_request_tx_phase *phase,
		GRilIoChannelResponseFunc handler)
{
	guint i;
	const GPtrArray *list = self->caps_list;

	DBG("%s transaction %d", phase->name, self->tx_id);
	for (i = 0; i < list->len; i++) {
		RilRadioCaps *caps = list->pdata[i];

		/* Ignore the modems not associated with this transaction */
		if (caps->tx_id == self->tx_id) {
			GRilIoRequest *req = grilio_request_new();
			const struct ril_radio_capability *cap =
				phase->send_new_cap ? &caps->new_cap :
							&caps->old_cap;

			/* Count it */
			caps->tx_pending++;
			DBG_(caps, "tx_pending=%d", caps->tx_pending);

			/* Encode and send the request */
			grilio_request_append_int32(req,
					RIL_RADIO_CAPABILITY_VERSION);
			grilio_request_append_int32(req, self->tx_id);
			grilio_request_append_int32(req, phase->phase);
			grilio_request_append_int32(req, cap->rat);
			grilio_request_append_utf8(req, cap->logicalModemUuid);
			grilio_request_append_int32(req, phase->status);
			grilio_request_set_timeout(req, SET_CAPS_TIMEOUT_MS);
			grilio_queue_send_request_full(caps->q, req,
					RIL_REQUEST_SET_RADIO_CAPABILITY,
					handler, NULL, caps);
			grilio_request_unref(req);
		}
	}
}

static void ril_radio_caps_manager_next_transaction_cb
		(RilRadioCapsManager *self,  RilRadioCaps *caps)
{
	grilio_queue_cancel_all(caps->q, FALSE);
	grilio_channel_remove_handlers(caps->io, caps->io_event_id +
					IO_EVENT_PENDING, 1);
	grilio_channel_remove_handlers(caps->io, caps->io_event_id +
					IO_EVENT_OWNER, 1);
	ril_sim_card_remove_handlers(caps->simcard, caps->simcard_event_id +
					SIM_EVENT_IO_ACTIVE_CHANGED, 1);
}

static void ril_radio_caps_manager_next_transaction(RilRadioCapsManager *self)
{
	ril_radio_caps_manager_foreach(self,
				ril_radio_caps_manager_next_transaction_cb);
	self->tx_failed = FALSE;
	self->tx_phase_index = -1;
	self->tx_id++;
	if (self->tx_id <= 0) self->tx_id = 1;
}

static void ril_radio_caps_manager_cancel_cb(RilRadioCapsManager *self,
						 RilRadioCaps *caps)
{
	GASSERT(!caps->io_event_id[IO_EVENT_OWNER]);
	GASSERT(!caps->io_event_id[IO_EVENT_PENDING]);
	grilio_queue_transaction_finish(caps->q);
}

static void ril_radio_caps_manager_transaction_done(RilRadioCapsManager *self)
{
	ril_radio_caps_manager_schedule_check(self);
	ril_data_manager_assert_data_on(self->data_manager);
	ril_radio_caps_manager_foreach(self, ril_radio_caps_manager_cancel_cb);
}

static void ril_radio_caps_manager_abort_cb(GRilIoChannel *io,
		int ril_status, const void *data, guint len, void *user_data)
{
	RilRadioCaps *caps = RADIO_CAPS(user_data);
	RilRadioCapsManager *self = caps->pub.mgr;

	GASSERT(caps->tx_pending > 0);
	caps->tx_pending--;
	DBG_(caps, "tx_pending=%d", caps->tx_pending);
	if (!ril_radio_caps_manager_tx_pending(self)) {
		DBG("transaction aborted");
		ril_radio_caps_manager_transaction_done(self);
	}
}

static void ril_radio_caps_manager_abort_transaction(RilRadioCapsManager *self)
{
	guint i;
	const GPtrArray *list = self->caps_list;
	const int prev_tx_id = self->tx_id;

	/* Generate new transaction id */
	DBG("aborting transaction %d", prev_tx_id);
	ril_radio_caps_manager_next_transaction(self);

	/* Re-associate the modems with the new transaction */
	for (i = 0; i < list->len; i++) {
		RilRadioCaps *caps = list->pdata[i];

		if (caps->tx_id == prev_tx_id) {
			caps->tx_id = self->tx_id;
		}
	}

	/*
	 * Issue a FINISH with RC_STATUS_FAIL. That's what
	 * com.android.internal.telephony.ProxyController does
	 * when something goes wrong.
	 */
	ril_radio_caps_manager_issue_requests(self, &ril_radio_caps_fail_phase,
					ril_radio_caps_manager_abort_cb);

	/* Notify the listeners */
	g_signal_emit(self, ril_radio_caps_manager_signals
					[CAPS_MANAGER_SIGNAL_ABORTED], 0);
}

static void ril_radio_caps_manager_next_phase_cb(GRilIoChannel *io,
		int ril_status, const void *data, guint len, void *user_data)
{
	RilRadioCaps *caps = RADIO_CAPS(user_data);
	RilRadioCapsManager *self = caps->pub.mgr;
	gboolean ok = FALSE;

	GASSERT(caps->tx_pending > 0);
	if (ril_status == RIL_E_SUCCESS) {
		struct ril_radio_capability cap;

		if (ril_radio_caps_parse(caps->log_prefix, data, len, &cap) &&
					cap.status != RC_STATUS_FAIL) {
			ok = TRUE;
		}
	}

	if (!ok) {
		if (!self->tx_failed) {
			self->tx_failed = TRUE;
			DBG("transaction %d failed", self->tx_id);
		}
	}

	caps->tx_pending--;
	DBG_(caps, "tx_pending=%d", caps->tx_pending);
	if (!ril_radio_caps_manager_tx_pending(self)) {
		if (self->tx_failed) {
			ril_radio_caps_manager_abort_transaction(self);
		} else {
			ril_radio_caps_manager_next_phase(self);
		}
	}
}

static void ril_radio_caps_manager_next_phase(RilRadioCapsManager *self)
{
	/* Note: -1 > 2 if 2 is unsigned (which turns -1 into 4294967295) */
	const int max_index = G_N_ELEMENTS(ril_radio_caps_tx_phase) - 1;

	GASSERT(!ril_radio_caps_manager_tx_pending(self));
	if (self->tx_phase_index >= max_index) {
		const GPtrArray *list = self->caps_list;
		GSList *updated_caps = NULL;
		GSList *l;
		guint i;

		DBG("transaction %d is done", self->tx_id);

		/* Update all caps before emitting signals */
		for (i = 0; i < list->len; i++) {
			RilRadioCaps *caps = list->pdata[i];

			if (caps->tx_id == self->tx_id) {
				caps->cap = caps->new_cap;
				/* Better bump refs to make sure RilRadioCaps
				 * don't get freed by a signal handler */
				updated_caps = g_slist_append(updated_caps,
						g_object_ref(caps));
			}
		}
		/* ril_radio_caps_update_modes will emit signals if needed */
		for (l = updated_caps; l; l = l->next) {
			ril_radio_caps_update_modes((RilRadioCaps *)l->data);
		}
		ril_radio_caps_manager_transaction_done(self);
		/* Free temporary RilRadioCaps references */
		g_slist_free_full(updated_caps, g_object_unref);
		g_signal_emit(self, ril_radio_caps_manager_signals
					[CAPS_MANAGER_SIGNAL_TX_DONE], 0);
	} else {
		const struct ril_radio_caps_request_tx_phase *phase =
				ril_radio_caps_tx_phase +
				(++self->tx_phase_index);

		ril_radio_caps_manager_issue_requests(self, phase,
					ril_radio_caps_manager_next_phase_cb);
	}
}

static void ril_radio_caps_manager_data_off_done(GRilIoChannel *io,
		int status, const void *req_data, guint len, void *user_data)
{
	RilRadioCaps *caps = RADIO_CAPS(user_data);
	RilRadioCapsManager *self = caps->pub.mgr;

	GASSERT(caps->tx_pending > 0);
	if (status != GRILIO_STATUS_OK) {
		self->tx_failed = TRUE;
	}
	caps->tx_pending--;
	DBG_(caps, "tx_pending=%d", caps->tx_pending);
	if (!ril_radio_caps_manager_tx_pending(self)) {
		if (self->tx_failed) {
			DBG("failed to start the transaction");
			ril_data_manager_assert_data_on(self->data_manager);
			ril_radio_caps_manager_recheck_later(self);
			ril_radio_caps_manager_foreach(self,
					ril_radio_caps_manager_cancel_cb);
			g_signal_emit(self, ril_radio_caps_manager_signals
					[CAPS_MANAGER_SIGNAL_ABORTED], 0);
		} else {
			DBG("starting transaction");
			ril_radio_caps_manager_next_phase(self);
		}
	}
}

static void ril_radio_caps_manager_data_off(RilRadioCapsManager *self,
						 RilRadioCaps *caps)
{
	GRilIoRequest *req = ril_request_allow_data_new(FALSE);

	caps->tx_pending++;
	DBG_(caps, "tx_pending=%d", caps->tx_pending);
	grilio_request_set_timeout(req, DATA_OFF_TIMEOUT_MS);
	grilio_queue_send_request_full(caps->q, req,
					RIL_REQUEST_ALLOW_DATA,
					ril_radio_caps_manager_data_off_done,
					NULL, caps);
	grilio_request_unref(req);
}

static void ril_radio_caps_manager_deactivate_data_call_done(GRilIoChannel *io,
		int status, const void *data, guint len, void *user_data)
{
	RilRadioCaps *caps = RADIO_CAPS(user_data);
	RilRadioCapsManager *self = caps->pub.mgr;

	GASSERT(caps->tx_pending > 0);
	if (status != GRILIO_STATUS_OK) {
		self->tx_failed = TRUE;
		/* Something seems to be slightly broken, try requesting the
		 * current state (later, after we release the transaction). */
		ril_data_poll_call_state(caps->data);
	}
	caps->tx_pending--;
	DBG_(caps, "tx_pending=%d", caps->tx_pending);
	if (!ril_radio_caps_manager_tx_pending(self)) {
		if (self->tx_failed) {
			DBG("failed to start the transaction");
			ril_radio_caps_manager_recheck_later(self);
			ril_radio_caps_manager_foreach(self,
					ril_radio_caps_manager_cancel_cb);
		} else {
			ril_radio_caps_manager_foreach_tx(self,
					ril_radio_caps_manager_data_off);
		}
	}
}

static void ril_radio_caps_deactivate_data_call(RilRadioCaps *caps, int cid)
{
	GRilIoRequest *req = ril_request_deactivate_data_call_new(cid);

	caps->tx_pending++;
	DBG_(caps, "cid=%u, tx_pending=%d", cid, caps->tx_pending);
	grilio_request_set_blocking(req, TRUE);
	grilio_request_set_timeout(req, DEACTIVATE_TIMEOUT_MS);
	grilio_queue_send_request_full(caps->q, req,
			RIL_REQUEST_DEACTIVATE_DATA_CALL,
			ril_radio_caps_manager_deactivate_data_call_done,
			NULL, caps);
	grilio_request_unref(req);
}

static void ril_radio_caps_deactivate_data_call_cb(gpointer list_data,
							gpointer user_data)
{
	struct ril_data_call *call = list_data;

	if (call->status == PDP_FAIL_NONE) {
		ril_radio_caps_deactivate_data_call(RADIO_CAPS(user_data),
								call->cid);
	}
}

static void ril_radio_caps_manager_deactivate_all_cb(RilRadioCapsManager *self,
						 RilRadioCaps *caps)
{
	struct ril_data *data = caps->data;

	if (data && data->data_calls) {
		g_slist_foreach(data->data_calls->calls,
			ril_radio_caps_deactivate_data_call_cb, caps);
	}
}

static void ril_radio_caps_manager_deactivate_all(RilRadioCapsManager *self)
{
	ril_radio_caps_manager_foreach_tx(self,
				ril_radio_caps_manager_deactivate_all_cb);
	if (!ril_radio_caps_manager_tx_pending(self)) {
		/* No data calls, submit ALLOW_DATA requests right away */
		ril_radio_caps_manager_foreach_tx(self,
					ril_radio_caps_manager_data_off);
		GASSERT(ril_radio_caps_manager_tx_pending(self));
	}
}

static void ril_radio_caps_tx_wait_cb(GRilIoChannel *io, void *user_data)
{
	RilRadioCaps *caps = RADIO_CAPS(user_data);
	RilRadioCapsManager *self = caps->pub.mgr;
	const GPtrArray *list = self->caps_list;
	gboolean can_start = TRUE;
	guint i;

	if (grilio_queue_transaction_state(caps->q) ==
						GRILIO_TRANSACTION_STARTED) {
		/* We no longer need owner notifications from this channel */
		grilio_channel_remove_handlers(caps->io,
				caps->io_event_id + IO_EVENT_OWNER, 1);
		if (!grilio_channel_has_pending_requests(caps->io)) {
			/* And pending notifications too */
			grilio_channel_remove_handlers(caps->io,
				caps->io_event_id + IO_EVENT_PENDING, 1);
		}
	}

	/* Check if all channels are ours */
	for (i = 0; i < list->len && can_start; i++) {
		const RilRadioCaps *caps = list->pdata[i];

		if (caps->tx_id == self->tx_id &&
			(grilio_channel_has_pending_requests(caps->io) ||
				grilio_queue_transaction_state(caps->q) !=
					GRILIO_TRANSACTION_STARTED)) {
			/* Still waiting for this one */
			DBG_(caps, "still waiting");
			can_start = FALSE;
		}
	}

	if (can_start) {
		/* All modems are ready */
		ril_radio_caps_manager_deactivate_all(self);
	}
}

static void ril_radio_caps_manager_lock_io_for_transaction
					(RilRadioCapsManager *self)
{
	const GPtrArray *list = self->caps_list;
	gboolean can_start = TRUE;
	guint i;

	/* We want to actually start the transaction when all the
	 * involved modems stop doing other things. Otherwise some
	 * RILs get confused and break. We have already checked that
	 * SIM I/O has stopped. The next synchronization point is the
	 * completion of all DEACTIVATE_DATA_CALL and ALLOW_DATA requests.
	 * Then we can start the capability switch transaction. */
	for (i = 0; i < list->len; i++) {
		RilRadioCaps *caps = list->pdata[i];
		GRILIO_TRANSACTION_STATE state;

		/* Restart the queue transation to make sure that
		 * we get to the end of the owner queue (to avoid
		 * deadlocks since we are going to wait for all
		 * queues to become the owners before actually
		 * starting the transaction) */
		grilio_queue_transaction_finish(caps->q);
		state = grilio_queue_transaction_start(caps->q);

		/* Check if we need to wait for all transaction to
		 * complete on this I/O channel before we can actually
		 * start the transaction */
		if (state == GRILIO_TRANSACTION_QUEUED) {
			GASSERT(!caps->io_event_id[IO_EVENT_OWNER]);
			caps->io_event_id[IO_EVENT_OWNER] =
				grilio_channel_add_owner_changed_handler(
					caps->io, ril_radio_caps_tx_wait_cb,
					caps);
			can_start = FALSE;
		}

		if (state == GRILIO_TRANSACTION_QUEUED ||
			grilio_channel_has_pending_requests(caps->io)) {
			GASSERT(!caps->io_event_id[IO_EVENT_PENDING]);
			caps->io_event_id[IO_EVENT_PENDING] =
				grilio_channel_add_pending_changed_handler(
					caps->io, ril_radio_caps_tx_wait_cb,
					caps);
			can_start = FALSE;
		}
	}

	if (can_start) {
		/* All modems are ready */
		ril_radio_caps_manager_deactivate_all(self);
	}
}

static void ril_radio_caps_manager_stop_sim_io_watch(RilRadioCapsManager *self,
						 RilRadioCaps *caps)
{
	/* ril_sim_card_remove_handlers zeros the id */
	ril_sim_card_remove_handlers(caps->simcard, caps->simcard_event_id +
					SIM_EVENT_IO_ACTIVE_CHANGED, 1);
}

static void ril_radio_caps_tx_wait_sim_io_cb(struct ril_sim_card *simcard,
						void *user_data)
{
	RilRadioCaps *src = RADIO_CAPS(user_data);
	RilRadioCapsManager *self = src->pub.mgr;
	const GPtrArray *list = self->caps_list;
	guint i;

	for (i = 0; i < list->len; i++) {
		const RilRadioCaps *caps = list->pdata[i];

		if (caps->simcard->sim_io_active) {
			DBG_(caps, "still waiting for SIM I/O to calm down");
			return;
		}
	}

	/* We no longer need to be notified about SIM I/O activity */
	DBG("SIM I/O has calmed down");
	ril_radio_caps_manager_foreach(self,
				ril_radio_caps_manager_stop_sim_io_watch);

	/* Now this looks like a good moment to start the transaction */
	ril_radio_caps_manager_lock_io_for_transaction(self);
}

static void ril_radio_caps_manager_start_sim_io_watch(RilRadioCapsManager *self,
						 RilRadioCaps *caps)
{
	caps->simcard_event_id[SIM_EVENT_IO_ACTIVE_CHANGED] =
		ril_sim_card_add_sim_io_active_changed_handler(caps->simcard,
				ril_radio_caps_tx_wait_sim_io_cb, caps);
}

static void ril_radio_caps_manager_start_transaction(RilRadioCapsManager *self)
{
	const GPtrArray *list = self->caps_list;
	gboolean sim_io_active = FALSE;
	guint i, count = 0;

	/* Start the new request transaction */
	ril_radio_caps_manager_next_transaction(self);
	DBG("transaction %d", self->tx_id);

	for (i = 0; i < list->len; i++) {
		RilRadioCaps *caps = list->pdata[i];

		if (memcmp(&caps->new_cap, &caps->old_cap, sizeof(caps->cap))) {
			/* Mark it as taking part in this transaction */
			caps->tx_id = self->tx_id;
			count++;
			if (caps->simcard->sim_io_active) {
				sim_io_active = TRUE;
			}
		}
	}

	GASSERT(count);
	if (!count) {
		/* This is not supposed to happen */
		DBG("nothing to do!");
	} else if (sim_io_active) {
		DBG("waiting for SIM I/O to calm down");
		ril_radio_caps_manager_foreach_tx(self,
				ril_radio_caps_manager_start_sim_io_watch);
	} else {
		/* Make sure we don't get notified about SIM I/O activity */
		ril_radio_caps_manager_foreach(self,
				ril_radio_caps_manager_stop_sim_io_watch);

		/* And continue with locking RIL I/O for the transaction */
		ril_radio_caps_manager_lock_io_for_transaction(self);
	}
}

static void ril_radio_caps_manager_set_order(RilRadioCapsManager *self,
						const guint *order)
{
	const GPtrArray *list = self->caps_list;
	guint i;

	DBG("%s => %s", ril_radio_caps_manager_order_str
			(self, self->order_list->pdata[0]),
				ril_radio_caps_manager_order_str(self, order));

	for (i = 0; i < list->len; i++) {
		RilRadioCaps *dest = list->pdata[i];
		const RilRadioCaps *src = list->pdata[order[i]];

		dest->old_cap = dest->cap;
		dest->new_cap = src->cap;
	}
	ril_radio_caps_manager_start_transaction(self);
}

static void ril_radio_caps_manager_check(RilRadioCapsManager *self)
{
	if (ril_radio_caps_manager_can_check(self)) {
		guint i;
		const GPtrArray *list = self->caps_list;
		const GPtrArray *permutations = self->order_list;
		int highest_score = -INT_MAX, best_index = -1;

		for (i = 0; i < permutations->len; i++) {
			const guint *order = permutations->pdata[i];
			int score = 0;
			guint k;

			for (k = 0; k < list->len; k++) {
				const RilRadioCaps *c1 = list->pdata[k];
				const RilRadioCaps *c2 = list->pdata[order[k]];

				score += ril_radio_caps_score(c1, &c2->cap);
			}

			DBG("%s %d", ril_radio_caps_manager_order_str
							(self, order), score);
			if (score > highest_score) {
				highest_score = score;
				best_index = i;
			}
		}

		if (best_index > 0) {
			ril_radio_caps_manager_set_order(self,
					permutations->pdata[best_index]);
		}
	}
}

static gboolean ril_radio_caps_manager_check_cb(gpointer data)
{
	RilRadioCapsManager *self = RADIO_CAPS_MANAGER(data);

	GASSERT(self->check_id);
	self->check_id = 0;
	ril_radio_caps_manager_check(self);
	return G_SOURCE_REMOVE;
}

static void ril_radio_caps_manager_recheck_later(RilRadioCapsManager *self)
{
	if (!ril_radio_caps_manager_tx_pending(self)) {
		if (self->check_id) {
			g_source_remove(self->check_id);
			self->check_id = 0;
		}
		self->check_id = g_timeout_add_seconds(CHECK_LATER_TIMEOUT_SEC,
				ril_radio_caps_manager_check_cb, self);
	}
}

static void ril_radio_caps_manager_schedule_check(RilRadioCapsManager *self)
{
	if (!self->check_id && !ril_radio_caps_manager_tx_pending(self)) {
		self->check_id = g_idle_add(ril_radio_caps_manager_check_cb,
									self);
	}
}

static gint ril_caps_manager_sort_requests(gconstpointer a, gconstpointer b)
{
	const struct ril_radio_caps_request *r1 = *(void**)a;
	const struct ril_radio_caps_request *r2 = *(void**)b;

	/* MMS requests have higher priority */
	if (r1->role == RIL_DATA_ROLE_MMS && r2->role != RIL_DATA_ROLE_MMS) {
		return -1;
	}
	if (r1->role != RIL_DATA_ROLE_MMS && r2->role == RIL_DATA_ROLE_MMS) {
		return 1;
	}
	return (int)r2->role - (int)r1->role;
}

static void ril_radio_caps_manager_consider_requests(RilRadioCapsManager *self)
{
	guint i;
	gboolean changed = FALSE;
	const GPtrArray *list = self->caps_list;
	GPtrArray *requests = self->requests;

	if (requests->len) {
		const struct ril_radio_caps_request *req;

		g_ptr_array_sort(requests, ril_caps_manager_sort_requests);
		req = self->requests->pdata[0];

		for (i = 0; i < list->len; i++) {
			RilRadioCaps *caps = list->pdata[i];
			struct ril_sim_settings *settings = caps->settings;
			enum ofono_radio_access_mode modes;

			if (req->caps == caps) {
				modes = (req->mode && settings->pref_mode) ?
					MIN(req->mode, settings->pref_mode) :
					req->mode ? req->mode :
					settings->pref_mode;
			} else {
				modes = 0;
			}

			if (caps->requested_modes != modes) {
				caps->requested_modes = modes;
				changed = TRUE;
			}
		}
	} else {
		for (i = 0; i < list->len; i++) {
			RilRadioCaps *caps = list->pdata[i];

			if (caps->requested_modes) {
				caps->requested_modes = 0;
				changed = TRUE;
			}
		}
	}
	if (changed) {
		ril_radio_caps_manager_schedule_check(self);
	}
}

static gint ril_caps_manager_sort_caps(gconstpointer a, gconstpointer b)
{
	const RilRadioCaps *c1 = *(void**)a;
	const RilRadioCaps *c2 = *(void**)b;

	return c1->slot < c2->slot ? (-1) : c1->slot > c2->slot ? 1 : 0;
}

static void ril_radio_caps_manager_list_changed(RilRadioCapsManager *self)
{
	/* Order list elements according to slot numbers */
	g_ptr_array_sort(self->caps_list, ril_caps_manager_sort_caps);

	/* Generate full list of available permutations */
	ril_radio_caps_generate_permutations(self->order_list,
						self->caps_list->len);
}

static void ril_radio_caps_manager_add(RilRadioCapsManager *self,
							RilRadioCaps *caps)
{
	g_ptr_array_add(self->caps_list, caps);
	ril_radio_caps_manager_list_changed(self);
}

static void ril_radio_caps_manager_remove(RilRadioCapsManager *self,
							RilRadioCaps *caps)
{
	if (g_ptr_array_remove(self->caps_list, caps)) {
		ril_radio_caps_manager_list_changed(self);
	}
}

gulong ril_radio_caps_manager_add_tx_aborted_handler(RilRadioCapsManager *self,
				ril_radio_caps_manager_cb_t cb, void *arg)
{
	return (G_LIKELY(self) && G_LIKELY(cb)) ? g_signal_connect(self,
		CAPS_MANAGER_SIGNAL_ABORTED_NAME, G_CALLBACK(cb), arg) : 0;
}

gulong ril_radio_caps_manager_add_tx_done_handler(RilRadioCapsManager *self,
				ril_radio_caps_manager_cb_t cb, void *arg)
{
	return (G_LIKELY(self) && G_LIKELY(cb)) ? g_signal_connect(self,
		CAPS_MANAGER_SIGNAL_TX_DONE_NAME, G_CALLBACK(cb), arg) : 0;
}

void ril_radio_caps_manager_remove_handler(RilRadioCapsManager *self, gulong id)
{
	if (G_LIKELY(self) && G_LIKELY(id)) {
		g_signal_handler_disconnect(self, id);
	}
}

void ril_radio_caps_manager_remove_handlers(RilRadioCapsManager *self,
						gulong *ids, int count)
{
	gutil_disconnect_handlers(self, ids, count);
}

RilRadioCapsManager *ril_radio_caps_manager_ref(RilRadioCapsManager *self)
{
	if (G_LIKELY(self)) {
		g_object_ref(RADIO_CAPS_MANAGER(self));
	}
	return self;
}

void ril_radio_caps_manager_unref(RilRadioCapsManager *self)
{
	if (G_LIKELY(self)) {
		g_object_unref(RADIO_CAPS_MANAGER(self));
	}
}

RilRadioCapsManager *ril_radio_caps_manager_new(struct ril_data_manager *dm)
{
	RilRadioCapsManager *self = g_object_new(RADIO_CAPS_MANAGER_TYPE, 0);

	self->data_manager = ril_data_manager_ref(dm);
	return self;
}

static void ril_radio_caps_manager_init(RilRadioCapsManager *self)
{
	self->caps_list = g_ptr_array_new();
	self->order_list = g_ptr_array_new();
	self->requests = g_ptr_array_new();
	self->tx_phase_index = -1;
	self->idle_pool = gutil_idle_pool_ref
		(gutil_idle_pool_get(&ril_radio_caps_shared_pool));
}

static void ril_radio_caps_manager_finalize(GObject *object)
{
	RilRadioCapsManager *self = RADIO_CAPS_MANAGER(object);

	GASSERT(!self->caps_list->len);
	GASSERT(!self->order_list->len);
	GASSERT(!self->requests->len);
	g_ptr_array_free(self->caps_list, TRUE);
	g_ptr_array_free(self->order_list, TRUE);
	g_ptr_array_free(self->requests, TRUE);
	if (self->check_id) {
		g_source_remove(self->check_id);
	}
	ril_data_manager_unref(self->data_manager);
	gutil_idle_pool_unref(self->idle_pool);
	G_OBJECT_CLASS(ril_radio_caps_manager_parent_class)->finalize(object);
}

static void ril_radio_caps_manager_class_init(RilRadioCapsManagerClass *klass)
{
	G_OBJECT_CLASS(klass)->finalize = ril_radio_caps_manager_finalize;
	ril_radio_caps_manager_signals[CAPS_MANAGER_SIGNAL_ABORTED] =
		g_signal_new(CAPS_MANAGER_SIGNAL_ABORTED_NAME,
			G_OBJECT_CLASS_TYPE(klass), G_SIGNAL_RUN_FIRST,
			0, NULL, NULL, NULL, G_TYPE_NONE, 0);
	ril_radio_caps_manager_signals[CAPS_MANAGER_SIGNAL_TX_DONE] =
		g_signal_new(CAPS_MANAGER_SIGNAL_TX_DONE_NAME,
			G_OBJECT_CLASS_TYPE(klass), G_SIGNAL_RUN_FIRST,
			0, NULL, NULL, NULL, G_TYPE_NONE, 0);
}

/*==========================================================================*
 * ril_radio_caps_request
 *==========================================================================*/

struct ril_radio_caps_request *ril_radio_caps_request_new
		(struct ril_radio_caps *pub, enum ofono_radio_access_mode mode,
						enum ril_data_role role)
{
	struct ril_radio_caps_request *req = NULL;
	RilRadioCaps *caps = ril_radio_caps_cast(pub);

	if (caps) {
		RilRadioCapsManager *mgr = pub->mgr;

		DBG_(caps, "%s (%s)",
			ril_radio_caps_manager_role_str(pub->mgr, role),
			ofono_radio_access_mode_to_string(mode));
		req = g_slice_new(struct ril_radio_caps_request);
		g_object_ref(req->caps = caps);
		req->mode = mode;
		req->role = role;
		g_ptr_array_add(mgr->requests, req);
		ril_radio_caps_manager_consider_requests(mgr);
	}
	return req;
}

void ril_radio_caps_request_free(struct ril_radio_caps_request *req)
{
	if (req) {
		/* In case if g_object_unref frees the caps */
		RilRadioCapsManager *mgr = ril_radio_caps_manager_ref
			(req->caps->pub.mgr);

		DBG_(req->caps, "%s (%s)",
			ril_radio_caps_manager_role_str(mgr, req->role),
			ofono_radio_access_mode_to_string(req->mode));
		g_ptr_array_remove(mgr->requests, req);
		g_object_unref(req->caps);
		g_slice_free1(sizeof(*req), req);
		ril_radio_caps_manager_consider_requests(mgr);
		ril_radio_caps_manager_unref(mgr);
	}
}

/*
 * Local Variables:
 * mode: C
 * c-basic-offset: 8
 * indent-tabs-mode: t
 * End:
 */
