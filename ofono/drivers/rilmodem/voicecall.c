/*
 *
 *  oFono - Open Source Telephony
 *
 *  Copyright (C) 2008-2011  Intel Corporation. All rights reserved.
 *  Copyright (C) 2012-2013 Canonical Ltd.
 *  Copyright (C) 2013 Jolla Ltd.
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License version 2 as
 *  published by the Free Software Foundation.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
 *
 */

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#define _GNU_SOURCE
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <errno.h>

#include <glib.h>

#include <ofono.h>

#include <ofono/log.h>
#include <ofono/modem.h>
#include <ofono/voicecall.h>

#include <gril/gril.h>

#include "common.h"
#include "rilmodem.h"
#include "voicecall.h"

/* Amount of ms we wait between CLCC calls */
#define POLL_CLCC_INTERVAL 300

#define FLAG_NEED_CLIP 1

#define MAX_DTMF_BUFFER 32

/* To use with change_state_req::affected_types */
#define AFFECTED_STATES_ALL 0x3F

/* Auto-answer delay in seconds */
#define AUTO_ANSWER_DELAY_S 3

struct release_id_req {
	struct ofono_voicecall *vc;
	ofono_voicecall_cb_t cb;
	void *data;
	int id;
};

struct change_state_req {
	struct ofono_voicecall *vc;
	ofono_voicecall_cb_t cb;
	void *data;
	/* Call states affected by a local release (1 << enum call_status) */
	int affected_types;
};

struct lastcause_req {
	struct ofono_voicecall *vc;
	int id;
};

/* Data for dial after swap */
struct hold_before_dial_req {
	struct ofono_voicecall *vc;
	struct ofono_phone_number dial_ph;
	enum ofono_clir_option dial_clir;
};

static void send_one_dtmf(struct ril_voicecall_data *vd);
static void clear_dtmf_queue(struct ril_voicecall_data *vd);

static void lastcause_cb(struct ril_msg *message, gpointer user_data)
{
	struct lastcause_req *reqdata = user_data;
	struct ofono_voicecall *vc = reqdata->vc;
	struct ril_voicecall_data *vd = ofono_voicecall_get_data(vc);
	enum ofono_disconnect_reason reason = OFONO_DISCONNECT_REASON_ERROR;
	int last_cause = CALL_FAIL_ERROR_UNSPECIFIED;
	struct parcel rilp;

	g_ril_init_parcel(message, &rilp);

	if (rilp.size < sizeof(int32_t))
		goto done;

	if (parcel_r_int32(&rilp) > 0)
		last_cause = parcel_r_int32(&rilp);

	g_ril_append_print_buf(vd->ril, "{%d}", last_cause);
	g_ril_print_response(vd->ril, message);

	if (last_cause == CALL_FAIL_NORMAL || last_cause == CALL_FAIL_BUSY)
		reason = OFONO_DISCONNECT_REASON_REMOTE_HANGUP;

done:
	DBG("Call %d ended with reason %d", reqdata->id, reason);

	ofono_voicecall_disconnected(vc, reqdata->id, reason, NULL);
}

static int call_compare(gconstpointer a, gconstpointer b)
{
	const struct ofono_call *ca = a;
	const struct ofono_call *cb = b;

	if (ca->id < cb->id)
		return -1;

	if (ca->id > cb->id)
		return 1;

	return 0;
}

