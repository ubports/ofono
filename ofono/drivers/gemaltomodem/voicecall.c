/*
 *
 *  oFono - Open Source Telephony
 *
 *  Copyright (C) 2008-2011  Intel Corporation. All rights reserved.
 *  Copyright (C) 2018 Gemalto M2M
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

#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <errno.h>

#include <glib.h>

#include <ofono/log.h>
#include <ofono/modem.h>
#include <ofono/voicecall.h>

#include "gatchat.h"
#include "gatresult.h"

#include "common.h"

#include "gemaltomodem.h"

static const char *clcc_prefix[] = { "+CLCC:", NULL };
static const char *none_prefix[] = { NULL };

struct voicecall_data {
	GAtChat *chat;
	GSList *calls;
	unsigned int local_release;
	GSList *new_calls;
};

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
	int affected_types;
};

static void generic_cb(gboolean ok, GAtResult *result, gpointer user_data)
{
	struct change_state_req *req = user_data;
	struct voicecall_data *vd = ofono_voicecall_get_data(req->vc);
	struct ofono_error error;

	decode_at_error(&error, g_at_result_final_response(result));

	if (ok && req->affected_types) {
		GSList *l;
		struct ofono_call *call;

		for (l = vd->calls; l; l = l->next) {
			call = l->data;

			if (req->affected_types & (1 << call->status))
				vd->local_release |= (1 << call->id);
		}
	}

	req->cb(&error, req->data);
}

static void gemalto_call_common(const char *cmd, struct ofono_voicecall *vc,
				GAtResultFunc result_cb,
				unsigned int affected_types,
				ofono_voicecall_cb_t cb, void *data)
{
	struct voicecall_data *vd = ofono_voicecall_get_data(vc);
	struct change_state_req *req = g_new0(struct change_state_req, 1);

	req->vc = vc;
	req->cb = cb;
	req->data = data;
	req->affected_types = affected_types;

	if (g_at_chat_send(vd->chat, cmd, none_prefix,
				result_cb, req, g_free) > 0)
		return;

	g_free(req);
	CALLBACK_WITH_FAILURE(cb, data);
}

static void gemalto_answer(struct ofono_voicecall *vc,
				ofono_voicecall_cb_t cb, void *data)
{
	gemalto_call_common("ATA", vc, generic_cb, 0, cb, data);
}

static void gemalto_hangup_all(struct ofono_voicecall *vc,
				ofono_voicecall_cb_t cb, void *data)
{
	unsigned int affected = (1 << CALL_STATUS_INCOMING) |
				(1 << CALL_STATUS_DIALING) |
				(1 << CALL_STATUS_ALERTING) |
				(1 << CALL_STATUS_WAITING) |
				(1 << CALL_STATUS_HELD) |
				(1 << CALL_STATUS_ACTIVE);

	/* Hangup all calls */
	gemalto_call_common("AT+CHUP", vc, generic_cb, affected, cb, data);
}

static void gemalto_hangup(struct ofono_voicecall *vc,
				ofono_voicecall_cb_t cb, void *data)
{
	unsigned int affected = (1 << CALL_STATUS_ACTIVE);

	/* Hangup current active call */
	gemalto_call_common("AT+CHLD=1", vc, generic_cb, affected, cb, data);
}

static void gemalto_hold_all_active(struct ofono_voicecall *vc,
					ofono_voicecall_cb_t cb, void *data)
{
	unsigned int affected = (1 << CALL_STATUS_ACTIVE);
	gemalto_call_common("AT+CHLD=2", vc, generic_cb, affected, cb, data);
}

static void gemalto_release_all_held(struct ofono_voicecall *vc,
					ofono_voicecall_cb_t cb, void *data)
{
	unsigned int affected = (1 << CALL_STATUS_INCOMING) |
				(1 << CALL_STATUS_WAITING);

	gemalto_call_common("AT+CHLD=0", vc, generic_cb, affected, cb, data);
}

static void gemalto_set_udub(struct ofono_voicecall *vc,
				ofono_voicecall_cb_t cb, void *data)
{
	unsigned int affected = (1 << CALL_STATUS_INCOMING) |
				(1 << CALL_STATUS_WAITING);

	gemalto_call_common("AT+CHLD=0", vc, generic_cb, affected, cb, data);
}

