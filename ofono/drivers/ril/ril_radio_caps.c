/*
 *  oFono - Open Source Telephony - RIL-based devices
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

#include "ril_radio_caps.h"
#include "ril_radio.h"
#include "ril_network.h"
#include "ril_sim_card.h"
#include "ril_sim_settings.h"
#include "ril_data.h"
#include "ril_log.h"

#include <grilio_queue.h>
#include <grilio_channel.h>
#include <grilio_parser.h>
#include <grilio_request.h>

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

enum ril_radio_caps_sim_events {
	SIM_EVENT_STATE_CHANGED,
	SIM_EVENT_IO_ACTIVE_CHANGED,
	SIM_EVENT_COUNT
};

enum ril_radio_caps_settings_events {
	SETTINGS_EVENT_PREF_MODE,
	SETTINGS_EVENT_IMSI,
	SETTINGS_EVENT_COUNT
};

enum ril_radio_caps_io_events {
	IO_EVENT_UNSOL_RADIO_CAPABILITY,
	IO_EVENT_PENDING,
	IO_EVENT_OWNER,
	IO_EVENT_COUNT
};

struct ril_radio_caps {
	gint ref_count;
	guint slot;
	char *log_prefix;
	GRilIoQueue *q;
	GRilIoChannel *io;
	gulong settings_event_id[SETTINGS_EVENT_COUNT];
	gulong simcard_event_id[SIM_EVENT_COUNT];
	gulong io_event_id[IO_EVENT_COUNT];
	gulong max_pref_mode_event_id;
	gulong radio_event_id;
	int tx_id;
	int tx_pending;
	struct ril_data *data;
	struct ril_radio *radio;
	struct ril_network *network;
	struct ril_sim_card *simcard;
	struct ril_radio_caps_manager *mgr;
	struct ril_radio_capability cap;
	struct ril_radio_capability old_cap;
	struct ril_radio_capability new_cap;
};

typedef struct ril_radio_caps_manager {
	GObject object;
	GPtrArray *caps_list;
	guint check_id;
	int tx_id;
	int tx_phase_index;
	gboolean tx_failed;
	struct ril_data_manager *data_manager;
} RilRadioCapsManager;

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

typedef void (*ril_radio_caps_cb_t)(struct ril_radio_caps_manager *self,
						struct ril_radio_caps *caps);

typedef GObjectClass RilRadioCapsManagerClass;
G_DEFINE_TYPE(RilRadioCapsManager, ril_radio_caps_manager, G_TYPE_OBJECT)
#define RADIO_CAPS_MANAGER_TYPE (ril_radio_caps_manager_get_type())
#define RADIO_CAPS_MANAGER(obj) (G_TYPE_CHECK_INSTANCE_CAST((obj), \
        RADIO_CAPS_MANAGER_TYPE, RilRadioCapsManager))

enum ril_radio_caps_manager_signal {
    SIGNAL_ABORTED,
    SIGNAL_COUNT
};

#define SIGNAL_ABORTED_NAME  "ril-capsmgr-aborted"

static guint ril_radio_caps_manager_signals[SIGNAL_COUNT] = { 0 };

static const struct ril_radio_caps_request_tx_phase
					ril_radio_caps_tx_phase[] = {
	{ "START", RC_PHASE_START, RC_STATUS_NONE, FALSE },
	{ "APPLY", RC_PHASE_APPLY, RC_STATUS_NONE, TRUE },
	{ "FINISH", RC_PHASE_FINISH, RC_STATUS_SUCCESS, FALSE }
};

static const struct ril_radio_caps_request_tx_phase
					ril_radio_caps_fail_phase =
	{ "ABORT", RC_PHASE_FINISH, RC_STATUS_FAIL, FALSE };

#define DBG_(caps, fmt, args...) DBG("%s" fmt, (caps)->log_prefix, ##args)

static void ril_radio_caps_manager_next_phase
				(struct ril_radio_caps_manager *self);
static void ril_radio_caps_manager_schedule_check
				(struct ril_radio_caps_manager *self);
static void ril_radio_caps_manager_recheck_later
				(struct ril_radio_caps_manager *self);

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

static enum ofono_radio_access_mode ril_radio_caps_access_mode
					(const struct ril_radio_caps *caps)
{
	const enum ril_radio_access_family raf = caps->cap.rat;

	if (raf & (RAF_LTE | RAF_LTE_CA)) {
		return OFONO_RADIO_ACCESS_MODE_LTE;
	} else if (raf & RAF_UMTS) {
		return OFONO_RADIO_ACCESS_MODE_UMTS;
	} else if (raf & (RAF_EDGE | RAF_GPRS | RAF_GSM)) {
		return OFONO_RADIO_ACCESS_MODE_GSM;
	} else {
		return OFONO_RADIO_ACCESS_MODE_ANY;
	}
}

static enum ofono_radio_access_mode ril_radio_caps_pref_mode_limit
					(const struct ril_radio_caps *caps)
{
	struct ril_network *network = caps->network;
	struct ril_sim_settings *settings = network->settings;

	if (network->max_pref_mode == settings->pref_mode) {
		return network->max_pref_mode;
	} else if (network->max_pref_mode == OFONO_RADIO_ACCESS_MODE_ANY) {
		return settings->pref_mode;
	} else {
		return network->max_pref_mode;
	}
}

static gboolean ril_radio_caps_ready(const struct ril_radio_caps *caps)
{
	/* We don't want to start messing with radio capabilities before
	 * the user has entered the pin. Some RIL don't like it so much
	 * thet they refuse to work after that. */
	return caps->radio->state == RADIO_STATE_ON && caps->simcard->status &&
		(caps->simcard->status->card_state != RIL_CARDSTATE_PRESENT ||
					caps->network->settings->imsi);
}