static void clcc_poll_cb(struct ril_msg *message, gpointer user_data)
{
	struct ofono_voicecall *vc = user_data;
	struct ril_voicecall_data *vd = ofono_voicecall_get_data(vc);
	int reqid = RIL_REQUEST_LAST_CALL_FAIL_CAUSE;
	struct parcel rilp;
	GSList *calls = NULL;
	GSList *n, *o;
	struct ofono_call *nc, *oc;
	int num, i;
	char *number, *name;

	/*
	 * We consider all calls have been dropped if there is no radio, which
	 * happens, for instance, when flight mode is set whilst in a call.
	 */
	if (message->error != RIL_E_SUCCESS &&
			message->error != RIL_E_RADIO_NOT_AVAILABLE) {
		ofono_error("We are polling CLCC and received an error");
		ofono_error("All bets are off for call management");
		return;
	}


	g_ril_init_parcel(message, &rilp);

	/* maguro signals no calls with empty event data */
	if (rilp.size < sizeof(int32_t))
		goto no_calls;

	DBG("[%d,%04d]< %s", g_ril_get_slot(vd->ril),
				message->serial_no,
				"RIL_REQUEST_GET_CURRENT_CALLS");

	/* Number of RIL_Call structs */
	num = parcel_r_int32(&rilp);

	for (i = 0; i < num; i++) {
		struct ofono_call *call;

		call = g_new0(struct ofono_call, 1);

		ofono_call_init(call);
		call->status = parcel_r_int32(&rilp);
		call->id = parcel_r_int32(&rilp);
		call->phone_number.type = parcel_r_int32(&rilp);
		parcel_r_int32(&rilp); /* isMpty */
		parcel_r_int32(&rilp); /* isMT */
		parcel_r_int32(&rilp); /* als */
		call->type = parcel_r_int32(&rilp); /* isVoice */
		parcel_r_int32(&rilp); /* isVoicePrivacy */
		number = parcel_r_string(&rilp);
		if (number) {
			strncpy(call->phone_number.number, number,
				OFONO_MAX_PHONE_NUMBER_LENGTH);
			g_free(number);
		}

		parcel_r_int32(&rilp); /* numberPresentation */
		name = parcel_r_string(&rilp);
		if (name) {
			strncpy(call->name, name,
				OFONO_MAX_CALLER_NAME_LENGTH);
			g_free(name);
		}

		parcel_r_int32(&rilp); /* namePresentation */
		parcel_r_int32(&rilp); /* uusInfo */

		if (strlen(call->phone_number.number) > 0)
			call->clip_validity = 0;
		else
			call->clip_validity = 2;

		DBG("[id=%d,status=%d,type=%d,number=%s,name=%s]",
			call->id, call->status, call->type,
			call->phone_number.number, call->name);

		calls = g_slist_insert_sorted(calls, call, call_compare);
	}

no_calls:
	n = calls;
	o = vd->calls;

	while (n || o) {
		nc = n ? n->data : NULL;
		oc = o ? o->data : NULL;

		/* TODO: Add comments explaining call id handling */
		if (oc && (nc == NULL || (nc->id > oc->id))) {
			if (vd->local_release & (1 << oc->id)) {
				ofono_voicecall_disconnected(vc, oc->id,
					OFONO_DISCONNECT_REASON_LOCAL_HANGUP,
					NULL);
			} else if (message->error ==
						RIL_E_RADIO_NOT_AVAILABLE) {
				ofono_voicecall_disconnected(vc, oc->id,
					OFONO_DISCONNECT_REASON_ERROR,
					NULL);
			} else {
				/* Get disconnect cause before calling core */
				struct lastcause_req *reqdata =
					g_new0(struct lastcause_req, 1);

				reqdata->vc = user_data;
				reqdata->id = oc->id;

				g_ril_send(vd->ril, reqid, NULL,
						lastcause_cb, reqdata, g_free);
			}

			clear_dtmf_queue(vd);

			o = o->next;
		} else if (nc && (oc == NULL || (nc->id < oc->id))) {
			/* new call, signal it */
			if (nc->type) {
				ofono_voicecall_notify(vc, nc);

				if (vd->cb) {
					struct ofono_error error;
					ofono_voicecall_cb_t cb = vd->cb;
					decode_ril_error(&error, "OK");
					cb(&error, vd->data);
					vd->cb = NULL;
					vd->data = NULL;
				}
			}

			n = n->next;
		} else {
			/*
			 * Always use the clip_validity from old call
			 * the only place this is truly told to us is
			 * in the CLIP notify, the rest are fudged
			 * anyway.  Useful when RING, CLIP is used,
			 * and we're forced to use CLCC and clip_validity
			 * is 1
			 */
			if (oc->clip_validity == 1)
				nc->clip_validity = oc->clip_validity;

			nc->cnap_validity = oc->cnap_validity;

			/*
			 * CDIP doesn't arrive as part of CLCC, always
			 * re-use from the old call
			 */
			memcpy(&nc->called_number, &oc->called_number,
					sizeof(oc->called_number));

			/*
			 * If the CLIP is not provided and the CLIP never
			 * arrives, or RING is used, then signal the call
			 * here
			 */
			if (nc->status == CALL_STATUS_INCOMING &&
					(vd->flags & FLAG_NEED_CLIP)) {
				if (nc->type)
					ofono_voicecall_notify(vc, nc);

				vd->flags &= ~FLAG_NEED_CLIP;
			} else if (memcmp(nc, oc, sizeof(*nc)) && nc->type)
				ofono_voicecall_notify(vc, nc);

			n = n->next;
			o = o->next;
		}
	}

	g_slist_free_full(vd->calls, g_free);

	vd->calls = calls;
	vd->local_release = 0;
}

