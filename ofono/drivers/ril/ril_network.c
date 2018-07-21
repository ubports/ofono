/*
 *  oFono - Open Source Telephony - RIL-based devices
 *
 *  Copyright (C) 2015-2018 Jolla Ltd.
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

#include "ril_network.h"
#include "ril_radio.h"
#include "ril_sim_card.h"
#include "ril_sim_settings.h"
#include "ril_util.h"
#include "ril_log.h"

#include <grilio_queue.h>
#include <grilio_request.h>
#include <grilio_parser.h>

#include <gutil_misc.h>

#include <ofono/netreg.h>

#include "common.h"

#define SET_PREF_MODE_HOLDOFF_SEC RIL_RETRY_SECS

typedef GObjectClass RilNetworkClass;
typedef struct ril_network RilNetwork;

enum ril_network_timer {
	TIMER_SET_RAT_HOLDOFF,
	TIMER_FORCE_CHECK_PREF_MODE,
	TIMER_COUNT
};

enum ril_network_radio_event {
	RADIO_EVENT_STATE_CHANGED,
	RADIO_EVENT_ONLINE_CHANGED,
	RADIO_EVENT_COUNT
};

enum ril_network_sim_events {
	SIM_EVENT_STATUS_CHANGED,
	SIM_EVENT_IO_ACTIVE_CHANGED,
	SIM_EVENT_COUNT
};

enum ril_network_unsol_event {
	UNSOL_EVENT_NETWORK_STATE,
	UNSOL_EVENT_RADIO_CAPABILITY,
	UNSOL_EVENT_COUNT
};

struct ril_network_priv {
	GRilIoChannel *io;
	GRilIoQueue *q;
	struct ril_radio *radio;
	struct ril_sim_card *simcard;
	int rat;
	int lte_network_mode;
	int network_mode_timeout;
	char *log_prefix;
	guint operator_poll_id;
	guint voice_poll_id;
	guint data_poll_id;
	guint timer[TIMER_COUNT];
	gulong query_rat_id;
	gulong set_rat_id;
	gulong unsol_event_id[UNSOL_EVENT_COUNT];
	gulong settings_event_id;
	gulong radio_event_id[RADIO_EVENT_COUNT];
	gulong simcard_event_id[SIM_EVENT_COUNT];
	struct ofono_network_operator operator;
	gboolean assert_rat;
};

enum ril_network_signal {
	SIGNAL_OPERATOR_CHANGED,
	SIGNAL_VOICE_STATE_CHANGED,
	SIGNAL_DATA_STATE_CHANGED,
	SIGNAL_PREF_MODE_CHANGED,
	SIGNAL_MAX_PREF_MODE_CHANGED,
	SIGNAL_COUNT
};

#define SIGNAL_OPERATOR_CHANGED_NAME      "ril-network-operator-changed"
#define SIGNAL_VOICE_STATE_CHANGED_NAME   "ril-network-voice-state-changed"
#define SIGNAL_DATA_STATE_CHANGED_NAME    "ril-network-data-state-changed"
#define SIGNAL_PREF_MODE_CHANGED_NAME     "ril-network-pref-mode-changed"
#define SIGNAL_MAX_PREF_MODE_CHANGED_NAME "ril-network-max-pref-mode-changed"

static guint ril_network_signals[SIGNAL_COUNT] = { 0 };

G_DEFINE_TYPE(RilNetwork, ril_network, G_TYPE_OBJECT)
#define RIL_NETWORK_TYPE (ril_network_get_type())
#define RIL_NETWORK(obj) (G_TYPE_CHECK_INSTANCE_CAST(obj,\
        RIL_NETWORK_TYPE,RilNetwork))

#define RIL_NETWORK_SIGNAL(klass,name) \
	ril_network_signals[SIGNAL_##name##_CHANGED] = \
		g_signal_new(SIGNAL_##name##_CHANGED_NAME, \
			G_OBJECT_CLASS_TYPE(klass), G_SIGNAL_RUN_FIRST, \
			0, NULL, NULL, NULL, G_TYPE_NONE, 0)

#define DBG_(self,fmt,args...) DBG("%s" fmt, (self)->priv->log_prefix, ##args)

/* Some assumptions: */
G_STATIC_ASSERT(OFONO_RADIO_ACCESS_MODE_ANY == 0);
G_STATIC_ASSERT(OFONO_RADIO_ACCESS_MODE_GSM > OFONO_RADIO_ACCESS_MODE_ANY);
G_STATIC_ASSERT(OFONO_RADIO_ACCESS_MODE_UMTS > OFONO_RADIO_ACCESS_MODE_GSM);
G_STATIC_ASSERT(OFONO_RADIO_ACCESS_MODE_LTE > OFONO_RADIO_ACCESS_MODE_UMTS);

