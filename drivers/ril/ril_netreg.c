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
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 */

#include "ril_plugin.h"
#include "ril_network.h"
#include "ril_util.h"
#include "ril_log.h"

#include "common.h"
#include "simutil.h"

#define REGISTRATION_TIMEOUT (100*1000) /* ms */
#define REGISTRATION_MAX_RETRIES (2)

enum ril_netreg_events {
	NETREG_RIL_EVENT_NITZ_TIME_RECEIVED,
	NETREG_RIL_EVENT_SIGNAL_STRENGTH,
	NETREG_RIL_EVENT_COUNT
};

enum ril_netreg_network_events {
	NETREG_NETWORK_EVENT_OPERATOR_CHANGED,
	NETREG_NETWORK_EVENT_VOICE_STATE_CHANGED,
	NETREG_NETWORK_EVENT_COUNT
};

struct ril_netreg {
	GRilIoChannel *io;
	GRilIoQueue *q;
	struct ofono_netreg *netreg;
	struct ril_network *network;
	char *log_prefix;
	guint timer_id;
	guint notify_id;
	guint current_operator_id;
	gulong ril_event_id[NETREG_RIL_EVENT_COUNT];
	gulong network_event_id[NETREG_NETWORK_EVENT_COUNT];
};

struct ril_netreg_cbd {
	struct ril_netreg *nd;
	union {
		ofono_netreg_status_cb_t status;
		ofono_netreg_operator_cb_t operator;
		ofono_netreg_operator_list_cb_t operator_list;
		ofono_netreg_register_cb_t reg;
		ofono_netreg_strength_cb_t strength;
		gpointer ptr;
	} cb;
	gpointer data;
};

#define ril_netreg_cbd_free g_free

#define DBG_(nd,fmt,args...) DBG("%s" fmt, (nd)->log_prefix, ##args)

static inline struct ril_netreg *ril_netreg_get_data(struct ofono_netreg *ofono)
{
	return ofono ? ofono_netreg_get_data(ofono) : NULL;
}

static struct ril_netreg_cbd *ril_netreg_cbd_new(struct ril_netreg *nd,
						void *cb, void *data)
{
	struct ril_netreg_cbd *cbd = g_new0(struct ril_netreg_cbd, 1);

	cbd->nd = nd;
	cbd->cb.ptr = cb;
	cbd->data = data;
	return cbd;
}

int ril_netreg_check_if_really_roaming(struct ofono_netreg *netreg,
								gint status)
{
	if (status == NETWORK_REGISTRATION_STATUS_ROAMING) {
		/* These functions tolerate NULL argument */
		const char *net_mcc = ofono_netreg_get_mcc(netreg);
		const char *net_mnc = ofono_netreg_get_mnc(netreg);
		struct sim_spdi *spdi = ofono_netreg_get_spdi(netreg);

		if (spdi && net_mcc && net_mnc) {
			if (sim_spdi_lookup(spdi, net_mcc, net_mnc)) {
				ofono_info("not roaming based on spdi");
				return NETWORK_REGISTRATION_STATUS_REGISTERED;
			}
		}
	}

	return status;
}

static int ril_netreg_check_status(struct ril_netreg *nd, int status)
{
	return (nd && nd->netreg) ?
		ril_netreg_check_if_really_roaming(nd->netreg, status) :
		status;
}

static gboolean ril_netreg_status_notify_cb(gpointer user_data)
{
	struct ril_netreg *nd = user_data;
	const struct ril_registration_state *reg = &nd->network->voice;

	DBG_(nd, "");
	GASSERT(nd->notify_id);
	nd->notify_id = 0;
	ofono_netreg_status_notify(nd->netreg,
			ril_netreg_check_status(nd, reg->status),
			reg->lac, reg->ci, reg->access_tech);
	return FALSE;
}

static void ril_netreg_status_notify(struct ril_network *net, void *user_data)
{
	struct ril_netreg *nd = user_data;

	/* Coalesce multiple notifications into one */
	if (nd->notify_id) {
		DBG_(nd, "notification aready queued");
	} else {
		DBG_(nd, "queuing notification");
		nd->notify_id = g_idle_add(ril_netreg_status_notify_cb, nd);
	}
}

