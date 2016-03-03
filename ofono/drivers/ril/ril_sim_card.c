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

#include "ril_sim_card.h"
#include "ril_radio.h"
#include "ril_util.h"
#include "ril_log.h"

#include <grilio_queue.h>
#include <grilio_request.h>
#include <grilio_parser.h>

#include <gutil_misc.h>

typedef GObjectClass RilSimCardClass;
typedef struct ril_sim_card RilSimCard;

enum ril_sim_card_event {
	EVENT_SIM_STATUS_CHANGED,
	EVENT_UICC_SUBSCRIPTION_STATUS_CHANGED,
	EVENT_COUNT
};

struct ril_sim_card_priv {
	GRilIoChannel *io;
	GRilIoQueue *q;
	int flags;
	guint status_req_id;
	gulong event_id[EVENT_COUNT];
};

enum ril_sim_card_signal {
	SIGNAL_STATUS_RECEIVED,
	SIGNAL_STATUS_CHANGED,
	SIGNAL_STATE_CHANGED,
	SIGNAL_APP_CHANGED,
	SIGNAL_COUNT
};

#define SIGNAL_STATUS_RECEIVED_NAME     "ril-simcard-status-received"
#define SIGNAL_STATUS_CHANGED_NAME      "ril-simcard-status-changed"
#define SIGNAL_STATE_CHANGED_NAME       "ril-simcard-state-changed"
#define SIGNAL_APP_CHANGED_NAME         "ril-simcard-app-changed"

static guint ril_sim_card_signals[SIGNAL_COUNT] = { 0 };

G_DEFINE_TYPE(RilSimCard, ril_sim_card, G_TYPE_OBJECT)
#define RIL_SIMCARD_TYPE (ril_sim_card_get_type())
#define RIL_SIMCARD(obj) (G_TYPE_CHECK_INSTANCE_CAST((obj), \
	RIL_SIMCARD_TYPE, RilSimCard))

#define RIL_SIMCARD_STATE_CHANGED  (0x01)
#define RIL_SIMCARD_STATUS_CHANGED (0x02)

static void ril_sim_card_request_status(struct ril_sim_card *self);

static gboolean ril_sim_card_app_equal(const struct ril_sim_card_app *a1,
					const struct ril_sim_card_app *a2)
{
	if (a1 == a2) {
		return TRUE;
	} else if (!a1 || !a2) {
		return FALSE;
	} else {
		return a1->app_type == a2->app_type &&
			a1->app_state == a2->app_state &&
			a1->perso_substate == a2->perso_substate &&
			a1->pin_replaced == a2->pin_replaced &&
			a1->pin1_state == a2->pin1_state &&
			a1->pin2_state == a2->pin2_state &&
			!g_strcmp0(a1->aid, a2->aid) &&
			!g_strcmp0(a1->label, a2->label);
	}
}

static int ril_sim_card_status_compare(const struct ril_sim_card_status *s1,
					const struct ril_sim_card_status *s2)
{
	if (s1 == s2) {
		return 0;
	} else if (!s1 || !s2) {
		return RIL_SIMCARD_STATE_CHANGED | RIL_SIMCARD_STATUS_CHANGED;
	} else {
		int diff = 0;

		if (s1->card_state != s2->card_state) {
			diff |= RIL_SIMCARD_STATE_CHANGED;
		}

		if (s1->pin_state != s2->pin_state ||
			s1->gsm_umts_index != s2->gsm_umts_index ||
			s1->cdma_index != s2->cdma_index ||
			s1->ims_index != s2->ims_index ||
			s1->num_apps != s2->num_apps) {
			diff |= RIL_SIMCARD_STATUS_CHANGED;
		} else {
			int i;

			for (i = 0; i < s1->num_apps; i++) {
				if (!ril_sim_card_app_equal(s1->apps + i,
							     s2->apps + i)) {
					diff |= RIL_SIMCARD_STATUS_CHANGED;
					break;
				}
			}
		}

		return diff;
	}
}