static void ril_network_query_pref_mode(struct ril_network *self);
static void ril_network_check_pref_mode(struct ril_network *self,
							gboolean immediate);

static void ril_network_emit(struct ril_network *self,
						enum ril_network_signal sig)
{
	g_signal_emit(self, ril_network_signals[sig], 0);
}

static void ril_network_stop_timer(struct ril_network *self,
					enum ril_network_timer tid)
{
	struct ril_network_priv *priv = self->priv;

	if (priv->timer[tid]) {
		g_source_remove(priv->timer[tid]);
		priv->timer[tid] = 0;
	}
}

static void ril_network_reset_state(struct ril_registration_state *reg)
{
	memset(reg, 0, sizeof(*reg));
	reg->status = NETWORK_REGISTRATION_STATUS_UNKNOWN;
	reg->access_tech = -1;
	reg->ril_tech = -1;
	reg->lac = -1;
	reg->ci = -1;
}

static gboolean ril_network_parse_response(struct ril_network *self,
	const void *data, guint len, struct ril_registration_state *reg)
{
	int nparams, ril_status;
	gchar *sstatus = NULL, *slac = NULL, *sci = NULL;
	gchar *stech = NULL, *sreason = NULL, *smax = NULL;
	GRilIoParser rilp;

	ril_network_reset_state(reg);

	/* Size of response string array. The minimum seen in the wild is 3 */
	grilio_parser_init(&rilp, data, len);
	if (!grilio_parser_get_int32(&rilp, &nparams) || nparams < 3) {
		DBG_(self, "broken response");
		return FALSE;
	}

	sstatus = grilio_parser_get_utf8(&rilp);              /* response[0] */
	if (!sstatus) {
		DBG_(self, "No sstatus value returned!");
		return FALSE;
	}

	slac = grilio_parser_get_utf8(&rilp);                 /* response[1] */
	sci = grilio_parser_get_utf8(&rilp);                  /* response[2] */

	if (nparams > 3) {
		stech = grilio_parser_get_utf8(&rilp);        /* response[3] */
	}

	ril_status = atoi(sstatus);
	if (ril_status > 10) {
		reg->status = ril_status - 10;
	} else {
		reg->status = ril_status;
	}

	/* FIXME: need to review VOICE_REGISTRATION response
	 * as it returns up to 15 parameters ( vs. 6 for DATA ).
	 *
	 * The first four parameters are the same for both
	 * responses ( although status includes values for
	 * emergency calls for VOICE response ).
	 *
	 * Parameters 5 & 6 have different meanings for
	 * voice & data response.
	 */
	if (nparams > 4) {
		/* TODO: different use for CDMA */
		sreason = grilio_parser_get_utf8(&rilp);      /* response[4] */
		if (nparams > 5) {
			/* TODO: different use for CDMA */
			smax = grilio_parser_get_utf8(&rilp); /* response[5] */
			if (smax) {
				reg->max_calls = atoi(smax);
			}
		}
	}

	/*
	 * Some older RILs don't provide max calls, in that case let's
	 * supply some reasonable default. We don't need more than 2
	 * simultaneous data calls anyway.
	 */
	if (reg->max_calls < 1) {
		reg->max_calls = 2;
	}

	if (!gutil_parse_int(slac, 16, &reg->lac)) {
		reg->lac = -1;
	}

	if (!gutil_parse_int(sci, 16, &reg->ci)) {
		reg->ci = -1;
	}

	reg->access_tech = ril_parse_tech(stech, &reg->ril_tech);

	DBG_(self, "%s,%s,%s,%d,%s,%s,%s",
				registration_status_to_string(reg->status),
				slac, sci, reg->ril_tech,
				registration_tech_to_string(reg->access_tech),
				sreason, smax);

	g_free(sstatus);
	g_free(slac);
	g_free(sci);
	g_free(stech);
	g_free(sreason);
	g_free(smax);
	return TRUE;
}

static void ril_network_op_copy(struct ofono_network_operator *dest,
				const struct ofono_network_operator *src)
{
	strncpy(dest->mcc, src->mcc, sizeof(dest->mcc));
	strncpy(dest->mnc, src->mnc, sizeof(dest->mnc));
	strncpy(dest->name, src->name, sizeof(dest->name));
	dest->mcc[sizeof(dest->mcc)-1] = 0;
	dest->mnc[sizeof(dest->mnc)-1] = 0;
	dest->name[sizeof(dest->name)-1] = 0;
	dest->status = src->status;
	dest->tech = src->tech;
}