gboolean ril_poll_clcc(gpointer user_data)
{
	struct ofono_voicecall *vc = user_data;
	struct ril_voicecall_data *vd = ofono_voicecall_get_data(vc);

	g_ril_send(vd->ril, RIL_REQUEST_GET_CURRENT_CALLS, NULL,
			clcc_poll_cb, vc, NULL);

	vd->clcc_source = 0;

	return FALSE;
}

static void generic_cb(struct ril_msg *message, gpointer user_data)
{
	struct change_state_req *req = user_data;
	struct ril_voicecall_data *vd = ofono_voicecall_get_data(req->vc);
	struct ofono_error error;

	if (message->error == RIL_E_SUCCESS) {
		decode_ril_error(&error, "OK");
	} else {
		decode_ril_error(&error, "FAIL");
		goto out;
	}

	g_ril_print_response_no_args(vd->ril, message);

	if (req->affected_types) {
		GSList *l;
		struct ofono_call *call;

		for (l = vd->calls; l; l = l->next) {
			call = l->data;

			if (req->affected_types & (1 << call->status))
				vd->local_release |= (1 << call->id);
		}
	}

out:
	g_ril_send(vd->ril, RIL_REQUEST_GET_CURRENT_CALLS, NULL,
			clcc_poll_cb, req->vc, NULL);

	/* We have to callback after we schedule a poll if required */
	if (req->cb)
		req->cb(&error, req->data);
}

static int ril_template(const guint rreq, struct ofono_voicecall *vc,
			GRilResponseFunc func, unsigned int affected_types,
			gpointer pdata, ofono_voicecall_cb_t cb, void *data)
{
	struct ril_voicecall_data *vd = ofono_voicecall_get_data(vc);
	struct change_state_req *req = g_try_new0(struct change_state_req, 1);
	int ret;

	if (req == NULL)
		goto error;

	req->vc = vc;
	req->cb = cb;
	req->data = data;
	req->affected_types = affected_types;

	ret = g_ril_send(vd->ril, rreq, pdata, func, req, g_free);
	if (ret > 0)
		return ret;
error:
	g_free(req);

	if (cb)
		CALLBACK_WITH_FAILURE(cb, data);

	return 0;
}

static void rild_cb(struct ril_msg *message, gpointer user_data)
{
	struct cb_data *cbd = user_data;
	struct ofono_voicecall *vc = cbd->user;
	struct ril_voicecall_data *vd = ofono_voicecall_get_data(vc);
	ofono_voicecall_cb_t cb = cbd->cb;
	struct ofono_error error;

	/*
	 * DIAL_MODIFIED_TO_DIAL means redirection. The call we will see when
	 * polling will have a different called number.
	 */
	if (message->error == RIL_E_SUCCESS ||
			(g_ril_vendor(vd->ril) == OFONO_RIL_VENDOR_AOSP &&
			message->error == RIL_E_DIAL_MODIFIED_TO_DIAL)) {
		decode_ril_error(&error, "OK");
	} else {
		decode_ril_error(&error, "FAIL");
		goto out;
	}

	g_ril_print_response_no_args(vd->ril, message);

	/* CLCC will update the oFono call list with proper ids  */
	if (!vd->clcc_source)
		vd->clcc_source = g_timeout_add(POLL_CLCC_INTERVAL,
						ril_poll_clcc, vc);

	/* we cannot answer just yet since we don't know the call id */
	vd->cb = cb;
	vd->data = cbd->data;

	return;

out:
	cb(&error, cbd->data);
}

