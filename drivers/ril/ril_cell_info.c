/*
 *  oFono - Open Source Telephony - RIL-based devices
 *
 *  Copyright (C) 2016 Jolla Ltd.
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

#include "ril_cell_info.h"
#include "ril_sim_card.h"
#include "ril_radio.h"
#include "ril_util.h"
#include "ril_mce.h"
#include "ril_log.h"

#include <grilio_channel.h>
#include <grilio_request.h>
#include <grilio_parser.h>

#define DISPLAY_ON_UPDATE_RATE  (1000)  /* 1 sec */
#define DISPLAY_OFF_UPDATE_RATE (60000) /* 1 min */

typedef GObjectClass RilCellInfoClass;
typedef struct ril_cell_info RilCellInfo;

struct ril_cell_info_priv {
	GRilIoChannel *io;
	struct ril_mce *mce;
	struct ril_radio *radio;
	struct ril_sim_card *sim_card;
	gulong display_state_event_id;
	gulong radio_state_event_id;
	gulong sim_status_event_id;
	gboolean sim_card_ready;
	char *log_prefix;
	gulong event_id;
	guint query_id;
	guint set_rate_id;
};

enum ril_cell_info_signal {
	SIGNAL_CELLS_CHANGED,
	SIGNAL_COUNT
};

#define SIGNAL_CELLS_CHANGED_NAME   "ril-cell-info-cells-changed"

static guint ril_cell_info_signals[SIGNAL_COUNT] = { 0 };

G_DEFINE_TYPE(RilCellInfo, ril_cell_info, G_TYPE_OBJECT)
#define RIL_CELL_INFO_TYPE (ril_cell_info_get_type())
#define RIL_CELL_INFO(obj) (G_TYPE_CHECK_INSTANCE_CAST((obj),\
	RIL_CELL_INFO_TYPE, RilCellInfo))

#define DBG_(self,fmt,args...) DBG("%s" fmt, (self)->priv->log_prefix, ##args)

gint ril_cell_compare_location(const struct ril_cell *c1,
					const struct ril_cell *c2)
{
	if (c1 && c2) {
		if (c1->type != c2->type) {
			return c1->type - c2->type;
		} else if (c1->type == RIL_CELL_INFO_TYPE_GSM) {
			const struct ril_cell_info_gsm *g1 = &c1->info.gsm;
			const struct ril_cell_info_gsm *g2 = &c2->info.gsm;

			if (g1->lac != g2->lac) {
				return g1->lac - g2->lac;
			} else {
				return g1->cid - g2->cid;
			}
		} else if (c2->type == RIL_CELL_INFO_TYPE_WCDMA) {
			const struct ril_cell_info_wcdma *w1 = &c1->info.wcdma;
			const struct ril_cell_info_wcdma *w2 = &c2->info.wcdma;

			if (w1->lac != w2->lac) {
				return w1->lac - w2->lac;
			} else {
				return w1->cid - w2->cid;
			}
		} else {
			const struct ril_cell_info_lte *l1 = &c1->info.lte;
			const struct ril_cell_info_lte *l2 = &c2->info.lte;

			GASSERT(c1->type == RIL_CELL_INFO_TYPE_LTE);
			if (l1->ci != l2->ci) {
				return l1->ci - l2->ci;
			} else if (l1->pci != l2->pci) {
				return l1->pci - l2->pci;
			} else {
				return l1->tac - l2->tac;
			}
		}
	} else if (c1) {
		return 1;
	} else if (c2) {
		return -1;
	} else {
		return 0;
	}
}

gint ril_cell_compare_func(gconstpointer v1, gconstpointer v2)
{
	return ril_cell_compare_location(v1, v2);
}

static gboolean ril_cell_info_list_identical(GSList *l1, GSList *l2)
{
	while (l1 && l2) {
		if (memcmp(l1->data, l2->data, sizeof(struct ril_cell))) {
			return FALSE;
		}
		l1 = l1->next;
		l2 = l2->next;
	}
	return !l1 && !l2;
}