static gboolean ril_radio_caps_ok(const struct ril_radio_caps *caps,
			const enum ofono_radio_access_mode limit)
{
	/* Check if the slot is happy with its present state */
	return caps->radio->state != RADIO_STATE_ON ||
		!caps->simcard->status ||
		caps->simcard->status->card_state != RIL_CARDSTATE_PRESENT ||
		!caps->network->settings->imsi ||
		limit == OFONO_RADIO_ACCESS_MODE_ANY ||
		ril_radio_caps_access_mode(caps) <= limit;
}

static gboolean ril_radio_caps_wants_upgrade(const struct ril_radio_caps *caps)
{
	if (caps->radio->state == RADIO_STATE_ON &&
		caps->simcard->status &&
		caps->simcard->status->card_state == RIL_CARDSTATE_PRESENT &&
		caps->network->settings->imsi) {
		enum ofono_radio_access_mode limit =
			ril_radio_caps_pref_mode_limit(caps);

		if (!limit) limit = OFONO_RADIO_ACCESS_MODE_LTE;
		return ril_radio_caps_access_mode(caps) < limit;
	}
	return FALSE;
}

static int ril_radio_caps_index(const struct ril_radio_caps * caps)
{
	guint i;
	const GPtrArray *list = caps->mgr->caps_list;

	for (i = 0; i < list->len; i++) {
		if (list->pdata[i] == caps) {
			return i;
		}
	}

	return -1;
}

static void ril_radio_caps_radio_event(struct ril_radio *radio, void *arg)
{
	struct ril_radio_caps *self = arg;

	DBG_(self, "");
	ril_radio_caps_manager_schedule_check(self->mgr);
}

static void ril_radio_caps_simcard_event(struct ril_sim_card *sim,
							void *arg)
{
	struct ril_radio_caps *self = arg;

	DBG_(self, "");
	ril_radio_caps_manager_schedule_check(self->mgr);
}

static void ril_radio_caps_settings_event(struct ril_sim_settings *settings,
							void *arg)
{
	struct ril_radio_caps *self = arg;

	DBG_(self, "");
	ril_radio_caps_manager_schedule_check(self->mgr);
}

static void ril_radio_caps_network_event(struct ril_network *network,
							void *arg)
{
	struct ril_radio_caps *self = arg;

	DBG_(self, "");
	ril_radio_caps_manager_schedule_check(self->mgr);
}

static void ril_radio_caps_changed_cb(GRilIoChannel *io, guint code,
				const void *data, guint len, void *arg)
{
	struct ril_radio_caps *self = arg;

	DBG_(self, "");
	GASSERT(code == RIL_UNSOL_RADIO_CAPABILITY);
	if (ril_radio_caps_parse(self->log_prefix, data, len, &self->cap)) {
		ril_radio_caps_manager_schedule_check(self->mgr);
	}
}

