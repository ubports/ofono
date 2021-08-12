/*
 *  oFono - Open Source Telephony - RIL-based devices
 *
 *  Copyright (C) 2016-2021 Jolla Ltd.
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
#include <gutil_macros.h>
#include <gutil_misc.h>

#define DEFAULT_UPDATE_RATE_MS  (10000) /* 10 sec */
#define MAX_RETRIES             (5)

typedef GObjectClass RilCellInfoClass;
typedef struct ril_cell_info RilCellInfo;

struct ril_cell_info {
	GObject object;
	struct ofono_cell_info info;
	struct ofono_cell **cells;
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

#define PARENT_TYPE G_TYPE_OBJECT
#define PARENT_CLASS ril_cell_info_parent_class
#define THIS_TYPE (ril_cell_info_get_type())
#define THIS(obj) (G_TYPE_CHECK_INSTANCE_CAST((obj), THIS_TYPE, RilCellInfo))

G_DEFINE_TYPE(RilCellInfo, ril_cell_info, PARENT_TYPE)

#define DBG_(self,fmt,args...) DBG("%s" fmt, (self)->log_prefix, ##args)

static const char *ril_cell_info_int_format(int value, const char *format)
{
	if (value == OFONO_CELL_INVALID_VALUE) {
		return "";
	} else {
		static GUtilIdlePool *ril_cell_info_pool = NULL;
		GUtilIdlePool *pool = gutil_idle_pool_get(&ril_cell_info_pool);
		char *str = g_strdup_printf(format, value);

		gutil_idle_pool_add(pool, str, g_free);
		return str;
	}
}

static gint ril_cell_info_list_sort_cb(gconstpointer a, gconstpointer b)
{
	return ofono_cell_compare_location(*(struct ofono_cell **)a,
		*(struct ofono_cell **)b);
}

static gboolean ril_cell_info_list_identical(const ofono_cell_ptr *l1,
	const ofono_cell_ptr *l2)
{
	if (l1 && l2) {
		while (*l1 && *l2) {
			if (memcmp(*l1, *l2, sizeof(struct ofono_cell))) {
				return FALSE;
			}
			l1++;
			l2++;
		}
		return !*l1 && !*l2;
	} else {
		return (!l1 || !*l1) && (!l2 || !*l2);
	}
}

/* Takes ownership of GPtrArray */
static void ril_cell_info_update_cells(RilCellInfo *self, GPtrArray *l)
{
	if (l && !ril_cell_info_list_identical(self->cells,
		(struct ofono_cell **)l->pdata)) {
		gutil_ptrv_free((void**)self->cells);
		self->info.cells = (struct ofono_cell **)
			g_ptr_array_free(l, FALSE);
		g_signal_emit(self, ril_cell_info_signals
			[SIGNAL_CELLS_CHANGED], 0);
	} else if (l) {
		g_ptr_array_set_free_func(l, g_free);
		g_ptr_array_free(l, TRUE);
	}
}

static struct ofono_cell *ril_cell_info_parse_cell_gsm(GRilIoParser *rilp,
					guint version, gboolean registered)
{
	struct ofono_cell *cell = g_new0(struct ofono_cell, 1);
	struct ofono_cell_info_gsm *gsm = &cell->info.gsm;

	/* Optional RIL_CellIdentityGsm_v12 part */
	gsm->arfcn = OFONO_CELL_INVALID_VALUE;
	gsm->bsic = OFONO_CELL_INVALID_VALUE;
	/* Optional RIL_GSM_SignalStrength_v12 part */
	gsm->timingAdvance = OFONO_CELL_INVALID_VALUE;
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
		cell->type = OFONO_CELL_TYPE_GSM;
		cell->registered = registered;
		return cell;
	}

	ofono_error("failed to parse GSM cell info");
	g_free(cell);
	return NULL;
}