static void dial(struct ofono_voicecall *vc,
			const struct ofono_phone_number *ph,
			enum ofono_clir_option clir, ofono_voicecall_cb_t cb,
			void *data)
{
	struct ril_voicecall_data *vd = ofono_voicecall_get_data(vc);
	struct cb_data *cbd = cb_data_new(cb, data, vc);
	struct parcel rilp;

	parcel_init(&rilp);

	/* Number to dial */
	parcel_w_string(&rilp, phone_number_to_string(ph));
	/* CLIR mode */
	parcel_w_int32(&rilp, clir);
	/* USS, empty string */
	/* TODO: Deal with USS properly */
	parcel_w_int32(&rilp, 0);
	parcel_w_int32(&rilp, 0);

	g_ril_append_print_buf(vd->ril, "(%s,%d,0,0)",
				phone_number_to_string(ph),
				clir);

	/* Send request to RIL */
	if (g_ril_send(vd->ril, RIL_REQUEST_DIAL, &rilp,
			rild_cb, cbd, g_free) > 0)
		return;

	g_free(cbd);
	CALLBACK_WITH_FAILURE(cb, data);
}

static void hold_before_dial_cb(struct ril_msg *message, gpointer user_data)
{
	struct cb_data *cbd = user_data;
	struct hold_before_dial_req *req = cbd->user;
	struct ril_voicecall_data *vd = ofono_voicecall_get_data(req->vc);
	ofono_voicecall_cb_t cb = cbd->cb;

	if (message->error != RIL_E_SUCCESS) {
		g_free(req);
		CALLBACK_WITH_FAILURE(cb, cbd->data);
		return;
	}

	g_ril_print_response_no_args(vd->ril, message);

	/* Current calls held: we can dial now */
	dial(req->vc, &req->dial_ph, req->dial_clir, cb, cbd->data);

	g_free(req);
}

void ril_dial(struct ofono_voicecall *vc, const struct ofono_phone_number *ph,
		enum ofono_clir_option clir, ofono_voicecall_cb_t cb,
		void *data)
{
	struct ril_voicecall_data *vd = ofono_voicecall_get_data(vc);
	int current_active = 0;
	struct ofono_call *call;
	GSList *l;

	/* Check for current active calls */
	for (l = vd->calls; l; l = l->next) {
		call = l->data;

		if (call->status == CALL_STATUS_ACTIVE) {
			current_active = 1;
			break;
		}
	}

	/*
	 * The network will put current active calls on hold. In some cases
	 * (mako), the modem also updates properly the state. In others
	 * (maguro), we need to explicitly set the state to held. In both cases
	 * we send a request for holding the active call, as it is not harmful
	 * when it is not really needed, and is what Android does.
	 */
	if (current_active) {
		struct hold_before_dial_req *req;
		struct cb_data *cbd;

		req = g_malloc0(sizeof(*req));
		req->vc = vc;
		req->dial_ph = *ph;
		req->dial_clir = clir;

		cbd = cb_data_new(cb, data, req);

		if (g_ril_send(vd->ril, RIL_REQUEST_SWITCH_HOLDING_AND_ACTIVE,
				NULL, hold_before_dial_cb, cbd, g_free) == 0) {
			g_free(cbd);
			CALLBACK_WITH_FAILURE(cb, data);
		}

	} else {
		dial(vc, ph, clir, cb, data);
	}
}

