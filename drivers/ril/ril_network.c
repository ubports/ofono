/*
 *  oFono - Open Source Telephony - RIL-based devices
 *
 *  Copyright (C) 2015-2016 Jolla Ltd.
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

#include "ril_network.h"
#include "ril_util.h"
#include "ril_log.h"

#include <grilio_queue.h>
#include <grilio_request.h>
#include <grilio_parser.h>

#include <ofono/netreg.h>
#include "common.h"

typedef GObjectClass RilNetworkClass;
typedef struct ril_network RilNetwork;

struct ril_network_priv {
	GRilIoChannel *io;
	GRilIoQueue *q;
	char *log_prefix;
	gulong event_id;
	guint operator_poll_id;
	guint voice_poll_id;
	guint data_poll_id;
	struct ofono_network_operator operator;
};

enum ril_network_signal {
	SIGNAL_OPERATOR_CHANGED,
	SIGNAL_VOICE_STATE_CHANGED,
	SIGNAL_DATA_STATE_CHANGED,
	SIGNAL_COUNT
};

#define SIGNAL_OPERATOR_CHANGED_NAME    "ril-network-operator-changed"
#define SIGNAL_VOICE_STATE_CHANGED_NAME "ril-network-voice-state-changed"
#define SIGNAL_DATA_STATE_CHANGED_NAME  "ril-network-data-state-changed"

static guint ril_network_signals[SIGNAL_COUNT] = { 0 };

G_DEFINE_TYPE(RilNetwork, ril_network, G_TYPE_OBJECT)
#define RIL_NETWORK_TYPE (ril_network_get_type())
#define RIL_NETWORK(obj) (G_TYPE_CHECK_INSTANCE_CAST(obj,\
        RIL_NETWORK_TYPE,RilNetwork))

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
	struct ril_network_priv *priv = self->priv;
	int nparams, ril_status;
	gchar *sstatus = NULL, *slac = NULL, *sci = NULL;
	gchar *stech = NULL, *sreason = NULL, *smax = NULL;
	GRilIoParser rilp;

	ril_network_reset_state(reg);

	/* Size of response string array
	 *
	 * Should be:
	 *   >= 4 for VOICE_REG reply
	 *   >= 5 for DATA_REG reply
	 */
	grilio_parser_init(&rilp, data, len);
	if (!grilio_parser_get_int32(&rilp, &nparams) || nparams < 4) {
		DBG("%sbroken response", priv->log_prefix);
		return FALSE;
	}

	sstatus = grilio_parser_get_utf8(&rilp);
	if (!sstatus) {
		DBG("%sNo sstatus value returned!", priv->log_prefix);
		return FALSE;
	}

	slac = grilio_parser_get_utf8(&rilp);
	sci = grilio_parser_get_utf8(&rilp);
	stech = grilio_parser_get_utf8(&rilp);
	nparams -= 4;

	ril_status = atoi(sstatus);
	if (ril_status > 10) {
		reg->status = ril_status - 10;
	} else {
		reg->status = ril_status;
	}

	/* FIXME: need to review VOICE_REGISTRATION response
	 * as it returns ~15 parameters ( vs. 6 for DATA ).
	 *
	 * The first four parameters are the same for both
	 * responses ( although status includes values for
	 * emergency calls for VOICE response ).
	 *
	 * Parameters 5 & 6 have different meanings for
	 * voice & data response.
	 */
	if (nparams--) {
		/* TODO: different use for CDMA */
		sreason = grilio_parser_get_utf8(&rilp);
		if (nparams--) {
			/* TODO: different use for CDMA */
			smax = grilio_parser_get_utf8(&rilp);
			if (smax) {
				reg->max_calls = atoi(smax);
			}
		}
	}

	reg->lac = slac ? strtol(slac, NULL, 16) : -1;
	reg->ci = sci ? strtol(sci, NULL, 16) : -1;
	reg->access_tech = ril_parse_tech(stech, &reg->ril_tech);

	DBG("%s%s,%s,%s%d,%s,%s,%s", priv->log_prefix,
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

static guint ril_network_poll_and_retry(struct ril_network *self, int code,
						GRilIoChannelResponseFunc fn)
{
	guint id;
	GRilIoRequest *req = grilio_request_new();
	struct ril_network_priv *priv = self->priv;

	grilio_request_set_retry(req, RIL_RETRY_SECS*1000, -1);
	id = grilio_queue_send_request_full(priv->q, req, code, fn, NULL, self);
	grilio_request_unref(req);
	return id;
}

static void ril_network_poll_operator_cb(GRilIoChannel *io, int req_status,
				const void *data, guint len, void *user_data)
{
	struct ril_network *self = user_data;
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
			op.status = self->voice.status;
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
				DBG("%slalpha=%s, salpha=%s, numeric=%s, %s, "
					"mcc=%s, mnc=%s, %s", priv->log_prefix,
					lalpha, salpha, numeric,
					op.name, op.mcc, op.mnc,
					registration_tech_to_string(op.tech));
			} else {
				DBG("%sno operator", priv->log_prefix);
			}
			g_signal_emit(self, ril_network_signals[
					SIGNAL_OPERATOR_CHANGED], 0);
		}

		g_free(lalpha);
		g_free(salpha);
		g_free(numeric);
	}
}

