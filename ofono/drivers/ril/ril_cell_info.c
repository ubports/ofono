/*
 *  oFono - Open Source Telephony - RIL-based devices
 *
 *  Copyright (C) 2016-2020 Jolla Ltd.
 *  Copyright (C) 2020 Open Mobile Platform LLC.
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
#include "ril_log.h"

#include <grilio_channel.h>
#include <grilio_request.h>
#include <grilio_parser.h>

#include <gutil_idlepool.h>
#include <gutil_misc.h>

#define DEFAULT_UPDATE_RATE_MS  (10000) /* 10 sec */
#define MAX_RETRIES             (5)

typedef GObjectClass RilCellInfoClass;
typedef struct ril_cell_info RilCellInfo;

struct ril_cell_info {
	GObject object;
	struct sailfish_cell_info info;
	GRilIoChannel *io;
	struct ril_radio *radio;
	struct ril_sim_card *sim_card;
	gulong radio_state_event_id;
	gulong sim_status_event_id;
	gboolean sim_card_ready;
	int update_rate_ms;
	char *log_prefix;
	gulong event_id;
	guint query_id;
	guint set_rate_id;
	gboolean enabled;
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

#define DBG_(self,fmt,args...) DBG("%s" fmt, (self)->log_prefix, ##args)

static inline void ril_cell_free(struct sailfish_cell *cell)
{
	g_slice_free(struct sailfish_cell, cell);
}

static void ril_cell_free1(gpointer cell)
{
	ril_cell_free(cell);
}

static const char *ril_cell_info_int_format(int value, const char *format)
{
	if (value == SAILFISH_CELL_INVALID_VALUE) {
		return "";
	} else {
		static GUtilIdlePool *ril_cell_info_pool = NULL;
		GUtilIdlePool *pool = gutil_idle_pool_get(&ril_cell_info_pool);
		char *str = g_strdup_printf(format, value);

		gutil_idle_pool_add(pool, str, g_free);
		return str;
	}
}

static gboolean ril_cell_info_list_identical(GSList *l1, GSList *l2)
{
	while (l1 && l2) {
		if (memcmp(l1->data, l2->data, sizeof(struct sailfish_cell))) {
			return FALSE;
		}
		l1 = l1->next;
		l2 = l2->next;
	}
	return !l1 && !l2;
}

static void ril_cell_info_update_cells(struct ril_cell_info *self, GSList *l)
{
	if (!ril_cell_info_list_identical(self->info.cells, l)) {
		g_slist_free_full(self->info.cells, ril_cell_free1);
		self->info.cells = l;
		g_signal_emit(self, ril_cell_info_signals
			[SIGNAL_CELLS_CHANGED], 0);
	} else {
		g_slist_free_full(l, ril_cell_free1);
	}
}

static struct sailfish_cell *ril_cell_info_parse_cell_gsm(GRilIoParser *rilp,
					guint version, gboolean registered)
{
	struct sailfish_cell *cell = g_slice_new0(struct sailfish_cell);
	struct sailfish_cell_info_gsm *gsm = &cell->info.gsm;

	/* Optional RIL_CellIdentityGsm_v12 part */
	gsm->arfcn = SAILFISH_CELL_INVALID_VALUE;
	gsm->bsic = SAILFISH_CELL_INVALID_VALUE;
	/* Optional RIL_GSM_SignalStrength_v12 part */
	gsm->timingAdvance = SAILFISH_CELL_INVALID_VALUE;
	/* RIL_CellIdentityGsm */
	if (grilio_parser_get_int32(rilp, &gsm->mcc) &&
		grilio_parser_get_int32(rilp, &gsm->mnc) &&
		grilio_parser_get_int32(rilp, &gsm->lac) &&
		grilio_parser_get_int32(rilp, &gsm->cid) &&
		(version < 12 || /* RIL_CellIdentityGsm_v12 part */
			(grilio_parser_get_int32(rilp, &gsm->arfcn) &&
			grilio_parser_get_int32(rilp, &gsm->bsic))) &&
		/* RIL_GW_SignalStrength */
		grilio_parser_get_int32(rilp, &gsm->signalStrength) &&
		grilio_parser_get_int32(rilp, &gsm->bitErrorRate) &&
		(version < 12 || /* RIL_GSM_SignalStrength_v12 part */
			grilio_parser_get_int32(rilp, &gsm->timingAdvance))) {
		DBG("[gsm] reg=%d%s%s%s%s%s%s%s%s%s", registered,
			ril_cell_info_int_format(gsm->mcc, ",mcc=%d"),
			ril_cell_info_int_format(gsm->mnc, ",mnc=%d"),
			ril_cell_info_int_format(gsm->lac, ",lac=%d"),
			ril_cell_info_int_format(gsm->cid, ",cid=%d"),
			ril_cell_info_int_format(gsm->arfcn, ",arfcn=%d"),
			ril_cell_info_int_format(gsm->bsic, ",bsic=%d"),
			ril_cell_info_int_format(gsm->signalStrength,
							",strength=%d"),
			ril_cell_info_int_format(gsm->bitErrorRate, ",err=%d"),
			ril_cell_info_int_format(gsm->timingAdvance, ",t=%d"));
		cell->type = SAILFISH_CELL_TYPE_GSM;
		cell->registered = registered;
		return cell;
	}

	ofono_error("failed to parse GSM cell info");
	ril_cell_free(cell);
	return NULL;
}

static struct sailfish_cell *ril_cell_info_parse_cell_wcdma(GRilIoParser *rilp,
					guint version, gboolean registered)
{
	struct sailfish_cell *cell = g_slice_new0(struct sailfish_cell);
	struct sailfish_cell_info_wcdma *wcdma = &cell->info.wcdma;

	/* Optional RIL_CellIdentityWcdma_v12 part */
	wcdma->uarfcn = SAILFISH_CELL_INVALID_VALUE;
	if (grilio_parser_get_int32(rilp, &wcdma->mcc) &&
		grilio_parser_get_int32(rilp, &wcdma->mnc) &&
		grilio_parser_get_int32(rilp, &wcdma->lac) &&
		grilio_parser_get_int32(rilp, &wcdma->cid) &&
		grilio_parser_get_int32(rilp, &wcdma->psc) &&
		(version < 12 || /* RIL_CellIdentityWcdma_v12 part */
			grilio_parser_get_int32(rilp, &wcdma->uarfcn)) &&
		grilio_parser_get_int32(rilp, &wcdma->signalStrength) &&
		grilio_parser_get_int32(rilp, &wcdma->bitErrorRate)) {
		DBG("[wcdma] reg=%d%s%s%s%s%s%s%s", registered,
			ril_cell_info_int_format(wcdma->mcc, ",mcc=%d"),
			ril_cell_info_int_format(wcdma->mnc, ",mnc=%d"),
			ril_cell_info_int_format(wcdma->lac, ",lac=%d"),
			ril_cell_info_int_format(wcdma->cid, ",cid=%d"),
			ril_cell_info_int_format(wcdma->psc, ",psc=%d"),
			ril_cell_info_int_format(wcdma->signalStrength,
							",strength=%d"),
			ril_cell_info_int_format(wcdma->bitErrorRate,
							",err=%d"));
		cell->type = SAILFISH_CELL_TYPE_WCDMA;
		cell->registered = registered;
		return cell;
	}

	ofono_error("failed to parse WCDMA cell info");
	ril_cell_free(cell);
	return NULL;
}

static struct sailfish_cell *ril_cell_info_parse_cell_lte(GRilIoParser *rilp,
					guint version, gboolean registered)
{
	struct sailfish_cell *cell = g_slice_new0(struct sailfish_cell);
	struct sailfish_cell_info_lte *lte = &cell->info.lte;

	/* Optional RIL_CellIdentityLte_v12 part */
	lte->earfcn = SAILFISH_CELL_INVALID_VALUE;
	if (grilio_parser_get_int32(rilp, &lte->mcc) &&
		grilio_parser_get_int32(rilp, &lte->mnc) &&
		grilio_parser_get_int32(rilp, &lte->ci) &&
		grilio_parser_get_int32(rilp, &lte->pci) &&
		grilio_parser_get_int32(rilp, &lte->tac) &&
		(version < 12 || /* RIL_CellIdentityLte_v12 part */
			grilio_parser_get_int32(rilp, &lte->earfcn)) &&
		grilio_parser_get_int32(rilp, &lte->signalStrength) &&
		grilio_parser_get_int32(rilp, &lte->rsrp) &&
		grilio_parser_get_int32(rilp, &lte->rsrq) &&
		grilio_parser_get_int32(rilp, &lte->rssnr) &&
		grilio_parser_get_int32(rilp, &lte->cqi) &&
		grilio_parser_get_int32(rilp, &lte->timingAdvance)) {
		DBG("[lte] reg=%d%s%s%s%s%s%s%s%s%s%s%s", registered,
			ril_cell_info_int_format(lte->mcc, ",mcc=%d"),
			ril_cell_info_int_format(lte->mnc, ",mnc=%d"),
			ril_cell_info_int_format(lte->ci, ",ci=%d"),
			ril_cell_info_int_format(lte->pci, ",pci=%d"),
			ril_cell_info_int_format(lte->tac, ",tac=%d"),
			ril_cell_info_int_format(lte->signalStrength,
							",strength=%d"),
			ril_cell_info_int_format(lte->rsrp, ",rsrp=%d"),
			ril_cell_info_int_format(lte->rsrq, ",rsrq=%d"),
			ril_cell_info_int_format(lte->rssnr, ",rssnr=%d"),
			ril_cell_info_int_format(lte->cqi, ",cqi=%d"),
			ril_cell_info_int_format(lte->timingAdvance, ",t=%d"));
		cell->type = SAILFISH_CELL_TYPE_LTE;
		cell->registered = registered;
		return cell;
	}

	ofono_error("failed to parse LTE cell info");
	ril_cell_free(cell);
	return NULL;
}

static gboolean ril_cell_info_parse_cell(GRilIoParser *rilp, guint v,
					struct sailfish_cell **cell_ptr)
{
	int type, reg;