static void ril_sim_card_status_free(struct ril_sim_card_status *status)
{
	if (status) {
		if (status->apps) {
			int i;

			for (i = 0; i < status->num_apps; i++) {
				g_free(status->apps[i].aid);
				g_free(status->apps[i].label);
			}
			g_free(status->apps);
		}
		g_free(status);
	}
}

static void ril_sim_card_subscribe(struct ril_sim_card *self,
						int app_index, int sub_status)
{
	struct ril_sim_card_priv *priv = self->priv;
	GRilIoRequest *req = grilio_request_sized_new(16);
	const guint sub_id = self->slot;

	DBG("%u,%d,%u,%d", self->slot, app_index, sub_id, sub_status);
	grilio_request_append_int32(req, self->slot);
	grilio_request_append_int32(req, app_index);
	grilio_request_append_int32(req, sub_id);
	grilio_request_append_int32(req, sub_status);
	grilio_queue_send_request(priv->q, req, (priv->io->ril_version <= 9 &&
		(priv->flags & RIL_SIM_CARD_V9_UICC_SUBSCRIPTION_WORKAROUND)) ?
				RIL_REQUEST_V9_SET_UICC_SUBSCRIPTION :
				RIL_REQUEST_SET_UICC_SUBSCRIPTION);
	grilio_request_unref(req);
}

static int ril_sim_card_select_app(const struct ril_sim_card_status *status)
{
	int selected_app = -1;
	guint i;

	for (i = 0; i < status->num_apps; i++) {
		const int type = status->apps[i].app_type;
		if (type == RIL_APPTYPE_USIM || type == RIL_APPTYPE_RUIM) {
			selected_app = i;
			break;
		} else if (type != RIL_APPTYPE_UNKNOWN && selected_app == -1) {
			selected_app = i;
		}
	}

	DBG("%d", selected_app);
	return selected_app;
}

static void ril_sim_card_update_app(struct ril_sim_card *self)
{
	const struct ril_sim_card_app *old_app = self->app;
	const struct ril_sim_card_status *status = self->status;
	int app_index;

	if (status->card_state == RIL_CARDSTATE_PRESENT) {
		if (status->gsm_umts_index >= 0 &&
				status->gsm_umts_index < status->num_apps) {
			app_index = status->gsm_umts_index;
		} else {
			app_index = ril_sim_card_select_app(status);
			if (app_index >= 0) {
				ril_sim_card_subscribe(self, app_index, 1);
			}
		}
	} else {
		app_index = -1;
	}

	if (app_index >= 0 &&
		status->apps[app_index].app_type != RIL_APPTYPE_UNKNOWN) {
		self->app = status->apps + app_index;
	} else {
		self->app = NULL;
	}

	if (!ril_sim_card_app_equal(old_app, self->app)) {
		g_signal_emit(self,
			ril_sim_card_signals[SIGNAL_APP_CHANGED], 0);
	}
}

static void ril_sim_card_update_status(struct ril_sim_card *self,
					struct ril_sim_card_status *status)
{
	const int diff = ril_sim_card_status_compare(self->status, status);

	if (diff) {
		struct ril_sim_card_status *old_status = self->status;

		self->status = status;
		ril_sim_card_update_app(self);
		g_signal_emit(self, ril_sim_card_signals[
						SIGNAL_STATUS_RECEIVED], 0);
		if (diff & RIL_SIMCARD_STATUS_CHANGED) {
			DBG("status changed");
			g_signal_emit(self, ril_sim_card_signals[
						SIGNAL_STATUS_CHANGED], 0);
		}
		if (diff & RIL_SIMCARD_STATE_CHANGED) {
			DBG("state changed");
			g_signal_emit(self, ril_sim_card_signals[
						SIGNAL_STATE_CHANGED], 0);
		}
		ril_sim_card_status_free(old_status);
	} else {
		ril_sim_card_status_free(status);
		g_signal_emit(self, ril_sim_card_signals[
						SIGNAL_STATUS_RECEIVED], 0);
	}
}

