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

#include "ril_plugin.h"
#include "ril_constants.h"
#include "ril_ecclist.h"
#include "ril_util.h"
#include "ril_log.h"

#include "common.h"

/* Amount of ms we wait between CLCC calls */
#define FLAG_NEED_CLIP 1
#define MAX_DTMF_BUFFER 32

enum ril_voicecall_events {
	VOICECALL_EVENT_CALL_STATE_CHANGED,
	VOICECALL_EVENT_SUPP_SVC_NOTIFICATION,
	VOICECALL_EVENT_RINGBACK_TONE,
	VOICECALL_EVENT_COUNT,
};

struct ril_voicecall {
	GSList *calls;
	GRilIoChannel *io;
	GRilIoQueue *q;
	struct ofono_voicecall *vc;
	struct ril_ecclist *ecclist;
	unsigned int local_release;
	unsigned char flags;
	ofono_voicecall_cb_t cb;
	void *data;
	guint timer_id;
	gchar *tone_queue;
	guint send_dtmf_id;
	guint clcc_poll_id;
	gulong event_id[VOICECALL_EVENT_COUNT];
	gulong supp_svc_notification_id;
	gulong ringback_tone_event_id;
	gulong ecclist_change_id;
};

struct ril_voicecall_change_state_req {
	struct ofono_voicecall *vc;
	ofono_voicecall_cb_t cb;
	gpointer data;
	int affected_types;
};

struct lastcause_req {
	struct ofono_voicecall *vc;
	int id;
};

static void ril_voicecall_send_one_dtmf(struct ril_voicecall *vd);
static void ril_voicecall_clear_dtmf_queue(struct ril_voicecall *vd);

/*
 * structs ofono_voicecall and voicecall are fully defined
 * in src/voicecall.c; we need (read) access to the
 * call objects, so partially redefine them here.
 */
struct ofono_voicecall {
	GSList *call_list;
	/* ... */
};

struct voicecall {
	struct ofono_call *call;
	/* ... */
};

static inline struct ril_voicecall *ril_voicecall_get_data(
						struct ofono_voicecall *vc)
{
	return ofono_voicecall_get_data(vc);
}

static gint ril_voicecall_compare(gconstpointer a, gconstpointer b)
{
	const struct ofono_call *ca = a;
	const struct ofono_call *cb = b;

	if (ca->id < cb->id)
		return -1;

	if (ca->id > cb->id)
		return 1;

	return 0;
}

static GSList *ril_voicecall_parse_clcc(const void *data, guint len)
{
	GRilIoParser rilp;
	GSList *l = NULL;
	int num = 0, i;
	gchar *number, *name;

	grilio_parser_init(&rilp, data, len);

	/* Number of RIL_Call structs */
	
	grilio_parser_get_int32(&rilp, &num);
	for (i = 0; i < num; i++) {
		struct ofono_call *call = g_new(struct ofono_call, 1);
		gint tmp;

		ofono_call_init(call);
		grilio_parser_get_int32(&rilp, &call->status);
		grilio_parser_get_uint32(&rilp, &call->id);
		grilio_parser_get_int32(&rilp, &call->phone_number.type);
		grilio_parser_get_int32(&rilp, NULL); /* isMpty */

		tmp = 0;
		grilio_parser_get_int32(&rilp, &tmp);
		call->direction = (tmp ?              /* isMT */
			CALL_DIRECTION_MOBILE_TERMINATED :
			CALL_DIRECTION_MOBILE_ORIGINATED);

		grilio_parser_get_int32(&rilp, NULL); /* als */
		grilio_parser_get_int32(&rilp, &call->type); /* isVoice */
		grilio_parser_get_int32(&rilp, NULL); /* isVoicePrivacy */
		number = grilio_parser_get_utf8(&rilp);
		if (number) {
			strncpy(call->phone_number.number, number,
				OFONO_MAX_PHONE_NUMBER_LENGTH);
			g_free(number);
		}
		grilio_parser_get_int32(&rilp, NULL); /* numberPresentation */
		name = grilio_parser_get_utf8(&rilp);
		if (name) {
			strncpy(call->name, name, OFONO_MAX_CALLER_NAME_LENGTH);
			g_free(name);
		}
		grilio_parser_get_int32(&rilp, NULL); /* namePresentation */
		grilio_parser_get_int32(&rilp, &tmp); /* uusInfo */
		GASSERT(!tmp);

		if (strlen(call->phone_number.number) > 0) {
			call->clip_validity = 0;
		} else {
			call->clip_validity = 2;
		}

		DBG("[id=%d,status=%d,type=%d,number=%s,name=%s]",
				call->id, call->status, call->type,
				call->phone_number.number, call->name);

		l = g_slist_insert_sorted(l, call, ril_voicecall_compare);
	}

	return l;
}