static void ril_network_poll_voice_state_cb(GRilIoChannel *io, int req_status,
				const void *data, guint len, void *user_data)
{
	struct ril_network *self = user_data;
	struct ril_network_priv *priv = self->priv;

	GASSERT(priv->voice_poll_id);
	priv->voice_poll_id = 0;

	if (req_status == RIL_E_SUCCESS) {
		struct ril_registration_state state;

		ril_network_parse_response(self, data, len, &state);
		if (memcmp(&state, &self->voice, sizeof(state))) {
			DBG("%svoice registration changed", priv->log_prefix);
			self->voice = state;
			g_signal_emit(self, ril_network_signals[
					SIGNAL_VOICE_STATE_CHANGED], 0);
		}
	}
}

static void ril_network_poll_data_state_cb(GRilIoChannel *io, int req_status,
				const void *data, guint len, void *user_data)
{
	struct ril_network *self = user_data;
	struct ril_network_priv *priv = self->priv;

	GASSERT(priv->data_poll_id);
	priv->data_poll_id = 0;

	if (req_status == RIL_E_SUCCESS) {
		struct ril_registration_state state;

		ril_network_parse_response(self, data, len, &state);
		if (memcmp(&state, &self->data, sizeof(state))) {
			DBG("%sdata registration changed", priv->log_prefix);
			self->data = state;
			g_signal_emit(self, ril_network_signals[
					SIGNAL_DATA_STATE_CHANGED], 0);
		}
	}
}

static void ril_network_poll_operator(struct ril_network *self)
{
	struct ril_network_priv *priv = self->priv;

	if (!priv->operator_poll_id) {
		DBG("%s", priv->log_prefix);
		priv->operator_poll_id = ril_network_poll_and_retry(self,
			RIL_REQUEST_OPERATOR, ril_network_poll_operator_cb);
	}
}

static void ril_network_poll_voice_state(struct ril_network *self)
{
	struct ril_network_priv *priv = self->priv;

	if (!priv->voice_poll_id) {
		DBG("%s", priv->log_prefix);
		priv->voice_poll_id = ril_network_poll_and_retry(self,
				RIL_REQUEST_VOICE_REGISTRATION_STATE,
				ril_network_poll_voice_state_cb);
	}
}

static void ril_network_poll_data_state(struct ril_network *self)
{
	struct ril_network_priv *priv = self->priv;

	if (!priv->data_poll_id) {
		DBG("%s", priv->log_prefix);
		priv->data_poll_id = ril_network_poll_and_retry(self,
				RIL_REQUEST_DATA_REGISTRATION_STATE,
				ril_network_poll_data_state_cb);
	}
}

