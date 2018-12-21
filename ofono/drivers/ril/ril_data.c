/*
 *  oFono - Open Source Telephony - RIL-based devices
 *
 *  Copyright (C) 2016-2018 Jolla Ltd.
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

#include "ril_data.h"
#include "ril_radio.h"
#include "ril_network.h"
#include "ril_sim_settings.h"
#include "ril_util.h"
#include "ril_vendor.h"
#include "ril_log.h"

#include <gutil_strv.h>

#include <grilio_queue.h>
#include <grilio_channel.h>
#include <grilio_parser.h>
#include <grilio_request.h>

#define DATA_PROFILE_DEFAULT_STR "0"

#define PROTO_IP_STR "IP"
#define PROTO_IPV6_STR "IPV6"
#define PROTO_IPV4V6_STR "IPV4V6"

/* Yes, it does sometimes take minutes in roaming */
#define SETUP_DATA_CALL_TIMEOUT (300*1000) /* ms */

enum ril_data_priv_flags {
	RIL_DATA_FLAG_NONE = 0x00,
	RIL_DATA_FLAG_ALLOWED = 0x01,
	RIL_DATA_FLAG_MAX_SPEED = 0x02,
	RIL_DATA_FLAG_ON = 0x04
};

/*
 * How it works:
 *
 * This code implements "one data SIM at a time" model. It will have to be
 * updated to support multiple data SIMs active simultanously.
 *
 * There's one ril_data per slot.
 *
 * RIL_DATA_FLAG_ALLOWED is set for the last SIM for which ril_data_allow()
 * was called with non-zero role. No more than one SIM at a time has this
 * flag set.
 *
 * RIL_DATA_FLAG_MAX_SPEED is set for the last SIM for which ril_data_allow()
 * was called with RIL_DATA_ROLE_INTERNET. No more than one SIM at a time has
 * this flag set.
 *
 * RIL_DATA_FLAG_ON is set for the active SIM after RIL_REQUEST_ALLOW_DATA
 * has successfully completed. For RIL version < 10 it's set immediately.
 *
 * Each ril_data has a request queue which serializes RIL_REQUEST_ALLOW_DATA,
 * RIL_REQUEST_SETUP_DATA_CALL and RIL_REQUEST_DEACTIVATE_DATA_CALL requests
 * for this SIM.
 *
 * RIL_REQUEST_ALLOW_DATA isn't sent to the selected data SIM until all
 * requests are finished for the other SIM. It's not set at all if RIL
 * version is less than 10.
 *
 * Power on is requested with ril_radio_power_on while data is allowed or
 * any requests are pending for the SIM. Once data is disallowed and all
 * requests are finished, power is released with ril_radio_power_off.
 */

typedef GObjectClass RilDataClass;
typedef struct ril_data RilData;

enum ril_data_io_event_id {
	IO_EVENT_DATA_CALL_LIST_CHANGED,
	IO_EVENT_RESTRICTED_STATE_CHANGED,
	IO_EVENT_COUNT
};

enum ril_data_settings_event_id {
	SETTINGS_EVENT_IMSI_CHANGED,
	SETTINGS_EVENT_PREF_MODE,
	SETTINGS_EVENT_COUNT
};

struct ril_data_manager {
	gint ref_count;
	GSList *data_list;
	enum ril_data_manager_flags flags;
};

struct ril_data_priv {
	GRilIoQueue *q;
	GRilIoChannel *io;
	struct ril_radio *radio;
	struct ril_network *network;
	struct ril_data_manager *dm;
	struct ril_vendor_hook *vendor_hook;

	enum ril_data_priv_flags flags;
	enum ril_restricted_state restricted_state;

	struct ril_data_request *req_queue;
	struct ril_data_request *pending_req;

	struct ril_data_options options;
	guint slot;
	char *log_prefix;
	guint query_id;
	gulong io_event_id[IO_EVENT_COUNT];
	gulong settings_event_id[SETTINGS_EVENT_COUNT];
	GHashTable* grab;
};

enum ril_data_signal {
	SIGNAL_ALLOW_CHANGED,
	SIGNAL_CALLS_CHANGED,
	SIGNAL_COUNT
};

#define SIGNAL_ALLOW_CHANGED_NAME   "ril-data-allow-changed"
#define SIGNAL_CALLS_CHANGED_NAME   "ril-data-calls-changed"

static guint ril_data_signals[SIGNAL_COUNT] = { 0 };