static void gemalto_release_all_active(struct ofono_voicecall *vc,
					ofono_voicecall_cb_t cb, void *data)
{
	unsigned int affected = (1 << CALL_STATUS_ACTIVE);

	gemalto_call_common("AT+CHLD=1", vc, generic_cb, affected, cb, data);
}

static void release_id_cb(gboolean ok, GAtResult *result, gpointer user_data)
{
	struct release_id_req *req = user_data;
	struct voicecall_data *vd = ofono_voicecall_get_data(req->vc);
	struct ofono_error error;

	decode_at_error(&error, g_at_result_final_response(result));

	if (ok)
		vd->local_release = 1 << req->id;

	req->cb(&error, req->data);
}

static void gemalto_release_specific(struct ofono_voicecall *vc, int id,
					ofono_voicecall_cb_t cb, void *data)
{
	struct voicecall_data *vd = ofono_voicecall_get_data(vc);
	struct release_id_req *req = g_new0(struct release_id_req, 1);
	char buf[32];

	req->vc = vc;
	req->cb = cb;
	req->data = data;
	req->id = id;

	snprintf(buf, sizeof(buf), "AT+CHLD=1%d", id);

	if (g_at_chat_send(vd->chat, buf, none_prefix,
				release_id_cb, req, g_free) > 0)
		return;

	g_free(req);
	CALLBACK_WITH_FAILURE(cb, data);
}

static void gemalto_private_chat(struct ofono_voicecall *vc, int id,
					ofono_voicecall_cb_t cb, void *data)
{
	char buf[32];

	snprintf(buf, sizeof(buf), "AT+CHLD=2%d", id);
	gemalto_call_common(buf, vc, generic_cb, 0, cb, data);
}

static void gemalto_create_multiparty(struct ofono_voicecall *vc,
					ofono_voicecall_cb_t cb, void *data)
{
	gemalto_call_common("AT+CHLD=3", vc, generic_cb, 0, cb, data);
}

static void gemalto_transfer(struct ofono_voicecall *vc,
				ofono_voicecall_cb_t cb, void *data)
{
	/* Held & Active */
	unsigned int affected = (1 << CALL_STATUS_ACTIVE) |
				(1 << CALL_STATUS_HELD);

	/* Transfer can puts held & active calls together and disconnects
	 * from both.  However, some networks support transferring of
	 * dialing/ringing calls as well.
	 */
	affected |= (1 << CALL_STATUS_DIALING) |
				(1 << CALL_STATUS_ALERTING);

	gemalto_call_common("AT+CHLD=4", vc, generic_cb, affected, cb, data);
}

static void gemalto_send_dtmf(struct ofono_voicecall *vc, const char *dtmf,
				ofono_voicecall_cb_t cb, void *data)
{
	struct ofono_modem *modem = ofono_voicecall_get_modem(vc);
	int use_quotes = ofono_modem_get_integer(modem, "GemaltoVtsQuotes");
	int len = strlen(dtmf);
	int s;
	int i;
	char *buf;

	/* strlen("+VTS=\"T\";") = 9 + initial AT + null */
	buf = (char *)alloca(len * 9 + 3);

	if (use_quotes)
		s = sprintf(buf, "AT+VTS=\"%c\"", dtmf[0]);
	else
		s = sprintf(buf, "AT+VTS=%c", dtmf[0]);

	for (i = 1; i < len; i++) {
		if (use_quotes)
			s += sprintf(buf + s, ";+VTS=\"%c\"", dtmf[i]);
		else
			s += sprintf(buf + s, ";+VTS=%c", dtmf[i]);
	}

	gemalto_call_common(buf, vc, generic_cb, 0, cb, data);
}

static void gemalto_dial(struct ofono_voicecall *vc,
				const struct ofono_phone_number *ph,
				enum ofono_clir_option clir,
				ofono_voicecall_cb_t cb, void *data)
{
	struct cb_data *cbd = cb_data_new(cb, data);
	char buf[256];
	size_t len;

	cbd->user = vc;

	if (ph->type == 145)
		len = snprintf(buf, sizeof(buf), "ATD+%s", ph->number);
	else
		len = snprintf(buf, sizeof(buf), "ATD%s", ph->number);

	switch (clir) {
	case OFONO_CLIR_OPTION_INVOCATION:
		len += snprintf(buf+len, sizeof(buf)-len, "I");
		break;
	case OFONO_CLIR_OPTION_SUPPRESSION:
		len += snprintf(buf+len, sizeof(buf)-len, "i");
		break;
	default:
		break;
	}

	snprintf(buf + len, sizeof(buf) - len, ";");

	gemalto_call_common(buf, vc, generic_cb, 0, cb, data);
}