/* Valid call statuses have value >= 0 */
static int call_status_with_id(struct ofono_voicecall *vc, int id)
{
	GSList *l;
	struct voicecall *v;

	GASSERT(vc);

	for (l = vc->call_list; l; l = l->next) {
		v = l->data;
		if (v->call->id == id) {
			return v->call->status;
		}
	}

	return -1;
}

static void ril_voicecall_lastcause_cb(GRilIoChannel *io, int status,
				const void *data, guint len, void *user_data)
{
	struct lastcause_req *reqdata = user_data;
	struct ofono_voicecall *vc = reqdata->vc;
	int tmp;
	int id = reqdata->id;
	int call_status;

	enum ofono_disconnect_reason reason = OFONO_DISCONNECT_REASON_ERROR;
	int last_cause = CALL_FAIL_ERROR_UNSPECIFIED;
	GRilIoParser rilp;
	grilio_parser_init(&rilp, data, len);
	if (grilio_parser_get_int32(&rilp, &tmp) && tmp > 0) {
		grilio_parser_get_int32(&rilp, &last_cause);
	}

	/*
	 * Not all call control cause values specified in 3GPP TS 24.008
	 * "Mobile radio interface Layer 3 specification; Core network
	 * protocols", Annex H, are properly reflected in the RIL API.
	 * For example, cause #21 "call rejected" is mapped to
	 * CALL_FAIL_ERROR_UNSPECIFIED, and thus indistinguishable
	 * from a network failure.
	 */
	switch (last_cause) {
		case CALL_FAIL_UNOBTAINABLE_NUMBER:
		case CALL_FAIL_NORMAL:
		case CALL_FAIL_BUSY:
		case CALL_FAIL_NO_ROUTE_TO_DESTINATION:
		case CALL_FAIL_CHANNEL_UNACCEPTABLE:
		case CALL_FAIL_OPERATOR_DETERMINED_BARRING:
		case CALL_FAIL_NO_USER_RESPONDING:
		case CALL_FAIL_USER_ALERTING_NO_ANSWER:
		case CALL_FAIL_CALL_REJECTED:
		case CALL_FAIL_NUMBER_CHANGED:
		case CALL_FAIL_ANONYMOUS_CALL_REJECTION:
		case CALL_FAIL_PRE_EMPTION:
		case CALL_FAIL_DESTINATION_OUT_OF_ORDER:
		case CALL_FAIL_INCOMPLETE_NUMBER:
		case CALL_FAIL_FACILITY_REJECTED:
			reason = OFONO_DISCONNECT_REASON_REMOTE_HANGUP;
			break;

		case CALL_FAIL_NORMAL_UNSPECIFIED:
			call_status = call_status_with_id(vc, id);
			if (call_status == CALL_STATUS_ACTIVE ||
			    call_status == CALL_STATUS_HELD ||
			    call_status == CALL_STATUS_DIALING ||
			    call_status == CALL_STATUS_ALERTING) {
				reason = OFONO_DISCONNECT_REASON_REMOTE_HANGUP;
			} else if (call_status == CALL_STATUS_INCOMING) {
				reason = OFONO_DISCONNECT_REASON_LOCAL_HANGUP;
			}
			break;

		case CALL_FAIL_ERROR_UNSPECIFIED:
			call_status = call_status_with_id(vc, id);
			if (call_status == CALL_STATUS_DIALING ||
			    call_status == CALL_STATUS_ALERTING) {
				reason = OFONO_DISCONNECT_REASON_REMOTE_HANGUP;
			}
			break;

		default:
			reason = OFONO_DISCONNECT_REASON_ERROR;
			break;
	}

	ofono_info("Call %d ended with RIL cause %d -> ofono reason %d",
						id, last_cause, reason);

	ofono_voicecall_disconnected(vc, id, reason, NULL);
}