static void ril_netreg_registration_status(struct ofono_netreg *netreg,
			ofono_netreg_status_cb_t cb, void *data)
{
	struct ril_netreg *nd = ril_netreg_get_data(netreg);
	const struct ril_registration_state *reg = &nd->network->voice;
	struct ofono_error error;

	DBG_(nd, "");
	cb(ril_error_ok(&error),
			ril_netreg_check_status(nd, reg->status),
			reg->lac, reg->ci, reg->access_tech, data);
}

static gboolean ril_netreg_current_operator_cb(void *user_data)
{
	struct ril_netreg_cbd *cbd = user_data;
	struct ril_netreg *nd = cbd->nd;
	ofono_netreg_operator_cb_t cb = cbd->cb.operator;
	struct ofono_error error;

	DBG_(nd, "");
	GASSERT(nd->current_operator_id);
	nd->current_operator_id = 0;

	cb(ril_error_ok(&error), nd->network->operator, cbd->data);
	return FALSE;
}

static void ril_netreg_current_operator(struct ofono_netreg *netreg,
				ofono_netreg_operator_cb_t cb, void *data)
{
	struct ril_netreg *nd = ril_netreg_get_data(netreg);

	/*
	 * Calling ofono_netreg_status_notify() may result in
	 * ril_netreg_current_operator() being invoked even if one
	 * is already pending. Since ofono core doesn't associate
	 * any context with individual calls, we can safely assume
	 * that such a call essentially cancels the previous one.
	 */
	if (nd->current_operator_id) {
		g_source_remove(nd->current_operator_id);
	}

	nd->current_operator_id = g_idle_add_full(G_PRIORITY_DEFAULT_IDLE,
					ril_netreg_current_operator_cb,
					ril_netreg_cbd_new(nd, cb, data),
					ril_netreg_cbd_free);
}

static void ril_netreg_list_operators_cb(GRilIoChannel *io, int status,
				const void *data, guint len, void *user_data)
{
	struct ril_netreg_cbd *cbd = user_data;
	ofono_netreg_operator_list_cb_t cb = cbd->cb.operator_list;
	struct ofono_network_operator *list;
	struct ofono_error error;
	int noperators = 0, i;
	GRilIoParser rilp;
	gboolean ok = TRUE;

	if (status != RIL_E_SUCCESS) {
		ofono_error("Failed to retrive the list of operators: %s",
						ril_error_to_string(status));
		cb(ril_error_failure(&error), 0, NULL, cbd->data);
		return;
	}

	grilio_parser_init(&rilp, data, len);

	/* Number of operators at the list (4 strings for every operator) */
	grilio_parser_get_int32(&rilp, &noperators);
	GASSERT(!(noperators % 4));
	noperators /= 4;
	ofono_info("noperators = %d", noperators);

	list = g_new0(struct ofono_network_operator, noperators);
	for (i = 0; i < noperators && ok; i++) {
		struct ofono_network_operator *op = list + i;
		char *lalpha = grilio_parser_get_utf8(&rilp);
		char *salpha = grilio_parser_get_utf8(&rilp);
		char *numeric = grilio_parser_get_utf8(&rilp);
		char *status = grilio_parser_get_utf8(&rilp);

		/* Try to use long by default */
		if (lalpha) {
			strncpy(op->name, lalpha,
					OFONO_MAX_OPERATOR_NAME_LENGTH);
		} else if (salpha) {
			strncpy(op->name, salpha,
					OFONO_MAX_OPERATOR_NAME_LENGTH);
		} else {
			op->name[0] = 0;
		}

		/* Set the proper status  */
		if (!strcmp(status, "available")) {
			list[i].status = OPERATOR_STATUS_AVAILABLE;
		} else if (!strcmp(status, "current")) {
			list[i].status = OPERATOR_STATUS_CURRENT;
		} else if (!strcmp(status, "forbidden")) {
			list[i].status = OPERATOR_STATUS_FORBIDDEN;
		} else {
			list[i].status = OPERATOR_STATUS_UNKNOWN;
		}

		op->tech = -1;
		ok = ril_parse_mcc_mnc(numeric, op);
		if (ok) {
			if (op->tech < 0) {
				op->tech = cbd->nd->network->voice.access_tech;
			}
			DBG("[operator=%s, %s, %s, status: %s]", op->name,
						op->mcc, op->mnc, status);
		} else {
			DBG("failed to parse operator list");
		}

		g_free(lalpha);
		g_free(salpha);
		g_free(numeric);
		g_free(status);
	}

	if (ok) {
		cb(ril_error_ok(&error), noperators, list, cbd->data);
	} else {
		cb(ril_error_failure(&error), 0, NULL, cbd->data);
	}

	g_free(list);
}

