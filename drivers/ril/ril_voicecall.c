/*
 *  oFono - Open Source Telephony - RIL-based devices
 *
 *  Copyright (C) 2015-2017 Jolla Ltd.
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
#include "ril_ecclist.h"
#include "ril_util.h"
#include "ril_log.h"

#include "common.h"

#include <gutil_ints.h>
#include <gutil_ring.h>
#include <gutil_idlequeue.h>
#include <gutil_intarray.h>

#define FLAG_NEED_CLIP 1

#define VOICECALL_BLOCK_TIMEOUT_MS (5*1000)

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
	unsigned char flags;
	ofono_voicecall_cb_t cb;
	void *data;
	GUtilIntArray *local_release_ids;
	GUtilIdleQueue *idleq;
	GUtilRing *dtmf_queue;
	GUtilInts *local_hangup_reasons;
	GUtilInts *remote_hangup_reasons;
	guint send_dtmf_id;
	guint clcc_poll_id;
	gulong event_id[VOICECALL_EVENT_COUNT];
	gulong supp_svc_notification_id;
	gulong ringback_tone_event_id;
	gulong ecclist_change_id;
};

struct ril_voicecall_request_data {
	int ref_count;
	int pending_call_count;
	int success;
	struct ofono_voicecall *vc;
	ofono_voicecall_cb_t cb;
	gpointer data;
};

struct lastcause_req {
	struct ril_voicecall *vd;
	int id;
};

static void ril_voicecall_send_one_dtmf(struct ril_voicecall *vd);
static void ril_voicecall_clear_dtmf_queue(struct ril_voicecall *vd);

struct ril_voicecall_request_data *ril_voicecall_request_data_new
	(struct ofono_voicecall *vc, ofono_voicecall_cb_t cb, void *data)
{
	struct ril_voicecall_request_data *req =
		g_slice_new0(struct ril_voicecall_request_data);

	req->ref_count = 1;
	req->vc = vc;
	req->cb = cb;
	req->data = data;
	return req;
}

static void ril_voicecall_request_data_unref
				(struct ril_voicecall_request_data *req)
{
	if (!--req->ref_count) {
		g_slice_free(struct ril_voicecall_request_data, req);
	}
}

static void ril_voicecall_request_data_free(gpointer data)
{
	ril_voicecall_request_data_unref(data);
}

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
static int ril_voicecall_status_with_id(struct ofono_voicecall *vc,
							unsigned int id)
{
	struct ofono_call *call = ofono_voicecall_find_call(vc, id);

	return call ? call->status : -1;
}

/* Tries to parse the payload as a uint followed by a string */
static int ril_voicecall_parse_lastcause_1(const void *data, guint len)
{
	int result = -1;

	if (len > 8) {
		int code;
		char *msg = NULL;
		GRilIoParser rilp;

		grilio_parser_init(&rilp, data, len);
		if (grilio_parser_get_int32(&rilp, &code) && code >= 0 &&
				(msg = grilio_parser_get_utf8(&rilp)) &&
				grilio_parser_at_end(&rilp)) {
			DBG("%d \"%s\"", code, msg);
			result = code;
		}
		g_free(msg);
	}

	return result;
}