static gboolean ril_sim_card_app_parse(GRilIoParser *rilp,
						struct ril_sim_card_app *app)
{
	gint32 app_type, app_state, perso_substate;
	gint32 pin_replaced, pin1_state, pin2_state;

	grilio_parser_get_int32(rilp, &app_type);
	grilio_parser_get_int32(rilp, &app_state);

	/*
	 * Consider RIL_APPSTATE_ILLEGAL also READY. Even if app state is
	 * RIL_APPSTATE_ILLEGAL (-1), ICC operations must be permitted.
	 * Network access requests will anyway be rejected and ME will be
	 * in limited service.
	 */
	if (app_state == RIL_APPSTATE_ILLEGAL) {
		DBG("RIL_APPSTATE_ILLEGAL => RIL_APPSTATE_READY");
		app_state = RIL_APPSTATE_READY;
	}

	grilio_parser_get_int32(rilp, &perso_substate);
	app->aid = grilio_parser_get_utf8(rilp);
	app->label = grilio_parser_get_utf8(rilp);

	if (grilio_parser_get_int32(rilp, &pin_replaced) &&
		grilio_parser_get_int32(rilp, &pin1_state) &&
		grilio_parser_get_int32(rilp, &pin2_state)) {

		app->app_type = app_type;
		app->app_state = app_state;
		app->perso_substate = perso_substate;
		app->pin_replaced = pin_replaced;
		app->pin1_state = pin1_state;
		app->pin2_state = pin2_state;

		return TRUE;
	}

	return FALSE;
}

static struct ril_sim_card_status *ril_sim_card_status_parse(const void *data,
								guint len)
{
	GRilIoParser rilp;
	gint32 card_state, pin_state, gsm_umts_index, cdma_index;
        gint32 ims_index, num_apps;

	grilio_parser_init(&rilp, data, len);

	if (!grilio_parser_get_int32(&rilp, &card_state) ||
		!grilio_parser_get_int32(&rilp, &pin_state) ||
		!grilio_parser_get_int32(&rilp, &gsm_umts_index) ||
		!grilio_parser_get_int32(&rilp, &cdma_index) ||
		!grilio_parser_get_int32(&rilp, &ims_index) ||
		!grilio_parser_get_int32(&rilp, &num_apps)) {
		ofono_error("Failed to parse SIM card status request");
		return NULL;
	} else if (num_apps < 0 || num_apps > RIL_CARD_MAX_APPS) {
		ofono_error("Invalid SIM app count %d", num_apps);
		return NULL;
	} else {
		int i;
		struct ril_sim_card_status *status =
			g_new0(struct ril_sim_card_status, 1);

		DBG("card_state=%d, universal_pin_state=%d, gsm_umts_index=%d, "
			"cdma_index=%d, ims_index=%d, num_apps=%d",
			card_state, pin_state, gsm_umts_index, cdma_index,
			ims_index, num_apps);

		status->card_state = card_state;
		status->pin_state = pin_state;
		status->gsm_umts_index = gsm_umts_index;
		status->cdma_index = cdma_index;
		status->ims_index = ims_index;
		status->num_apps = num_apps;

		if (num_apps > 0) {
			status->apps = g_new0(struct ril_sim_card_app, num_apps);
		}

		for (i = 0; i < num_apps; i++) {
			struct ril_sim_card_app *app = status->apps + i;

			if (ril_sim_card_app_parse(&rilp, app)) {
				DBG("app[%d]: type=%d, state=%d, "
					"perso_substate=%d, aid_ptr=%s, "
					"label=%s, pin1_replaced=%d, pin1=%d, "
					"pin2=%d", i, app->app_type,
					app->app_state, app->perso_substate,
					app->aid, app->label,
					app->pin_replaced, app->pin1_state,
					app->pin2_state);
			} else {
				break;
			}
		}

		if (i == num_apps) {
			return status;
		} else {
			ril_sim_card_status_free(status);
			return NULL;
		}
	}
}