static struct ofono_cell *ril_cell_info_parse_cell_wcdma(GRilIoParser *rilp,
					guint version, gboolean registered)
{
	struct ofono_cell *cell = g_new0(struct ofono_cell, 1);
	struct ofono_cell_info_wcdma *wcdma = &cell->info.wcdma;

	/* Optional RIL_CellIdentityWcdma_v12 part */
	wcdma->uarfcn = OFONO_CELL_INVALID_VALUE;
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
		cell->type = OFONO_CELL_TYPE_WCDMA;
		cell->registered = registered;
		return cell;
	}

	ofono_error("failed to parse WCDMA cell info");
	g_free(cell);
	return NULL;
}

static struct ofono_cell *ril_cell_info_parse_cell_lte(GRilIoParser *rilp,
					guint version, gboolean registered)
{
	struct ofono_cell *cell = g_new0(struct ofono_cell, 1);
	struct ofono_cell_info_lte *lte = &cell->info.lte;

	/* Optional RIL_CellIdentityLte_v12 part */
	lte->earfcn = OFONO_CELL_INVALID_VALUE;
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
		cell->type = OFONO_CELL_TYPE_LTE;
		cell->registered = registered;
		return cell;
	}

	ofono_error("failed to parse LTE cell info");
	g_free(cell);
	return NULL;
}

static gboolean ril_cell_info_parse_cell(GRilIoParser *rilp, guint v,
					struct ofono_cell **cell_ptr)
{
	int type, reg;