static gboolean ril_network_op_equal(const struct ofono_network_operator *op1,
				const struct ofono_network_operator *op2)
{
	if (op1 == op2) {
		return TRUE;
	} else if (!op1 || !op2) {
		return FALSE;
	} else {
		return op1->status == op2->status &&
			op1->tech == op2->tech &&
			!strncmp(op1->mcc, op2->mcc, sizeof(op2->mcc)) &&
			!strncmp(op1->mnc, op2->mnc, sizeof(op2->mnc)) &&
			!strncmp(op1->name, op2->name, sizeof(op2->name));
	}
}

static void ril_network_poll_operator_cb(GRilIoChannel *io, int req_status,
				const void *data, guint len, void *user_data)
{
	struct ril_network *self = RIL_NETWORK(user_data);
	struct ril_network_priv *priv = self->priv;

	GASSERT(priv->operator_poll_id);
	priv->operator_poll_id = 0;

	if (req_status == RIL_E_SUCCESS) {
		struct ofono_network_operator op;
		gboolean changed = FALSE;
		gchar *lalpha;
		char *salpha;
		char *numeric;
		GRilIoParser rilp;

		grilio_parser_init(&rilp, data, len);
		grilio_parser_get_int32(&rilp, NULL);
		lalpha = grilio_parser_get_utf8(&rilp);
		salpha = grilio_parser_get_utf8(&rilp);
		numeric = grilio_parser_get_utf8(&rilp);

		op.tech = -1;
		if (ril_parse_mcc_mnc(numeric, &op)) {
			if (op.tech < 0) op.tech = self->voice.access_tech;
			op.status = OPERATOR_STATUS_CURRENT;
			op.name[0] = 0;
			if (lalpha) {
				strncpy(op.name, lalpha, sizeof(op.name));
			} else if (salpha) {
				strncpy(op.name, salpha, sizeof(op.name));
			} else {
				strncpy(op.name, numeric, sizeof(op.name));
			}
			op.name[sizeof(op.name)-1] = 0;
			if (!self->operator) {
				self->operator = &priv->operator;
				ril_network_op_copy(&priv->operator, &op);
				changed = TRUE;
			} else if (!ril_network_op_equal(&op, &priv->operator)) {
				ril_network_op_copy(&priv->operator, &op);
				changed = TRUE;
			}
		} else if (self->operator) {
			self->operator = NULL;
			changed = TRUE;
		}

		if (changed) {
			if (self->operator) {
				DBG_(self, "lalpha=%s, salpha=%s, numeric=%s, "
					"%s, mcc=%s, mnc=%s, %s",
					lalpha, salpha, numeric,
					op.name, op.mcc, op.mnc,
					registration_tech_to_string(op.tech));
			} else {
				DBG_(self, "no operator");
			}
			ril_network_emit(self, SIGNAL_OPERATOR_CHANGED);
		}

		g_free(lalpha);
		g_free(salpha);
		g_free(numeric);
	}
}

static void ril_network_poll_voice_state_cb(GRilIoChannel *io, int req_status,
				const void *data, guint len, void *user_data)
{
	struct ril_network *self = RIL_NETWORK(user_data);
	struct ril_network_priv *priv = self->priv;

	GASSERT(priv->voice_poll_id);
	priv->voice_poll_id = 0;

	if (req_status == RIL_E_SUCCESS) {
		struct ril_registration_state state;

		ril_network_parse_response(self, data, len, &state);
		if (memcmp(&state, &self->voice, sizeof(state))) {
			DBG_(self, "voice registration changed");
			self->voice = state;
			ril_network_emit(self, SIGNAL_VOICE_STATE_CHANGED);
		}
	}
}

static void ril_network_poll_data_state_cb(GRilIoChannel *io, int req_status,
				const void *data, guint len, void *user_data)
{
	struct ril_network *self = RIL_NETWORK(user_data);
	struct ril_network_priv *priv = self->priv;

	GASSERT(priv->data_poll_id);
	priv->data_poll_id = 0;

	if (req_status == RIL_E_SUCCESS) {
		struct ril_registration_state state;

		ril_network_parse_response(self, data, len, &state);
		if (memcmp(&state, &self->data, sizeof(state))) {
			DBG_(self, "data registration changed");
			self->data = state;
			ril_network_emit(self, SIGNAL_DATA_STATE_CHANGED);
		}
	}
}