static void ril_voicecall_clcc_poll_cb(GRilIoChannel *io, int status,
				const void *data, guint len, void *user_data)
{
	struct ril_voicecall *vd = user_data;
	GSList *calls;
	GSList *n, *o;
	struct ofono_error error;

	GASSERT(vd->clcc_poll_id);
	vd->clcc_poll_id = 0;

	if (status != RIL_E_SUCCESS) {
		ofono_error("We are polling CLCC and received an error");
		ofono_error("All bets are off for call management");
		return;
	}

	calls = ril_voicecall_parse_clcc(data, len);

	n = calls;
	o = vd->calls;

	while (n || o) {
		struct ofono_call *nc = n ? n->data : NULL;
		struct ofono_call *oc = o ? o->data : NULL;

		if (oc && (nc == NULL || (nc->id > oc->id))) {
			if (vd->local_release & (1 << oc->id)) {
				ofono_voicecall_disconnected(vd->vc, oc->id,
					OFONO_DISCONNECT_REASON_LOCAL_HANGUP,
					NULL);
			} else {
				/* Get disconnect cause before informing
				 * oFono core */
				struct lastcause_req *reqdata =
					g_new0(struct lastcause_req, 1);

				reqdata->vc = vd->vc;
				reqdata->id = oc->id;
				grilio_queue_send_request_full(vd->q, NULL,
					RIL_REQUEST_LAST_CALL_FAIL_CAUSE,
					ril_voicecall_lastcause_cb,
					g_free, reqdata);
			}

			ril_voicecall_clear_dtmf_queue(vd);
			o = o->next;

		} else if (nc && (oc == NULL || (nc->id < oc->id))) {
			/* new call, signal it */
			if (nc->type) {
				ofono_voicecall_notify(vd->vc, nc);
				if (vd->cb) {
					ofono_voicecall_cb_t cb = vd->cb;
					void *cbdata = vd->data;
					vd->cb = NULL;
					vd->data = NULL;
					cb(ril_error_ok(&error), cbdata);
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
			if (oc->clip_validity == 1) {
				nc->clip_validity = oc->clip_validity;
			}

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
				if (nc->type) {
					ofono_voicecall_notify(vd->vc, nc);
				}

				vd->flags &= ~FLAG_NEED_CLIP;
			} else if (memcmp(nc, oc, sizeof(*nc)) && nc->type) {
				ofono_voicecall_notify(vd->vc, nc);
			}

			n = n->next;
			o = o->next;
		}
	}

	g_slist_free_full(vd->calls, g_free);

	vd->calls = calls;
	vd->local_release = 0;
}

static void ril_voicecall_clcc_poll(struct ril_voicecall *vd)
{
	GASSERT(vd);
	if (!vd->clcc_poll_id) {
		GRilIoRequest* req = grilio_request_new();
		grilio_request_set_retry(req, RIL_RETRY_MS, -1);
		vd->clcc_poll_id = grilio_queue_send_request_full(vd->q,
					req, RIL_REQUEST_GET_CURRENT_CALLS,
					ril_voicecall_clcc_poll_cb, NULL, vd);
		grilio_request_unref(req);
	}
}

static void ril_voicecall_request_cb(GRilIoChannel *io, int status,
				const void *data, guint len, void *user_data)
{
	struct ril_voicecall_change_state_req *req = user_data;
	struct ril_voicecall *vd = ril_voicecall_get_data(req->vc);
	struct ofono_error error;

	if (status == RIL_E_SUCCESS) {
		GSList *l;

		if (req->affected_types) {
			for (l = vd->calls; l; l = l->next) {
				struct ofono_call *call = l->data;

				if (req->affected_types & (1 << call->status)) {
					vd->local_release |= (1 << call->id);
				}
			}
		}

		ril_error_init_ok(&error);
	} else {
		ofono_error("generic fail");
		ril_error_init_failure(&error);
	}

	ril_voicecall_clcc_poll(vd);

	/* We have to callback after we schedule a poll if required */
	if (req->cb) {
		req->cb(&error, req->data);
	}
}

static void ril_voicecall_request(const guint rreq, struct ofono_voicecall *vc,
		unsigned int affected_types, GRilIoRequest *ioreq,
		ofono_voicecall_cb_t cb, void *data)
{
	struct ril_voicecall *vd = ril_voicecall_get_data(vc);
	struct ril_voicecall_change_state_req *req;

	req = g_new0(struct ril_voicecall_change_state_req, 1);
	req->vc = vc;
	req->cb = cb;
	req->data = data;
	req->affected_types = affected_types;

	grilio_queue_send_request_full(vd->q, ioreq, rreq,
				ril_voicecall_request_cb, g_free, req);
}

static void ril_voicecall_dial_cb(GRilIoChannel *io, int status,
				const void *data, guint len, void *user_data)
{
	struct ril_voicecall *vd = user_data;

	if (status == RIL_E_SUCCESS) {
		if (vd->cb) {
			/* CLCC will update the oFono call list with
			 * proper ids if it's not done yet */
			ril_voicecall_clcc_poll(vd);
		}
	} else {
		ofono_error("call failed.");

		/*
		 * Even though this dial request may have already been
		 * completed (successfully) by ril_voicecall_clcc_poll_cb,
		 * RIL_REQUEST_DIAL may still fail.
		 */
		if (vd->cb) {
			struct ofono_error error;
			ofono_voicecall_cb_t cb = vd->cb;
			void *cbdata = vd->data;
			vd->cb = NULL;
			vd->data = NULL;
			cb(ril_error_failure(&error), cbdata);
		}
	}
}

static void ril_voicecall_dial(struct ofono_voicecall *vc,
			const struct ofono_phone_number *ph,
			enum ofono_clir_option clir, ofono_voicecall_cb_t cb,
			void *data)
{
	struct ril_voicecall *vd = ril_voicecall_get_data(vc);
	const char *phstr =  phone_number_to_string(ph);
	GRilIoRequest *req = grilio_request_new();

	ofono_info("dialing \"%s\"", phstr);

	DBG("%s,%d,0", phstr, clir);
	GASSERT(!vd->cb);
	vd->cb = cb;
	vd->data = data;

	grilio_request_append_utf8(req, phstr); /* Number to dial */
	grilio_request_append_int32(req, clir); /* CLIR mode */
	grilio_request_append_int32(req, 0);    /* UUS information (absent) */

	grilio_queue_send_request_full(vd->q, req, RIL_REQUEST_DIAL,
					ril_voicecall_dial_cb, NULL, vd);
	grilio_request_unref(req);
}

static void ril_voicecall_hangup_all(struct ofono_voicecall *vc,
			ofono_voicecall_cb_t cb, void *data)
{
	struct ril_voicecall *vd = ril_voicecall_get_data(vc);
	struct ofono_error error;
	GSList *l;

	for (l = vd->calls; l; l = l->next) {
		struct ofono_call *call = l->data;
		GRilIoRequest *req = grilio_request_sized_new(8);

		/* TODO: Hangup just the active ones once we have call
		 * state tracking (otherwise it can't handle ringing) */
		DBG("Hanging up call with id %d", call->id);
		grilio_request_append_int32(req, 1); /* Always 1 - AT+CHLD=1x */
		grilio_request_append_int32(req, call->id);

		/* Send request to RIL */
		ril_voicecall_request(RIL_REQUEST_HANGUP, vc, 0x3f, req,
								NULL, NULL);
		grilio_request_unref(req);
	}

	/* TODO: Deal in case of an error at hungup */
	cb(ril_error_ok(&error), data);
}

static void ril_voicecall_hangup_specific(struct ofono_voicecall *vc,
		int id, ofono_voicecall_cb_t cb, void *data)
{
	GRilIoRequest *req = grilio_request_sized_new(8);
	struct ofono_error error;

	DBG("Hanging up call with id %d", id);
	grilio_request_append_int32(req, 1); /* Always 1 - AT+CHLD=1x */
	grilio_request_append_int32(req, id);

	/* Send request to RIL */
	ril_voicecall_request(RIL_REQUEST_HANGUP, vc, 0x3f, req, NULL, NULL);
	grilio_request_unref(req);
	cb(ril_error_ok(&error), data);
}

static void ril_voicecall_call_state_changed_event(GRilIoChannel *io,
		guint ril_event, const void *data, guint len, void *user_data)
{
	struct ril_voicecall *vd = user_data;

	GASSERT(ril_event == RIL_UNSOL_RESPONSE_CALL_STATE_CHANGED);

	/* Just need to request the call list again */
	ril_voicecall_clcc_poll(vd);
}

static void ril_voicecall_supp_svc_notification_event(GRilIoChannel *io,
		guint ril_event, const void *data, guint len, void *user_data)
{
	GRilIoParser rilp;
	struct ril_voicecall *vd = user_data;
	struct ofono_phone_number phone;
	int type = 0, code = 0, index = 0;
	char *tmp = NULL;

	GASSERT(ril_event == RIL_UNSOL_SUPP_SVC_NOTIFICATION);

	grilio_parser_init(&rilp, data, len);
	grilio_parser_get_int32(&rilp, &type);
	grilio_parser_get_int32(&rilp, &code);
	grilio_parser_get_int32(&rilp, &index);
	grilio_parser_get_int32(&rilp, NULL);
	tmp = grilio_parser_get_utf8(&rilp);

	if (tmp) {
		strncpy(phone.number, tmp, OFONO_MAX_PHONE_NUMBER_LENGTH);
		phone.number[OFONO_MAX_PHONE_NUMBER_LENGTH] = 0;
		g_free(tmp);
	} else {
		phone.number[0] = 0;
	}

	DBG("RIL data: MT/MO: %i, code: %i, index: %i",  type, code, index);

	/* 0 stands for MO intermediate (support TBD), 1 for MT unsolicited */
	if (type == 1) {
		ofono_voicecall_ssn_mt_notify(vd->vc, 0, code, index, &phone);
	} else {
		ofono_error("Unknown SS notification");
	}
}

static void ril_voicecall_answer(struct ofono_voicecall *vc,
			ofono_voicecall_cb_t cb, void *data)
{
	/* Send request to RIL */
	DBG("Answering current call");
	ril_voicecall_request(RIL_REQUEST_ANSWER, vc, 0, NULL, cb, data);
}

static void ril_voicecall_send_dtmf_cb(GRilIoChannel *io, int status,
				const void *data, guint len, void *user_data)
{
	struct ril_voicecall *vd = user_data;

	GASSERT(vd->send_dtmf_id);
	vd->send_dtmf_id = 0;

	if (status == RIL_E_SUCCESS) {
		/* Remove sent DTMF character from queue */
		gchar *tmp = g_strdup(vd->tone_queue + 1);
		g_free(vd->tone_queue);
		vd->tone_queue = tmp;

		/* Send the next one */
		ril_voicecall_send_one_dtmf(vd);
	} else {
		DBG("error=%d", status);
		ril_voicecall_clear_dtmf_queue(vd);
	}
}

static void ril_voicecall_send_one_dtmf(struct ril_voicecall *vd)
{
	if (!vd->send_dtmf_id && vd->tone_queue && vd->tone_queue[0]) {
		GRilIoRequest *req = grilio_request_sized_new(4);

		/* RIL wants just one character */
		DBG("%c", vd->tone_queue[0]);
		grilio_request_append_utf8_chars(req, vd->tone_queue, 1);
		vd->send_dtmf_id = grilio_queue_send_request_full(vd->q, req,
			RIL_REQUEST_DTMF, ril_voicecall_send_dtmf_cb, NULL, vd);
		grilio_request_unref(req);
	}
}

static void ril_voicecall_send_dtmf(struct ofono_voicecall *vc,
		const char *dtmf, ofono_voicecall_cb_t cb, void *data)
{
	struct ril_voicecall *vd = ril_voicecall_get_data(vc);
	struct ofono_error error;

	DBG("Queue '%s'",dtmf);

	/*
	 * Queue any incoming DTMF (up to MAX_DTMF_BUFFER characters),
	 * send them to RIL one-by-one, immediately call back
	 * core with no error
	 */
	g_strlcat(vd->tone_queue, dtmf, MAX_DTMF_BUFFER);
	ril_voicecall_send_one_dtmf(vd);

	cb(ril_error_ok(&error), data);
}

static void ril_voicecall_clear_dtmf_queue(struct ril_voicecall *vd)
{
	g_free(vd->tone_queue);
	vd->tone_queue = g_strnfill(MAX_DTMF_BUFFER + 1, '\0');
	if (vd->send_dtmf_id) {
		grilio_channel_cancel_request(vd->io, vd->send_dtmf_id, FALSE);
		vd->send_dtmf_id = 0;
	}
}

static void ril_voicecall_clcc_poll_on_success(GRilIoChannel *io,
		int status, const void *data, guint len, void *user_data)
{
	if (status == RIL_E_SUCCESS) {
		ril_voicecall_clcc_poll((struct ril_voicecall *)user_data);
	}
}

static void ril_voicecall_create_multiparty(struct ofono_voicecall *vc,
			ofono_voicecall_cb_t cb, void *data)
{
	struct ril_voicecall *vd = ril_voicecall_get_data(vc);
	grilio_queue_send_request_full(vd->q, NULL, RIL_REQUEST_CONFERENCE,
			ril_voicecall_clcc_poll_on_success, NULL, vd);
}

static void ril_voicecall_transfer(struct ofono_voicecall *vc,
			ofono_voicecall_cb_t cb, void *data)
{
	ril_voicecall_request(RIL_REQUEST_EXPLICIT_CALL_TRANSFER,
			vc, 0, NULL, cb, data);
}

static void ril_voicecall_private_chat(struct ofono_voicecall *vc, int id,
			ofono_voicecall_cb_t cb, void *data)
{
	struct ril_voicecall *vd = ril_voicecall_get_data(vc);
	GRilIoRequest *req = grilio_request_sized_new(8);
	grilio_request_append_int32(req, 1);
	grilio_request_append_int32(req, id);
	grilio_queue_send_request_full(vd->q, req,
				RIL_REQUEST_SEPARATE_CONNECTION,
				ril_voicecall_clcc_poll_on_success, NULL, vd);
	grilio_request_unref(req);
}

static void ril_voicecall_swap_without_accept(struct ofono_voicecall *vc,
			ofono_voicecall_cb_t cb, void *data)
{
	ril_voicecall_request(RIL_REQUEST_SWITCH_HOLDING_AND_ACTIVE,
			vc, 0, NULL, cb, data);
}

static void ril_voicecall_hold_all_active(struct ofono_voicecall *vc,
			ofono_voicecall_cb_t cb, void *data)
{
	ril_voicecall_request(RIL_REQUEST_SWITCH_HOLDING_AND_ACTIVE,
			vc, 0, NULL, cb, data);
}

static void ril_voicecall_release_all_held(struct ofono_voicecall *vc,
					ofono_voicecall_cb_t cb, void *data)
{
	ril_voicecall_request(RIL_REQUEST_HANGUP_WAITING_OR_BACKGROUND,
			vc, 0, NULL, cb, data);
}

static void ril_voicecall_release_all_active(struct ofono_voicecall *vc,
					ofono_voicecall_cb_t cb, void *data)
{
	ril_voicecall_request(RIL_REQUEST_HANGUP_FOREGROUND_RESUME_BACKGROUND,
			vc, 0, NULL, cb, data);
}

static void ril_voicecall_set_udub(struct ofono_voicecall *vc,
					ofono_voicecall_cb_t cb, void *data)
{
	ril_voicecall_request(RIL_REQUEST_HANGUP_WAITING_OR_BACKGROUND,
			vc, 0, NULL, cb, data);
}

static gboolean ril_voicecall_enable_supp_svc(struct ril_voicecall *vd)
{
	GRilIoRequest *req = grilio_request_sized_new(8);

	grilio_request_append_int32(req, 1); /* size of array */
	grilio_request_append_int32(req, 1); /* notifications enabled */

	grilio_queue_send_request(vd->q, req,
				RIL_REQUEST_SET_SUPP_SVC_NOTIFICATION);
	grilio_request_unref(req);

	/* Makes this a single shot */
	return FALSE;
}

static void ril_voicecall_ringback_tone_event(GRilIoChannel *io,
		guint code, const void *data, guint len, void *user_data)
{
	struct ril_voicecall *vd = user_data;
	GRilIoParser rilp;
	guint32 playTone = FALSE;
	int tmp;

	GASSERT(code == RIL_UNSOL_RINGBACK_TONE);
	grilio_parser_init(&rilp, data, len);
	if (grilio_parser_get_int32(&rilp, &tmp) && tmp > 0) {
		grilio_parser_get_uint32(&rilp, &playTone);
	}

	DBG("play ringback tone: %d", playTone);
	ofono_voicecall_ringback_tone_notify(vd->vc, playTone);
}

static void ril_voicecall_ecclist_changed(struct ril_ecclist *list, void *data)
{
	struct ril_voicecall *vd = data;

	ofono_voicecall_en_list_notify(vd->vc, vd->ecclist->list);
}

static gboolean ril_delayed_register(gpointer user_data)
{
	struct ril_voicecall *vd = user_data;

	GASSERT(vd->timer_id);
	vd->timer_id = 0;
	ofono_voicecall_register(vd->vc);

	/* Emergency Call Codes */
	if (vd->ecclist) {
		ofono_voicecall_en_list_notify(vd->vc, vd->ecclist->list);
		vd->ecclist_change_id =
			ril_ecclist_add_list_changed_handler(vd->ecclist,
					ril_voicecall_ecclist_changed, vd);
	}

	/* Initialize call list */
	ril_voicecall_clcc_poll(vd);

	/* request supplementary service notifications*/
	ril_voicecall_enable_supp_svc(vd);

	/* Unsol when call state changes */
	vd->event_id[VOICECALL_EVENT_CALL_STATE_CHANGED] =
		grilio_channel_add_unsol_event_handler(vd->io,
				ril_voicecall_call_state_changed_event,
				RIL_UNSOL_RESPONSE_CALL_STATE_CHANGED, vd);

	/* Unsol when call set in hold */
	vd->event_id[VOICECALL_EVENT_SUPP_SVC_NOTIFICATION] =
		grilio_channel_add_unsol_event_handler(vd->io,
				ril_voicecall_supp_svc_notification_event,
				RIL_UNSOL_SUPP_SVC_NOTIFICATION, vd);

	/* Register for ringback tone notifications */
	vd->event_id[VOICECALL_EVENT_RINGBACK_TONE] =
		grilio_channel_add_unsol_event_handler(vd->io,
				ril_voicecall_ringback_tone_event,
				RIL_UNSOL_RINGBACK_TONE, vd);

	/* This makes the timeout a single-shot */
	return FALSE;
}

static int ril_voicecall_probe(struct ofono_voicecall *vc, unsigned int vendor,
				void *data)
{
	struct ril_modem *modem = data;
	struct ril_voicecall *vd;

	DBG("");
	vd = g_new0(struct ril_voicecall, 1);
	vd->io = grilio_channel_ref(ril_modem_io(modem));
	vd->q = grilio_queue_new(vd->io);
	vd->vc = vc;
	vd->timer_id = g_idle_add(ril_delayed_register, vd);
	if (modem->ecclist_file) {
		vd->ecclist = ril_ecclist_new(modem->ecclist_file);
	}
	ril_voicecall_clear_dtmf_queue(vd);
	ofono_voicecall_set_data(vc, vd);
	return 0;
}

static void ril_voicecall_remove(struct ofono_voicecall *vc)
{
	struct ril_voicecall *vd = ril_voicecall_get_data(vc);

	DBG("");
	ofono_voicecall_set_data(vc, NULL);
	g_slist_free_full(vd->calls, g_free);

	if (vd->timer_id > 0) {
		g_source_remove(vd->timer_id);
	}

	ril_ecclist_remove_handler(vd->ecclist, vd->ecclist_change_id);
	ril_ecclist_unref(vd->ecclist);

	grilio_channel_remove_handlers(vd->io, vd->event_id,
						G_N_ELEMENTS(vd->event_id));
	grilio_channel_unref(vd->io);
	grilio_queue_cancel_all(vd->q, FALSE);
	grilio_queue_unref(vd->q);
	g_free(vd->tone_queue);
	g_free(vd);
}

const struct ofono_voicecall_driver ril_voicecall_driver = {
	.name                   = RILMODEM_DRIVER,
	.probe                  = ril_voicecall_probe,
	.remove                 = ril_voicecall_remove,
	.dial                   = ril_voicecall_dial,
	.answer                 = ril_voicecall_answer,
	.hangup_all             = ril_voicecall_hangup_all,
	.release_specific       = ril_voicecall_hangup_specific,
	.send_tones             = ril_voicecall_send_dtmf,
	.create_multiparty      = ril_voicecall_create_multiparty,
	.transfer               = ril_voicecall_transfer,
	.private_chat           = ril_voicecall_private_chat,
	.swap_without_accept    = ril_voicecall_swap_without_accept,
	.hold_all_active        = ril_voicecall_hold_all_active,
	.release_all_held       = ril_voicecall_release_all_held,
	.set_udub               = ril_voicecall_set_udub,
	.release_all_active     = ril_voicecall_release_all_active
};

/*
 * Local Variables:
 * mode: C
 * c-basic-offset: 8
 * indent-tabs-mode: t
 * End:
 */