void ril_hangup_all(struct ofono_voicecall *vc, ofono_voicecall_cb_t cb,
			void *data)
{
	struct ril_voicecall_data *vd = ofono_voicecall_get_data(vc);
	struct ofono_error error;
	struct ofono_call *call;
	GSList *l;

	for (l = vd->calls; l; l = l->next) {
		call = l->data;

		if (call->status == CALL_STATUS_INCOMING) {
			/*
			 * Need to use this request so that declined
			 * calls in this state, are properly forwarded
			 * to voicemail.  REQUEST_HANGUP doesn't do the
			 * right thing for some operators, causing the
			 * caller to hear a fast busy signal.
			 */
			ril_template(RIL_REQUEST_HANGUP_WAITING_OR_BACKGROUND,
					vc, generic_cb, AFFECTED_STATES_ALL,
					NULL, NULL, NULL);
		} else {
			struct parcel rilp;

			/* TODO: Hangup just the active ones once we have call
			 * state tracking (otherwise it can't handle ringing) */
			parcel_init(&rilp);
			parcel_w_int32(&rilp, 1); /* Always 1 - AT+CHLD=1x */
			parcel_w_int32(&rilp, call->id);

			g_ril_append_print_buf(vd->ril, "(%u)", call->id);

			/* Send request to RIL */
			ril_template(RIL_REQUEST_HANGUP, vc, generic_cb,
					AFFECTED_STATES_ALL, &rilp, NULL, NULL);
		}
	}

	/* TODO: Deal in case of an error at hungup */
	decode_ril_error(&error, "OK");
	cb(&error, data);
}

void ril_hangup_specific(struct ofono_voicecall *vc,
				int id, ofono_voicecall_cb_t cb, void *data)
{
	struct ril_voicecall_data *vd = ofono_voicecall_get_data(vc);
	struct parcel rilp;

	DBG("Hanging up call with id %d", id);

	parcel_init(&rilp);
	parcel_w_int32(&rilp, 1); /* Always 1 - AT+CHLD=1x */
	parcel_w_int32(&rilp, id);

	g_ril_append_print_buf(vd->ril, "(%u)", id);

	/* Send request to RIL */
	ril_template(RIL_REQUEST_HANGUP, vc, generic_cb,
			AFFECTED_STATES_ALL, &rilp, cb, data);
}

void ril_call_state_notify(struct ril_msg *message, gpointer user_data)
{
	struct ofono_voicecall *vc = user_data;
	struct ril_voicecall_data *vd = ofono_voicecall_get_data(vc);

	g_ril_print_unsol_no_args(vd->ril, message);

	/* Just need to request the call list again */
	ril_poll_clcc(vc);

	return;
}

static void ril_ss_notify(struct ril_msg *message, gpointer user_data)
{
	struct ofono_voicecall *vc = user_data;
	struct ril_voicecall_data *vd = ofono_voicecall_get_data(vc);
	struct parcel rilp;
	int notif_type;
	int code;
	int index;
	int ton;
	char *tmp_number;
	struct ofono_phone_number number;

	g_ril_init_parcel(message, &rilp);

	notif_type = parcel_r_int32(&rilp);
	code = parcel_r_int32(&rilp);
	index = parcel_r_int32(&rilp);
	ton = parcel_r_int32(&rilp);
	tmp_number = parcel_r_string(&rilp);

	g_ril_append_print_buf(vd->ril, "{%d,%d,%d,%d,%s}",
				notif_type, code, index,
				ton, tmp_number);
	g_ril_print_unsol(vd->ril, message);

	if (tmp_number != NULL) {
		strncpy(number.number, tmp_number,
				OFONO_MAX_PHONE_NUMBER_LENGTH);
		number.number[OFONO_MAX_PHONE_NUMBER_LENGTH] = '\0';
		number.type = ton;
		g_free(tmp_number);
	}

	/* 0 stands for MO intermediate, 1 for MT unsolicited */
	/* TODO How do we know the affected call? Refresh call list? */
	if (notif_type == 1)
		ofono_voicecall_ssn_mt_notify(vc, 0, code, index, &number);
	else
		ofono_voicecall_ssn_mo_notify(vc, 0, code, index);
}