static void ril_cell_info_update_cells(struct ril_cell_info *self, GSList *l)
{
	if (!ril_cell_info_list_identical(self->cells, l)) {
		g_slist_free_full(self->cells, g_free);
		self->cells = l;
		g_signal_emit(self, ril_cell_info_signals[
						SIGNAL_CELLS_CHANGED], 0);
	} else {
		g_slist_free_full(l, g_free);
	}
}

static struct ril_cell *ril_cell_info_parse_cell_gsm(GRilIoParser *rilp,
							gboolean registered)
{
	struct ril_cell *cell = g_new0(struct ril_cell, 1);
	struct ril_cell_info_gsm *gsm = &cell->info.gsm;

	if (grilio_parser_get_int32(rilp, &gsm->mcc) &&
		grilio_parser_get_int32(rilp, &gsm->mnc) &&
		grilio_parser_get_int32(rilp, &gsm->lac) &&
		grilio_parser_get_int32(rilp, &gsm->cid) &&
		grilio_parser_get_int32(rilp, &gsm->signalStrength) &&
		grilio_parser_get_int32(rilp, &gsm->bitErrorRate)) {
		DBG("[gsm] reg=%d,mcc=%d,mnc=%d,lac=%d,cid=%d,"
			"strength=%d,err=%d", registered, gsm->mcc, gsm->mnc,
			gsm->lac, gsm->cid, gsm->signalStrength,
			gsm->bitErrorRate);
		cell->type = RIL_CELL_INFO_TYPE_GSM;
		cell->registered = registered;
		return cell;
	}

	ofono_error("failed to parse GSM cell info");
	g_free(cell);
	return NULL;
}

static struct ril_cell *ril_cell_info_parse_cell_wcdma(GRilIoParser *rilp,
							gboolean registered)
{
	struct ril_cell *cell = g_new0(struct ril_cell, 1);
	struct ril_cell_info_wcdma *wcdma = &cell->info.wcdma;

	if (grilio_parser_get_int32(rilp, &wcdma->mcc) &&
		grilio_parser_get_int32(rilp, &wcdma->mnc) &&
		grilio_parser_get_int32(rilp, &wcdma->lac) &&
		grilio_parser_get_int32(rilp, &wcdma->cid) &&
		grilio_parser_get_int32(rilp, &wcdma->psc) &&
		grilio_parser_get_int32(rilp, &wcdma->signalStrength) &&
		grilio_parser_get_int32(rilp, &wcdma->bitErrorRate)) {
		DBG("[wcdma] reg=%d,mcc=%d,mnc=%d,lac=%d,cid=%d,psc=%d,"
			"strength=%d,err=%d", registered, wcdma->mcc,
			wcdma->mnc, wcdma->lac, wcdma->cid, wcdma->psc,
			wcdma->signalStrength, wcdma->bitErrorRate);
		cell->type = RIL_CELL_INFO_TYPE_WCDMA;
		cell->registered = registered;
		return cell;
	}

	ofono_error("failed to parse WCDMA cell info");
	g_free(cell);
	return NULL;
}

static struct ril_cell *ril_cell_info_parse_cell_lte(GRilIoParser *rilp,
							gboolean registered)
{
	struct ril_cell *cell = g_new0(struct ril_cell, 1);
	struct ril_cell_info_lte *lte = &cell->info.lte;

	if (grilio_parser_get_int32(rilp, &lte->mcc) &&
		grilio_parser_get_int32(rilp, &lte->mnc) &&
		grilio_parser_get_int32(rilp, &lte->ci) &&
		grilio_parser_get_int32(rilp, &lte->pci) &&
		grilio_parser_get_int32(rilp, &lte->tac) &&
		grilio_parser_get_int32(rilp, &lte->signalStrength) &&
		grilio_parser_get_int32(rilp, &lte->rsrp) &&
		grilio_parser_get_int32(rilp, &lte->rsrq) &&
		grilio_parser_get_int32(rilp, &lte->rssnr) &&
		grilio_parser_get_int32(rilp, &lte->cqi) &&
		grilio_parser_get_int32(rilp, &lte->timingAdvance)) {
		DBG("[lte] reg=%d,mcc=%d,mnc=%d,ci=%d,pci=%d,tac=%d,"
			"strength=%d,rsrp=%d,rsrq=0x%x,rssnr=0x%x,cqi=%d,"
			"t=0x%x", registered, lte->mcc, lte->mnc, lte->ci,
			lte->pci, lte->tac, lte->signalStrength, lte->rsrp,
			lte->rsrq, lte->rssnr, lte->cqi, lte->timingAdvance);
		cell->type = RIL_CELL_INFO_TYPE_LTE;
		cell->registered = registered;
		return cell;
	}

	ofono_error("failed to parse LTE cell info");
	g_free(cell);
	return NULL;
}

