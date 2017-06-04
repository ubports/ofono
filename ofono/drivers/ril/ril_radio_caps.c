/*
 *  oFono - Open Source Telephony - RIL-based devices
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

#include "ril_radio_caps.h"
#include "ril_radio.h"
#include "ril_network.h"
#include "ril_sim_settings.h"
#include "ril_data.h"
#include "ril_log.h"

#include <grilio_queue.h>
#include <grilio_channel.h>
#include <grilio_parser.h>
#include <grilio_request.h>

#define SET_CAPS_TIMEOUT_MS (5*1000)
#define GET_CAPS_TIMEOUT_MS (5*1000)
#define GET_CAPS_RETRIES 60

/*
 * This code is doing something similar to what
 * com.android.internal.telephony.ProxyController
 * is doing.
 */

struct ril_radio_caps {
	gint ref_count;
	guint slot;
	char *log_prefix;
	GRilIoQueue *q;
	GRilIoChannel *io;
	gulong pref_mode_event_id;
	gulong max_pref_mode_event_id;
	gulong radio_event_id;
	gulong ril_event_id;
	int tx_id;
	struct ril_radio *radio;
	struct ril_network *network;
	struct ril_radio_caps_manager *mgr;
	struct ril_radio_capability cap;
	struct ril_radio_capability old_cap;
	struct ril_radio_capability new_cap;
};

struct ril_radio_caps_manager {
	gint ref_count;
	GPtrArray *caps_list;
	guint check_id;
	int tx_pending;
	int tx_id;
	int tx_phase_index;
	gboolean tx_failed;
	struct ril_data_manager *data_manager;
};

struct ril_radio_caps_check_data {
	ril_radio_caps_check_cb cb;
	void *data;
};

struct ril_radio_caps_request_tx_phase {
	const char *name;
	enum ril_radio_capability_phase phase;
	enum ril_radio_capability_status status;
	gboolean send_new_cap;
};

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

static void ril_radio_caps_check_done(GRilIoChannel* io, int ril_status,
				const void* data, guint len, void* user_data)
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

static gboolean ril_radio_caps_check_retry(GRilIoRequest* request,
		int ril_status, const void* resp, guint len, void* user_data)
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

guint ril_radio_caps_check(GRilIoChannel *io, ril_radio_caps_check_cb cb,
								void *data)
{
	guint id;
	GRilIoRequest *req = grilio_request_new();
	struct ril_radio_caps_check_data *check =
		g_new0(struct ril_radio_caps_check_data, 1);

	check->cb = cb;
	check->data = data;

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

static gboolean ril_radio_caps_pref_mode_limit
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

static gboolean ril_radio_caps_ok(const struct ril_radio_caps *caps,
			const enum ofono_radio_access_mode limit)
{
	return caps->radio->state != RADIO_STATE_ON ||
				limit == OFONO_RADIO_ACCESS_MODE_ANY ||
				ril_radio_caps_access_mode(caps) <= limit;
}

static void ril_radio_caps_radio_event(struct ril_radio *radio, void *arg)
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
	self->ril_event_id = grilio_channel_add_unsol_event_handler(self->io,
		ril_radio_caps_changed_cb, RIL_UNSOL_RADIO_CAPABILITY, self);

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
	ril_sim_settings_remove_handler(settings, self->pref_mode_event_id);
	ril_radio_remove_handler(self->radio, self->radio_event_id);
	g_ptr_array_remove(mgr->caps_list, self);
	ril_radio_caps_manager_unref(mgr);
	grilio_queue_cancel_all(self->q, FALSE);
	grilio_queue_unref(self->q);
	grilio_channel_remove_handlers(self->io, &self->ril_event_id, 1);
	grilio_channel_unref(self->io);
	ril_radio_unref(self->radio);
	ril_network_unref(self->network);
	g_free(self->log_prefix);
	g_slice_free(struct ril_radio_caps, self);
}