void ril_answer(struct ofono_voicecall *vc, ofono_voicecall_cb_t cb, void *data)
{
	DBG("Answering current call");

	/* Send request to RIL */
	ril_template(RIL_REQUEST_ANSWER, vc, generic_cb, 0, NULL, cb, data);
}

static void ril_send_dtmf_cb(struct ril_msg *message, gpointer user_data)
{
	struct ril_voicecall_data *vd = user_data;

	if (message->error == RIL_E_SUCCESS) {
		/* Remove sent DTMF character from queue */
		gchar *tmp_tone_queue = g_strdup(vd->tone_queue + 1);
		int remaining = strlen(tmp_tone_queue);

		memcpy(vd->tone_queue, tmp_tone_queue, remaining);
		vd->tone_queue[remaining] = '\0';
		g_free(tmp_tone_queue);

		vd->tone_pending = FALSE;

		if (remaining > 0)
			send_one_dtmf(vd);
	} else {
		DBG("error=%d", message->error);
		clear_dtmf_queue(vd);
	}
}

static void send_one_dtmf(struct ril_voicecall_data *vd)
{
	struct parcel rilp;
	char ril_dtmf[2];

	if (vd->tone_pending == TRUE)
		return; /* RIL request pending */

	if (strlen(vd->tone_queue) == 0)
		return; /* nothing to send */

	parcel_init(&rilp);

	/* Ril wants just one character, but we need to send as string */
	ril_dtmf[0] = vd->tone_queue[0];
	ril_dtmf[1] = '\0';

	parcel_w_string(&rilp, ril_dtmf);

	g_ril_append_print_buf(vd->ril, "(%s)", ril_dtmf);

	g_ril_send(vd->ril, RIL_REQUEST_DTMF, &rilp,
			ril_send_dtmf_cb, vd, NULL);

	vd->tone_pending = TRUE;
}

void ril_send_dtmf(struct ofono_voicecall *vc, const char *dtmf,
			ofono_voicecall_cb_t cb, void *data)
{
	struct ril_voicecall_data *vd = ofono_voicecall_get_data(vc);
	struct ofono_error error;

	DBG("Queue '%s'", dtmf);

	/*
	 * Queue any incoming DTMF (up to MAX_DTMF_BUFFER characters),
	 * send them to RIL one-by-one, immediately call back
	 * core with no error
	 */
	g_strlcat(vd->tone_queue, dtmf, MAX_DTMF_BUFFER);
	send_one_dtmf(vd);

	/* We don't really care about errors here */
	decode_ril_error(&error, "OK");
	cb(&error, data);
}

static void clear_dtmf_queue(struct ril_voicecall_data *vd)
{
	g_free(vd->tone_queue);
	vd->tone_queue = g_strnfill(MAX_DTMF_BUFFER + 1, '\0');
	vd->tone_pending = FALSE;
}

void ril_create_multiparty(struct ofono_voicecall *vc,
				ofono_voicecall_cb_t cb, void *data)
{
	ril_template(RIL_REQUEST_CONFERENCE, vc, generic_cb, 0, NULL, cb, data);
}

void ril_private_chat(struct ofono_voicecall *vc, int id,
			ofono_voicecall_cb_t cb, void *data)
{
	struct ril_voicecall_data *vd = ofono_voicecall_get_data(vc);
	struct parcel rilp;

	parcel_init(&rilp);

	/* Payload is an array that holds just one element */
	parcel_w_int32(&rilp, 1);
	parcel_w_int32(&rilp, id);

	g_ril_append_print_buf(vd->ril, "(%d)", id);

	/* Send request to RIL */
	ril_template(RIL_REQUEST_SEPARATE_CONNECTION, vc,
			generic_cb, 0, &rilp, cb, data);
}

void ril_swap_without_accept(struct ofono_voicecall *vc,
				ofono_voicecall_cb_t cb, void *data)
{
	ril_template(RIL_REQUEST_SWITCH_HOLDING_AND_ACTIVE, vc,
			generic_cb, 0, NULL, cb, data);
}

void ril_hold_all_active(struct ofono_voicecall *vc,
				ofono_voicecall_cb_t cb, void *data)
{
	ril_template(RIL_REQUEST_SWITCH_HOLDING_AND_ACTIVE, vc,
			generic_cb, 0, NULL, cb, data);
}