	if (grilio_parser_get_int32(rilp, &type) &&
			grilio_parser_get_int32(rilp, &reg) &&
			/* Skip timestamp */
			grilio_parser_get_int32_array(rilp, NULL, 3)) {
		int skip = 0;
		struct sailfish_cell *cell = NULL;

		/* Normalize the boolean value */
		reg = (reg != FALSE);

		switch (type) {
		case RIL_CELL_INFO_TYPE_GSM:
			cell = ril_cell_info_parse_cell_gsm(rilp, v, reg);
			break;
		case RIL_CELL_INFO_TYPE_WCDMA:
			cell = ril_cell_info_parse_cell_wcdma(rilp, v, reg);
			break;
		case RIL_CELL_INFO_TYPE_LTE:
			cell = ril_cell_info_parse_cell_lte(rilp, v, reg);
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
			return TRUE;
		}

		if (skip && grilio_parser_get_int32_array(rilp, NULL, skip)) {
			*cell_ptr = NULL;
			return TRUE;
		}
	}

	*cell_ptr = NULL;
	return FALSE;
}

static GSList *ril_cell_info_parse_list(guint v, const void *data, guint len)
{
	GSList *l = NULL;
	GRilIoParser rilp;
	int i, n;

	grilio_parser_init(&rilp, data, len);
	if (grilio_parser_get_int32(&rilp, &n) && n > 0) {
		struct sailfish_cell *c;

		DBG("%d cell(s):", n);
		for (i=0; i<n && ril_cell_info_parse_cell(&rilp, v, &c); i++) {
			if (c) {
				l = g_slist_insert_sorted(l, c,
						sailfish_cell_compare_func);
			}
		}
	}

	GASSERT(grilio_parser_at_end(&rilp));
	return l;
}

static void ril_cell_info_list_changed_cb(GRilIoChannel *io, guint code,
				const void *data, guint len, void *user_data)
{
	struct ril_cell_info *self = RIL_CELL_INFO(user_data);

	DBG_(self, "");
	ril_cell_info_update_cells(self, ril_cell_info_parse_list
						(io->ril_version, data, len));
}

static void ril_cell_info_list_cb(GRilIoChannel *io, int status,
				const void *data, guint len, void *user_data)
{
	struct ril_cell_info *self = RIL_CELL_INFO(user_data);

	DBG_(self, "");
	GASSERT(self->query_id);
	self->query_id = 0;
	ril_cell_info_update_cells(self,
		(status == RIL_E_SUCCESS && self->enabled) ?
		ril_cell_info_parse_list(io->ril_version, data, len) : NULL);
}

static void ril_cell_info_set_rate_cb(GRilIoChannel *io, int status,
				const void *data, guint len, void *user_data)
{
	struct ril_cell_info *self = RIL_CELL_INFO(user_data);

	DBG_(self, "");
	GASSERT(self->set_rate_id);
	self->set_rate_id = 0;
}

static gboolean ril_cell_info_retry(GRilIoRequest* request, int ril_status,
		const void* response_data, guint response_len, void* user_data)
{
	struct ril_cell_info *self = RIL_CELL_INFO(user_data);

	switch (ril_status) {
	case RIL_E_SUCCESS:
	case RIL_E_RADIO_NOT_AVAILABLE:
		return FALSE;
	default:
		return self->enabled;
	}
}

static void ril_cell_info_query(struct ril_cell_info *self)
{
	GRilIoRequest *req = grilio_request_new();

	grilio_request_set_retry(req, RIL_RETRY_MS, MAX_RETRIES);
	grilio_request_set_retry_func(req, ril_cell_info_retry);
	grilio_channel_cancel_request(self->io, self->query_id, FALSE);
	self->query_id = grilio_channel_send_request_full(self->io, req,
		RIL_REQUEST_GET_CELL_INFO_LIST, ril_cell_info_list_cb,
		NULL, self);
	grilio_request_unref(req);
}

static void ril_cell_info_set_rate(struct ril_cell_info *self)
{
	GRilIoRequest *req = grilio_request_array_int32_new(1,
		(self->update_rate_ms >= 0 && self->enabled) ?
		self->update_rate_ms : INT_MAX);

	grilio_request_set_retry(req, RIL_RETRY_MS, MAX_RETRIES);
	grilio_request_set_retry_func(req, ril_cell_info_retry);
	grilio_channel_cancel_request(self->io, self->set_rate_id, FALSE);
	self->set_rate_id = grilio_channel_send_request_full(self->io, req,
			RIL_REQUEST_SET_UNSOL_CELL_INFO_LIST_RATE,
			ril_cell_info_set_rate_cb, NULL, self);
	grilio_request_unref(req);
}

static void ril_cell_info_refresh(struct ril_cell_info *self)
{
	/* RIL_REQUEST_GET_CELL_INFO_LIST fails without SIM card */
	if (self->enabled && self->radio->state == RADIO_STATE_ON &&
						self->sim_card_ready) {
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

	self->sim_card_ready = ril_sim_card_ready(sim);
	DBG_(self, "%sready", self->sim_card_ready ? "" : "not ");
	ril_cell_info_refresh(self);
	if (self->sim_card_ready) {
		ril_cell_info_set_rate(self);
	}
}

/* sailfish_cell_info interface callbacks */

struct ril_cell_info_closure {
	GCClosure cclosure;
	sailfish_cell_info_cb_t cb;
	void *arg;
};

static inline struct ril_cell_info *ril_cell_info_cast
					(struct sailfish_cell_info *info)
{
	return G_CAST(info, struct ril_cell_info, info);
}

static void ril_cell_info_ref_proc(struct sailfish_cell_info *info)
{
	g_object_ref(ril_cell_info_cast(info));
}

static void ril_cell_info_unref_proc(struct sailfish_cell_info *info)
{
	g_object_unref(ril_cell_info_cast(info));
}

static void ril_cell_info_cells_changed_cb(struct ril_cell_info *self,
					struct ril_cell_info_closure *closure)
{
	closure->cb(&self->info, closure->arg);
}

static gulong ril_cell_info_add_cells_changed_handler_proc
				(struct sailfish_cell_info *info,
					sailfish_cell_info_cb_t cb, void *arg)
{
	if (cb) {
		struct ril_cell_info_closure *closure =
			(struct ril_cell_info_closure *) g_closure_new_simple
				(sizeof(struct ril_cell_info_closure), NULL);
		GCClosure* cc = &closure->cclosure;

		cc->closure.data = closure;
		cc->callback = G_CALLBACK(ril_cell_info_cells_changed_cb);
		closure->cb = cb;
		closure->arg = arg;
		return g_signal_connect_closure_by_id(ril_cell_info_cast(info),
				ril_cell_info_signals[SIGNAL_CELLS_CHANGED], 0,
				&cc->closure, FALSE);
	} else {
		return 0;
	}
}

static void ril_cell_info_remove_handler_proc(struct sailfish_cell_info *info,
								gulong id)
{
	if (G_LIKELY(id)) {
		g_signal_handler_disconnect(ril_cell_info_cast(info), id);
	}
}

static void ril_cell_info_set_update_interval_proc
				(struct sailfish_cell_info *info, int ms)
{
	struct ril_cell_info *self = ril_cell_info_cast(info);

	if (self->update_rate_ms != ms) {
		self->update_rate_ms = ms;
		DBG_(self, "%d ms", ms);
		if (self->enabled && self->sim_card_ready) {
			ril_cell_info_set_rate(self);
		}
	}
}

void ril_cell_info_set_enabled_proc(struct sailfish_cell_info *info,
							gboolean enabled)
{
	struct ril_cell_info *self = ril_cell_info_cast(info);

	if (self->enabled != enabled) {
		self->enabled = enabled;
		DBG_(self, "%d", enabled);
		ril_cell_info_refresh(self);
		if (self->sim_card_ready) {
			ril_cell_info_set_rate(self);
		}
	}
}

struct sailfish_cell_info *ril_cell_info_new(GRilIoChannel *io,
			const char *log_prefix, struct ril_radio *radio,
			struct ril_sim_card *sim_card)
{
	static const struct sailfish_cell_info_proc ril_cell_info_proc = {
		ril_cell_info_ref_proc,
		ril_cell_info_unref_proc,
		ril_cell_info_add_cells_changed_handler_proc,
		ril_cell_info_remove_handler_proc,
		ril_cell_info_set_update_interval_proc,
		ril_cell_info_set_enabled_proc
	};

	struct ril_cell_info *self = g_object_new(RIL_CELL_INFO_TYPE, 0);

	self->info.proc = &ril_cell_info_proc;
	self->io = grilio_channel_ref(io);
	self->radio = ril_radio_ref(radio);
	self->sim_card = ril_sim_card_ref(sim_card);
	self->log_prefix = (log_prefix && log_prefix[0]) ?
		g_strconcat(log_prefix, " ", NULL) : g_strdup("");
	DBG_(self, "");
	self->event_id = grilio_channel_add_unsol_event_handler(self->io,
		ril_cell_info_list_changed_cb, RIL_UNSOL_CELL_INFO_LIST, self);
	self->radio_state_event_id =
		ril_radio_add_state_changed_handler(radio,
			ril_cell_info_radio_state_cb, self);
	self->sim_status_event_id =
		ril_sim_card_add_status_changed_handler(self->sim_card,
			ril_cell_info_sim_status_cb, self);
	self->sim_card_ready = ril_sim_card_ready(sim_card);
	ril_cell_info_refresh(self);

	/* Disable updates by default */
	self->enabled = FALSE;
	if (self->sim_card_ready) {
		ril_cell_info_set_rate(self);
	}
	return &self->info;
}

static void ril_cell_info_init(struct ril_cell_info *self)
{
	self->update_rate_ms = DEFAULT_UPDATE_RATE_MS;
}

static void ril_cell_info_dispose(GObject *object)
{
	struct ril_cell_info *self = RIL_CELL_INFO(object);

	grilio_channel_remove_handlers(self->io, &self->event_id, 1);
	if (self->query_id) {
		grilio_channel_cancel_request(self->io, self->query_id, FALSE);
		self->query_id = 0;
	}
	if (self->set_rate_id) {
		grilio_channel_cancel_request(self->io, self->set_rate_id,
									FALSE);
		self->set_rate_id = 0;
	}
	ril_radio_remove_handlers(self->radio, &self->radio_state_event_id, 1);
	ril_sim_card_remove_handlers(self->sim_card,
					&self->sim_status_event_id, 1);
	G_OBJECT_CLASS(ril_cell_info_parent_class)->dispose(object);
}

static void ril_cell_info_finalize(GObject *object)
{
	struct ril_cell_info *self = RIL_CELL_INFO(object);

	DBG_(self, "");
	g_free(self->log_prefix);
	grilio_channel_unref(self->io);
	ril_radio_unref(self->radio);
	ril_sim_card_unref(self->sim_card);
	g_slist_free_full(self->info.cells, ril_cell_free1);
	G_OBJECT_CLASS(ril_cell_info_parent_class)->finalize(object);
}

static void ril_cell_info_class_init(RilCellInfoClass *klass)
{
	GObjectClass *object_class = G_OBJECT_CLASS(klass);

	object_class->dispose = ril_cell_info_dispose;
	object_class->finalize = ril_cell_info_finalize;
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