static void ril_radio_caps_finish_init(struct ril_radio_caps *self)
{
	GASSERT(ril_radio_caps_access_mode(self));

	/* Register for update notifications */
	self->io_event_id[IO_EVENT_UNSOL_RADIO_CAPABILITY] =
		grilio_channel_add_unsol_event_handler(self->io,
			ril_radio_caps_changed_cb, RIL_UNSOL_RADIO_CAPABILITY,
			self);

	/* Schedule capability check */
	ril_radio_caps_manager_schedule_check(self->mgr);
}

static void ril_radio_caps_initial_query_cb(GRilIoChannel *io, int ril_status,
				const void *data, guint len, void *user_data)
{
	struct ril_radio_caps *self = user_data;

	if (ril_status == RIL_E_SUCCESS) {
		ril_radio_caps_parse(self->log_prefix, data, len, &self->cap);
	}

	if (self->cap.rat) {
		ril_radio_caps_finish_init(self);
	} else {
		DBG_(self, "failed to query radio capabilities");
	}
}

static gint ril_caps_compare_cb(gconstpointer a, gconstpointer b)
{
	const struct ril_radio_caps *c1 = *(void**)a;
	const struct ril_radio_caps *c2 = *(void**)b;

	return c1->slot < c2->slot ? (-1) : c1->slot > c2->slot ? 1 : 0;
}

static void ril_radio_caps_free(struct ril_radio_caps *self)
{
	struct ril_radio_caps_manager *mgr = self->mgr;
	struct ril_sim_settings *settings = self->network->settings;

	ril_network_remove_handler(self->network, self->max_pref_mode_event_id);
	ril_radio_remove_handler(self->radio, self->radio_event_id);
	ril_sim_settings_remove_handlers(settings, self->settings_event_id,
				G_N_ELEMENTS(self->settings_event_id));
	ril_sim_card_remove_handlers(self->simcard, self->simcard_event_id,
				G_N_ELEMENTS(self->simcard_event_id));
	grilio_channel_remove_handlers(self->io, self->io_event_id,
				G_N_ELEMENTS(self->io_event_id));
	g_ptr_array_remove(mgr->caps_list, self);
	ril_radio_caps_manager_unref(mgr);
	grilio_queue_cancel_all(self->q, FALSE);
	grilio_queue_unref(self->q);
	grilio_channel_unref(self->io);
	ril_data_unref(self->data);
	ril_radio_unref(self->radio);
	ril_sim_card_unref(self->simcard);
	ril_network_unref(self->network);
	g_free(self->log_prefix);
	g_slice_free(struct ril_radio_caps, self);
}