struct ril_radio_caps *ril_radio_caps_new(struct ril_radio_caps_manager *mgr,
		const char *log_prefix, GRilIoChannel *io,
		struct ril_radio *radio, struct ril_network *network,
		const struct ril_slot_config *config,
		const struct ril_radio_capability *cap)
{
	GASSERT(mgr);
	if (G_LIKELY(mgr)) {
		struct ril_sim_settings *settings = network->settings;
		struct ril_radio_caps *self =
			g_slice_new0(struct ril_radio_caps);

		self->ref_count = 1;
		self->slot = config->slot;
		self->log_prefix = (log_prefix && log_prefix[0]) ?
			g_strconcat(log_prefix, " ", NULL) : g_strdup("");

		self->q = grilio_queue_new(io);
		self->io = grilio_channel_ref(io);
		self->mgr = ril_radio_caps_manager_ref(mgr);

		self->radio = ril_radio_ref(radio);
		self->radio_event_id = ril_radio_add_state_changed_handler(
				radio, ril_radio_caps_radio_event, self);

		self->network = ril_network_ref(network);
		self->pref_mode_event_id =
			ril_sim_settings_add_pref_mode_changed_handler(
				settings, ril_radio_caps_settings_event, self);
		self->max_pref_mode_event_id =
			ril_network_add_max_pref_mode_changed_handler(
				network, ril_radio_caps_network_event, self);

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

/**
 * Checks that all radio caps have been initialized (i.e. all the initial
 * GET_RADIO_CAPABILITY requests have completed) and there's no transaction
 * in progress.
 */
static gboolean ril_radio_caps_manager_ready
				(struct ril_radio_caps_manager *self)
{
	if (self->caps_list && !self->tx_pending) {
		const GPtrArray *list = self->caps_list;
		guint i;

		for (i = 0; i < list->len; i++) {
			const struct ril_radio_caps *caps = list->pdata[i];

			if (caps->radio->state == RADIO_STATE_ON &&
						!caps->cap.rat) {
				DBG_(caps, "not ready");
				return FALSE;
			}

			DBG_(caps, "radio=%s,raf=0x%x(%s),uuid=%s,limit=%s",
				(caps->radio->state == RADIO_STATE_ON) ?
				"on" : "off", caps->cap.rat,
				ofono_radio_access_mode_to_string
				(ril_radio_caps_access_mode(caps)),
				caps->cap.logicalModemUuid,
				ofono_radio_access_mode_to_string
				(ril_radio_caps_pref_mode_limit(caps)));
		}
		return TRUE;
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

	DBG("nothing to do");
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
		done[i] = (caps->radio->state != RADIO_STATE_ON);
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

			/* Count it */
			self->tx_pending++;
		}
	}
}

static void ril_radio_caps_manager_next_transaction
					(struct ril_radio_caps_manager *self)
{
	guint i;
	const GPtrArray *list = self->caps_list;

	for (i = 0; i < list->len; i++) {
		struct ril_radio_caps *caps = list->pdata[i];

		grilio_queue_cancel_all(caps->q, FALSE);
	}

	self->tx_pending = 0;
	self->tx_failed = FALSE;
	self->tx_phase_index = -1;
	self->tx_id++;
	if (self->tx_id <= 0) self->tx_id = 1;
}

static void ril_radio_caps_manager_abort_cb(GRilIoChannel *io,
		int ril_status, const void *data, guint len, void *user_data)
{
	struct ril_radio_caps *caps = user_data;
	struct ril_radio_caps_manager *self = caps->mgr;

	GASSERT(self->tx_pending > 0);
	if (!(--self->tx_pending)) {
		DBG("transaction aborted");
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
}

static void ril_radio_caps_manager_next_phase_cb(GRilIoChannel *io,
		int ril_status, const void *data, guint len, void *user_data)
{
	struct ril_radio_caps *caps = user_data;
	struct ril_radio_caps_manager *self = caps->mgr;
	gboolean ok = FALSE;

	GASSERT(self->tx_pending > 0);
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

	if (!(--self->tx_pending)) {
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

	GASSERT(!self->tx_pending);
	if (self->tx_phase_index >= max_index) {
		guint i;
		const GPtrArray *list = self->caps_list;


		DBG("transaction %d is done", self->tx_id);
		ril_radio_caps_manager_schedule_check(self);
		ril_data_manager_assert_data_on(self->data_manager);
		for (i = 0; i < list->len; i++) {
			struct ril_radio_caps *caps = list->pdata[i];
			ril_network_assert_pref_mode(caps->network, FALSE);
		}
	} else {
		const struct ril_radio_caps_request_tx_phase *phase =
				ril_radio_caps_tx_phase +
				(++self->tx_phase_index);

		ril_radio_caps_manager_issue_requests(self, phase,
					ril_radio_caps_manager_next_phase_cb);
	}
}

static void ril_radio_caps_manager_check(struct ril_radio_caps_manager *self)
{
	DBG("");
	if (ril_radio_caps_manager_ready(self)) {
		const int first = ril_radio_caps_manager_first_mismatch(self);

		if (first >= 0 &&
			ril_radio_caps_manager_update_caps(self, first)) {
			guint i;
			const GPtrArray *list = self->caps_list;

			/* Start the new request transaction */
			ril_radio_caps_manager_next_transaction(self);
			DBG("new transaction %d", self->tx_id);

			/* Ignore the modems that are powered off */
			for (i = 0; i < list->len; i++) {
				struct ril_radio_caps *caps = list->pdata[i];

				if (caps->radio->state == RADIO_STATE_ON) {
					/* Associate it with the transaction */
					caps->tx_id = self->tx_id;
				}
			}

			ril_radio_caps_manager_next_phase(self);
		}
	}
}

static gboolean ril_radio_caps_manager_check_cb(gpointer user_data)
{
	struct ril_radio_caps_manager *self = user_data;

	GASSERT(self->check_id);
	self->check_id = 0;
	ril_radio_caps_manager_check(self);
	return G_SOURCE_REMOVE;
}

static void ril_radio_caps_manager_schedule_check
					(struct ril_radio_caps_manager *self)
{
	if (!self->check_id && !self->tx_pending) {
		self->check_id = g_idle_add(ril_radio_caps_manager_check_cb,
									self);
	}
}

static void ril_radio_caps_manager_free(struct ril_radio_caps_manager *self)
{
	GASSERT(!self->caps_list->len);
	g_ptr_array_free(self->caps_list, TRUE);
	if (self->check_id) {
		g_source_remove(self->check_id);
	}
	ril_data_manager_unref(self->data_manager);
	g_slice_free(struct ril_radio_caps_manager, self);
}

struct ril_radio_caps_manager *ril_radio_caps_manager_new
						(struct ril_data_manager *dm)
{
	struct ril_radio_caps_manager *self =
		g_slice_new0(struct ril_radio_caps_manager);

	self->ref_count = 1;
	self->caps_list = g_ptr_array_new();
	self->tx_phase_index = -1;
	self->data_manager = ril_data_manager_ref(dm);
	return self;
}

struct ril_radio_caps_manager *ril_radio_caps_manager_ref
					(struct ril_radio_caps_manager *self)
{
	if (G_LIKELY(self)) {
		GASSERT(self->ref_count > 0);
		g_atomic_int_inc(&self->ref_count);
	}
	return self;
}

void ril_radio_caps_manager_unref(struct ril_radio_caps_manager *self)
{
	if (G_LIKELY(self)) {
		GASSERT(self->ref_count > 0);
		if (g_atomic_int_dec_and_test(&self->ref_count)) {
			ril_radio_caps_manager_free(self);
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