#define NEW_SIGNAL(klass,name) \
	ril_data_signals[SIGNAL_##name##_CHANGED] = \
		g_signal_new(SIGNAL_##name##_CHANGED_NAME, \
			G_OBJECT_CLASS_TYPE(klass), G_SIGNAL_RUN_FIRST, \
			0, NULL, NULL, NULL, G_TYPE_NONE, 0)

G_DEFINE_TYPE(RilData, ril_data, G_TYPE_OBJECT)
#define RIL_DATA_TYPE (ril_data_get_type())
#define RIL_DATA(obj) (G_TYPE_CHECK_INSTANCE_CAST(obj, RIL_DATA_TYPE,RilData))

#define DBG_(data,fmt,args...) DBG("%s" fmt, (data)->priv->log_prefix, ##args)

enum ril_data_request_flags {
	DATA_REQUEST_FLAG_COMPLETED = 0x1,
	DATA_REQUEST_FLAG_CANCEL_WHEN_ALLOWED = 0x2,
	DATA_REQUEST_FLAG_CANCEL_WHEN_DISALLOWED = 0x4
};

struct ril_data_request {
	struct ril_data_request *next;
	struct ril_data *data;
	union ril_data_request_cb {
		ril_data_call_setup_cb_t setup;
		ril_data_call_deactivate_cb_t deact;
		void (*ptr)();
	} cb;
	void *arg;
	gboolean (*submit)(struct ril_data_request *req);
	void (*cancel)(struct ril_data_request *req);
	void (*free)(struct ril_data_request *req);
	guint pending_id;
	enum ril_data_request_flags flags;
	const char *name;
};

struct ril_data_request_setup {
	struct ril_data_request req;
	char *apn;
	char *username;
	char *password;
	enum ofono_gprs_proto proto;
	enum ofono_gprs_auth_method auth_method;
	guint retry_count;
	guint retry_delay_id;
};

struct ril_data_request_deact {
	struct ril_data_request req;
	int cid;
};

struct ril_data_request_allow_data {
	struct ril_data_request req;
	gboolean allow;
};

static void ril_data_manager_check_data(struct ril_data_manager *dm);
static void ril_data_manager_check_network_mode(struct ril_data_manager *dm);
static void ril_data_call_deact_cid(struct ril_data *data, int cid);
static void ril_data_power_update(struct ril_data *self);
static void ril_data_signal_emit(struct ril_data *self, enum ril_data_signal id)
{
	g_signal_emit(self, ril_data_signals[id], 0);
}

/*==========================================================================*
 * RIL requests
 *==========================================================================*/

GRilIoRequest *ril_request_allow_data_new(gboolean allow)
{
	return grilio_request_array_int32_new(1, allow);
}

GRilIoRequest *ril_request_deactivate_data_call_new(int cid)
{
	GRilIoRequest *req = grilio_request_new();

	grilio_request_append_int32(req, 2 /* Parameter count */);
	grilio_request_append_format(req, "%d", cid);
	grilio_request_append_format(req, "%d",
					RIL_DEACTIVATE_DATA_CALL_NO_REASON);
	return req;
}

/*==========================================================================*
 * ril_data_call
 *==========================================================================*/

static struct ril_data_call *ril_data_call_new()
{
	return g_new0(struct ril_data_call, 1);
}

struct ril_data_call *ril_data_call_dup(const struct ril_data_call *call)
{
	if (call) {
		struct ril_data_call *dc = ril_data_call_new();
		dc->cid = call->cid;
		dc->status = call->status;
		dc->active = call->active;
		dc->prot = call->prot;
		dc->retry_time = call->retry_time;
		dc->mtu = call->mtu;
		dc->ifname = g_strdup(call->ifname);
		dc->dnses = g_strdupv(call->dnses);
		dc->gateways = g_strdupv(call->gateways);
		dc->addresses = g_strdupv(call->addresses);
		return dc;
	} else {
		return NULL;
	}
}

static void ril_data_call_destroy(struct ril_data_call *call)
{
	g_free(call->ifname);
	g_strfreev(call->dnses);
	g_strfreev(call->addresses);
	g_strfreev(call->gateways);
}

void ril_data_call_free(struct ril_data_call *call)
{
	if (call) {
		ril_data_call_destroy(call);
		g_free(call);
	}
}

static void ril_data_call_free1(gpointer data)
{
	ril_data_call_free(data);
}

static void ril_data_call_list_free(struct ril_data_call_list *list)
{
	if (list) {
		g_slist_free_full(list->calls, ril_data_call_free1);
		g_free(list);
	}
}

static gint ril_data_call_compare(gconstpointer a, gconstpointer b)
{
	const struct ril_data_call *ca = a;
	const struct ril_data_call *cb = b;

	if (ca->cid < cb->cid) {
		return -1;
	} else if (ca->cid > cb->cid) {
		return 1;
	} else {
		return 0;
	}
}

const char *ril_data_ofono_protocol_to_ril(enum ofono_gprs_proto proto)
{
	switch (proto) {
	case OFONO_GPRS_PROTO_IPV6:
		return PROTO_IPV6_STR;
	case OFONO_GPRS_PROTO_IPV4V6:
		return PROTO_IPV4V6_STR;
	case OFONO_GPRS_PROTO_IP:
		return PROTO_IP_STR;
	default:
		return NULL;
	}
}

int ril_data_protocol_to_ofono(const gchar *str)
{
	if (str) {
		if (!strcmp(str, PROTO_IPV6_STR)) {
			return OFONO_GPRS_PROTO_IPV6;
		} else if (!strcmp(str, PROTO_IPV4V6_STR)) {
			return OFONO_GPRS_PROTO_IPV4V6;
		} else if (!strcmp(str, PROTO_IP_STR)) {
			return OFONO_GPRS_PROTO_IP;
		}
	}
	return -1;
}

static gboolean ril_data_call_parse_default(struct ril_data_call *call,
					int version, GRilIoParser *rilp)
{
	int prot;
	char *prot_str;
	guint32 status = PDP_FAIL_ERROR_UNSPECIFIED;
	guint32 active = RIL_DATA_CALL_INACTIVE;

	/* RIL_Data_Call_Response_v6 (see ril.h) */
	grilio_parser_get_uint32(rilp, &status);
	grilio_parser_get_int32(rilp, &call->retry_time);
	grilio_parser_get_int32(rilp, &call->cid);
	grilio_parser_get_uint32(rilp, &active);
	prot_str = grilio_parser_get_utf8(rilp);
	call->ifname = grilio_parser_get_utf8(rilp);
	call->addresses = grilio_parser_split_utf8(rilp, " ");
	call->dnses = grilio_parser_split_utf8(rilp, " ");
	call->gateways = grilio_parser_split_utf8(rilp, " ");

	prot = ril_data_protocol_to_ofono(prot_str);
	if (prot < 0 && status == PDP_FAIL_NONE) {
		ofono_error("Invalid protocol: %s", prot_str);
	}

	call->prot = prot;
	call->status = status;
	call->active = active;

	/* RIL_Data_Call_Response_v9 */
	if (version >= 9) {
		/* PCSCF */
		grilio_parser_skip_string(rilp);

		/* RIL_Data_Call_Response_v11 */
		if (version >= 11) {
			/* MTU */
			grilio_parser_get_int32(rilp, &call->mtu);
		}
	}

	g_free(prot_str);
	return TRUE;
}

static struct ril_data_call *ril_data_call_parse(struct ril_vendor_hook *hook,
					int version, GRilIoParser *parser)
{
	GRilIoParser copy = *parser;
	struct ril_data_call *call = ril_data_call_new();
	gboolean parsed = ril_vendor_hook_data_call_parse(hook, call,
							version, parser);

	if (!parsed) {
		/* Try the default parser */
		ril_data_call_destroy(call);
		memset(call, 0, sizeof(*call));
		*parser = copy;
		parsed = ril_data_call_parse_default(call, version, parser);
	}

	if (parsed) {
		DBG("[status=%d,retry=%d,cid=%d,active=%d,type=%s,ifname=%s,"
			"mtu=%d,address=%s,dns=%s %s,gateways=%s]",
			call->status, call->retry_time,
			call->cid, call->active,
			ril_data_ofono_protocol_to_ril(call->prot),
			call->ifname, call->mtu,
			call->addresses ? call->addresses[0] : NULL,
			call->dnses ? call->dnses[0] : NULL,
			(call->dnses && call->dnses[0] &&
			call->dnses[1]) ? call->dnses[1] : "",
			call->gateways ? call->gateways[0] : NULL);
		return call;
	} else {
		ril_data_call_free(call);
		return NULL;
	}
}

static struct ril_data_call_list *ril_data_call_list_parse(const void *data,
				guint len, struct ril_vendor_hook *hook,
				enum ril_data_call_format format)
{
	guint32 version, n, i;
	GRilIoParser rilp;

	grilio_parser_init(&rilp, data, len);
	if (grilio_parser_get_uint32(&rilp, &version) &&
					grilio_parser_get_uint32(&rilp, &n)) {
		struct ril_data_call_list *list =
			g_new0(struct ril_data_call_list, 1);

		if (format == RIL_DATA_CALL_FORMAT_AUTO || format == version) {
			DBG("version=%u,num=%u", version, n);
			list->version = version;
		} else {
			DBG("version=%u(%d),num=%u", version, format, n);
			list->version = format;
		}

		for (i = 0; i < n && !grilio_parser_at_end(&rilp); i++) {
			struct ril_data_call *call = ril_data_call_parse(hook,
							list->version, &rilp);

			if (call) {
				list->num++;
				list->calls = g_slist_insert_sorted(list->calls,
						call, ril_data_call_compare);
			}
		}

		if (list->calls) {
			return list;
		}

		ril_data_call_list_free(list);
	}

	DBG("no data calls");
	return NULL;
}

static gboolean ril_data_call_equal(const struct ril_data_call *c1,
					const struct ril_data_call *c2)
{
	if (!c1 && !c2) {
		return TRUE;
	} else if (c1 && c2) {
		return c1->cid == c2->cid &&
			c1->status == c2->status &&
			c1->active == c2->active &&
			c1->prot == c2->prot &&
			c1->retry_time == c2->retry_time &&
			c1->mtu == c2->mtu &&
			!g_strcmp0(c1->ifname, c2->ifname) &&
			gutil_strv_equal(c1->dnses, c2->dnses) &&
			gutil_strv_equal(c1->gateways, c2->gateways) &&
			gutil_strv_equal(c1->addresses, c2->addresses);
	} else {
		return FALSE;
	}
}

static gboolean ril_data_call_list_equal(const struct ril_data_call_list *l1,
					const struct ril_data_call_list *l2)
{
	if (!l1 && !l2) {
		return TRUE;
	} else if (l1 && l2) {
		if (l1->version == l1->version && l1->num == l2->num) {
			GSList *p1 = l1->calls;
			GSList *p2 = l2->calls;

			while (p1 && p2) {
				if (!ril_data_call_equal(p1->data, p2->data)) {
					return FALSE;
				}
				p1 = p1->next;
				p2 = p2->next;
			}

			GASSERT(!p1 && !p2);
			return TRUE;
		}
	}
	return FALSE;
}

static gboolean ril_data_call_list_contains(struct ril_data_call_list *list,
					const struct ril_data_call *call)
{
	if (list) {
		GSList *l;

		for (l = list->calls; l; l = l->next) {
			if (ril_data_call_equal(l->data, call)) {
				return TRUE;
			}
		}
	}

	return FALSE;
}


static int ril_data_call_list_move_calls(struct ril_data_call_list *dest,
					struct ril_data_call_list *src)
{
	int count = 0;

	if (dest) {
		GSList *l = src->calls;

		while (l) {
			GSList *next = l->next;
			struct ril_data_call *call = l->data;

			if (!ril_data_call_list_contains(dest, call)) {
				count++;
				dest->num++;
				src->calls = g_slist_delete_link(src->calls, l);
				dest->calls = g_slist_insert_sorted(dest->calls,
					call, ril_data_call_compare);
			}

			l = next;
		}
	}

	return count;
}

struct ril_data_call *ril_data_call_find(struct ril_data_call_list *list,
								int cid)
{
	if (list) {
		GSList *l;

		for (l = list->calls; l; l = l->next) {
			struct ril_data_call *call = l->data;

			if (call->cid == cid) {
				return call;
			}
		}
	}

	return NULL;
}

static void ril_data_set_calls(struct ril_data *self,
					struct ril_data_call_list *list)
{
	struct ril_data_priv *priv = self->priv;
	GHashTableIter it;
	gpointer key;

	if (!ril_data_call_list_equal(self->data_calls, list)) {
		DBG("data calls changed");
		ril_data_call_list_free(self->data_calls);
		self->data_calls = list;
		ril_data_signal_emit(self, SIGNAL_CALLS_CHANGED);
	} else {
		ril_data_call_list_free(list);
	}

	/* Clean up the grab table */
	g_hash_table_iter_init(&it, priv->grab);
	while (g_hash_table_iter_next(&it, &key, NULL)) {
		const int cid = GPOINTER_TO_INT(key);

		if (!ril_data_call_find(self->data_calls, cid)) {
			g_hash_table_iter_remove(&it);
		}
	}

	if (self->data_calls) {
		GSList *l;

		/* Disconnect stray calls (one at a time) */
		for (l = self->data_calls->calls; l; l = l->next) {
			struct ril_data_call *dc = l->data;

			key = GINT_TO_POINTER(dc->cid);
			if (!g_hash_table_contains(priv->grab, key)) {
				DBG_(self, "stray call %u", dc->cid);
				ril_data_call_deact_cid(self, dc->cid);
				break;
			}
		}
	}
}

static void ril_data_check_allowed(struct ril_data *self, gboolean was_allowed)
{
	if (ril_data_allowed(self) != was_allowed) {
		ril_data_signal_emit(self, SIGNAL_ALLOW_CHANGED);
	}
}

static void ril_data_restricted_state_changed_cb(GRilIoChannel *io, guint event,
				const void *data, guint len, void *user_data)
{
	struct ril_data *self = RIL_DATA(user_data);
	GRilIoParser rilp;
	guint32 count, state;

	GASSERT(event == RIL_UNSOL_RESTRICTED_STATE_CHANGED);
	grilio_parser_init(&rilp, data, len);
	if (grilio_parser_get_uint32(&rilp, &count) && count == 1 &&
				grilio_parser_get_uint32(&rilp, &state) &&
				grilio_parser_at_end(&rilp)) {
		struct ril_data_priv *priv = self->priv;

		if (priv->restricted_state != state) {
			const gboolean was_allowed = ril_data_allowed(self);

			DBG_(self, "restricted state 0x%02x", state);
			priv->restricted_state = state;
			ril_data_check_allowed(self, was_allowed);
		}
	}
}

static void ril_data_call_list_changed_cb(GRilIoChannel *io, guint event,
				const void *data, guint len, void *user_data)
{
	struct ril_data *self = RIL_DATA(user_data);
	struct ril_data_priv *priv = self->priv;

	GASSERT(event == RIL_UNSOL_DATA_CALL_LIST_CHANGED);
	if (priv->query_id) {
		/* We have received change event before query has completed */
		DBG_(self, "cancelling query");
		grilio_queue_cancel_request(priv->q, priv->query_id, FALSE);
		priv->query_id = 0;
	}

	ril_data_set_calls(self, ril_data_call_list_parse(data, len,
			priv->vendor_hook, priv->options.data_call_format));
}

static void ril_data_query_data_calls_cb(GRilIoChannel *io, int ril_status,
				const void *data, guint len, void *user_data)
{
	struct ril_data *self = RIL_DATA(user_data);
	struct ril_data_priv *priv = self->priv;

	/*
	 * Only RIL_E_SUCCESS and RIL_E_RADIO_NOT_AVAILABLE are expected here,
	 * all other errors are filtered out by ril_voicecall_clcc_retry()
	 */
	GASSERT(priv->query_id);
	priv->query_id = 0;
	if (ril_status == RIL_E_SUCCESS) {
		ril_data_set_calls(self, ril_data_call_list_parse(data, len,
			priv->vendor_hook, priv->options.data_call_format));
	} else {
		/* RADIO_NOT_AVAILABLE == no calls */
		ril_data_set_calls(self, NULL);
	}
}

/*==========================================================================*
 * ril_data_request
 *==========================================================================*/

static void ril_data_request_free(struct ril_data_request *req)
{
	if (req->free) {
		req->free(req);
	} else {
		g_free(req);
	}
}

void ril_data_request_detach(struct ril_data_request *req)
{
	if (req) {
		req->cb.ptr = NULL;
		req->arg = NULL;
	}
}

static void ril_data_request_cancel_io(struct ril_data_request *req)
{
	if (req->pending_id) {
		grilio_queue_cancel_request(req->data->priv->q,
						req->pending_id, FALSE);
		req->pending_id = 0;
	}
}

static void ril_data_request_submit_next(struct ril_data *data)
{
	struct ril_data_priv *priv = data->priv;

	if (!priv->pending_req) {
		ril_data_power_update(data);

		while (priv->req_queue) {
			struct ril_data_request *req = priv->req_queue;

			GASSERT(req->data == data);
			priv->req_queue = req->next;
			req->next = NULL;

			priv->pending_req = req;
			if (req->submit(req)) {
				DBG_(data, "submitted %s request %p",
							req->name, req);
				break;
			} else {
				DBG_(data, "%s request %p is done (or failed)",
							req->name, req);
				priv->pending_req = NULL;
				ril_data_request_free(req);
			}
		}

		if (!priv->pending_req) {
			ril_data_manager_check_data(priv->dm);
		}
	}

	ril_data_power_update(data);
}

static gboolean ril_data_request_do_cancel(struct ril_data_request *req)
{
	if (req && !(req->flags & DATA_REQUEST_FLAG_COMPLETED)) {
		struct ril_data_priv *priv = req->data->priv;

		DBG_(req->data, "canceling %s request %p", req->name, req);
		if (req->cancel) {
			req->cancel(req);
		}
		if (priv->pending_req == req) {
			/* Request has been submitted already */
			priv->pending_req = NULL;
		} else if (priv->req_queue == req) {
			/* It's the first one in the queue */
			priv->req_queue = req->next;
		} else {
			/* It's somewhere in the queue */
			struct ril_data_request* prev = priv->req_queue;

			while (prev->next && prev->next != req) {
				prev = prev->next;
			}

			/* Assert that it's there */
			GASSERT(prev);
			if (prev) {
				prev->next = req->next;
			}
		}

		ril_data_request_free(req);
		return TRUE;
	} else {
		return FALSE;
	}
}

void ril_data_request_cancel(struct ril_data_request *req)
{
	if (req) {
		struct ril_data *data = req->data;
		if (ril_data_request_do_cancel(req)) {
			ril_data_request_submit_next(data);
		}
	}
}

static void ril_data_request_completed(struct ril_data_request *req)
{
	GASSERT(!(req->flags & DATA_REQUEST_FLAG_COMPLETED));
	req->flags |= DATA_REQUEST_FLAG_COMPLETED;
}

static void ril_data_request_finish(struct ril_data_request *req)
{
	struct ril_data *data = req->data;
	struct ril_data_priv *priv = data->priv;

	GASSERT(req == priv->pending_req);
	GASSERT(!req->next);
	priv->pending_req = NULL;

	ril_data_request_free(req);
	ril_data_request_submit_next(data);
}

static void ril_data_request_queue(struct ril_data_request *req)
{
	struct ril_data *data = req->data;
	struct ril_data_priv *priv = data->priv;

	req->next = NULL;

	if (!priv->req_queue) {
		priv->req_queue = req;
	} else {
		struct ril_data_request* last = priv->req_queue;
		while (last->next) {
			last = last->next;
		}
		last->next = req;
	}

	DBG_(data, "queued %s request %p", req->name, req);
	ril_data_request_submit_next(data);
}

/*==========================================================================*
 * ril_data_request_setup
 *==========================================================================*/

static void ril_data_call_setup_cancel(struct ril_data_request *req)
{
	struct ril_data_request_setup *setup =
		G_CAST(req, struct ril_data_request_setup, req);

	ril_data_request_cancel_io(req);
	if (setup->retry_delay_id) {
		g_source_remove(setup->retry_delay_id);
		setup->retry_delay_id = 0;
	}
	if (req->cb.setup) {
		ril_data_call_setup_cb_t cb = req->cb.setup;
		req->cb.setup = NULL;
		cb(req->data, GRILIO_STATUS_CANCELLED, NULL, req->arg);
	}
}

static gboolean ril_data_call_setup_retry(void *user_data)
{
	struct ril_data_request_setup *setup = user_data;
	struct ril_data_request *req = &setup->req;

	GASSERT(setup->retry_delay_id);
	setup->retry_delay_id = 0;
	setup->retry_count++;
	DBG("silent retry %u out of %u", setup->retry_count,
			req->data->priv->options.data_call_retry_limit);
	req->submit(req);
	return G_SOURCE_REMOVE;
}

static void ril_data_call_setup_cb(GRilIoChannel *io, int ril_status,
				const void *data, guint len, void *user_data)
{
	struct ril_data_request_setup *setup = user_data;
	struct ril_data_request *req = &setup->req;
	struct ril_data *self = req->data;
	struct ril_data_priv *priv = self->priv;
	struct ril_data_call_list *list = NULL;
	struct ril_data_call *call = NULL;

	if (ril_status == RIL_E_SUCCESS) {
		list = ril_data_call_list_parse(data, len,
			priv->vendor_hook, priv->options.data_call_format);
	}

	if (list) {
		if (list->num == 1) {
			call = list->calls->data;
		} else {
			ofono_error("Number of data calls: %u", list->num);
			ril_status = RIL_E_GENERIC_FAILURE;
		}
	}

	if (call && call->status == PDP_FAIL_ERROR_UNSPECIFIED &&
		setup->retry_count < priv->options.data_call_retry_limit) {
		/*
		 * According to the comment from ril.h we should silently
		 * retry. First time we retry immediately and if that doedsn't
		 * work, then after certain delay.
		 */
		req->pending_id = 0;
		GASSERT(!setup->retry_delay_id);
		if (!setup->retry_count) {
			setup->retry_count++;
			DBG("silent retry %u out of %u", setup->retry_count,
					priv->options.data_call_retry_limit);
			req->submit(req);
		} else {
			guint ms = priv->options.data_call_retry_delay_ms;
			DBG("silent retry scheduled in %u ms", ms);
			setup->retry_delay_id = g_timeout_add(ms,
					ril_data_call_setup_retry, setup);
		}
		ril_data_call_list_free(list);
		return;
	}

	ril_data_request_completed(req);

	if (call && call->status == PDP_FAIL_NONE) {
		if (ril_data_call_list_move_calls(self->data_calls, list) > 0) {
			DBG("data call(s) added");
			ril_data_signal_emit(self, SIGNAL_CALLS_CHANGED);
		} else if (!self->data_calls && list->num > 0) {
			DBG("data calls changed");
			self->data_calls = list;
			list = NULL;
		}
	}

	if (req->cb.setup) {
		req->cb.setup(req->data, ril_status, call, req->arg);
	}

	ril_data_request_finish(req);
	ril_data_call_list_free(list);
}

static gboolean ril_data_call_setup_submit(struct ril_data_request *req)
{
	struct ril_data_request_setup *setup =
		G_CAST(req, struct ril_data_request_setup, req);
	struct ril_data_priv *priv = req->data->priv;
	const char *proto_str = ril_data_ofono_protocol_to_ril(setup->proto);
	GRilIoRequest *ioreq;
	int tech, auth = RIL_AUTH_NONE;

	GASSERT(proto_str);

	/* ril.h has this to say about the radio tech parameter:
	 *
	 * ((const char **)data)[0] Radio technology to use: 0-CDMA,
	 *                          1-GSM/UMTS, 2... for values above 2
	 *                          this is RIL_RadioTechnology + 2.
	 *
	 * Makes little sense but it is what it is.
	 */
	tech = priv->network->data.ril_tech;
	if (tech > 2) {
		tech += 2;
	} else {
		/*
		 * This value used to be hardcoded, let's keep using it
		 * as the default.
		 */
		tech = RADIO_TECH_HSPA;
	}

	if (setup->username && setup->username[0]) {
		switch (setup->auth_method) {
		case OFONO_GPRS_AUTH_METHOD_ANY:
			auth = RIL_AUTH_BOTH;
			break;
		case OFONO_GPRS_AUTH_METHOD_NONE:
			auth = RIL_AUTH_NONE;
			break;
		case OFONO_GPRS_AUTH_METHOD_CHAP:
			auth = RIL_AUTH_CHAP;
			break;
		case OFONO_GPRS_AUTH_METHOD_PAP:
			auth = RIL_AUTH_PAP;
			break;
		}
	}

	/* Give vendor code a chance to build a vendor specific packet */
	ioreq = ril_vendor_hook_data_call_req(priv->vendor_hook, tech,
			DATA_PROFILE_DEFAULT_STR, setup->apn, setup->username,
			setup->password, auth, proto_str);

	if (!ioreq) {
		/* The default one */
		ioreq = grilio_request_new();
		grilio_request_append_int32(ioreq, 7 /* Parameter count */);
		grilio_request_append_format(ioreq, "%d", tech);
		grilio_request_append_utf8(ioreq, DATA_PROFILE_DEFAULT_STR);
		grilio_request_append_utf8(ioreq, setup->apn);
		grilio_request_append_utf8(ioreq, setup->username);
		grilio_request_append_utf8(ioreq, setup->password);
		grilio_request_append_format(ioreq, "%d", auth);
		grilio_request_append_utf8(ioreq, proto_str);
	}

	GASSERT(!req->pending_id);
	grilio_request_set_timeout(ioreq, SETUP_DATA_CALL_TIMEOUT);
	req->pending_id = grilio_queue_send_request_full(priv->q, ioreq,
			RIL_REQUEST_SETUP_DATA_CALL, ril_data_call_setup_cb,
			NULL, setup);
	grilio_request_unref(ioreq);
	return TRUE;
}

static void ril_data_call_setup_free(struct ril_data_request *req)
{
	struct ril_data_request_setup *setup =
		G_CAST(req, struct ril_data_request_setup, req);

	g_free(setup->apn);
	g_free(setup->username);
	g_free(setup->password);
	g_free(setup);
}

static struct ril_data_request *ril_data_call_setup_new(struct ril_data *data,
				const struct ofono_gprs_primary_context *ctx,
				ril_data_call_setup_cb_t cb, void *arg)
{
	struct ril_data_request_setup *setup =
		g_new0(struct ril_data_request_setup, 1);
	struct ril_data_request *req = &setup->req;

	setup->apn = g_strdup(ctx->apn);
	setup->username = g_strdup(ctx->username);
	setup->password = g_strdup(ctx->password);
	setup->proto = ctx->proto;
	setup->auth_method = ctx->auth_method;

	req->name = "CALL_SETUP";
	req->cb.setup = cb;
	req->arg = arg;
	req->data = data;
	req->submit = ril_data_call_setup_submit;
	req->cancel = ril_data_call_setup_cancel;
	req->free = ril_data_call_setup_free;
	req->flags = DATA_REQUEST_FLAG_CANCEL_WHEN_DISALLOWED;
	return req;
}

/*==========================================================================*
 * ril_data_request_deact
 *==========================================================================*/

static void ril_data_call_deact_cancel(struct ril_data_request *req)
{
	ril_data_request_cancel_io(req);
	if (req->cb.deact) {
		ril_data_call_deactivate_cb_t cb = req->cb.deact;
		req->cb.deact = NULL;
		cb(req->data, GRILIO_STATUS_CANCELLED, req->arg);
	}
}

static void ril_data_call_deact_cb(GRilIoChannel *io, int ril_status,
			const void *ril_data, guint len, void *user_data)
{
	struct ril_data_request_deact *deact = user_data;
	struct ril_data_request *req = &deact->req;
	struct ril_data *data = req->data;

	ril_data_request_completed(req);

	/*
	 * If RIL_REQUEST_DEACTIVATE_DATA_CALL succeeds, some RILs don't
	 * send RIL_UNSOL_DATA_CALL_LIST_CHANGED even though the list of
	 * calls has changed. Update the list of calls to account for that.
	 */
	if (ril_status == RIL_E_SUCCESS) {
		struct ril_data_call_list *list = data->data_calls;
		struct ril_data_call *call = ril_data_call_find(list,
								deact->cid);
		if (call) {
			DBG_(data, "removing call %d", deact->cid);
			list->calls = g_slist_remove(list->calls, call);
			if (list->calls) {
				list->num--;
				GASSERT(list->num > 0);
			} else {
				GASSERT(list->num == 1);
				ril_data_call_list_free(list);
				data->data_calls = NULL;
			}
			ril_data_call_free(call);
			ril_data_signal_emit(data, SIGNAL_CALLS_CHANGED);
		}
	} else {
		/* Something seems to be slightly broken, request the
		 * current state */
		ril_data_poll_call_state(data);
	}

	if (req->cb.deact) {
		req->cb.deact(req->data, ril_status, req->arg);
	}

	ril_data_request_finish(req);
}

static gboolean ril_data_call_deact_submit(struct ril_data_request *req)
{
	struct ril_data_request_deact *deact =
		G_CAST(req, struct ril_data_request_deact, req);
	struct ril_data_priv *priv = req->data->priv;
	GRilIoRequest *ioreq =
		ril_request_deactivate_data_call_new(deact->cid);

	req->pending_id = grilio_queue_send_request_full(priv->q, ioreq,
				RIL_REQUEST_DEACTIVATE_DATA_CALL,
				ril_data_call_deact_cb, NULL, deact);
	grilio_request_unref(ioreq);
	return TRUE;
}

static struct ril_data_request *ril_data_call_deact_new(struct ril_data *data,
		int cid, ril_data_call_deactivate_cb_t cb, void *arg)
{
	struct ril_data_request_deact *deact =
		g_new0(struct ril_data_request_deact, 1);
	struct ril_data_request *req = &deact->req;

	deact->cid = cid;

	req->cb.deact = cb;
	req->arg = arg;
	req->data = data;
	req->submit = ril_data_call_deact_submit;
	req->cancel = ril_data_call_deact_cancel;
	req->name = "DEACTIVATE";

	return req;
}

static void ril_data_call_deact_cid(struct ril_data *data, int cid)
{
	ril_data_request_queue(ril_data_call_deact_new(data, cid, NULL, NULL));
}

/*==========================================================================*
 * ril_data_allow_request
 *==========================================================================*/

static void ril_data_allow_cb(GRilIoChannel *io, int ril_status,
			const void *req_data, guint len, void *user_data)
{
	struct ril_data_request *req = user_data;
	struct ril_data *data = req->data;
	struct ril_data_priv *priv = data->priv;

	ril_data_request_completed(req);

	if (ril_status == RIL_E_SUCCESS) {
		const gboolean was_allowed = ril_data_allowed(data);
		struct ril_data_request_allow_data *ad =
			G_CAST(req, struct ril_data_request_allow_data, req);

		if (ad->allow) {
			priv->flags |= RIL_DATA_FLAG_ON;
			DBG_(data, "data on");
		} else {
			priv->flags &= ~RIL_DATA_FLAG_ON;
			DBG_(data, "data off");
		}

		ril_data_check_allowed(data, was_allowed);
	}

	ril_data_request_finish(req);
}

static gboolean ril_data_allow_submit(struct ril_data_request *req)
{
	struct ril_data_request_allow_data *ad =
		G_CAST(req, struct ril_data_request_allow_data, req);
	GRilIoRequest *ioreq = ril_request_allow_data_new(ad->allow);
	struct ril_data_priv *priv = req->data->priv;

	grilio_request_set_retry(ioreq, RIL_RETRY_SECS*1000, -1);
	grilio_request_set_blocking(ioreq, TRUE);
	req->pending_id = grilio_queue_send_request_full(priv->q, ioreq,
		RIL_REQUEST_ALLOW_DATA, ril_data_allow_cb, NULL, req);
	grilio_request_unref(ioreq);
	return TRUE;
}

static struct ril_data_request *ril_data_allow_new(struct ril_data *data,
							gboolean allow)
{
	struct ril_data_request_allow_data *ad =
		g_new0(struct ril_data_request_allow_data, 1);
	struct ril_data_request *req = &ad->req;

	req->name = "ALLOW_DATA";
	req->data = data;
	req->submit = ril_data_allow_submit;
	req->cancel = ril_data_request_cancel_io;
	req->flags = DATA_REQUEST_FLAG_CANCEL_WHEN_DISALLOWED;
	ad->allow = allow;
	return req;
}

/*==========================================================================*
 * ril_data
 *==========================================================================*/

gulong ril_data_add_allow_changed_handler(struct ril_data *self,
						ril_data_cb_t cb, void *arg)
{
	return (G_LIKELY(self) && G_LIKELY(cb)) ? g_signal_connect(self,
		SIGNAL_ALLOW_CHANGED_NAME, G_CALLBACK(cb), arg) : 0;
}

gulong ril_data_add_calls_changed_handler(struct ril_data *self,
					ril_data_cb_t cb, void *arg)
{
	return (G_LIKELY(self) && G_LIKELY(cb)) ? g_signal_connect(self,
		SIGNAL_CALLS_CHANGED_NAME, G_CALLBACK(cb), arg) : 0;
}

void ril_data_remove_handler(struct ril_data *self, gulong id)
{
	if (G_LIKELY(self) && G_LIKELY(id)) {
		g_signal_handler_disconnect(self, id);
	}
}

static void ril_data_settings_changed(struct ril_sim_settings *settings,
							void *user_data)
{
	ril_data_manager_check_network_mode(RIL_DATA(user_data)->priv->dm);
}

static gint ril_data_compare_cb(gconstpointer a, gconstpointer b)
{
	const struct ril_data *d1 = a;
	const struct ril_data *d2 = b;
	const struct ril_data_priv *p1 = d1->priv;
	const struct ril_data_priv *p2 = d2->priv;

	return p1->slot < p2->slot ? (-1) : p1->slot > p2->slot ? 1 : 0;
}

struct ril_data *ril_data_new(struct ril_data_manager *dm, const char *name,
		struct ril_radio *radio, struct ril_network *network,
		GRilIoChannel *io, const struct ril_data_options *options,
		const struct ril_slot_config *config,
		struct ril_vendor_hook *vendor_hook)
{
	GASSERT(dm);
	if (G_LIKELY(dm)) {
		struct ril_data *self = g_object_new(RIL_DATA_TYPE, NULL);
		struct ril_data_priv *priv = self->priv;
		struct ril_sim_settings *settings = network->settings;

		priv->options = *options;
		switch (priv->options.allow_data) {
		case RIL_ALLOW_DATA_ENABLED:
		case RIL_ALLOW_DATA_DISABLED:
			break;
		default:
			/*
			 * When RIL_REQUEST_ALLOW_DATA first appeared in ril.h
			 * RIL_VERSION was 10
			 */
			priv->options.allow_data = (io->ril_version > 10) ?
						RIL_ALLOW_DATA_ENABLED :
						RIL_ALLOW_DATA_DISABLED;
			break;
		}

		priv->log_prefix = (name && name[0]) ?
			g_strconcat(name, " ", NULL) : g_strdup("");

		priv->slot = config->slot;
		priv->q = grilio_queue_new(io);
		priv->io = grilio_channel_ref(io);
		priv->dm = ril_data_manager_ref(dm);
		priv->radio = ril_radio_ref(radio);
		priv->network = ril_network_ref(network);
		priv->vendor_hook = ril_vendor_hook_ref(vendor_hook);

		priv->io_event_id[IO_EVENT_DATA_CALL_LIST_CHANGED] =
			grilio_channel_add_unsol_event_handler(io,
				ril_data_call_list_changed_cb,
				RIL_UNSOL_DATA_CALL_LIST_CHANGED, self);
		priv->io_event_id[IO_EVENT_RESTRICTED_STATE_CHANGED] =
			grilio_channel_add_unsol_event_handler(io,
				ril_data_restricted_state_changed_cb,
				RIL_UNSOL_RESTRICTED_STATE_CHANGED, self);

		priv->settings_event_id[SETTINGS_EVENT_IMSI_CHANGED] =
			ril_sim_settings_add_imsi_changed_handler(settings,
					ril_data_settings_changed, self);
		priv->settings_event_id[SETTINGS_EVENT_PREF_MODE] =
			ril_sim_settings_add_pref_mode_changed_handler(settings,
					ril_data_settings_changed, self);

		/* Request the current state */
		ril_data_poll_call_state(self);

		/* Order data contexts according to slot numbers */
		dm->data_list = g_slist_insert_sorted(dm->data_list, self,
							ril_data_compare_cb);
		ril_data_manager_check_network_mode(dm);
		return self;
	}
	return NULL;
}

static gboolean ril_data_poll_call_state_retry(GRilIoRequest* req,
	int ril_status, const void* resp_data, guint resp_len, void* user_data)
{
	switch (ril_status) {
	case RIL_E_SUCCESS:
	case RIL_E_RADIO_NOT_AVAILABLE:
		return FALSE;
	default:
		return TRUE;
	}
}

void ril_data_poll_call_state(struct ril_data *self)
{
	if (G_LIKELY(self)) {
		struct ril_data_priv *priv = self->priv;

		if (!priv->query_id) {
			GRilIoRequest *req = grilio_request_new();

			grilio_request_set_retry(req, RIL_RETRY_SECS*1000, -1);
			grilio_request_set_retry_func(req,
					ril_data_poll_call_state_retry);
			priv->query_id =
				grilio_queue_send_request_full(priv->q, req,
					RIL_REQUEST_DATA_CALL_LIST,
					ril_data_query_data_calls_cb,
					NULL, self);
			grilio_request_unref(req);
		}
	}
}

struct ril_data *ril_data_ref(struct ril_data *self)
{
	if (G_LIKELY(self)) {
		g_object_ref(RIL_DATA(self));
		return self;
	} else {
		return NULL;
	}
}

void ril_data_unref(struct ril_data *self)
{
	if (G_LIKELY(self)) {
		g_object_unref(RIL_DATA(self));
	}
}

gboolean ril_data_allowed(struct ril_data *self)
{
	return G_LIKELY(self) &&
		(self->priv->restricted_state &
			RIL_RESTRICTED_STATE_PS_ALL) == 0 &&
		(self->priv->flags &
			(RIL_DATA_FLAG_ALLOWED | RIL_DATA_FLAG_ON)) ==
			(RIL_DATA_FLAG_ALLOWED | RIL_DATA_FLAG_ON);
}

static void ril_data_deactivate_all(struct ril_data *self)
{
	if (self->data_calls) {
		GSList *l;

		for (l = self->data_calls->calls; l; l = l->next) {
			struct ril_data_call *call = l->data;
			if (call->status == PDP_FAIL_NONE) {
				DBG_(self, "deactivating call %u", call->cid);
				ril_data_call_deact_cid(self, call->cid);
			}
		}
	}
}

static void ril_data_power_update(struct ril_data *self)
{
	struct ril_data_priv *priv = self->priv;

	if (priv->pending_req || priv->req_queue) {
		ril_radio_power_on(priv->radio, self);
	} else {
		ril_radio_power_off(priv->radio, self);
	}
}

static void ril_data_cancel_requests(struct ril_data *self,
					enum ril_data_request_flags flags)
{
	struct ril_data_priv *priv = self->priv;
	struct ril_data_request *req = priv->req_queue;

	while (req) {
		struct ril_data_request *next = req->next;
		GASSERT(req->data == self);
		if (req->flags & flags) {
			ril_data_request_do_cancel(req);
		}
		req = next;
	}

	if (priv->pending_req && (priv->pending_req->flags & flags)) {
		ril_data_request_cancel(priv->pending_req);
	}
}

static void ril_data_disallow(struct ril_data *self)
{
	struct ril_data_priv *priv = self->priv;
	const gboolean was_allowed = ril_data_allowed(self);

	DBG_(self, "disallowed");
	GASSERT(priv->flags & RIL_DATA_FLAG_ALLOWED);
	priv->flags &= ~RIL_DATA_FLAG_ALLOWED;

	/*
	 * Cancel all requests that can be canceled.
	 */
	ril_data_cancel_requests(self,
				DATA_REQUEST_FLAG_CANCEL_WHEN_DISALLOWED);

	/*
	 * Then deactivate active contexts (Hmm... what if deactivate
	 * requests are already pending? That's quite unlikely though)
	 */
	ril_data_deactivate_all(self);

	if (priv->options.allow_data == RIL_ALLOW_DATA_ENABLED) {
		/* Tell rild that the data is now disabled */
		ril_data_request_queue(ril_data_allow_new(self, FALSE));
	} else {
		priv->flags &= ~RIL_DATA_FLAG_ON;
		GASSERT(!ril_data_allowed(self));
		DBG_(self, "data off");
		ril_data_power_update(self);
	}

	ril_data_check_allowed(self, was_allowed);
}

static void ril_data_max_speed_cb(gpointer data, gpointer max_speed)
{
	if (data != max_speed) {
		((struct ril_data *)data)->priv->flags &=
						~RIL_DATA_FLAG_MAX_SPEED;
	}
}

static void ril_data_disallow_cb(gpointer data_ptr, gpointer allowed)
{
	if (data_ptr != allowed) {
		struct ril_data *data = data_ptr;

		if (data->priv->flags & RIL_DATA_FLAG_ALLOWED) {
			ril_data_disallow(data);
		}
	}
}

void ril_data_allow(struct ril_data *self, enum ril_data_role role)
{
	if (G_LIKELY(self)) {
		struct ril_data_priv *priv = self->priv;
		struct ril_data_manager *dm = priv->dm;

		DBG_(self, "%s", (role == RIL_DATA_ROLE_NONE) ? "none" :
			(role == RIL_DATA_ROLE_MMS) ? "mms" : "internet");

		if (role != RIL_DATA_ROLE_NONE) {
			gboolean speed_changed = FALSE;
			if (role == RIL_DATA_ROLE_INTERNET &&
				!(priv->flags & RIL_DATA_FLAG_MAX_SPEED)) {
				priv->flags |= RIL_DATA_FLAG_MAX_SPEED;
				speed_changed = TRUE;

				/*
				 * Clear RIL_DATA_FLAG_MAX_SPEED for
				 * all other slots
				 */
				g_slist_foreach(dm->data_list,
						ril_data_max_speed_cb, self);
			}
			if (priv->flags & RIL_DATA_FLAG_ALLOWED) {
				/*
				 * Data is already allowed for this slot,
				 * just adjust the speed if necessary.
				 */
				if (speed_changed) {
					ril_data_manager_check_network_mode(dm);
				}
			} else {
				priv->flags |= RIL_DATA_FLAG_ALLOWED;
				priv->flags &= ~RIL_DATA_FLAG_ON;

				/*
				 * Clear RIL_DATA_FLAG_ALLOWED for all
				 * other slots
				 */
				g_slist_foreach(dm->data_list,
						ril_data_disallow_cb, self);

				ril_data_cancel_requests(self,
					DATA_REQUEST_FLAG_CANCEL_WHEN_ALLOWED);
				ril_data_manager_check_data(dm);
				ril_data_power_update(self);
			}
		} else {
			if (priv->flags & RIL_DATA_FLAG_ALLOWED) {
				ril_data_disallow(self);
				ril_data_manager_check_data(dm);
			}
		}
	}
}

struct ril_data_request *ril_data_call_setup(struct ril_data *self,
				const struct ofono_gprs_primary_context *ctx,
				ril_data_call_setup_cb_t cb, void *arg)
{
	struct ril_data_request *req =
		ril_data_call_setup_new(self, ctx, cb, arg);

	ril_data_request_queue(req);
	return req;
}

struct ril_data_request *ril_data_call_deactivate(struct ril_data *self,
			int cid, ril_data_call_deactivate_cb_t cb, void *arg)
{
	struct ril_data_request *req =
		ril_data_call_deact_new(self, cid, cb, arg);

	ril_data_request_queue(req);
	return req;
}

gboolean ril_data_call_grab(struct ril_data *self, int cid, void *cookie)
{
	if (self && cookie && ril_data_call_find(self->data_calls, cid)) {
		struct ril_data_priv *priv = self->priv;
		gpointer key = GINT_TO_POINTER(cid);
		void *prev = g_hash_table_lookup(priv->grab, key);

		if (!prev) {
			g_hash_table_insert(priv->grab, key, cookie);
			return TRUE;
		} else {
			return (prev == cookie);
		}
	}
	return FALSE;
}

void ril_data_call_release(struct ril_data *self, int cid, void *cookie)
{
	if (self && cookie) {
		struct ril_data_priv *priv = self->priv;

		g_hash_table_remove(priv->grab, GUINT_TO_POINTER(cid));
	}
}

static void ril_data_init(struct ril_data *self)
{
	struct ril_data_priv *priv = G_TYPE_INSTANCE_GET_PRIVATE(self,
		RIL_DATA_TYPE, struct ril_data_priv);

	self->priv = priv;
	priv->grab = g_hash_table_new(g_direct_hash, g_direct_equal);
}

static void ril_data_dispose(GObject *object)
{
	struct ril_data *self = RIL_DATA(object);
	struct ril_data_priv *priv = self->priv;
	struct ril_data_manager	*dm = priv->dm;
	struct ril_network *network = priv->network;
	struct ril_sim_settings *settings = network->settings;
	struct ril_data_request *req;

	ril_sim_settings_remove_handlers(settings, priv->settings_event_id,
					G_N_ELEMENTS(priv->settings_event_id));
	grilio_channel_remove_all_handlers(priv->io, priv->io_event_id);
	grilio_queue_cancel_all(priv->q, FALSE);
	priv->query_id = 0;

	ril_data_request_do_cancel(priv->pending_req);
	req = priv->req_queue;
	while (req) {
		struct ril_data_request *next = req->next;
		ril_data_request_do_cancel(req);
		req = next;
	}

	dm->data_list = g_slist_remove(dm->data_list, self);
	ril_data_manager_check_data(dm);
	g_hash_table_destroy(priv->grab);
	G_OBJECT_CLASS(ril_data_parent_class)->dispose(object);
}

static void ril_data_finalize(GObject *object)
{
	struct ril_data *self = RIL_DATA(object);
	struct ril_data_priv *priv = self->priv;

	g_free(priv->log_prefix);
	grilio_queue_unref(priv->q);
	grilio_channel_unref(priv->io);
	ril_radio_power_off(priv->radio, self);
	ril_radio_unref(priv->radio);
	ril_network_unref(priv->network);
	ril_data_manager_unref(priv->dm);
	ril_data_call_list_free(self->data_calls);
	ril_vendor_hook_unref(priv->vendor_hook);
	G_OBJECT_CLASS(ril_data_parent_class)->finalize(object);
}

static void ril_data_class_init(RilDataClass *klass)
{
	GObjectClass *object_class = G_OBJECT_CLASS(klass);

	object_class->dispose = ril_data_dispose;
	object_class->finalize = ril_data_finalize;
	g_type_class_add_private(klass, sizeof(struct ril_data_priv));
	NEW_SIGNAL(klass,ALLOW);
	NEW_SIGNAL(klass,CALLS);
}

/*==========================================================================*
 * ril_data_manager
 *==========================================================================*/

struct ril_data_manager *ril_data_manager_new(enum ril_data_manager_flags flg)
{
	struct ril_data_manager *self = g_new0(struct ril_data_manager, 1);
	self->ref_count = 1;
	self->flags = flg;
	return self;
}

struct ril_data_manager *ril_data_manager_ref(struct ril_data_manager *self)
{
	if (self) {
		GASSERT(self->ref_count > 0);
		g_atomic_int_inc(&self->ref_count);
	}
	return self;
}

void ril_data_manager_unref(struct ril_data_manager *self)
{
	if (self) {
		GASSERT(self->ref_count > 0);
		if (g_atomic_int_dec_and_test(&self->ref_count)) {
			g_free(self);
		}
	}
}

static gboolean ril_data_manager_handover(struct ril_data_manager *self)
{
	/*
	 * The 3G/LTE handover thing only makes sense if we are managing
	 * more than one SIM slot. Otherwise leave things where they are.
	 */
	return (self->data_list && self->data_list->next &&
			(self->flags & RIL_DATA_MANAGER_3GLTE_HANDOVER));
}

static gboolean ril_data_manager_requests_pending(struct ril_data_manager *self)
{
	GSList *l;

	for (l= self->data_list; l; l = l->next) {
		struct ril_data *data = l->data;
		if (data->priv->pending_req || data->priv->req_queue) {
			return TRUE;
		}
	}

	return FALSE;
}

static void ril_data_manager_check_network_mode(struct ril_data_manager *self)
{
	GSList *l;

	if (ril_data_manager_handover(self)) {
		struct ril_network *lte_network = NULL;
		int non_gsm_count = 0;

		/*
		 * Count number of SIMs for which non-GSM mode is selected
		 */
		for (l= self->data_list; l; l = l->next) {
			struct ril_data *data = l->data;
			struct ril_data_priv *priv = data->priv;
			struct ril_network *network = priv->network;
			struct ril_sim_settings *sim = network->settings;

			if (sim->pref_mode != OFONO_RADIO_ACCESS_MODE_GSM) {
				non_gsm_count++;
				if ((priv->flags & RIL_DATA_FLAG_MAX_SPEED) &&
							!lte_network) {
					lte_network = network;
				}
			}
		}

		/*
		 * If there's no SIM selected for internet access
		 * then choose the first slot for LTE.
		 */
		if (!lte_network) {
			struct ril_data *data = self->data_list->data;
			lte_network = data->priv->network;
		}

		for (l= self->data_list; l; l = l->next) {
			struct ril_data *data = l->data;
			struct ril_network *network = data->priv->network;

			ril_network_set_max_pref_mode(network,
					(network == lte_network) ?
					OFONO_RADIO_ACCESS_MODE_ANY :
					OFONO_RADIO_ACCESS_MODE_GSM,
					FALSE);
		}

	} else {
		/* Otherwise there's no reason to limit anything */
		for (l= self->data_list; l; l = l->next) {
			struct ril_data *data = l->data;
			ril_network_set_max_pref_mode(data->priv->network,
					OFONO_RADIO_ACCESS_MODE_ANY, FALSE);
		}
	}
}

static struct ril_data *ril_data_manager_allowed(struct ril_data_manager *self)
{
	GSList *l;

	for (l= self->data_list; l; l = l->next) {
		struct ril_data *data = l->data;
		if (data->priv->flags & RIL_DATA_FLAG_ALLOWED) {
			return data;
		}
	}

	return NULL;
}

static void ril_data_manager_switch_data_on(struct ril_data_manager *self,
						struct ril_data *data)
{
	struct ril_data_priv *priv = data->priv;

	DBG_(data, "allowing data");
	GASSERT(!(priv->flags & RIL_DATA_FLAG_ON));

	if (ril_data_manager_handover(self)) {
		ril_network_set_max_pref_mode(priv->network,
					OFONO_RADIO_ACCESS_MODE_ANY, TRUE);
	}

	if (priv->options.allow_data == RIL_ALLOW_DATA_ENABLED) {
		ril_data_request_queue(ril_data_allow_new(data, TRUE));
	} else {
		priv->flags |= RIL_DATA_FLAG_ON;
		GASSERT(ril_data_allowed(data));
		DBG_(data, "data on");
		ril_data_signal_emit(data, SIGNAL_ALLOW_CHANGED);
	}
}

static void ril_data_manager_check_data(struct ril_data_manager *self)
{
	/*
	 * Don't do anything if there any requests pending.
	 */
	if (!ril_data_manager_requests_pending(self)) {
		struct ril_data *data = ril_data_manager_allowed(self);
		ril_data_manager_check_network_mode(self);
		if (data && !(data->priv->flags & RIL_DATA_FLAG_ON)) {
			ril_data_manager_switch_data_on(self, data);
		}
	}
}

void ril_data_manager_assert_data_on(struct ril_data_manager *self)
{
	if (self) {
		struct ril_data *data = ril_data_manager_allowed(self);
		if (data) {
			ril_data_request_queue(ril_data_allow_new(data, TRUE));
		}
	}
}

/*
 * Local Variables:
 * mode: C
 * c-basic-offset: 8
 * indent-tabs-mode: t
 * End:
 */