static void ril_netreg_list_operators(struct ofono_netreg *netreg,
				ofono_netreg_operator_list_cb_t cb, void *data)
{
	struct ril_netreg *nd = ril_netreg_get_data(netreg);

	grilio_queue_send_request_full(nd->q, NULL,
			RIL_REQUEST_QUERY_AVAILABLE_NETWORKS,
			ril_netreg_list_operators_cb, ril_netreg_cbd_free,
			ril_netreg_cbd_new(nd, cb, data));
}

static void ril_netreg_register_cb(GRilIoChannel *io, int status,
				const void *data, guint len, void *user_data)
{
	struct ril_netreg_cbd *cbd = user_data;
	ofono_netreg_register_cb_t cb = cbd->cb.reg;
	struct ofono_error error;

	if (status == RIL_E_SUCCESS) {
		cb(ril_error_ok(&error), cbd->data);
	} else {
		ofono_error("registration failed, ril result %d", status);
		cb(ril_error_failure(&error), cbd->data);
	}
}

static void ril_netreg_register_auto(struct ofono_netreg *netreg,
				ofono_netreg_register_cb_t cb, void *data)
{
	struct ril_netreg *nd = ril_netreg_get_data(netreg);
	GRilIoRequest *req = grilio_request_new();

	ofono_info("nw select automatic");
	grilio_request_set_timeout(req, REGISTRATION_TIMEOUT);
	grilio_request_set_retry(req, 0, REGISTRATION_MAX_RETRIES);
	grilio_queue_send_request_full(nd->q, req,
			RIL_REQUEST_SET_NETWORK_SELECTION_AUTOMATIC,
			ril_netreg_register_cb, ril_netreg_cbd_free,
			ril_netreg_cbd_new(nd, cb, data));
	grilio_request_unref(req);
}

static void ril_netreg_register_manual(struct ofono_netreg *netreg,
				const char *mcc, const char *mnc,
				ofono_netreg_register_cb_t cb, void *data)
{
	struct ril_netreg *nd = ril_netreg_get_data(netreg);
	GRilIoRequest *req = grilio_request_new();

	ofono_info("nw select manual: %s%s", mcc, mnc);
	grilio_request_append_format(req, "%s%s+0", mcc, mnc);
	grilio_request_set_timeout(req, REGISTRATION_TIMEOUT);
	grilio_request_set_retry(req, 0, REGISTRATION_MAX_RETRIES);
	grilio_queue_send_request_full(nd->q, req,
			RIL_REQUEST_SET_NETWORK_SELECTION_MANUAL,
			ril_netreg_register_cb, ril_netreg_cbd_free,
			ril_netreg_cbd_new(nd, cb, data));
	grilio_request_unref(req);
}