static void gemalto_parse_slcc(GAtResult *result, GSList **l,
				ofono_bool_t *ret_mpty, gboolean *last)
{
	GAtResultIter iter;
	int id, dir, status, type;
	ofono_bool_t mpty;
	struct ofono_call *call;
	const char *str = "";
	int number_type = 129;

	if (last)
		*last = TRUE;

	g_at_result_iter_init(&iter, result);

	g_at_result_iter_next(&iter, "^SLCC:");

	if (!g_at_result_iter_next_number(&iter, &id))
		return;

	if (last)
		*last = FALSE;

	if (id == 0)
		return;

	if (!g_at_result_iter_next_number(&iter, &dir))
		return;

	if (!g_at_result_iter_next_number(&iter, &status))
		return;

	if (status > 5)
		return;

	if (!g_at_result_iter_next_number(&iter, &type))
		return;

	if (!g_at_result_iter_next_number(&iter, &mpty))
		return;

	/* skip 'Reserved=0' parameter, only difference from CLCC */
	if (!g_at_result_iter_skip_next(&iter))
		return;

	if (g_at_result_iter_next_string(&iter, &str))
		g_at_result_iter_next_number(&iter, &number_type);

	call = g_new0(struct ofono_call, 1);
	ofono_call_init(call);
	call->id = id;
	call->direction = dir;
	call->status = status;
	call->type = type;
	strncpy(call->phone_number.number, str,
			OFONO_MAX_PHONE_NUMBER_LENGTH);
	call->phone_number.type = number_type;

	if (strlen(str) > 0)
		call->clip_validity = 2;
	else
		call->clip_validity = 0;

	*l = g_slist_insert_sorted(*l, call, at_util_call_compare);

	if (ret_mpty)
		*ret_mpty = mpty;
}

static void clcc_cb(gboolean ok, GAtResult *result, gpointer user_data)
{
	struct ofono_voicecall *vc = user_data;
	struct voicecall_data *vd = ofono_voicecall_get_data(vc);
	GSList *l;

	if (!ok)
		return;

	vd->calls = at_util_parse_clcc(result, NULL);

	for (l = vd->calls; l; l = l->next)
		ofono_voicecall_notify(vc, l->data);
}

/*
 * ^SLCC, except for one RFU parameter (see above in the parsing), is identical
 * to +CLCC, but as URC it is parsed line by line, and the last line is
 * indicated by an empty "^SLCC:" (equivalent to the "OK" for CLCC).
 */
static void slcc_notify(GAtResult *result, gpointer user_data)
{
	struct ofono_voicecall *vc = user_data;
	struct voicecall_data *vd = ofono_voicecall_get_data(vc);
	GSList *n, *o;
	struct ofono_call *nc, *oc;
	gboolean last;

	gemalto_parse_slcc(result, &vd->new_calls, NULL, &last);

	if (!last)
		return;

	n = vd->new_calls;
	o = vd->calls;

	while (n || o) {
		nc = n ? n->data : NULL;
		oc = o ? o->data : NULL;

		if (oc && (nc == NULL || (nc->id > oc->id))) {
			enum ofono_disconnect_reason reason;

			if (vd->local_release & (1 << oc->id))
				reason = OFONO_DISCONNECT_REASON_LOCAL_HANGUP;
			else
				reason = OFONO_DISCONNECT_REASON_REMOTE_HANGUP;

			if (!oc->type)
				ofono_voicecall_disconnected(vc, oc->id,
								reason, NULL);

			o = o->next;
		} else if (nc && (oc == NULL || (nc->id < oc->id))) {

			if (nc->type == 0) /* new call, signal it */
				ofono_voicecall_notify(vc, nc);

			n = n->next;
		} else {

			DBG("modify call part");

			/* notify in case of changes */
			if (memcmp(nc, oc, sizeof(*nc)))
				ofono_voicecall_notify(vc, nc);

			n = n->next;
			o = o->next;
		}
	}

	g_slist_free_full(vd->calls, g_free);
	vd->calls = vd->new_calls;
	vd->new_calls = NULL;
	vd->local_release = 0;
}