static guint ril_network_poll_and_retry(struct ril_network *self, guint id,
					int code, GRilIoChannelResponseFunc fn)
{
	struct ril_network_priv *priv = self->priv;

	if (id) {
		/* Retry right away, don't wait for retry timeout to expire */
		grilio_channel_retry_request(priv->io, id);
	} else {
		GRilIoRequest *req = grilio_request_new();

		grilio_request_set_retry(req, RIL_RETRY_SECS*1000, -1);
		id = grilio_queue_send_request_full(priv->q, req, code, fn,
								NULL, self);
		grilio_request_unref(req);
	}

	return id;
}

static void ril_network_poll_state(struct ril_network *self)
{
	struct ril_network_priv *priv = self->priv;

	DBG_(self, "");
	priv->operator_poll_id = ril_network_poll_and_retry(self,
		priv->operator_poll_id, RIL_REQUEST_OPERATOR,
		ril_network_poll_operator_cb);

	ril_network_query_registration_state(self);
}

void ril_network_query_registration_state(struct ril_network *self)
{
	if (self) {
		struct ril_network_priv *priv = self->priv;

		DBG_(self, "");
		priv->voice_poll_id = ril_network_poll_and_retry(self,
			priv->voice_poll_id,
			RIL_REQUEST_VOICE_REGISTRATION_STATE,
			ril_network_poll_voice_state_cb);
		priv->data_poll_id = ril_network_poll_and_retry(self,
			priv->data_poll_id,
			RIL_REQUEST_DATA_REGISTRATION_STATE,
			ril_network_poll_data_state_cb);
	}
}

static enum ofono_radio_access_mode ril_network_rat_to_mode(int rat)
{
	switch (rat) {
	case PREF_NET_TYPE_LTE_CDMA_EVDO:
	case PREF_NET_TYPE_LTE_GSM_WCDMA:
	case PREF_NET_TYPE_LTE_CMDA_EVDO_GSM_WCDMA:
	case PREF_NET_TYPE_LTE_ONLY:
	case PREF_NET_TYPE_LTE_WCDMA:
		return OFONO_RADIO_ACCESS_MODE_LTE;
	case PREF_NET_TYPE_GSM_WCDMA_AUTO:
	case PREF_NET_TYPE_WCDMA:
	case PREF_NET_TYPE_GSM_WCDMA:
		return OFONO_RADIO_ACCESS_MODE_UMTS;
	default:
		DBG("unexpected rat mode %d", rat);
	case PREF_NET_TYPE_GSM_ONLY:
		return OFONO_RADIO_ACCESS_MODE_GSM;
	}
}

static int ril_network_mode_to_rat(struct ril_network *self,
					enum ofono_radio_access_mode mode)
{
	switch (mode) {
	case OFONO_RADIO_ACCESS_MODE_ANY:
	case OFONO_RADIO_ACCESS_MODE_LTE:
		if (self->settings->techs & OFONO_RADIO_ACCESS_MODE_LTE) {
			return self->priv->lte_network_mode;
		}
		/* no break */
	default:
	case OFONO_RADIO_ACCESS_MODE_UMTS:
		if (self->settings->techs & OFONO_RADIO_ACCESS_MODE_UMTS) {
			return PREF_NET_TYPE_GSM_WCDMA_AUTO;
		}
		/* no break */
	case OFONO_RADIO_ACCESS_MODE_GSM:
		return PREF_NET_TYPE_GSM_ONLY;
	}
}

static int ril_network_pref_mode_expected(struct ril_network *self)
{
	struct ril_sim_settings *settings = self->settings;
	struct ril_network_priv *priv = self->priv;

	/*
	 * On dual-SIM phones such as Jolla C only one slot at a time
	 * is allowed to use LTE. Even if the slot which has been using
	 * LTE gets powered off, we still need to explicitely set its
	 * preferred mode to GSM, to make LTE machinery available to
	 * the other slot. This sort of behaviour might not be necessary
	 * on some hardware and can (should) be made configurable when
	 * it becomes necessary.
	 */
	const enum ofono_radio_access_mode max_pref_mode =
		(priv->radio->state == RADIO_STATE_ON) ? self->max_pref_mode :
		OFONO_RADIO_ACCESS_MODE_GSM;

	/*
	 * OFONO_RADIO_ACCESS_MODE_ANY is zero. If both pref_mode
	 * and max_pref_mode are not ANY, we pick the smallest value.
	 * Otherwise we take any non-zero value if there is one.
	 */
	const enum ofono_radio_access_mode pref_mode =
		(settings->pref_mode && max_pref_mode) ?
		MIN(settings->pref_mode, max_pref_mode) :
		settings->pref_mode ? settings->pref_mode :
		max_pref_mode;
	return ril_network_mode_to_rat(self, pref_mode);
}