static int ril_netreg_get_signal_strength(const void *data, guint len)
{
	GRilIoParser rilp;
	int gw_signal = 0, cdma_dbm = 0, evdo_dbm = 0, lte_signal = 0;
	int rsrp = 0;

	grilio_parser_init(&rilp, data, len);

	/* RIL_SignalStrength_v6 */
	/* GW_SignalStrength */
	grilio_parser_get_int32(&rilp, &gw_signal);
	grilio_parser_get_int32(&rilp, NULL); /* bitErrorRate */

	/* CDMA_SignalStrength */
	grilio_parser_get_int32(&rilp, &cdma_dbm);
	grilio_parser_get_int32(&rilp, NULL); /* ecio */

	/* EVDO_SignalStrength */
	grilio_parser_get_int32(&rilp, &evdo_dbm);
	grilio_parser_get_int32(&rilp, NULL); /* ecio */
	grilio_parser_get_int32(&rilp, NULL); /* signalNoiseRatio */

	/* LTE_SignalStrength */
	grilio_parser_get_int32(&rilp, &lte_signal);
	grilio_parser_get_int32(&rilp, &rsrp);
	/* The rest is ignored */

	if (rsrp == INT_MAX) {
		DBG("gw: %d, cdma: %d, evdo: %d, lte: %d", gw_signal,
					cdma_dbm, evdo_dbm, lte_signal);
	}  else {
		DBG("gw: %d, cdma: %d, evdo: %d, lte: %d rsrp: %d", gw_signal,
					cdma_dbm, evdo_dbm, lte_signal, rsrp);
	}

	/* Return the first valid one */

	/* Some RILs (namely, from MediaTek) report 0 here AND a valid LTE
	 * RSRP value. If we've got zero, don't report it just yet. */
	if (gw_signal >= 1 && gw_signal <= 31) {
		/* Valid values are (0-31, 99) as defined in TS 27.007 */
		return (gw_signal * 100) / 31;
	}

	/* Valid values are (0-31, 99) as defined in TS 27.007 */
	if (lte_signal >= 0 && lte_signal <= 31) {
		return (lte_signal * 100) / 31;
	}

	/* RSRP range: 44 to 140 dBm as defined in 3GPP TS 36.133 */
	if (lte_signal == 99 && rsrp >= 44 && rsrp <= 140) {
		return 140 - rsrp;
	}

	/* If we've got zero strength and no valid RSRP, then so be it */
	if (gw_signal == 0) {
		return 0;
	}

	/* In case of dbm, return the value directly */
	if (cdma_dbm != -1) {
		return MIN(cdma_dbm, 100);
	}

	if (evdo_dbm != -1) {
		return MIN(evdo_dbm, 100);
	}

	return -1;
}

static void ril_netreg_strength_notify(GRilIoChannel *io, guint ril_event,
				const void *data, guint len, void *user_data)
{
	struct ril_netreg *nd = user_data;
	int strength;

	GASSERT(ril_event == RIL_UNSOL_SIGNAL_STRENGTH);
	strength = ril_netreg_get_signal_strength(data, len);
	DBG_(nd, "%d", strength);
	ofono_netreg_strength_notify(nd->netreg, strength);
}

static void ril_netreg_strength_cb(GRilIoChannel *io, int status,
				const void *data, guint len, void *user_data)
{
	struct ril_netreg_cbd *cbd = user_data;
	ofono_netreg_strength_cb_t cb = cbd->cb.strength;
	struct ofono_error error;

	if (status == RIL_E_SUCCESS) {
		int strength = ril_netreg_get_signal_strength(data, len);
		cb(ril_error_ok(&error), strength, cbd->data);
	} else {
		ofono_error("Failed to retrive the signal strength: %s",
						ril_error_to_string(status));
		cb(ril_error_failure(&error), -1, cbd->data);
	}
}

static void ril_netreg_strength(struct ofono_netreg *netreg,
				ofono_netreg_strength_cb_t cb, void *data)
{
	struct ril_netreg *nd = ril_netreg_get_data(netreg);
	GRilIoRequest* req = grilio_request_new();

	grilio_request_set_retry(req, RIL_RETRY_MS, -1);
	grilio_queue_send_request_full(nd->q, req,
			RIL_REQUEST_SIGNAL_STRENGTH, ril_netreg_strength_cb,
			ril_netreg_cbd_free, ril_netreg_cbd_new(nd, cb, data));
	grilio_request_unref(req);
}

static void ril_netreg_nitz_notify(GRilIoChannel *io, guint ril_event,
				const void *data, guint len, void *user_data)
{
	struct ril_netreg *nd = user_data;
	GRilIoParser rilp;
	int year, mon, mday, hour, min, sec, tzi, dst = 0;
	char tzs;
	gchar *nitz;

	GASSERT(ril_event == RIL_UNSOL_NITZ_TIME_RECEIVED);

	grilio_parser_init(&rilp, data, len);
	nitz = grilio_parser_get_utf8(&rilp);

	DBG_(nd, "%s", nitz);

	/*
	 * Format: yy/mm/dd,hh:mm:ss(+/-)tz[,ds]
	 * The ds part is considered optional, initialized to zero.
	 */
	if (nitz && sscanf(nitz, "%u/%u/%u,%u:%u:%u%c%u,%u",
			&year, &mon, &mday, &hour, &min, &sec, &tzs, &tzi,
			&dst) >= 8 && (tzs == '+' || tzs == '-')) {
		struct ofono_network_time time;
		char tz[4];

		snprintf(tz, sizeof(tz), "%c%d", tzs, tzi);
		time.utcoff = atoi(tz) * 15 * 60;
		time.dst = dst;
		time.sec = sec;
		time.min = min;
		time.hour = hour;
		time.mday = mday;
		time.mon = mon;
		time.year = 2000 + year;

		ofono_netreg_time_notify(nd->netreg, &time);
	} else {
		ofono_warn("Failed to parse NITZ string \"%s\"", nitz);
	}

	g_free(nitz);
}