	if (grilio_parser_get_int32(rilp, &type) &&
			grilio_parser_get_int32(rilp, &reg) &&
			/* Skip timestamp */
			grilio_parser_get_int32_array(rilp, NULL, 3)) {
		int skip = 0;
		struct ofono_cell *cell = NULL;

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

static GPtrArray *ril_cell_info_parse_list(guint v, const void *data, guint len)
{
	GPtrArray *l = NULL;
	GRilIoParser rilp;
	int i, n;

	grilio_parser_init(&rilp, data, len);
	if (grilio_parser_get_int32(&rilp, &n) && n > 0) {
		struct ofono_cell *c;

		l = g_ptr_array_sized_new(n + 1);
		DBG("%d cell(s):", n);
		for (i=0; i<n && ril_cell_info_parse_cell(&rilp, v, &c); i++) {
			if (c) {
				g_ptr_array_add(l, c);
			}
		}
		g_ptr_array_sort(l, ril_cell_info_list_sort_cb);
		g_ptr_array_add(l, NULL);
	}

	GASSERT(grilio_parser_at_end(&rilp));
	return l;
}

static void ril_cell_info_list_changed_cb(GRilIoChannel *io, guint code,
				const void *data, guint len, void *user_data)
{
	RilCellInfo *self = THIS(user_data);

	DBG_(self, "");
	ril_cell_info_update_cells(self, ril_cell_info_parse_list
						(io->ril_version, data, len));
}

static void ril_cell_info_list_cb(GRilIoChannel *io, int status,
				const void *data, guint len, void *user_data)
{
	RilCellInfo *self = THIS(user_data);

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
	RilCellInfo *self = THIS(user_data);

	DBG_(self, "");
	GASSERT(self->set_rate_id);
	self->set_rate_id = 0;
}

static gboolean ril_cell_info_retry(GRilIoRequest *request, int ril_status,
		const void *response_data, guint response_len, void *user_data)
{
	RilCellInfo *self = THIS(user_data);

	switch (ril_status) {
	case RIL_E_SUCCESS:
	case RIL_E_RADIO_NOT_AVAILABLE:
		return FALSE;
	default:
		return self->enabled;
	}
}

static void ril_cell_info_query(RilCellInfo *self)
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

static void ril_cell_info_set_rate(RilCellInfo *self)
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

static void ril_cell_info_refresh(RilCellInfo *self)
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
	RilCellInfo *self = THIS(arg);

	DBG_(self, "%s", ril_radio_state_to_string(radio->state));
	ril_cell_info_refresh(self);
}

static void ril_cell_info_sim_status_cb(struct ril_sim_card *sim, void *arg)
{
	RilCellInfo *self = THIS(arg);

	self->sim_card_ready = ril_sim_card_ready(sim);
	DBG_(self, "%sready", self->sim_card_ready ? "" : "not ");
	ril_cell_info_refresh(self);
	if (self->sim_card_ready) {
		ril_cell_info_set_rate(self);
	}
}

/* ofono_cell_info interface callbacks */

typedef struct ril_cell_info_closure {
	GCClosure cclosure;
	ofono_cell_info_cb_t cb;
	void *arg;
} RilCellInfoClosure;

static inline RilCellInfo *ril_cell_info_cast(struct ofono_cell_info *info)
{
	return G_CAST(info, RilCellInfo, info);
}

static void ril_cell_info_ref_proc(struct ofono_cell_info *info)
{
	g_object_ref(ril_cell_info_cast(info));
}

static void ril_cell_info_unref_proc(struct ofono_cell_info *info)
{
	g_object_unref(ril_cell_info_cast(info));
}

static void ril_cell_info_cells_changed_cb(RilCellInfo *self,
	RilCellInfoClosure *closure)
{
	closure->cb(&self->info, closure->arg);
}

static gulong ril_cell_info_add_cells_changed_handler_proc
	(struct ofono_cell_info *info, ofono_cell_info_cb_t cb, void *arg)
{
	if (cb) {
		RilCellInfoClosure *closure = (RilCellInfoClosure *)
			g_closure_new_simple(sizeof(RilCellInfoClosure), NULL);
		GCClosure *cc = &closure->cclosure;

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

static void ril_cell_info_remove_handler_proc(struct ofono_cell_info *info,
	gulong id)
{
	if (G_LIKELY(id)) {
		g_signal_handler_disconnect(ril_cell_info_cast(info), id);
	}
}

static void ril_cell_info_set_update_interval_proc
	(struct ofono_cell_info *info, int ms)
{
	RilCellInfo *self = ril_cell_info_cast(info);

	if (self->update_rate_ms != ms) {
		self->update_rate_ms = ms;
		DBG_(self, "%d ms", ms);
		if (self->enabled && self->sim_card_ready) {
			ril_cell_info_set_rate(self);
		}
	}
}

void ril_cell_info_set_enabled_proc(struct ofono_cell_info *info,
	gboolean enabled)
{
	RilCellInfo *self = ril_cell_info_cast(info);

	if (self->enabled != enabled) {
		self->enabled = enabled;
		DBG_(self, "%d", enabled);
		ril_cell_info_refresh(self);
		if (self->sim_card_ready) {
			ril_cell_info_set_rate(self);
		}
	}
}

struct ofono_cell_info *ril_cell_info_new(GRilIoChannel *io,
	const char *log_prefix, struct ril_radio *radio,
	struct ril_sim_card *sim_card)
{
	RilCellInfo *self = g_object_new(THIS_TYPE, 0);

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

static void ril_cell_info_init(RilCellInfo *self)
{
	static const struct ofono_cell_info_proc ril_cell_info_proc = {
		ril_cell_info_ref_proc,
		ril_cell_info_unref_proc,
		ril_cell_info_add_cells_changed_handler_proc,
		ril_cell_info_remove_handler_proc,
		ril_cell_info_set_update_interval_proc,
		ril_cell_info_set_enabled_proc
	};

	self->update_rate_ms = DEFAULT_UPDATE_RATE_MS;
	self->info.cells = self->cells = g_new0(struct ofono_cell*, 1);
	self->info.proc = &ril_cell_info_proc;
}

static void ril_cell_info_dispose(GObject *object)
{
	RilCellInfo *self = THIS(object);

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
	/* xxx_remove_handlers() zero the ids */
	ril_radio_remove_handlers(self->radio,
		&self->radio_state_event_id, 1);
	ril_sim_card_remove_handlers(self->sim_card,
		&self->sim_status_event_id, 1);
	G_OBJECT_CLASS(PARENT_CLASS)->dispose(object);
}

static void ril_cell_info_finalize(GObject *object)
{
	RilCellInfo *self = THIS(object);

	DBG_(self, "");
	gutil_ptrv_free((void**)self->cells);
	g_free(self->log_prefix);
	grilio_channel_unref(self->io);
	ril_radio_unref(self->radio);
	ril_sim_card_unref(self->sim_card);
	G_OBJECT_CLASS(PARENT_CLASS)->finalize(object);
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