static gboolean ril_network_can_set_pref_mode(struct ril_network *self)
{
	struct ril_network_priv *priv = self->priv;

	/*
	 * With some modems an attempt to set rat significantly slows
	 * down SIM I/O, let's avoid that.
	 */
	return priv->radio->online && ril_sim_card_ready(priv->simcard) &&
				!priv->simcard->sim_io_active &&
				!priv->timer[TIMER_SET_RAT_HOLDOFF] ;
}

static gboolean ril_network_set_rat_holdoff_cb(gpointer user_data)
{
	struct ril_network *self = RIL_NETWORK(user_data);
	struct ril_network_priv *priv = self->priv;

	GASSERT(priv->timer[TIMER_SET_RAT_HOLDOFF]);
	priv->timer[TIMER_SET_RAT_HOLDOFF] = 0;

	ril_network_check_pref_mode(self, FALSE);
	return G_SOURCE_REMOVE;
}

static void ril_network_set_rat_cb(GRilIoChannel *io, int status,
				const void *data, guint len, void *user_data)
{
	struct ril_network *self = RIL_NETWORK(user_data);
	struct ril_network_priv *priv = self->priv;

	GASSERT(priv->set_rat_id);
	priv->set_rat_id = 0;
	if (status != RIL_E_SUCCESS) {
		ofono_error("failed to set rat mode");
	}

	ril_network_query_pref_mode(self);
}

static void ril_network_set_rat(struct ril_network *self, int rat)
{
	struct ril_network_priv *priv = self->priv;

	if (!priv->set_rat_id && priv->radio->online &&
		ril_sim_card_ready(priv->simcard) &&
		/*
		 * With some modems an attempt to set rat significantly
		 * slows down SIM I/O, let's avoid that.
		 */
		!priv->simcard->sim_io_active &&
		!priv->timer[TIMER_SET_RAT_HOLDOFF]) {
		GRilIoRequest *req = grilio_request_sized_new(8);

		DBG_(self, "setting rat mode %d", rat);
		grilio_request_append_int32(req, 1);   /* count */
		grilio_request_append_int32(req, rat);

		grilio_request_set_timeout(req, priv->network_mode_timeout);
		priv->set_rat_id = grilio_queue_send_request_full(priv->q, req,
					RIL_REQUEST_SET_PREFERRED_NETWORK_TYPE,
					ril_network_set_rat_cb, NULL, self);
		grilio_request_unref(req);

		/* We have submitted the request, clear the assertion flag */
		priv->assert_rat = FALSE;

		/* And don't do it too often */
		priv->timer[TIMER_SET_RAT_HOLDOFF] =
			g_timeout_add_seconds(SET_PREF_MODE_HOLDOFF_SEC,
				ril_network_set_rat_holdoff_cb, self);
	} else {
		DBG_(self, "need to set rat mode %d", rat);
	}
}

static void ril_network_set_pref_mode(struct ril_network *self, int rat)
{
	struct ril_network_priv *priv = self->priv;

	if (priv->rat != rat || priv->assert_rat) {
		ril_network_set_rat(self, rat);
	}
}

static void ril_network_check_pref_mode(struct ril_network *self,
							gboolean immediate)
{
	struct ril_network_priv *priv = self->priv;
	const int rat = ril_network_pref_mode_expected(self);

	if (priv->timer[TIMER_FORCE_CHECK_PREF_MODE]) {
		ril_network_stop_timer(self, TIMER_FORCE_CHECK_PREF_MODE);
		/*
		 * TIMER_FORCE_CHECK_PREF_MODE is scheduled by
		 * ril_network_pref_mode_changed_cb and is meant
		 * to force radio tech check right now.
		 */
		immediate = TRUE;
	}

	if (priv->rat != rat) {
		DBG_(self, "rat mode %d, expected %d", priv->rat, rat);
	}

	if (immediate) {
		ril_network_stop_timer(self, TIMER_SET_RAT_HOLDOFF);
	}

	if (priv->rat != rat || priv->assert_rat) {
		if (!priv->timer[TIMER_SET_RAT_HOLDOFF]) {
			ril_network_set_pref_mode(self, rat);
		} else {
			/* OK, later */
			DBG_(self, "need to set rat mode %d", rat);
		}
	}
}

static int ril_network_parse_pref_resp(const void *data, guint len)
{
	GRilIoParser rilp;
	int pref = -1;

	grilio_parser_init(&rilp, data, len);
	grilio_parser_get_int32(&rilp, NULL);
	grilio_parser_get_int32(&rilp, &pref);
	return pref;
}