static void ril_sim_card_status_cb(GRilIoChannel *io, int ril_status,
				const void *data, guint len, void *user_data)
{
	struct ril_sim_card *self = user_data;
	struct ril_sim_card_priv *priv = self->priv;

	GASSERT(priv->status_req_id);
	priv->status_req_id = 0;

	if (ril_status == RIL_E_SUCCESS) {
		struct ril_sim_card_status *status =
			ril_sim_card_status_parse(data, len);

		if (status) {
			ril_sim_card_update_status(self, status);
		}
	}
}

static void ril_sim_card_request_status(struct ril_sim_card *self)
{
	struct ril_sim_card_priv *priv = self->priv;

	if (priv->status_req_id) {
		/* Retry right away, don't wait for retry timeout to expire */
		grilio_channel_retry_request(priv->io, priv->status_req_id);
	} else {
		GRilIoRequest* req = grilio_request_new();

		grilio_request_set_retry(req, RIL_RETRY_SECS*1000, -1);
		priv->status_req_id = grilio_queue_send_request_full(priv->q,
					req, RIL_REQUEST_GET_SIM_STATUS,
					ril_sim_card_status_cb, NULL, self);
		grilio_request_unref(req);
	}
}

static void ril_sim_card_status_changed(GRilIoChannel *io, guint code,
				const void *data, guint len, void *user_data)
{
	struct ril_sim_card *self = user_data;

	ril_sim_card_request_status(self);
}

struct ril_sim_card *ril_sim_card_new(GRilIoChannel *io, guint slot, int flags)
{
	struct ril_sim_card *self = g_object_new(RIL_SIMCARD_TYPE, NULL);
	struct ril_sim_card_priv *priv = self->priv;

	/*
	 * We need to know the RIL version (for UICC subscription hack),
	 * so we must be connected. The caller is supposed to make sure
	 * that we get connected first.
	 */
	DBG("%u", slot);
	GASSERT(io->connected);

	self->slot = slot;
	priv->io = grilio_channel_ref(io);
	priv->q = grilio_queue_new(io);
	priv->flags = flags;

	priv->event_id[EVENT_SIM_STATUS_CHANGED] =
		grilio_channel_add_unsol_event_handler(priv->io,
			ril_sim_card_status_changed,
			RIL_UNSOL_RESPONSE_SIM_STATUS_CHANGED, self);
	priv->event_id[EVENT_UICC_SUBSCRIPTION_STATUS_CHANGED] =
		grilio_channel_add_unsol_event_handler(priv->io,
			ril_sim_card_status_changed,
			RIL_UNSOL_UICC_SUBSCRIPTION_STATUS_CHANGED, self);
	ril_sim_card_request_status(self);
	return self;
}

struct ril_sim_card *ril_sim_card_ref(struct ril_sim_card *self)
{
	if (G_LIKELY(self)) {
		g_object_ref(RIL_SIMCARD(self));
		return self;
	} else {
		return NULL;
	}
}

void ril_sim_card_unref(struct ril_sim_card *self)
{
	if (G_LIKELY(self)) {
		g_object_unref(RIL_SIMCARD(self));
	}
}

gboolean ril_sim_card_ready(struct ril_sim_card *self)
{
	return self && self->app &&
		((self->app->app_state == RIL_APPSTATE_READY) ||
		(self->app->app_state == RIL_APPSTATE_SUBSCRIPTION_PERSO &&
		self->app->perso_substate == RIL_PERSOSUBSTATE_READY));
}

gulong ril_sim_card_add_status_received_handler(struct ril_sim_card *self,
					ril_sim_card_cb_t cb, void *arg)
{
	return (G_LIKELY(self) && G_LIKELY(cb)) ? g_signal_connect(self,
		SIGNAL_STATUS_RECEIVED_NAME, G_CALLBACK(cb), arg) : 0;
}

