/*
 *  oFono - Open Source Telephony - RIL-based devices
 *
 *  Copyright (C) 2015 Jolla Ltd.
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
#include "ril_util.h"
#include "ril_log.h"
#include "ril_constants.h"

#include "common.h"
#include "simutil.h"

#include <ctype.h>

enum ril_netreg_events {
	NETREG_EVENT_VOICE_NETWORK_STATE_CHANGED,
	NETREG_EVENT_NITZ_TIME_RECEIVED,
	NETREG_EVENT_SIGNAL_STRENGTH,
	NETREG_EVENT_COUNT
};

struct ril_netreg {
	GRilIoChannel *io;
	GRilIoQueue *q;
	struct ofono_netreg *netreg;
	char mcc[OFONO_MAX_MCC_LENGTH + 1];
	char mnc[OFONO_MAX_MNC_LENGTH + 1];
	int tech;
	struct ofono_network_time time;
	guint timer_id;
	int corestatus; /* Registration status previously reported to core */
	gulong event_id[NETREG_EVENT_COUNT];
};

/* 27.007 Section 7.3 <stat> */
enum operator_status {
	OPERATOR_STATUS_UNKNOWN =	0,
	OPERATOR_STATUS_AVAILABLE =	1,
	OPERATOR_STATUS_CURRENT =	2,
	OPERATOR_STATUS_FORBIDDEN =	3,
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

static inline struct ril_netreg *ril_netreg_get_data(struct ofono_netreg *nr)
{
	return ofono_netreg_get_data(nr);
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

static gboolean ril_netreg_extract_mcc_mnc(const char *str,
					struct ofono_network_operator *op)
{
	if (str) {
		int i;
		const char *ptr = str;

		/* Three digit country code */
		for (i = 0;
		     i < OFONO_MAX_MCC_LENGTH && *ptr && isdigit(*ptr);
		     i++) {
			op->mcc[i] = *ptr++;
		}
		op->mcc[i] = 0;

		if (i == OFONO_MAX_MCC_LENGTH) {
			/* Usually a 2 but sometimes 3 digit network code */
			for (i=0;
			     i<OFONO_MAX_MNC_LENGTH && *ptr && isdigit(*ptr);
			     i++) {
				op->mnc[i] = *ptr++;
			}
			op->mnc[i] = 0;

			if (i > 0) {

				/*
				 * Sometimes MCC/MNC are followed by + and
				 * what looks like the technology code. This
				 * is of course completely undocumented.
				 */
				if (*ptr == '+') {
					int tech = ril_parse_tech(ptr+1, NULL);
					if (tech >= 0) {
						op->tech = tech;
					}
				}

				return TRUE;
			}
		}
	}
	return FALSE;
}

static void ril_netreg_state_cb(GRilIoChannel *io, int call_status,
				const void *data, guint len, void *user_data)
{
	struct ofono_error error;
	struct ril_netreg_cbd *cbd = user_data;
	ofono_netreg_status_cb_t cb = cbd->cb.status;
	struct ril_netreg *nd = cbd->nd;
	struct ril_reg_data reg;
	int rawstatus;

	DBG("");
	if (call_status != RIL_E_SUCCESS || !nd->netreg) {
		ofono_error("voice registration status query fail");
		nd->corestatus = -1;
		cb(ril_error_failure(&error), -1, -1, -1, -1, cbd->data);
		return;
	}

	if (!ril_util_parse_reg(data, len, &reg)) {
		DBG("voice registration status parsing fail");
		nd->corestatus = -1;
		cb(ril_error_failure(&error), -1, -1, -1, -1, cbd->data);
		return;
	}

	rawstatus = reg.status;
	if (reg.status == NETWORK_REGISTRATION_STATUS_ROAMING) {
		reg.status = ril_netreg_check_if_really_roaming(nd->netreg,
								reg.status);
	}

	if (rawstatus != reg.status) {
		ofono_info("voice registration modified %d => %d",
				rawstatus, reg.status);
	}

	DBG("status:%d corestatus:%d", reg.status, nd->corestatus);

	if (nd->corestatus != reg.status) {
		ofono_info("voice registration changes %d (%d)",
				reg.status, nd->corestatus);
	}

	nd->corestatus = reg.status;
	nd->tech = reg.access_tech;
	cb(ril_error_ok(&error), reg.status, reg.lac, reg.ci, reg.access_tech,
								cbd->data);
}

static void ril_netreg_status_notify(struct ofono_error *error, int status,
				int lac, int ci, int tech, gpointer user_data)
{
	struct ril_netreg *nd = user_data;

	if (error->type != OFONO_ERROR_TYPE_NO_ERROR) {
		DBG("Error during status notification");
	} else if (nd->netreg) {
		ofono_netreg_status_notify(nd->netreg, status, lac, ci, tech);
	}
}

static void ril_netreg_network_state_change(GRilIoChannel *io,
		guint ril_event, const void *data, guint len, void *user_data)
{
	struct ril_netreg *nd = user_data;

	GASSERT(ril_event == RIL_UNSOL_RESPONSE_VOICE_NETWORK_STATE_CHANGED);
	grilio_queue_send_request_full(nd->q, NULL,
			RIL_REQUEST_VOICE_REGISTRATION_STATE,
			ril_netreg_state_cb, ril_netreg_cbd_free, 
			ril_netreg_cbd_new(nd, ril_netreg_status_notify, nd));
}

static void ril_netreg_registration_status(struct ofono_netreg *netreg,
			ofono_netreg_status_cb_t cb, void *data)
{
	struct ril_netreg *nd = ril_netreg_get_data(netreg);

	grilio_queue_send_request_full(nd->q, NULL,
		RIL_REQUEST_VOICE_REGISTRATION_STATE, ril_netreg_state_cb,
		ril_netreg_cbd_free, ril_netreg_cbd_new(nd, cb, data));
}

static void ril_netreg_current_operator_cb(GRilIoChannel *io, int status,
				const void *data, guint len, void *user_data)
{
	struct ril_netreg_cbd *cbd = user_data;
	struct ril_netreg *nd = cbd->nd;
	struct ofono_error error;
	struct ofono_network_operator op;
	struct ofono_network_operator *result = NULL;
	gchar *lalpha = NULL, *salpha = NULL, *numeric = NULL;
	int tmp;
	GRilIoParser rilp;

	ril_error_init_failure(&error);
	if (status != RIL_E_SUCCESS) {
		ofono_error("Failed to retrive the current operator: %s",
						ril_error_to_string(status));
		goto done;
	}

	grilio_parser_init(&rilp, data, len);
	if (!grilio_parser_get_int32(&rilp, &tmp) || !tmp) {
		goto done;
	}

	lalpha = grilio_parser_get_utf8(&rilp);
	salpha = grilio_parser_get_utf8(&rilp);
	numeric = grilio_parser_get_utf8(&rilp);

	/* Try to use long by default */
	if (lalpha) {
		strncpy(op.name, lalpha, OFONO_MAX_OPERATOR_NAME_LENGTH);
	} else if (salpha) {
		strncpy(op.name, salpha, OFONO_MAX_OPERATOR_NAME_LENGTH);
	} else {
		goto done;
	}

	if (!ril_netreg_extract_mcc_mnc(numeric, &op)) {
		goto done;
	}

	/* Set to current */
	op.status = OPERATOR_STATUS_CURRENT;
	op.tech = nd->tech;
	result = &op;
	ril_error_init_ok(&error);

	DBG("lalpha=%s, salpha=%s, numeric=%s, %s, mcc=%s, mnc=%s, %s",
			lalpha, salpha, numeric, op.name, op.mcc, op.mnc,
				registration_tech_to_string(op.tech));

done:
	cbd->cb.operator(&error, result, cbd->data);
	g_free(lalpha);
	g_free(salpha);
	g_free(numeric);
}

static void ril_netreg_current_operator(struct ofono_netreg *netreg,
				ofono_netreg_operator_cb_t cb, void *data)
{
	struct ril_netreg *nd = ril_netreg_get_data(netreg);

	grilio_queue_send_request_full(nd->q, NULL, RIL_REQUEST_OPERATOR,
			ril_netreg_current_operator_cb, ril_netreg_cbd_free,
			ril_netreg_cbd_new(nd, cb, data));
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

		op->tech = ACCESS_TECHNOLOGY_GSM;
		ok = ril_netreg_extract_mcc_mnc(numeric, op);
		if (ok) {
			DBG("[operator=%s, %s, %s, status: %s]", op->name,
						op->mcc, op->mnc, status);
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

	ofono_info("nw select automatic");
	grilio_queue_send_request_full(nd->q, NULL,
			RIL_REQUEST_SET_NETWORK_SELECTION_AUTOMATIC,
			ril_netreg_register_cb, ril_netreg_cbd_free,
			ril_netreg_cbd_new(nd, cb, data));
}

static void ril_netreg_register_manual(struct ofono_netreg *netreg,
				const char *mcc, const char *mnc,
				ofono_netreg_register_cb_t cb, void *data)
{
	struct ril_netreg *nd = ril_netreg_get_data(netreg);
	GRilIoRequest *req = grilio_request_new();

	ofono_info("nw select manual: %s%s", mcc, mnc);
	grilio_request_append_format(req, "%s%s+0", mcc, mnc);
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
	grilio_parser_get_int32(&rilp, NULL); /* rsrp */
	grilio_parser_get_int32(&rilp, NULL); /* rsrq */
	grilio_parser_get_int32(&rilp, NULL); /* rssnr */
	grilio_parser_get_int32(&rilp, NULL); /* cqi */

	DBG("gw: %d, cdma: %d, evdo: %d, lte: %d", gw_signal, cdma_dbm,
							evdo_dbm, lte_signal);

	/* Return the first valid one */
	if (gw_signal != 99 && gw_signal != -1) {
		return (gw_signal * 100) / 31;
	}

	if (lte_signal != 99 && lte_signal != -1) {
		return (lte_signal * 100) / 31;
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
	DBG("%d", strength);
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

	grilio_queue_send_request_full(nd->q, NULL,
			RIL_REQUEST_SIGNAL_STRENGTH, ril_netreg_strength_cb,
			ril_netreg_cbd_free, ril_netreg_cbd_new(nd, cb, data));
}

static void ril_netreg_nitz_notify(GRilIoChannel *io, guint ril_event,
				const void *data, guint len, void *user_data)
{
	struct ril_netreg *nd = user_data;
	GRilIoParser rilp;
	int year, mon, mday, hour, min, sec, dst, tzi;
	char tzs, tz[4];
	gchar *nitz;

	GASSERT(ril_event == RIL_UNSOL_NITZ_TIME_RECEIVED);

	grilio_parser_init(&rilp, data, len);
	nitz = grilio_parser_get_utf8(&rilp);

	DBG("%s", nitz);
	sscanf(nitz, "%u/%u/%u,%u:%u:%u%c%u,%u", &year, &mon, &mday,
			&hour, &min, &sec, &tzs, &tzi, &dst);
	snprintf(tz, sizeof(tz), "%c%d", tzs, tzi);

	nd->time.utcoff = atoi(tz) * 15 * 60;
	nd->time.dst = dst;
	nd->time.sec = sec;
	nd->time.min = min;
	nd->time.hour = hour;
	nd->time.mday = mday;
	nd->time.mon = mon;
	nd->time.year = 2000 + year;

	ofono_netreg_time_notify(nd->netreg, &nd->time);
	g_free(nitz);
}

int ril_netreg_check_if_really_roaming(struct ofono_netreg *netreg,
								gint status)
{
	/* These functions tolerate NULL argument */
	const char *net_mcc = ofono_netreg_get_mcc(netreg);
	const char *net_mnc = ofono_netreg_get_mnc(netreg);
	struct sim_spdi *spdi = ofono_netreg_get_spdi(netreg);

	if (spdi && net_mcc && net_mnc) {
		if (sim_spdi_lookup(spdi, net_mcc, net_mnc)) {
			ofono_info("voice reg: not roaming based on spdi");
			return NETWORK_REGISTRATION_STATUS_REGISTERED;
		}
	}

	return status;
}

static gboolean ril_netreg_register(gpointer user_data)
{
	struct ril_netreg *nd = user_data;

	GASSERT(nd->timer_id);
	nd->timer_id = 0;
	ofono_netreg_register(nd->netreg);

	/* Register for network state changes */
	nd->event_id[NETREG_EVENT_VOICE_NETWORK_STATE_CHANGED] =
		grilio_channel_add_unsol_event_handler(nd->io,
			ril_netreg_network_state_change,
			RIL_UNSOL_RESPONSE_VOICE_NETWORK_STATE_CHANGED,	nd);

	/* Register for network time update reports */
	nd->event_id[NETREG_EVENT_NITZ_TIME_RECEIVED] =
		grilio_channel_add_unsol_event_handler(nd->io,
			ril_netreg_nitz_notify, 
			RIL_UNSOL_NITZ_TIME_RECEIVED, nd);

	/* Register for signal strength changes */
	nd->event_id[NETREG_EVENT_SIGNAL_STRENGTH] =
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

	DBG("[%u] %p", ril_modem_slot(modem), netreg);
	nd->io = grilio_channel_ref(ril_modem_io(modem));
	nd->q = grilio_queue_new(nd->io);
	nd->netreg = netreg;
	nd->tech = -1;
	nd->time.sec = -1;
	nd->time.min = -1;
	nd->time.hour = -1;
	nd->time.mday = -1;
	nd->time.mon = -1;
	nd->time.year = -1;
	nd->time.dst = 0;
	nd->time.utcoff = 0;
	nd->corestatus = -1;

	ofono_netreg_set_data(netreg, nd);
	nd->timer_id = g_idle_add(ril_netreg_register, nd);
	return 0;
}

static void ril_netreg_remove(struct ofono_netreg *netreg)
{
	int i;
	struct ril_netreg *nd = ril_netreg_get_data(netreg);

	DBG("%p", netreg);
	grilio_queue_cancel_all(nd->q, FALSE);
	ofono_netreg_set_data(netreg, NULL);

	for (i=0; i<G_N_ELEMENTS(nd->event_id); i++) {
		grilio_channel_remove_handler(nd->io, nd->event_id[i]);
	}

	if (nd->timer_id > 0) {
		g_source_remove(nd->timer_id);
	}

	grilio_channel_unref(nd->io);
	grilio_queue_unref(nd->q);
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