static void ril_network_startup_query_pref_mode_cb(GRilIoChannel *io,
		int status, const void *data, guint len, void *user_data)
{
	if (status == RIL_E_SUCCESS) {
		struct ril_network *self = RIL_NETWORK(user_data);
		struct ril_network_priv *priv = self->priv;
		const enum ofono_radio_access_mode pref_mode = self->pref_mode;

		priv->rat = ril_network_parse_pref_resp(data, len);
		self->pref_mode = ril_network_rat_to_mode(priv->rat);
		DBG_(self, "rat mode %d (%s)", priv->rat,
			ofono_radio_access_mode_to_string(self->pref_mode));

		if (self->pref_mode != pref_mode) {
			ril_network_emit(self, SIGNAL_PREF_MODE_CHANGED);
		}

		/*
		 * Unlike ril_network_query_pref_mode_cb, this one always
		 * checks the preferred mode.
		 */
		ril_network_check_pref_mode(self, FALSE);
	}
}

static void ril_network_query_pref_mode_cb(GRilIoChannel *io, int status,
				const void *data, guint len, void *user_data)
{
	struct ril_network *self = RIL_NETWORK(user_data);
	struct ril_network_priv *priv = self->priv;
	const enum ofono_radio_access_mode pref_mode = self->pref_mode;

	/* This request never fails because in case of error it gets retried */
	GASSERT(status == RIL_E_SUCCESS);
	GASSERT(priv->query_rat_id);

	priv->query_rat_id = 0;
	priv->rat = ril_network_parse_pref_resp(data, len);
	self->pref_mode = ril_network_rat_to_mode(priv->rat);
	DBG_(self, "rat mode %d (%s)", priv->rat,
			ofono_radio_access_mode_to_string(self->pref_mode));

	if (self->pref_mode != pref_mode) {
		ril_network_emit(self, SIGNAL_PREF_MODE_CHANGED);
	}

	if (ril_network_can_set_pref_mode(self)) {
		ril_network_check_pref_mode(self, FALSE);
	}
}

static void ril_network_query_pref_mode(struct ril_network *self)
{
	struct ril_network_priv *priv = self->priv;
	GRilIoRequest *req = grilio_request_new();

	grilio_request_set_retry(req, RIL_RETRY_SECS*1000, -1);
	grilio_queue_cancel_request(priv->q, priv->query_rat_id, FALSE);
	priv->query_rat_id = grilio_queue_send_request_full(priv->q, req,
				RIL_REQUEST_GET_PREFERRED_NETWORK_TYPE,
				ril_network_query_pref_mode_cb, NULL, self);
	grilio_request_unref(req);
}

void ril_network_set_max_pref_mode(struct ril_network *self,
				enum ofono_radio_access_mode max_mode,
				gboolean force_check)
{
	if (self && (self->max_pref_mode != max_mode || force_check)) {
		if (self->max_pref_mode != max_mode) {
			DBG_(self, "rat mode %d (%s)", max_mode,
				ofono_radio_access_mode_to_string(max_mode));
			self->max_pref_mode = max_mode;
			ril_network_emit(self, SIGNAL_MAX_PREF_MODE_CHANGED);
		}
		ril_network_check_pref_mode(self, TRUE);
	}
}

void ril_network_assert_pref_mode(struct ril_network *self, gboolean immediate)
{
	struct ril_network_priv *priv = self->priv;

	priv->assert_rat = TRUE;
	ril_network_check_pref_mode(self, immediate);
}

gulong ril_network_add_operator_changed_handler(struct ril_network *self,
					ril_network_cb_t cb, void *arg)
{
	return (G_LIKELY(self) && G_LIKELY(cb)) ? g_signal_connect(self,
		SIGNAL_OPERATOR_CHANGED_NAME, G_CALLBACK(cb), arg) : 0;
}

gulong ril_network_add_voice_state_changed_handler(struct ril_network *self,
					ril_network_cb_t cb, void *arg)
{
	return (G_LIKELY(self) && G_LIKELY(cb)) ? g_signal_connect(self,
		SIGNAL_VOICE_STATE_CHANGED_NAME, G_CALLBACK(cb), arg) : 0;
}

gulong ril_network_add_data_state_changed_handler(struct ril_network *self,
					ril_network_cb_t cb, void *arg)
{
	return (G_LIKELY(self) && G_LIKELY(cb)) ? g_signal_connect(self,
		SIGNAL_DATA_STATE_CHANGED_NAME, G_CALLBACK(cb), arg) : 0;
}

gulong ril_network_add_pref_mode_changed_handler(struct ril_network *self,
					ril_network_cb_t cb, void *arg)
{
	return (G_LIKELY(self) && G_LIKELY(cb)) ? g_signal_connect(self,
		SIGNAL_PREF_MODE_CHANGED_NAME, G_CALLBACK(cb), arg) : 0;
}