static enum ril_cell_info_type ril_cell_info_parse_cell(GRilIoParser *rilp,
						struct ril_cell **cell_ptr)
{
	int type, reg;

	if (grilio_parser_get_int32(rilp, &type) &&
			grilio_parser_get_int32(rilp, &reg) &&
			grilio_parser_get_int32_array(rilp, NULL, 3)) {
		int skip = 0;
		struct ril_cell *cell = NULL;

		switch (type) {
		case RIL_CELL_INFO_TYPE_GSM:
			cell = ril_cell_info_parse_cell_gsm(rilp, reg);
			break;
		case RIL_CELL_INFO_TYPE_WCDMA:
			cell = ril_cell_info_parse_cell_wcdma(rilp, reg);
			break;
		case RIL_CELL_INFO_TYPE_LTE:
			cell = ril_cell_info_parse_cell_lte(rilp, reg);
			break;
		case RIL_CELL_INFO_TYPE_CDMA:
			skip = 10;
			break;
		case RIL_CELL_INFO_TYPE_TD_SCDMA:
			skip = 6;
			break;
		default:
			skip = 0;
			break;
		}

		if (cell) {
			*cell_ptr = cell;
			return type;
		}

		if (skip && grilio_parser_get_int32_array(rilp, NULL, skip)) {
			*cell_ptr = NULL;
			return type;
		}
	}

	*cell_ptr = NULL;
	return RIL_CELL_INFO_TYPE_NONE;
}

static GSList *ril_cell_info_parse_list(const void *data, guint len)
{
	GSList *l = NULL;
	GRilIoParser rilp;
	int i, n;

	grilio_parser_init(&rilp, data, len);
	if (grilio_parser_get_int32(&rilp, &n) && n > 0) {
		struct ril_cell *c;

		DBG("%d cell(s):", n);
		for (i=0; i<n && ril_cell_info_parse_cell(&rilp, &c); i++) {
			if (c) {
				l = g_slist_insert_sorted(l, c,
						ril_cell_compare_func);
			}
		}
	}

	return l;
}

static void ril_cell_info_list_changed_cb(GRilIoChannel *io, guint code,
				const void *data, guint len, void *user_data)
{
	struct ril_cell_info *self = RIL_CELL_INFO(user_data);

	DBG_(self, "");
	ril_cell_info_update_cells(self, ril_cell_info_parse_list(data, len));
}

static void ril_cell_info_list_cb(GRilIoChannel *io, int status,
				const void *data, guint len, void *user_data)
{
	struct ril_cell_info *self = RIL_CELL_INFO(user_data);
	struct ril_cell_info_priv *priv = self->priv;

	DBG_(self, "");
	GASSERT(priv->query_id);
	priv->query_id = 0;
	ril_cell_info_update_cells(self, ril_cell_info_parse_list(data, len));
}

static void ril_cell_info_set_rate_cb(GRilIoChannel *io, int status,
				const void *data, guint len, void *user_data)
{
	struct ril_cell_info *self = RIL_CELL_INFO(user_data);
	struct ril_cell_info_priv *priv = self->priv;

	DBG_(self, "");
	GASSERT(priv->set_rate_id);
	priv->set_rate_id = 0;
}