gulong ril_sim_card_add_status_changed_handler(struct ril_sim_card *self,
					ril_sim_card_cb_t cb, void *arg)
{
	return (G_LIKELY(self) && G_LIKELY(cb)) ? g_signal_connect(self,
		SIGNAL_STATUS_CHANGED_NAME, G_CALLBACK(cb), arg) : 0;
}

gulong ril_sim_card_add_state_changed_handler(struct ril_sim_card *self,
					ril_sim_card_cb_t cb, void *arg)
{
	return (G_LIKELY(self) && G_LIKELY(cb)) ? g_signal_connect(self,
		SIGNAL_STATE_CHANGED_NAME, G_CALLBACK(cb), arg) : 0;
}

gulong ril_sim_card_add_app_changed_handler(struct ril_sim_card *self,
					ril_sim_card_cb_t cb, void *arg)
{
	return (G_LIKELY(self) && G_LIKELY(cb)) ? g_signal_connect(self,
		SIGNAL_APP_CHANGED_NAME, G_CALLBACK(cb), arg) : 0;
}

void ril_sim_card_remove_handler(struct ril_sim_card *self, gulong id)
{
	if (G_LIKELY(self) && G_LIKELY(id)) {
		g_signal_handler_disconnect(self, id);
	}
}

void ril_sim_card_remove_handlers(struct ril_sim_card *self, gulong *ids, int n)
{
	gutil_disconnect_handlers(self, ids, n);
}

static void ril_sim_card_init(struct ril_sim_card *self)
{
	self->priv = G_TYPE_INSTANCE_GET_PRIVATE(self, RIL_SIMCARD_TYPE,
						struct ril_sim_card_priv);
}

static void ril_sim_card_dispose(GObject *object)
{
	struct ril_sim_card *self = RIL_SIMCARD(object);
	struct ril_sim_card_priv *priv = self->priv;

	grilio_channel_remove_handlers(priv->io, priv->event_id, EVENT_COUNT);
	grilio_queue_cancel_all(priv->q, TRUE);
	G_OBJECT_CLASS(ril_sim_card_parent_class)->dispose(object);
}

static void ril_sim_card_finalize(GObject *object)
{
	struct ril_sim_card *self = RIL_SIMCARD(object);
	struct ril_sim_card_priv *priv = self->priv;

	grilio_channel_unref(priv->io);
	grilio_queue_unref(priv->q);
	ril_sim_card_status_free(self->status);
	G_OBJECT_CLASS(ril_sim_card_parent_class)->finalize(object);
}

static void ril_sim_card_class_init(RilSimCardClass *klass)
{
	GObjectClass *object_class = G_OBJECT_CLASS(klass);

	object_class->dispose = ril_sim_card_dispose;
	object_class->finalize = ril_sim_card_finalize;
	g_type_class_add_private(klass, sizeof(struct ril_sim_card_priv));
	ril_sim_card_signals[SIGNAL_STATUS_RECEIVED] =
		g_signal_new(SIGNAL_STATUS_RECEIVED_NAME,
			G_OBJECT_CLASS_TYPE(klass), G_SIGNAL_RUN_FIRST,
			0, NULL, NULL, NULL, G_TYPE_NONE, 0);
	ril_sim_card_signals[SIGNAL_STATUS_CHANGED] =
		g_signal_new(SIGNAL_STATUS_CHANGED_NAME,
			G_OBJECT_CLASS_TYPE(klass), G_SIGNAL_RUN_FIRST,
			0, NULL, NULL, NULL, G_TYPE_NONE, 0);
	ril_sim_card_signals[SIGNAL_STATE_CHANGED] =
		g_signal_new(SIGNAL_STATE_CHANGED_NAME,
			G_OBJECT_CLASS_TYPE(klass), G_SIGNAL_RUN_FIRST,
			0, NULL, NULL, NULL, G_TYPE_NONE, 0);
	ril_sim_card_signals[SIGNAL_APP_CHANGED] =
		g_signal_new(SIGNAL_APP_CHANGED_NAME,
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