static gboolean ril_netreg_register(gpointer user_data)
{
	struct ril_netreg *nd = user_data;

	GASSERT(nd->timer_id);
	nd->timer_id = 0;
	ofono_netreg_register(nd->netreg);

	/* Register for network state changes */
	nd->network_event_id[NETREG_NETWORK_EVENT_OPERATOR_CHANGED] =
		ril_network_add_operator_changed_handler(nd->network,
			ril_netreg_status_notify, nd);
	nd->network_event_id[NETREG_NETWORK_EVENT_VOICE_STATE_CHANGED] =
		ril_network_add_voice_state_changed_handler(nd->network,
			ril_netreg_status_notify, nd);

	/* Register for network time updates */
	nd->ril_event_id[NETREG_RIL_EVENT_NITZ_TIME_RECEIVED] =
		grilio_channel_add_unsol_event_handler(nd->io,
			ril_netreg_nitz_notify, 
			RIL_UNSOL_NITZ_TIME_RECEIVED, nd);

	/* Register for signal strength changes */
	nd->ril_event_id[NETREG_RIL_EVENT_SIGNAL_STRENGTH] =
		grilio_channel_add_unsol_event_handler(nd->io,
			ril_netreg_strength_notify,
			RIL_UNSOL_SIGNAL_STRENGTH, nd);

	/* This makes the timeout a single-shot */
	return FALSE;
}

static int ril_netreg_probe(struct ofono_netreg *netreg, unsigned int vendor,
				void *data)
{
	struct ril_modem *modem = data;
	struct ril_netreg *nd = g_new0(struct ril_netreg, 1);

	nd->log_prefix = (modem->log_prefix && modem->log_prefix[0]) ?
		g_strconcat(modem->log_prefix, " ", NULL) : g_strdup("");

	DBG_(nd, "%p", netreg);
	nd->io = grilio_channel_ref(ril_modem_io(modem));
	nd->q = grilio_queue_new(nd->io);
	nd->network = ril_network_ref(modem->network);
	nd->netreg = netreg;

	ofono_netreg_set_data(netreg, nd);
	nd->timer_id = g_idle_add(ril_netreg_register, nd);
	return 0;
}

static void ril_netreg_remove(struct ofono_netreg *netreg)
{
	struct ril_netreg *nd = ril_netreg_get_data(netreg);

	DBG_(nd, "%p", netreg);
	grilio_queue_cancel_all(nd->q, FALSE);
	ofono_netreg_set_data(netreg, NULL);

	if (nd->timer_id > 0) {
		g_source_remove(nd->timer_id);
	}

	if (nd->notify_id) {
		g_source_remove(nd->notify_id);
	}

	if (nd->current_operator_id) {
		g_source_remove(nd->current_operator_id);
	}

	ril_network_remove_all_handlers(nd->network, nd->network_event_id);
	ril_network_unref(nd->network);

	grilio_channel_remove_all_handlers(nd->io, nd->ril_event_id);
	grilio_channel_unref(nd->io);
	grilio_queue_unref(nd->q);
	g_free(nd->log_prefix);
	g_free(nd);
}

const struct ofono_netreg_driver ril_netreg_driver = {
	.name                   = RILMODEM_DRIVER,
	.probe                  = ril_netreg_probe,
	.remove                 = ril_netreg_remove,
	.registration_status    = ril_netreg_registration_status,
	.current_operator       = ril_netreg_current_operator,
	.list_operators         = ril_netreg_list_operators,
	.register_auto          = ril_netreg_register_auto,
	.register_manual        = ril_netreg_register_manual,
	.strength               = ril_netreg_strength
};

/*
 * Local Variables:
 * mode: C
 * c-basic-offset: 8
 * indent-tabs-mode: t
 * End:
 */