static void ril_cell_info_query(struct ril_cell_info *self)
{
	struct ril_cell_info_priv *priv = self->priv;
	GRilIoRequest *req = grilio_request_new();

	grilio_request_set_retry(req, RIL_RETRY_MS, -1);
	grilio_channel_cancel_request(priv->io, priv->query_id, FALSE);
	priv->query_id = grilio_channel_send_request_full(priv->io, req,
		RIL_REQUEST_GET_CELL_INFO_LIST, ril_cell_info_list_cb,
		NULL, self);
	grilio_request_unref(req);
}

static void ril_cell_info_set_rate(struct ril_cell_info *self, int ms)
{
	struct ril_cell_info_priv *priv = self->priv;
	GRilIoRequest *req = grilio_request_sized_new(8);

	grilio_request_append_int32(req, 1);
	grilio_request_append_int32(req, ms);
	grilio_request_set_retry(req, RIL_RETRY_MS, -1);
	grilio_channel_cancel_request(priv->io, priv->set_rate_id, FALSE);
	priv->set_rate_id = grilio_channel_send_request_full(priv->io, req,
			RIL_REQUEST_SET_UNSOL_CELL_INFO_LIST_RATE,
			ril_cell_info_set_rate_cb, NULL, self);
	grilio_request_unref(req);
}

static void ril_cell_info_update_rate(struct ril_cell_info *self)
{
	struct ril_cell_info_priv *priv = self->priv;

	ril_cell_info_set_rate(self,
		(priv->mce->display_state == RIL_MCE_DISPLAY_OFF) ?
		DISPLAY_OFF_UPDATE_RATE : DISPLAY_ON_UPDATE_RATE);
}

static void ril_cell_info_display_state_cb(struct ril_mce *mce, void *arg)
{
	struct ril_cell_info *self = RIL_CELL_INFO(arg);
	struct ril_cell_info_priv *priv = self->priv;

	if (priv->sim_card_ready) {
		ril_cell_info_update_rate(self);
	}
}

static void ril_cell_info_refresh(struct ril_cell_info *self)
{
	struct ril_cell_info_priv *priv = self->priv;

	/* RIL_REQUEST_GET_CELL_INFO_LIST fails without SIM card */
	if (priv->radio->state == RADIO_STATE_ON && priv->sim_card_ready) {
		ril_cell_info_query(self);
	} else {
		ril_cell_info_update_cells(self, NULL);
	}
}

static void ril_cell_info_radio_state_cb(struct ril_radio *radio, void *arg)
{
	struct ril_cell_info *self = RIL_CELL_INFO(arg);

	DBG_(self, "%s", ril_radio_state_to_string(radio->state));
	ril_cell_info_refresh(self);
}

static void ril_cell_info_sim_status_cb(struct ril_sim_card *sim, void *arg)
{
	struct ril_cell_info *self = RIL_CELL_INFO(arg);
	struct ril_cell_info_priv *priv = self->priv;
	const gboolean sim_card_was_ready = priv->sim_card_ready;

	DBG_(self, "%sready", ril_sim_card_ready(sim) ? "" : "not ");
	priv->sim_card_ready = ril_sim_card_ready(sim);
	if (priv->sim_card_ready != sim_card_was_ready) {
		ril_cell_info_refresh(self);
		if (priv->sim_card_ready) {
			ril_cell_info_update_rate(self);
		}
	}
}

gulong ril_cell_info_add_cells_changed_handler(struct ril_cell_info *self,
					ril_cell_info_cb_t cb, void *arg)
{
	return (G_LIKELY(self) && G_LIKELY(cb)) ? g_signal_connect(self,
		SIGNAL_CELLS_CHANGED_NAME, G_CALLBACK(cb), arg) : 0;
}

void ril_cell_info_remove_handler(struct ril_cell_info *self, gulong id)
{
	if (G_LIKELY(self) && G_LIKELY(id)) {
		g_signal_handler_disconnect(self, id);
	}
}

struct ril_cell_info *ril_cell_info_new(GRilIoChannel *io,
		const char *log_prefix, struct ril_mce *mce,
		struct ril_radio *radio, struct ril_sim_card *sim_card)
{
	struct ril_cell_info *self = g_object_new(RIL_CELL_INFO_TYPE, 0);
	struct ril_cell_info_priv *priv = self->priv;