gulong ril_network_add_max_pref_mode_changed_handler(struct ril_network *self,
					ril_network_cb_t cb, void *arg)
{
	return (G_LIKELY(self) && G_LIKELY(cb)) ? g_signal_connect(self,
		SIGNAL_MAX_PREF_MODE_CHANGED_NAME, G_CALLBACK(cb), arg) : 0;
}

void ril_network_remove_handler(struct ril_network *self, gulong id)
{
	if (G_LIKELY(self) && G_LIKELY(id)) {
		g_signal_handler_disconnect(self, id);
	}
}

void ril_network_remove_handlers(struct ril_network *self, gulong *ids, int n)
{
	gutil_disconnect_handlers(self, ids, n);
}

static void ril_network_state_changed_cb(GRilIoChannel *io, guint code,
				const void *data, guint len, void *user_data)
{
	struct ril_network *self = RIL_NETWORK(user_data);

	DBG_(self, "");
	GASSERT(code == RIL_UNSOL_RESPONSE_VOICE_NETWORK_STATE_CHANGED);
	ril_network_poll_state(self);
}

static void ril_network_radio_capability_changed_cb(GRilIoChannel *io,
		guint code, const void *data, guint len, void *user_data)
{
	struct ril_network *self = RIL_NETWORK(user_data);

	DBG_(self, "");
	GASSERT(code == RIL_UNSOL_RADIO_CAPABILITY);
	ril_network_assert_pref_mode(self, FALSE);
}

static void ril_network_radio_state_cb(struct ril_radio *radio, void *data)
{
	struct ril_network *self = RIL_NETWORK(data);

	ril_network_check_pref_mode(self, FALSE);
	if (radio->state == RADIO_STATE_ON) {
		ril_network_poll_state(self);
	}
}

static void ril_network_radio_online_cb(struct ril_radio *radio, void *data)
{
	struct ril_network *self = RIL_NETWORK(data);

	if (ril_network_can_set_pref_mode(self)) {
		ril_network_check_pref_mode(self, TRUE);
	}
}

static gboolean ril_network_check_pref_mode_cb(gpointer user_data)
{
	struct ril_network *self = RIL_NETWORK(user_data);
	struct ril_network_priv *priv = self->priv;

	GASSERT(priv->timer[TIMER_FORCE_CHECK_PREF_MODE]);
	priv->timer[TIMER_FORCE_CHECK_PREF_MODE] = 0;

	DBG_(self, "checking pref mode");
	ril_network_check_pref_mode(self, TRUE);

	return G_SOURCE_REMOVE;
}

static void ril_network_pref_mode_changed_cb(struct ril_sim_settings *settings,
							void *user_data)
{
	struct ril_network *self = RIL_NETWORK(user_data);
	struct ril_network_priv *priv = self->priv;

	/*
	 * Postpone ril_network_check_pref_mode because other pref_mode
	 * listeners (namely, ril_data) may want to tweak max_pref_mode
	 */
	if (!priv->timer[TIMER_FORCE_CHECK_PREF_MODE]) {
		DBG_(self, "scheduling pref mode check");
		priv->timer[TIMER_FORCE_CHECK_PREF_MODE] =
			g_idle_add(ril_network_check_pref_mode_cb, self);
	} else {
		DBG_(self, "pref mode check already scheduled");
	}
}

static void ril_network_sim_status_changed_cb(struct ril_sim_card *sc,
							void *user_data)
{
	struct ril_network *self = RIL_NETWORK(user_data);

	if (ril_network_can_set_pref_mode(self)) {
		ril_network_check_pref_mode(self, FALSE);
	}
}