static void ril_voicecall_lastcause_cb(GRilIoChannel *io, int status,
				const void *data, guint len, void *user_data)
{
	struct lastcause_req *reqdata = user_data;
	struct ril_voicecall *vd = reqdata->vd;
	struct ofono_voicecall *vc = vd->vc;
	int id = reqdata->id;
	int call_status;

	enum ofono_disconnect_reason reason = OFONO_DISCONNECT_REASON_ERROR;
	int last_cause;

	/*
	 * According to ril.h:
	 *
	 *   "response" is a "int *"
	 *   ((int *)response)[0] is RIL_LastCallFailCause. GSM failure
	 *   reasons are mapped to cause codes defined in TS 24.008 Annex H
	 *   where possible.
	 *
	 * However some RILs feel free to invent their own formats,
	 * try those first.
	 */

	last_cause = ril_voicecall_parse_lastcause_1(data, len);
	if (last_cause < 0) {
		GRilIoParser rilp;
		int num, code;

		/* Default format described in ril.h */
		grilio_parser_init(&rilp, data, len);
		if (grilio_parser_get_int32(&rilp, &num) && num == 1 &&
				grilio_parser_get_int32(&rilp, &code) &&
				grilio_parser_at_end(&rilp)) {
			last_cause = code;
		} else {
			ofono_warn("Unable to parse last call fail cause");
			last_cause = CALL_FAIL_ERROR_UNSPECIFIED;
		}
	}

	/*
	 * Not all call control cause values specified in 3GPP TS 24.008
	 * "Mobile radio interface Layer 3 specification; Core network
	 * protocols", Annex H, are properly reflected in the RIL API.
	 * For example, cause #21 "call rejected" is mapped to
	 * CALL_FAIL_ERROR_UNSPECIFIED, and thus indistinguishable
	 * from a network failure.
	 */
	if (gutil_ints_contains(vd->remote_hangup_reasons, last_cause)) {
		DBG("hangup cause %d => remote hangup", last_cause);
		reason = OFONO_DISCONNECT_REASON_REMOTE_HANGUP;
	} else if (gutil_ints_contains(vd->local_hangup_reasons, last_cause)) {
		DBG("hangup cause %d => local hangup", last_cause);
		reason = OFONO_DISCONNECT_REASON_LOCAL_HANGUP;
	} else {
		switch (last_cause) {
		case CALL_FAIL_UNOBTAINABLE_NUMBER:
		case CALL_FAIL_NORMAL:
		case CALL_FAIL_BUSY:
		case CALL_FAIL_NO_ROUTE_TO_DESTINATION:
		case CALL_FAIL_CHANNEL_UNACCEPTABLE:
		case CALL_FAIL_OPERATOR_DETERMINED_BARRING:
		case CALL_FAIL_NO_USER_RESPONDING:
		case CALL_FAIL_NO_ANSWER_FROM_USER:
		case CALL_FAIL_CALL_REJECTED:
		case CALL_FAIL_NUMBER_CHANGED:
		case CALL_FAIL_ANONYMOUS_CALL_REJECTION:
		case CALL_FAIL_PRE_EMPTION:
		case CALL_FAIL_DESTINATION_OUT_OF_ORDER:
		case CALL_FAIL_INVALID_NUMBER_FORMAT:
		case CALL_FAIL_FACILITY_REJECTED:
			reason = OFONO_DISCONNECT_REASON_REMOTE_HANGUP;
			break;

		case CALL_FAIL_NORMAL_UNSPECIFIED:
			call_status = ril_voicecall_status_with_id(vc, id);
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
			call_status = ril_voicecall_status_with_id(vc, id);
			if (call_status == CALL_STATUS_DIALING ||
			    call_status == CALL_STATUS_ALERTING ||
			    call_status == CALL_STATUS_INCOMING) {
				reason = OFONO_DISCONNECT_REASON_REMOTE_HANGUP;
			}
			break;

		default:
			reason = OFONO_DISCONNECT_REASON_ERROR;
			break;
		}
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

	/*
	 * Only RIL_E_SUCCESS and RIL_E_RADIO_NOT_AVAILABLE are expected here,
	 * all other errors are filtered out by ril_voicecall_clcc_retry()
	 */
	if (status == RIL_E_SUCCESS) {
		calls = ril_voicecall_parse_clcc(data, len);
	} else {
		/* RADIO_NOT_AVAILABLE == no calls */
		GASSERT(status == RIL_E_RADIO_NOT_AVAILABLE);
		calls = NULL;
	}

	n = calls;
	o = vd->calls;

	while (n || o) {
		struct ofono_call *nc = n ? n->data : NULL;
		struct ofono_call *oc = o ? o->data : NULL;

		if (oc && (nc == NULL || (nc->id > oc->id))) {
			/* old call is gone */
			if (gutil_int_array_remove_all_fast(
					vd->local_release_ids, oc->id)) {
				ofono_voicecall_disconnected(vd->vc, oc->id,
					OFONO_DISCONNECT_REASON_LOCAL_HANGUP,
					NULL);
			} else {
				/* Get disconnect cause before informing
				 * oFono core */
				struct lastcause_req *reqdata =
					g_new0(struct lastcause_req, 1);

				reqdata->vd = vd;
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
}

static gboolean ril_voicecall_clcc_retry(GRilIoRequest* req, int ril_status,
		const void* response_data, guint response_len, void* user_data)
{
	switch (ril_status) {
	case RIL_E_SUCCESS:
	case RIL_E_RADIO_NOT_AVAILABLE:
		return FALSE;
	default:
		return TRUE;
	}
}

static void ril_voicecall_clcc_poll(struct ril_voicecall *vd)
{
	GASSERT(vd);
	if (!vd->clcc_poll_id) {
		GRilIoRequest* req = grilio_request_new();
		grilio_request_set_retry(req, RIL_RETRY_MS, -1);
		grilio_request_set_retry_func(req, ril_voicecall_clcc_retry);
		vd->clcc_poll_id = grilio_queue_send_request_full(vd->q,
					req, RIL_REQUEST_GET_CURRENT_CALLS,
					ril_voicecall_clcc_poll_cb, NULL, vd);
		grilio_request_unref(req);
	}
}

static void ril_voicecall_request_cb(GRilIoChannel *io, int status,
				const void *data, guint len, void *user_data)
{
	struct ril_voicecall_request_data *req = user_data;
	struct ril_voicecall *vd = ril_voicecall_get_data(req->vc);

	ril_voicecall_clcc_poll(vd);

	/*
	 * The ofono API call is considered successful if at least one
	 * associated RIL request succeeds.
	 */
	if (status == RIL_E_SUCCESS) {
		req->success++;
	}

	/*
	 * Only invoke the callback if this is the last request associated
	 * with this ofono api call (pending call count becomes zero).
	 */
	GASSERT(req->pending_call_count > 0);
	if (!--req->pending_call_count && req->cb) {
		struct ofono_error error;

		if (req->success) {
			ril_error_init_ok(&error);
		} else {
			ril_error_init_failure(&error);
		}

		req->cb(&error, req->data);
	}
}

static void ril_voicecall_request(const guint code, struct ofono_voicecall *vc,
		GRilIoRequest *req, ofono_voicecall_cb_t cb, void *data)
{
	struct ril_voicecall_request_data *req_data =
		ril_voicecall_request_data_new(vc, cb, data);

	req_data->pending_call_count++;
	grilio_queue_send_request_full(ril_voicecall_get_data(vc)->q, req,
				code, ril_voicecall_request_cb,
				ril_voicecall_request_data_free, req_data);
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

static void ril_voicecall_submit_hangup_req(struct ofono_voicecall *vc,
			int id, struct ril_voicecall_request_data *req)
{
	struct ril_voicecall *vd = ril_voicecall_get_data(vc);
	GRilIoRequest *ioreq = grilio_request_array_int32_new(1, id);

	/* Append the call id to the list of calls being released locally */
	GASSERT(!gutil_int_array_contains(vd->local_release_ids, id));
	gutil_int_array_append(vd->local_release_ids, id);

	/* Send request to RIL. ril_voicecall_request_data_free will unref
	 * the request data */
	req->ref_count++;
	req->pending_call_count++;
	grilio_queue_send_request_full(vd->q, ioreq, RIL_REQUEST_HANGUP,
				ril_voicecall_request_cb,
				ril_voicecall_request_data_free, req);
	grilio_request_unref(ioreq);
}

static void ril_voicecall_hangup_all(struct ofono_voicecall *vc,
			ofono_voicecall_cb_t cb, void *data)
{
	struct ril_voicecall *vd = ril_voicecall_get_data(vc);

	if (vd->calls) {
		GSList *l;
		struct ril_voicecall_request_data *req =
			ril_voicecall_request_data_new(vc, cb, data);

		/*
		 * Here the idea is that we submit (potentially) multiple
		 * hangup requests to RIL and invoke the callback after
		 * the last request has completed (pending call count
		 * becomes zero).
		 */
		for (l = vd->calls; l; l = l->next) {
			struct ofono_call *call = l->data;

			/* Send request to RIL */
			DBG("Hanging up call with id %d", call->id);
			ril_voicecall_submit_hangup_req(vc, call->id, req);
		}

		/* Release our reference */
		ril_voicecall_request_data_unref(req);
	} else {
		/* No calls */
		struct ofono_error error;
		cb(ril_error_ok(&error), data);
	}
}

static void ril_voicecall_release_specific(struct ofono_voicecall *vc,
		int id, ofono_voicecall_cb_t cb, void *data)
{
	struct ril_voicecall_request_data *req =
		ril_voicecall_request_data_new(vc, cb, data);

	DBG("Hanging up call with id %d", id);
	ril_voicecall_submit_hangup_req(vc, id, req);
	ril_voicecall_request_data_unref(req);
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
	ril_voicecall_request(RIL_REQUEST_ANSWER, vc, NULL, cb, data);
}

static void ril_voicecall_send_dtmf_cb(GRilIoChannel *io, int status,
				const void *data, guint len, void *user_data)
{
	struct ril_voicecall *vd = user_data;

	GASSERT(vd->send_dtmf_id);
	vd->send_dtmf_id = 0;

	if (status == RIL_E_SUCCESS) {
		/* Send the next one */
		ril_voicecall_send_one_dtmf(vd);
	} else {
		DBG("error=%d", status);
		ril_voicecall_clear_dtmf_queue(vd);
	}
}

static void ril_voicecall_send_one_dtmf(struct ril_voicecall *vd)
{
	if (!vd->send_dtmf_id && gutil_ring_size(vd->dtmf_queue) > 0) {
		GRilIoRequest *req = grilio_request_sized_new(4);
		const char dtmf_char = (char)
			GPOINTER_TO_UINT(gutil_ring_get(vd->dtmf_queue));

		/* RIL wants just one character */
		GASSERT(dtmf_char);
		DBG("%c", dtmf_char);
		grilio_request_append_utf8_chars(req, &dtmf_char, 1);
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

	/*
	 * Queue any incoming DTMF, send them to RIL one-by-one,
	 * immediately call back core with no error
	 */
	DBG("Queue '%s'", dtmf);
	while (*dtmf) {
		gutil_ring_put(vd->dtmf_queue, GUINT_TO_POINTER(*dtmf));
		dtmf++;
	}

	ril_voicecall_send_one_dtmf(vd);
	cb(ril_error_ok(&error), data);
}

static void ril_voicecall_clear_dtmf_queue(struct ril_voicecall *vd)
{
	gutil_ring_clear(vd->dtmf_queue);
	if (vd->send_dtmf_id) {
		grilio_channel_cancel_request(vd->io, vd->send_dtmf_id, FALSE);
		vd->send_dtmf_id = 0;
	}
}

static void ril_voicecall_create_multiparty(struct ofono_voicecall *vc,
			ofono_voicecall_cb_t cb, void *data)
{
	ril_voicecall_request(RIL_REQUEST_CONFERENCE, vc, NULL, cb, data);
}

static void ril_voicecall_transfer(struct ofono_voicecall *vc,
			ofono_voicecall_cb_t cb, void *data)
{
	ril_voicecall_request(RIL_REQUEST_EXPLICIT_CALL_TRANSFER,
						vc, NULL, cb, data);
}

static void ril_voicecall_private_chat(struct ofono_voicecall *vc, int id,
			ofono_voicecall_cb_t cb, void *data)
{
	GRilIoRequest *req = grilio_request_array_int32_new(1, id);
	struct ofono_error error;

	DBG("Private chat with id %d", id);
	ril_voicecall_request(RIL_REQUEST_SEPARATE_CONNECTION,
			vc, req, NULL, NULL);
	grilio_request_unref(req);
	cb(ril_error_ok(&error), data);
}

static void ril_voicecall_swap_without_accept(struct ofono_voicecall *vc,
			ofono_voicecall_cb_t cb, void *data)
{
	DBG("");
	ril_voicecall_request(RIL_REQUEST_SWITCH_HOLDING_AND_ACTIVE,
						vc, NULL, cb, data);
}

static void ril_voicecall_hold_all_active(struct ofono_voicecall *vc,
			ofono_voicecall_cb_t cb, void *data)
{
	DBG("");
	ril_voicecall_request(RIL_REQUEST_SWITCH_HOLDING_AND_ACTIVE,
						vc, NULL, cb, data);
}

static void ril_voicecall_release_all_held(struct ofono_voicecall *vc,
					ofono_voicecall_cb_t cb, void *data)
{
	DBG("");
	ril_voicecall_request(RIL_REQUEST_HANGUP_WAITING_OR_BACKGROUND,
						vc, NULL, cb, data);
}

static void ril_voicecall_release_all_active(struct ofono_voicecall *vc,
					ofono_voicecall_cb_t cb, void *data)
{
	DBG("");
	ril_voicecall_request(RIL_REQUEST_HANGUP_FOREGROUND_RESUME_BACKGROUND,
						vc, NULL, cb, data);
}

static void ril_voicecall_set_udub(struct ofono_voicecall *vc,
					ofono_voicecall_cb_t cb, void *data)
{
	DBG("");
	ril_voicecall_request(RIL_REQUEST_HANGUP_WAITING_OR_BACKGROUND,
						vc, NULL, cb, data);
}

static void ril_voicecall_enable_supp_svc(struct ril_voicecall *vd)
{
	GRilIoRequest *req = grilio_request_array_int32_new(1, 1);

	grilio_request_set_timeout(req, VOICECALL_BLOCK_TIMEOUT_MS);
	grilio_request_set_blocking(req, TRUE);
	grilio_queue_send_request(vd->q, req,
				RIL_REQUEST_SET_SUPP_SVC_NOTIFICATION);
	grilio_request_unref(req);
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

static void ril_voicecall_register(gpointer user_data)
{
	struct ril_voicecall *vd = user_data;

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
}

static int ril_voicecall_probe(struct ofono_voicecall *vc, unsigned int vendor,
				void *data)
{
	struct ril_modem *modem = data;
	const struct ril_slot_config *cfg = &modem->config;
	struct ril_voicecall *vd;

	DBG("");
	vd = g_new0(struct ril_voicecall, 1);
	vd->io = grilio_channel_ref(ril_modem_io(modem));
	vd->q = grilio_queue_new(vd->io);
	vd->dtmf_queue = gutil_ring_new();
	vd->local_hangup_reasons = gutil_ints_ref(cfg->local_hangup_reasons);
	vd->remote_hangup_reasons = gutil_ints_ref(cfg->remote_hangup_reasons);
	vd->local_release_ids = gutil_int_array_new();
	vd->idleq = gutil_idle_queue_new();
	vd->vc = vc;
	if (modem->ecclist_file) {
		vd->ecclist = ril_ecclist_new(modem->ecclist_file);
	}
	ril_voicecall_clear_dtmf_queue(vd);
	ofono_voicecall_set_data(vc, vd);
	gutil_idle_queue_add(vd->idleq, ril_voicecall_register, vd);
	return 0;
}

static void ril_voicecall_remove(struct ofono_voicecall *vc)
{
	struct ril_voicecall *vd = ril_voicecall_get_data(vc);

	DBG("");
	ofono_voicecall_set_data(vc, NULL);
	g_slist_free_full(vd->calls, g_free);

	ril_ecclist_remove_handler(vd->ecclist, vd->ecclist_change_id);
	ril_ecclist_unref(vd->ecclist);

	grilio_channel_remove_handlers(vd->io, vd->event_id,
						G_N_ELEMENTS(vd->event_id));
	grilio_channel_unref(vd->io);
	grilio_queue_cancel_all(vd->q, FALSE);
	grilio_queue_unref(vd->q);
	gutil_ring_unref(vd->dtmf_queue);
	gutil_ints_unref(vd->local_hangup_reasons);
	gutil_ints_unref(vd->remote_hangup_reasons);
	gutil_int_array_free(vd->local_release_ids, TRUE);
	gutil_idle_queue_free(vd->idleq);
	g_free(vd);
}

const struct ofono_voicecall_driver ril_voicecall_driver = {
	.name                   = RILMODEM_DRIVER,
	.probe                  = ril_voicecall_probe,
	.remove                 = ril_voicecall_remove,
	.dial                   = ril_voicecall_dial,
	.answer                 = ril_voicecall_answer,
	.hangup_all             = ril_voicecall_hangup_all,
	.release_specific       = ril_voicecall_release_specific,
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