	priv->io = grilio_channel_ref(io);
	priv->mce = ril_mce_ref(mce);
	priv->radio = ril_radio_ref(radio);
	priv->sim_card = ril_sim_card_ref(sim_card);
	priv->log_prefix = (log_prefix && log_prefix[0]) ?
		g_strconcat(log_prefix, " ", NULL) : g_strdup("");
	DBG_(self, "");
	priv->event_id = grilio_channel_add_unsol_event_handler(priv->io,
		ril_cell_info_list_changed_cb, RIL_UNSOL_CELL_INFO_LIST, self);
	priv->display_state_event_id =
		ril_mce_add_display_state_changed_handler(mce,
			ril_cell_info_display_state_cb, self);
	priv->radio_state_event_id =
		ril_radio_add_state_changed_handler(radio,
			ril_cell_info_radio_state_cb, self);
	priv->sim_status_event_id =
		ril_sim_card_add_status_changed_handler(priv->sim_card,
			ril_cell_info_sim_status_cb, self);
	priv->sim_card_ready = ril_sim_card_ready(sim_card);
	if (priv->sim_card_ready) {
		ril_cell_info_query(self);
		ril_cell_info_update_rate(self);
	}
	return self;
}

struct ril_cell_info *ril_cell_info_ref(struct ril_cell_info *self)
{
	if (G_LIKELY(self)) {
		g_object_ref(RIL_CELL_INFO(self));
		return self;
	} else {
		return NULL;
	}
}

void ril_cell_info_unref(struct ril_cell_info *self)
{
	if (G_LIKELY(self)) {
		g_object_unref(RIL_CELL_INFO(self));
	}
}

static void ril_cell_info_init(struct ril_cell_info *self)
{
	self->priv = G_TYPE_INSTANCE_GET_PRIVATE(self, RIL_CELL_INFO_TYPE,
						struct ril_cell_info_priv);
}

static void ril_cell_info_dispose(GObject *object)
{
	struct ril_cell_info *self = RIL_CELL_INFO(object);
	struct ril_cell_info_priv *priv = self->priv;

	grilio_channel_remove_handlers(priv->io, &priv->event_id, 1);
	if (priv->query_id) {
		grilio_channel_cancel_request(priv->io, priv->query_id, FALSE);
		priv->query_id = 0;
	}
	if (priv->set_rate_id) {
		grilio_channel_cancel_request(priv->io, priv->set_rate_id,
									FALSE);
		priv->set_rate_id = 0;
	}
	if (priv->display_state_event_id) {
		ril_mce_remove_handler(priv->mce, priv->display_state_event_id);
		priv->display_state_event_id = 0;
	}
	ril_radio_remove_handlers(priv->radio, &priv->radio_state_event_id, 1);
	ril_sim_card_remove_handlers(priv->sim_card,
					&priv->sim_status_event_id, 1);
	G_OBJECT_CLASS(ril_cell_info_parent_class)->dispose(object);
}

static void ril_cell_info_finalize(GObject *object)
{
	struct ril_cell_info *self = RIL_CELL_INFO(object);
	struct ril_cell_info_priv *priv = self->priv;

	DBG_(self, "");
	g_free(priv->log_prefix);
	grilio_channel_unref(priv->io);
	ril_mce_unref(priv->mce);
	ril_radio_unref(priv->radio);
	ril_sim_card_unref(priv->sim_card);
	g_slist_free_full(self->cells, g_free);
	G_OBJECT_CLASS(ril_cell_info_parent_class)->finalize(object);
}

static void ril_cell_info_class_init(RilCellInfoClass *klass)
{
	GObjectClass *object_class = G_OBJECT_CLASS(klass);

	object_class->dispose = ril_cell_info_dispose;
	object_class->finalize = ril_cell_info_finalize;
	g_type_class_add_private(klass, sizeof(struct ril_cell_info_priv));
	ril_cell_info_signals[SIGNAL_CELLS_CHANGED] =
		g_signal_new(SIGNAL_CELLS_CHANGED_NAME,
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