struct ril_radio_caps *ril_radio_caps_new(struct ril_radio_caps_manager *mgr,
		const char *log_prefix, GRilIoChannel *io,
		struct ril_data *data, struct ril_radio *radio,
		struct ril_sim_card *sim, struct ril_network *net,
		const struct ril_slot_config *config,
		const struct ril_radio_capability *cap)
{
	GASSERT(mgr);
	if (G_LIKELY(mgr)) {
		struct ril_sim_settings *settings = net->settings;
		struct ril_radio_caps *self =
			g_slice_new0(struct ril_radio_caps);

		g_atomic_int_set(&self->ref_count, 1);
		self->slot = config->slot;
		self->log_prefix = (log_prefix && log_prefix[0]) ?
			g_strconcat(log_prefix, " ", NULL) : g_strdup("");

		self->q = grilio_queue_new(io);
		self->io = grilio_channel_ref(io);
		self->data = ril_data_ref(data);
		self->mgr = ril_radio_caps_manager_ref(mgr);

		self->radio = ril_radio_ref(radio);
		self->radio_event_id = ril_radio_add_state_changed_handler(
				radio, ril_radio_caps_radio_event, self);

		self->simcard = ril_sim_card_ref(sim);
		self->simcard_event_id[SIM_EVENT_STATE_CHANGED] =
			ril_sim_card_add_state_changed_handler(sim,
				ril_radio_caps_simcard_event, self);
		self->simcard_event_id[SIM_EVENT_IO_ACTIVE_CHANGED] =
			ril_sim_card_add_sim_io_active_changed_handler(sim,
				ril_radio_caps_simcard_event, self);

		self->network = ril_network_ref(net);
		self->settings_event_id[SETTINGS_EVENT_PREF_MODE] =
			ril_sim_settings_add_pref_mode_changed_handler(
				settings, ril_radio_caps_settings_event, self);
		self->settings_event_id[SETTINGS_EVENT_IMSI] =
			ril_sim_settings_add_imsi_changed_handler(
				settings, ril_radio_caps_settings_event, self);

		self->max_pref_mode_event_id =
			ril_network_add_max_pref_mode_changed_handler(net,
				ril_radio_caps_network_event, self);

		/* Order list elements according to slot numbers */
		g_ptr_array_add(mgr->caps_list, self);
		g_ptr_array_sort(mgr->caps_list, ril_caps_compare_cb);

		if (cap) {
			/* Current capabilities are provided by the caller */
			self->cap = *cap;
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

		return self;
	}
	return NULL;
}

struct ril_radio_caps *ril_radio_caps_ref(struct ril_radio_caps *self)
{
	if (G_LIKELY(self)) {
		GASSERT(self->ref_count > 0);
		g_atomic_int_inc(&self->ref_count);
	}
	return self;
}

void ril_radio_caps_unref(struct ril_radio_caps *self)
{
	if (G_LIKELY(self)) {
		GASSERT(self->ref_count > 0);
		if (g_atomic_int_dec_and_test(&self->ref_count)) {
			ril_radio_caps_free(self);
		}
	}
}

/*==========================================================================*
 * ril_radio_caps_manager
 *==========================================================================*/

static void ril_radio_caps_manager_foreach(struct ril_radio_caps_manager *self,
						ril_radio_caps_cb_t cb)
{
	guint i;
	const GPtrArray *list = self->caps_list;

	for (i = 0; i < list->len; i++) {
		cb(self, (struct ril_radio_caps *)(list->pdata[i]));
	}
}

static void ril_radio_caps_manager_foreach_tx
					(struct ril_radio_caps_manager *self,
						ril_radio_caps_cb_t cb)
{
	guint i;
	const GPtrArray *list = self->caps_list;

	for (i = 0; i < list->len; i++) {
		struct ril_radio_caps *caps = list->pdata[i];

		/* Ignore the modems not associated with this transaction */
		if (caps->tx_id == self->tx_id) {
			cb(self, caps);
		}
	}
}

static gboolean ril_radio_caps_manager_tx_pending
				(struct ril_radio_caps_manager *self)
{
	guint i;
	const GPtrArray *list = self->caps_list;

	for (i = 0; i < list->len; i++) {
		const struct ril_radio_caps *caps = list->pdata[i];

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
static gboolean ril_radio_caps_manager_can_check
				(struct ril_radio_caps_manager *self)
{
	if (self->caps_list && !ril_radio_caps_manager_tx_pending(self)) {
		const GPtrArray *list = self->caps_list;
		const struct ril_radio_caps *prev_caps = NULL;
		gboolean all_modes_equal = TRUE;
		guint i;

		for (i = 0; i < list->len; i++) {
			const struct ril_radio_caps *caps = list->pdata[i];

			if (caps->radio->state == RADIO_STATE_ON &&
						!caps->cap.rat) {
				DBG_(caps, "not ready");
				return FALSE;
			}

			if (!prev_caps) {
				prev_caps = caps;
			} else if (ril_radio_caps_access_mode(prev_caps) !=
					ril_radio_caps_access_mode(caps)) {
				all_modes_equal = FALSE;
			}

			DBG_(caps, "radio=%s,sim=%s,imsi=%s,raf=0x%x(%s),"
				"uuid=%s,limit=%s", (caps->radio->state ==
				RADIO_STATE_ON) ? "on" : "off",
				caps->simcard->status ?
				(caps->simcard->status->card_state ==
				RIL_CARDSTATE_PRESENT) ? "yes" : "no" : "?",
				caps->network->settings->imsi ?
				caps->network->settings->imsi : "",
				caps->cap.rat,
				ofono_radio_access_mode_to_string
				(ril_radio_caps_access_mode(caps)),
				caps->cap.logicalModemUuid,
				ofono_radio_access_mode_to_string
				(ril_radio_caps_pref_mode_limit(caps)));
		}
		return !all_modes_equal;
	}
	return FALSE;
}

static int ril_radio_caps_manager_first_mismatch
					(struct ril_radio_caps_manager *self)
{
	guint i;
	const GPtrArray *list = self->caps_list;

	for (i = 0; i < list->len; i++) {
		const struct ril_radio_caps *caps = list->pdata[i];

		if (!ril_radio_caps_ok(caps,
				ril_radio_caps_pref_mode_limit(caps))) {
			return i;
		}
	}

	return -1;
}

static int ril_radio_caps_manager_find_mismatch
			(struct ril_radio_caps_manager *self,
				const guint *order, const gboolean *done)
{
	guint i;
	const GPtrArray *list = self->caps_list;

	for (i = 0; i < list->len; i++) {
		if (!done[i] && !ril_radio_caps_ok(list->pdata[order[i]],
			ril_radio_caps_pref_mode_limit(list->pdata[i]))) {
			return i;
		}
	}

	return -1;
}

static int ril_radio_caps_manager_find_match
			(struct ril_radio_caps_manager *self,
				guint from, const guint *order,
				const gboolean *done)
{
	guint i;
	const GPtrArray *list = self->caps_list;
	const struct ril_radio_caps *src = list->pdata[order[from]];

	for (i = 0; i < list->len; i++) {
		if (!done[i] && ril_radio_caps_ok(src,
			ril_radio_caps_pref_mode_limit(list->pdata[i]))) {
			return i;
		}
	}

	return -1;
}

/**
 * Updates the order of capabilities (i.e. which slots should get
 * assigned which capabilities). Returns FALSE if nothing can be
 * done due to impossible constraints. If everything is already
 * fine, we shouldn't even get here - the caller makes sure of that.
 */
static gboolean ril_radio_caps_manager_update_caps
		(struct ril_radio_caps_manager *self, int mismatch)
{
	guint i;
	int from, to;
	gboolean ok = TRUE;
	const GPtrArray *list = self->caps_list;
	guint *order = g_new(guint, list->len);
	gboolean *done = g_new(gboolean, list->len);

	for (i = 0; i < list->len; i++) {
		const struct ril_radio_caps *caps = list->pdata[i];

		/* Not touching powered off modems */
		done[i] = !ril_radio_caps_ready(caps);
		order[i] = i;
	}

	/* The first mismatch is already known */
	to = ril_radio_caps_manager_find_match(self, mismatch, order, done);
	if (to < 0) {
		ok = FALSE;
	} else {
		DBG("%d <-> %d", mismatch, to);
 		order[mismatch] = to;
		order[to] = mismatch;
		done[to] = TRUE;
	}

	/* Handle other mismatched slots (if any) */
	while (ok && (from = ril_radio_caps_manager_find_mismatch(self, order,
							done)) >= 0) {
		to = ril_radio_caps_manager_find_match(self, from, order,
								done);
		if (to < 0) {
			ok = FALSE;
		} else {
			const guint tmp = order[from];
			DBG("%d <-> %d", order[from], order[to]);
			order[from] = order[to];
			order[to] = tmp;
			done[to] = TRUE;
		}
	}

	if (ok) {
		for (i = 0; i < list->len; i++) {
			struct ril_radio_caps *caps = list->pdata[i];
			caps->new_cap = caps->old_cap = caps->cap;
		}

		/* Update the rafs */
		for (i = 0; i < list->len; i++) {
			struct ril_radio_caps *src = list->pdata[i];
			struct ril_radio_caps *dest = list->pdata[order[i]];
			dest->new_cap = src->cap;
		}
	}

	g_free(order);
	g_free(done);
	return ok;
}

static void ril_radio_caps_manager_issue_requests
		(struct ril_radio_caps_manager *self,
		const struct ril_radio_caps_request_tx_phase *phase,
		GRilIoChannelResponseFunc handler)
{
	guint i;
	const GPtrArray *list = self->caps_list;

	DBG("%s transaction %d", phase->name, self->tx_id);
	for (i = 0; i < list->len; i++) {
		struct ril_radio_caps *caps = list->pdata[i];

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
					(struct ril_radio_caps_manager *self,
						 struct ril_radio_caps *caps)
{
	grilio_queue_cancel_all(caps->q, FALSE);
	grilio_channel_remove_handlers(caps->io, caps->io_event_id +
					IO_EVENT_PENDING, 1);
	grilio_channel_remove_handlers(caps->io, caps->io_event_id +
					IO_EVENT_OWNER, 1);
	ril_sim_card_remove_handlers(caps->simcard, caps->simcard_event_id +
					SIM_EVENT_IO_ACTIVE_CHANGED, 1);
}

static void ril_radio_caps_manager_next_transaction
					(struct ril_radio_caps_manager *self)
{
	ril_radio_caps_manager_foreach(self,
				ril_radio_caps_manager_next_transaction_cb);
	self->tx_failed = FALSE;
	self->tx_phase_index = -1;
	self->tx_id++;
	if (self->tx_id <= 0) self->tx_id = 1;
}

static void ril_radio_caps_manager_cancel_cb
					(struct ril_radio_caps_manager *self,
						 struct ril_radio_caps *caps)
{
	GASSERT(!caps->io_event_id[IO_EVENT_OWNER]);
	GASSERT(!caps->io_event_id[IO_EVENT_PENDING]);
	grilio_queue_transaction_finish(caps->q);
}

static void ril_radio_caps_manager_finish_cb
					(struct ril_radio_caps_manager *self,
						 struct ril_radio_caps *caps)
{
	ril_radio_caps_manager_cancel_cb(self, caps);
	ril_network_assert_pref_mode(caps->network, FALSE);
}

static void ril_radio_caps_manager_transaction_done
					(struct ril_radio_caps_manager *self)
{
	ril_radio_caps_manager_schedule_check(self);
	ril_data_manager_assert_data_on(self->data_manager);
	ril_radio_caps_manager_foreach(self, ril_radio_caps_manager_finish_cb);
}

static void ril_radio_caps_manager_abort_cb(GRilIoChannel *io,
		int ril_status, const void *data, guint len, void *user_data)
{
	struct ril_radio_caps *caps = user_data;
	struct ril_radio_caps_manager *self = caps->mgr;

	GASSERT(caps->tx_pending > 0);
	caps->tx_pending--;
	DBG_(caps, "tx_pending=%d", caps->tx_pending);
	if (!ril_radio_caps_manager_tx_pending(self)) {
		DBG("transaction aborted");
		ril_radio_caps_manager_transaction_done(self);
	}
}

static void ril_radio_caps_manager_abort_transaction
					(struct ril_radio_caps_manager *self)
{
	guint i;
	const GPtrArray *list = self->caps_list;
	const int prev_tx_id = self->tx_id;

	/* Generate new transaction id */
	DBG("aborting transaction %d", prev_tx_id);
	ril_radio_caps_manager_next_transaction(self);

	/* Re-associate the modems with the new transaction */
	for (i = 0; i < list->len; i++) {
		struct ril_radio_caps *caps = list->pdata[i];

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
	g_signal_emit(self, ril_radio_caps_manager_signals[SIGNAL_ABORTED], 0);
}

static void ril_radio_caps_manager_next_phase_cb(GRilIoChannel *io,
		int ril_status, const void *data, guint len, void *user_data)
{
	struct ril_radio_caps *caps = user_data;
	struct ril_radio_caps_manager *self = caps->mgr;
	gboolean ok = FALSE;

	GASSERT(caps->tx_pending > 0);
	if (ril_status == RIL_E_SUCCESS) {
		struct ril_radio_capability cap;
		if (ril_radio_caps_parse(caps->log_prefix, data, len, &cap) &&
					cap.status == RC_STATUS_SUCCESS) {
			caps->cap = cap;
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

static void ril_radio_caps_manager_next_phase
					(struct ril_radio_caps_manager *self)
{
	/* Note: -1 > 2 if 2 is unsigned (which turns -1 into 4294967295) */
	const int max_index = G_N_ELEMENTS(ril_radio_caps_tx_phase) - 1;

	GASSERT(!ril_radio_caps_manager_tx_pending(self));
	if (self->tx_phase_index >= max_index) {
		DBG("transaction %d is done", self->tx_id);
		ril_radio_caps_manager_transaction_done(self);
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
	struct ril_radio_caps *caps = user_data;
	struct ril_radio_caps_manager *self = caps->mgr;

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
		} else {
			DBG("starting transaction");
			ril_radio_caps_manager_next_phase(self);
		}
	}
}

static void ril_radio_caps_manager_data_off
					(struct ril_radio_caps_manager *self,
						 struct ril_radio_caps *caps)
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
	struct ril_radio_caps *caps = user_data;
	struct ril_radio_caps_manager *self = caps->mgr;

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

static void ril_radio_caps_deactivate_data_call(struct ril_radio_caps *caps,
								int cid)
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
		ril_radio_caps_deactivate_data_call(user_data, call->cid);
	}
}

static void ril_radio_caps_manager_deactivate_all_cb
					(struct ril_radio_caps_manager *self,
						 struct ril_radio_caps *caps)
{
	if (caps->data->data_calls) {
		g_slist_foreach(caps->data->data_calls->calls,
			ril_radio_caps_deactivate_data_call_cb, caps);
	}
}

static void ril_radio_caps_manager_deactivate_all
					(struct ril_radio_caps_manager *self)
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
	struct ril_radio_caps *caps = user_data;
	struct ril_radio_caps_manager *self = caps->mgr;
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
		struct ril_radio_caps *caps = list->pdata[i];

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
					(struct ril_radio_caps_manager *self)
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
		struct ril_radio_caps *caps = list->pdata[i];
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

static void ril_radio_caps_manager_stop_sim_io_watch
					(struct ril_radio_caps_manager *self,
						 struct ril_radio_caps *caps)
{
	/* ril_sim_card_remove_handlers zeros the id */
	ril_sim_card_remove_handlers(caps->simcard, caps->simcard_event_id +
					SIM_EVENT_IO_ACTIVE_CHANGED, 1);
}

static void ril_radio_caps_tx_wait_sim_io_cb(struct ril_sim_card *simcard,
								void *data)
{
	struct ril_radio_caps *caps = data;
	struct ril_radio_caps_manager *self = caps->mgr;
	const GPtrArray *list = self->caps_list;
	guint i;

	for (i = 0; i < list->len; i++) {
		const struct ril_radio_caps *caps = list->pdata[i];

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

static void ril_radio_caps_manager_start_sim_io_watch
					(struct ril_radio_caps_manager *self,
						 struct ril_radio_caps *caps)
{
	caps->simcard_event_id[SIM_EVENT_IO_ACTIVE_CHANGED] =
		ril_sim_card_add_sim_io_active_changed_handler(caps->simcard,
				ril_radio_caps_tx_wait_sim_io_cb, caps);
}

static void ril_radio_caps_manager_start_transaction
					(struct ril_radio_caps_manager *self)
{
	const GPtrArray *list = self->caps_list;
	gboolean sim_io_active = FALSE;
	guint i, count = 0;

	/* Start the new request transaction */
	ril_radio_caps_manager_next_transaction(self);
	DBG("transaction %d", self->tx_id);

	for (i = 0; i < list->len; i++) {
		struct ril_radio_caps *caps = list->pdata[i];

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

static GSList *ril_radio_caps_manager_upgradable_slots
					(struct ril_radio_caps_manager *self)
{
	GSList *found = NULL;
	const GPtrArray *list = self->caps_list;
	guint i;

	for (i = 0; i < list->len; i++) {
		struct ril_radio_caps *caps = list->pdata[i];

		if (ril_radio_caps_wants_upgrade(caps)) {
			found = g_slist_append(found, caps);
		}
	}

	return found;
}

static GSList *ril_radio_caps_manager_empty_slots
					(struct ril_radio_caps_manager *self)
{
	GSList *found = NULL;
	const GPtrArray *list = self->caps_list;
	guint i;

	for (i = 0; i < list->len; i++) {
		struct ril_radio_caps *caps = list->pdata[i];

		if (ril_radio_caps_ready(caps) &&
					caps->simcard->status->card_state !=
						RIL_CARDSTATE_PRESENT) {
			found = g_slist_append(found, caps);
		}
	}

	return found;
}

/**
 * There could be no capability mismatch but LTE could be enabled for
 * the slot that has no SIM card in it. That's a waste, fix it.
 */
static gboolean ril_radio_caps_manager_upgrade_caps
					(struct ril_radio_caps_manager *self)
{
	gboolean upgrading = FALSE;
	GSList *upgradable = ril_radio_caps_manager_upgradable_slots(self);

	if (upgradable) {
		GSList *empty = ril_radio_caps_manager_empty_slots(self);

		if (empty) {
			struct ril_radio_caps *dest = upgradable->data;
			struct ril_radio_caps *src = empty->data;

			if (ril_radio_caps_access_mode(src) >
					ril_radio_caps_access_mode(dest)) {

				DBG("%d <-> %d", ril_radio_caps_index(src),
						ril_radio_caps_index(dest));
				src->old_cap = src->cap;
				src->new_cap = dest->cap;
				dest->old_cap = dest->cap;
				dest->new_cap = src->cap;
				ril_radio_caps_manager_start_transaction(self);
				upgrading = TRUE;
			}
			g_slist_free(empty);
		}
		g_slist_free(upgradable);
	}

	return upgrading;
}

static void ril_radio_caps_manager_check(struct ril_radio_caps_manager *self)
{
	DBG("");
	if (ril_radio_caps_manager_can_check(self)) {
		const int first = ril_radio_caps_manager_first_mismatch(self);

		if (first >= 0) {
			if (ril_radio_caps_manager_update_caps(self, first)) {
				ril_radio_caps_manager_start_transaction(self);
			}
		} else if (!ril_radio_caps_manager_upgrade_caps(self)) {
			DBG("nothing to do");
		}
	}
}

static gboolean ril_radio_caps_manager_check_cb(gpointer data)
{
	struct ril_radio_caps_manager *self = RADIO_CAPS_MANAGER(data);

	GASSERT(self->check_id);
	self->check_id = 0;
	ril_radio_caps_manager_check(self);
	return G_SOURCE_REMOVE;
}

static void ril_radio_caps_manager_recheck_later
					(struct ril_radio_caps_manager *self)
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

static void ril_radio_caps_manager_schedule_check
					(struct ril_radio_caps_manager *self)
{
	if (!self->check_id && !ril_radio_caps_manager_tx_pending(self)) {
		self->check_id = g_idle_add(ril_radio_caps_manager_check_cb,
									self);
	}
}

gulong ril_radio_caps_manager_add_aborted_handler
				(struct ril_radio_caps_manager *self,
				ril_radio_caps_manager_cb_t cb, void *arg)
{
	return (G_LIKELY(self) && G_LIKELY(cb)) ? g_signal_connect(self,
		SIGNAL_ABORTED_NAME, G_CALLBACK(cb), arg) : 0;
}

void ril_radio_caps_manager_remove_handler(struct ril_radio_caps_manager *self,
								gulong id)
{
	if (G_LIKELY(self) && G_LIKELY(id)) {
		g_signal_handler_disconnect(self, id);
	}
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
		g_object_ref(RADIO_CAPS_MANAGER(self));
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
	self->tx_phase_index = -1;
}

static void ril_radio_caps_manager_finalize(GObject *object)
{
	RilRadioCapsManager *self = RADIO_CAPS_MANAGER(object);

	GASSERT(!self->caps_list->len);
	g_ptr_array_free(self->caps_list, TRUE);
	if (self->check_id) {
		g_source_remove(self->check_id);
	}
	ril_data_manager_unref(self->data_manager);
	G_OBJECT_CLASS(ril_radio_caps_manager_parent_class)->finalize(object);
}

static void ril_radio_caps_manager_class_init(RilRadioCapsManagerClass *klass)
{
	G_OBJECT_CLASS(klass)->finalize = ril_radio_caps_manager_finalize;
	ril_radio_caps_manager_signals[SIGNAL_ABORTED] =
		g_signal_new(SIGNAL_ABORTED_NAME,
			G_OBJECT_CLASS_TYPE(klass), G_SIGNAL_RUN_FIRST,
			0, NULL, NULL, NULL, G_TYPE_NONE, 0);
}

/*
 * Local Variables:
 * mode: C
 * c-basic-offset: 8
 * indent-tabs-mode: t
 * End:
 */