void ril_release_all_held(struct ofono_voicecall *vc,
				ofono_voicecall_cb_t cb, void *data)
{
	ril_template(RIL_REQUEST_HANGUP_WAITING_OR_BACKGROUND, vc,
			generic_cb, 0, NULL, cb, data);
}

void ril_release_all_active(struct ofono_voicecall *vc,
				ofono_voicecall_cb_t cb, void *data)
{
	ril_template(RIL_REQUEST_HANGUP_FOREGROUND_RESUME_BACKGROUND, vc,
			generic_cb, 0, NULL, cb, data);
}

void ril_set_udub(struct ofono_voicecall *vc,
			ofono_voicecall_cb_t cb, void *data)
{
	ril_template(RIL_REQUEST_HANGUP_WAITING_OR_BACKGROUND, vc,
			generic_cb, 0, NULL, cb, data);
}

static gboolean ril_delayed_register(gpointer user_data)
{
	struct ofono_voicecall *vc = user_data;
	struct ril_voicecall_data *vd = ofono_voicecall_get_data(vc);
	struct parcel rilp;

	ofono_voicecall_register(vc);

	/* Initialize call list */
	ril_poll_clcc(vc);

	/* Unsol when call state changes */
	g_ril_register(vd->ril, RIL_UNSOL_RESPONSE_CALL_STATE_CHANGED,
			ril_call_state_notify, vc);

	/* Unsol when call set on hold */
	g_ril_register(vd->ril, RIL_UNSOL_SUPP_SVC_NOTIFICATION,
			ril_ss_notify, vc);

	/* request supplementary service notifications*/
	parcel_init(&rilp);
	parcel_w_int32(&rilp, 1); /* size of array */
	parcel_w_int32(&rilp, 1); /* notifications enabled */

	g_ril_append_print_buf(vd->ril, "(1)");

	g_ril_send(vd->ril, RIL_REQUEST_SET_SUPP_SVC_NOTIFICATION, &rilp,
			NULL, vc, NULL);

	return FALSE;
}

int ril_voicecall_probe(struct ofono_voicecall *vc, unsigned int vendor,
			void *data)
{
	GRil *ril = data;
	struct ril_voicecall_data *vd = g_new0(struct ril_voicecall_data, 1);

	vd->ril = g_ril_clone(ril);
	vd->vendor = vendor;
	vd->cb = NULL;
	vd->data = NULL;

	clear_dtmf_queue(vd);

	ofono_voicecall_set_data(vc, vd);

	g_idle_add(ril_delayed_register, vc);

	return 0;
}

void ril_voicecall_remove(struct ofono_voicecall *vc)
{
	struct ril_voicecall_data *vd = ofono_voicecall_get_data(vc);

	if (vd->clcc_source)
		g_source_remove(vd->clcc_source);

	g_slist_free_full(vd->calls, g_free);

	ofono_voicecall_set_data(vc, NULL);

	g_ril_unref(vd->ril);
	g_free(vd->tone_queue);
	g_free(vd);
}

static struct ofono_voicecall_driver driver = {
	.name			= RILMODEM,
	.probe			= ril_voicecall_probe,
	.remove			= ril_voicecall_remove,
	.dial			= ril_dial,
	.answer			= ril_answer,
	.hangup_all		= ril_hangup_all,
	.release_specific	= ril_hangup_specific,
	.send_tones		= ril_send_dtmf,
	.create_multiparty	= ril_create_multiparty,
	.private_chat		= ril_private_chat,
	.swap_without_accept	= ril_swap_without_accept,
	.hold_all_active	= ril_hold_all_active,
	.release_all_held	= ril_release_all_held,
	.set_udub		= ril_set_udub,
	.release_all_active	= ril_release_all_active,
};

void ril_voicecall_init(void)
{
	ofono_voicecall_driver_register(&driver);
}

void ril_voicecall_exit(void)
{
	ofono_voicecall_driver_unregister(&driver);
}