static void ril_network_poll_state(struct ril_network *self)
{
	ril_network_poll_operator(self);
	ril_network_poll_voice_state(self);
	ril_network_poll_data_state(self);
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

void ril_network_remove_handler(struct ril_network *self, gulong id)
{
	if (G_LIKELY(self) && G_LIKELY(id)) {
		g_signal_handler_disconnect(self, id);
	}
}

static void ril_network_voice_state_changed_cb(GRilIoChannel *io, guint code,
				const void *data, guint len, void *user_data)
{
	struct ril_network *self = user_data;
	struct ril_network_priv *priv = self->priv;

	DBG("%s", priv->log_prefix);
	GASSERT(code == RIL_UNSOL_RESPONSE_VOICE_NETWORK_STATE_CHANGED);
	ril_network_poll_state(self);
}

struct ril_network *ril_network_new(GRilIoChannel *io)
{
	struct ril_network *self = g_object_new(RIL_NETWORK_TYPE, NULL);
	struct ril_network_priv *priv = self->priv;

	priv->io = grilio_channel_ref(io);
	priv->q = grilio_queue_new(priv->io);
	priv->log_prefix =
		(io && io->name && io->name[0] && strcmp(io->name, "RIL")) ?
		g_strconcat(io->name, " ", NULL) : g_strdup("");
	DBG("%s", priv->log_prefix);
	priv->event_id = grilio_channel_add_unsol_event_handler(priv->io,
			ril_network_voice_state_changed_cb,
			RIL_UNSOL_RESPONSE_VOICE_NETWORK_STATE_CHANGED, self);

	/* Query the initial state */
	ril_network_poll_state(self);
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
}

static void ril_network_dispose(GObject *object)
{
	struct ril_network *self = RIL_NETWORK(object);
	struct ril_network_priv *priv = self->priv;

	if (priv->event_id) {
		grilio_channel_remove_handler(priv->io, priv->event_id);
		priv->event_id = 0;
	}

	grilio_queue_cancel_all(priv->q, FALSE);
	G_OBJECT_CLASS(ril_network_parent_class)->dispose(object);
}

static void ril_network_finalize(GObject *object)
{
	struct ril_network *self = RIL_NETWORK(object);
	struct ril_network_priv *priv = self->priv;

	DBG("%s", priv->log_prefix);
	g_free(priv->log_prefix);
	grilio_channel_unref(priv->io);
	grilio_queue_unref(priv->q);
	G_OBJECT_CLASS(ril_network_parent_class)->finalize(object);
}

static void ril_network_class_init(RilNetworkClass *klass)
{
	GObjectClass *object_class = G_OBJECT_CLASS(klass);

	object_class->dispose = ril_network_dispose;
	object_class->finalize = ril_network_finalize;
	g_type_class_add_private(klass, sizeof(struct ril_network_priv));
	ril_network_signals[SIGNAL_OPERATOR_CHANGED] =
		g_signal_new(SIGNAL_OPERATOR_CHANGED_NAME,
			G_OBJECT_CLASS_TYPE(klass), G_SIGNAL_RUN_FIRST,
			0, NULL, NULL, NULL, G_TYPE_NONE, 0);
	ril_network_signals[SIGNAL_VOICE_STATE_CHANGED] =
		g_signal_new(SIGNAL_VOICE_STATE_CHANGED_NAME,
			G_OBJECT_CLASS_TYPE(klass), G_SIGNAL_RUN_FIRST,
			0, NULL, NULL, NULL, G_TYPE_NONE, 0);
	ril_network_signals[SIGNAL_DATA_STATE_CHANGED] =
		g_signal_new(SIGNAL_DATA_STATE_CHANGED_NAME,
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