static void cssi_notify(GAtResult *result, gpointer user_data)
{
	struct ofono_voicecall *vc = user_data;
	GAtResultIter iter;
	int code, index;

	g_at_result_iter_init(&iter, result);

	if (!g_at_result_iter_next(&iter, "+CSSI:"))
		return;

	if (!g_at_result_iter_next_number(&iter, &code))
		return;

	if (!g_at_result_iter_next_number(&iter, &index))
		index = 0;

	ofono_voicecall_ssn_mo_notify(vc, 0, code, index);
}

static void cssu_notify(GAtResult *result, gpointer user_data)
{
	struct ofono_voicecall *vc = user_data;
	GAtResultIter iter;
	int code;
	int index;
	const char *num;
	struct ofono_phone_number ph;

	ph.number[0] = '\0';
	ph.type = 129;

	g_at_result_iter_init(&iter, result);

	if (!g_at_result_iter_next(&iter, "+CSSU:"))
		return;

	if (!g_at_result_iter_next_number(&iter, &code))
		return;

	if (!g_at_result_iter_next_number_default(&iter, -1, &index))
		goto out;

	if (!g_at_result_iter_next_string(&iter, &num))
		goto out;

	strncpy(ph.number, num, OFONO_MAX_PHONE_NUMBER_LENGTH);

	if (!g_at_result_iter_next_number(&iter, &ph.type))
		return;

out:
	ofono_voicecall_ssn_mt_notify(vc, 0, code, index, &ph);
}

static void gemalto_voicecall_initialized(gboolean ok, GAtResult *result,
					gpointer user_data)
{
	struct ofono_voicecall *vc = user_data;
	struct voicecall_data *vd = ofono_voicecall_get_data(vc);

	DBG("voicecall_init: registering to notifications");

	/* NO CARRIER, NO ANSWER, BUSY, NO DIALTONE are handled through SLCC */
	g_at_chat_register(vd->chat, "^SLCC:", slcc_notify, FALSE, vc, NULL);
	g_at_chat_register(vd->chat, "+CSSI:", cssi_notify, FALSE, vc, NULL);
	g_at_chat_register(vd->chat, "+CSSU:", cssu_notify, FALSE, vc, NULL);

	ofono_voicecall_register(vc);

	/* Populate the call list */
	g_at_chat_send(vd->chat, "AT+CLCC", clcc_prefix, clcc_cb, vc, NULL);
}

static int gemalto_voicecall_probe(struct ofono_voicecall *vc,
					unsigned int vendor, void *data)
{
	GAtChat *chat = data;
	struct voicecall_data *vd;

	vd = g_new0(struct voicecall_data, 1);
	vd->chat = g_at_chat_clone(chat);
	ofono_voicecall_set_data(vc, vd);
	g_at_chat_send(vd->chat, "AT+CSSN=1,1", NULL, NULL, NULL, NULL);
	g_at_chat_send(vd->chat, "AT^SLCC=1", NULL,
			gemalto_voicecall_initialized, vc, NULL);
	return 0;
}

static void gemalto_voicecall_remove(struct ofono_voicecall *vc)
{
	struct voicecall_data *vd = ofono_voicecall_get_data(vc);

	ofono_voicecall_set_data(vc, NULL);

	g_at_chat_unref(vd->chat);
	g_free(vd);
}

static const struct ofono_voicecall_driver driver = {
	.name			= "gemaltomodem",
	.probe			= gemalto_voicecall_probe,
	.remove			= gemalto_voicecall_remove,
	.dial			= gemalto_dial,
	.answer			= gemalto_answer,
	.hangup_all		= gemalto_hangup_all,
	.hangup_active		= gemalto_hangup,
	.hold_all_active	= gemalto_hold_all_active,
	.release_all_held	= gemalto_release_all_held,
	.set_udub		= gemalto_set_udub,
	.release_all_active	= gemalto_release_all_active,
	.release_specific	= gemalto_release_specific,
	.private_chat		= gemalto_private_chat,
	.create_multiparty	= gemalto_create_multiparty,
	.transfer		= gemalto_transfer,
	.send_tones		= gemalto_send_dtmf
};

void gemalto_voicecall_init(void)
{
	ofono_voicecall_driver_register(&driver);
}

void gemalto_voicecall_exit(void)
{
	ofono_voicecall_driver_unregister(&driver);
}