struct ril_network *ril_network_new(const char *path, GRilIoChannel *io,
			const char *log_prefix, struct ril_radio *radio,
			struct ril_sim_card *simcard,
			struct ril_sim_settings *settings,
			const struct ril_slot_config *config)
{
	struct ril_network *self = g_object_new(RIL_NETWORK_TYPE, NULL);
	struct ril_network_priv *priv = self->priv;

	self->settings = ril_sim_settings_ref(settings);
	priv->io = grilio_channel_ref(io);
	priv->q = grilio_queue_new(priv->io);
	priv->radio = ril_radio_ref(radio);
	priv->simcard = ril_sim_card_ref(simcard);
	priv->log_prefix = (log_prefix && log_prefix[0]) ?
		g_strconcat(log_prefix, " ", NULL) : g_strdup("");
	DBG_(self, "");

	/* Copy relevant config values */
	priv->lte_network_mode = config->lte_network_mode;
	priv->network_mode_timeout = config->network_mode_timeout;

	/* Register listeners */
	priv->unsol_event_id[UNSOL_EVENT_NETWORK_STATE] =
		grilio_channel_add_unsol_event_handler(priv->io,
			ril_network_state_changed_cb,
			RIL_UNSOL_RESPONSE_VOICE_NETWORK_STATE_CHANGED, self);
	priv->unsol_event_id[UNSOL_EVENT_RADIO_CAPABILITY] =
		grilio_channel_add_unsol_event_handler(priv->io,
			ril_network_radio_capability_changed_cb,
			RIL_UNSOL_RADIO_CAPABILITY, self);
	priv->radio_event_id[RADIO_EVENT_STATE_CHANGED] =
		ril_radio_add_state_changed_handler(priv->radio,
			ril_network_radio_state_cb, self);
	priv->radio_event_id[RADIO_EVENT_ONLINE_CHANGED] =
		ril_radio_add_online_changed_handler(priv->radio,
			ril_network_radio_online_cb, self);
	priv->simcard_event_id[SIM_EVENT_STATUS_CHANGED] =
		ril_sim_card_add_status_changed_handler(priv->simcard,
			ril_network_sim_status_changed_cb, self);
	priv->simcard_event_id[SIM_EVENT_IO_ACTIVE_CHANGED] =
		ril_sim_card_add_sim_io_active_changed_handler(priv->simcard,
			ril_network_sim_status_changed_cb, self);
	priv->settings_event_id =
		ril_sim_settings_add_pref_mode_changed_handler(settings,
			ril_network_pref_mode_changed_cb, self);

	/*
	 * Query the initial state. Querying network state before the radio
	 * has been turned on makes RIL unhappy.
	 */
	grilio_queue_send_request_full(priv->q, NULL,
			RIL_REQUEST_GET_PREFERRED_NETWORK_TYPE,
			ril_network_startup_query_pref_mode_cb, NULL, self);
	if (radio->state == RADIO_STATE_ON) {
		ril_network_poll_state(self);
	}

	return self;
}

struct ril_network *ril_network_ref(struct ril_network *self)
{
	if (G_LIKELY(self)) {
		g_object_ref(RIL_NETWORK(self));
		return self;
	} else {
		return NULL;
	}
}

void ril_network_unref(struct ril_network *self)
{
	if (G_LIKELY(self)) {
		g_object_unref(RIL_NETWORK(self));
	}
}

static void ril_network_init(struct ril_network *self)
{
	struct ril_network_priv *priv = G_TYPE_INSTANCE_GET_PRIVATE(self,
		RIL_NETWORK_TYPE, struct ril_network_priv);

	self->priv = priv;
	ril_network_reset_state(&self->voice);
	ril_network_reset_state(&self->data);
	priv->rat = -1;
}

static void ril_network_finalize(GObject *object)
{
	struct ril_network *self = RIL_NETWORK(object);
	struct ril_network_priv *priv = self->priv;
	enum ril_network_timer tid;

	DBG_(self, "");

	for (tid=0; tid<TIMER_COUNT; tid++) {
		ril_network_stop_timer(self, tid);
	}

	grilio_queue_cancel_all(priv->q, FALSE);
	grilio_channel_remove_all_handlers(priv->io, priv->unsol_event_id);
	grilio_channel_unref(priv->io);
	grilio_queue_unref(priv->q);
	ril_radio_remove_all_handlers(priv->radio, priv->radio_event_id);
	ril_radio_unref(priv->radio);
	ril_sim_card_remove_all_handlers(priv->simcard, priv->simcard_event_id);
	ril_sim_card_unref(priv->simcard);
	ril_sim_settings_remove_handler(self->settings,
						priv->settings_event_id);
	ril_sim_settings_unref(self->settings);
	g_free(priv->log_prefix);
	G_OBJECT_CLASS(ril_network_parent_class)->finalize(object);
}

static void ril_network_class_init(RilNetworkClass *klass)
{
	G_OBJECT_CLASS(klass)->finalize = ril_network_finalize;
	g_type_class_add_private(klass, sizeof(struct ril_network_priv));
	RIL_NETWORK_SIGNAL(klass, OPERATOR);
	RIL_NETWORK_SIGNAL(klass, VOICE_STATE);
	RIL_NETWORK_SIGNAL(klass, DATA_STATE);
	RIL_NETWORK_SIGNAL(klass, PREF_MODE);
	RIL_NETWORK_SIGNAL(klass, MAX_PREF_MODE);
}

/*
 * Local Variables:
 * mode: C
 * c-basic-offset: 8
 * indent-tabs-mode: t
 * End:
 */
