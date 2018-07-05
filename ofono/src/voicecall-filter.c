/*
 *  oFono - Open Source Telephony
 *
 *  Copyright (C) 2018 Jolla Ltd.
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

#include "ofono.h"
#include "common.h"

#include <errno.h>
#include <string.h>

struct voicecall_filter_request;
struct voicecall_filter_request_fn {
	const char *name;
	gboolean (*can_process)(const struct ofono_voicecall_filter *filter);
	guint (*process)(const struct ofono_voicecall_filter *filter,
					struct voicecall_filter_request *req);
	void (*allow)(struct voicecall_filter_request *req);
	void (*free)(struct voicecall_filter_request *req);
};

struct voicecall_filter_request {
	int refcount;
	const struct voicecall_filter_request_fn *fn;
	const struct ofono_call *call;
	struct voicecall_filter_chain *chain;
	GSList *filter_link;
	guint pending_id;
	guint next_id;
	ofono_destroy_func destroy;
	void* user_data;
};

struct voicecall_filter_request_dial {
	struct voicecall_filter_request req;
	const struct ofono_phone_number *number;
	enum ofono_clir_option clir;
	ofono_voicecall_filter_dial_cb_t cb;
};

struct voicecall_filter_request_incoming {
	struct voicecall_filter_request req;
	ofono_voicecall_filter_incoming_cb_t cb;
};

struct voicecall_filter_chain {
	struct ofono_voicecall *vc;
	GSList *req_list;
};

static GSList *voicecall_filters = NULL;

static void voicecall_filter_request_init(struct voicecall_filter_request *req,
	const struct voicecall_filter_request_fn *fn,
	struct voicecall_filter_chain *chain, const struct ofono_call *call,
	ofono_destroy_func destroy, void *user_data)
{
	req->fn = fn;
	req->chain = chain;
	req->call = call;
	req->filter_link = voicecall_filters;
	req->destroy = destroy;
	req->user_data = user_data;

	/*
	 * The list holds an implicit reference to the message. The reference
	 * is released by voicecall_filter_request_free when the message is
	 * removed from the list.
	 */
	req->refcount = 1;
	chain->req_list = g_slist_append(chain->req_list, req);
}

static void voicecall_filter_request_cancel
		(struct voicecall_filter_request *req)
{
	if (req->pending_id) {
		const struct ofono_voicecall_filter *f = req->filter_link->data;

		/*
		 * If the filter returns id of the pending operation,
		 * then it must provide the cancel callback
		 */
		f->filter_cancel(req->pending_id);
		req->pending_id = 0;
	}
	if (req->next_id) {
		g_source_remove(req->next_id);
		req->next_id = 0;
	}
}

static void voicecall_filter_request_dispose
		(struct voicecall_filter_request *req)
{
	/* May be invoked several times per request */
	if (req->destroy) {
		ofono_destroy_func destroy = req->destroy;

		req->destroy = NULL;
		destroy(req->user_data);
	}
}

static void voicecall_filter_request_free(struct voicecall_filter_request *req)
{
	voicecall_filter_request_dispose(req);
	req->fn->free(req);
}

#define voicecall_filter_request_ref(req) ((req)->refcount++, req)

static int voicecall_filter_request_unref(struct voicecall_filter_request *req)
{
	const int refcount = --(req->refcount);

	if (!refcount) {
		voicecall_filter_request_free(req);
	}
	return refcount;
}

static void voicecall_filter_request_done(struct voicecall_filter_request *req)
{
	/* Zero the pointer to it in case if this is not the last reference. */
	req->chain = NULL;
	voicecall_filter_request_unref(req);
}

static void voicecall_filter_request_dequeue
		(struct voicecall_filter_request *req)
{
	struct voicecall_filter_chain *chain = req->chain;
	GSList *l;

	/*
	 * Single-linked list is not particularly good at searching
	 * and removing the elements but since it should be pretty
	 * short (typically just one request), it's not worth optimization.
	 */
	if (chain && (l = g_slist_find(chain->req_list, req)) != NULL) {
		voicecall_filter_request_done(l->data);
		chain->req_list = g_slist_delete_link(chain->req_list, l);
	}
}

static void voicecall_filter_request_complete
		(struct voicecall_filter_request *req,
			void (*complete)(struct voicecall_filter_request *req))
{
	voicecall_filter_request_ref(req);
	complete(req);
	voicecall_filter_request_dispose(req);
	voicecall_filter_request_dequeue(req);
	voicecall_filter_request_unref(req);
}

static void voicecall_filter_request_process
		(struct voicecall_filter_request *req)
{
	GSList *l = req->filter_link;
	const struct ofono_voicecall_filter *f = l->data;
	const struct voicecall_filter_request_fn *fn = req->fn;

	while (f && !fn->can_process(f)) {
		l = l->next;
		f = l ? l->data : NULL;
	}

	voicecall_filter_request_ref(req);
	if (f) {
		req->filter_link = l;
		req->pending_id = fn->process(f, req);
	} else {
		voicecall_filter_request_complete(req, fn->allow);
	}
	voicecall_filter_request_unref(req);
}

static void voicecall_filter_request_next(struct voicecall_filter_request *req,
							GSourceFunc fn)
{
	req->pending_id = 0;
	req->next_id = g_idle_add(fn, req);
}

static gboolean voicecall_filter_request_continue_cb(gpointer data)
{
	struct voicecall_filter_request *req = data;

	req->next_id = 0;
	req->filter_link = req->filter_link->next;
	if (req->filter_link) {
		voicecall_filter_request_process(req);
	} else {
		voicecall_filter_request_complete(req, req->fn->allow);
	}
	return G_SOURCE_REMOVE;
}

/*==========================================================================*
 * voicecall_filter_request_dial
 *==========================================================================*/

static struct voicecall_filter_request_dial *
			voicecall_filter_request_dial_cast
					(struct voicecall_filter_request *req)
{
	return (struct voicecall_filter_request_dial *)req;
}

static void voicecall_filter_request_dial_block_complete_cb
					(struct voicecall_filter_request *req)
{
	struct voicecall_filter_request_dial *dial =
				voicecall_filter_request_dial_cast(req);

	dial->cb(OFONO_VOICECALL_FILTER_DIAL_BLOCK, req->user_data);
}

static gboolean voicecall_filter_request_dial_block_cb(gpointer data)
{
	struct voicecall_filter_request_dial *dial = data;
	struct voicecall_filter_request *req = &dial->req;

	req->next_id = 0;
	voicecall_filter_request_complete(req,
			voicecall_filter_request_dial_block_complete_cb);
	return G_SOURCE_REMOVE;
}

static void voicecall_filter_request_dial_cb
		(enum ofono_voicecall_filter_dial_result result, void *data)
{
	struct voicecall_filter_request_dial *dial = data;
	struct voicecall_filter_request *req = &dial->req;
	const struct ofono_voicecall_filter *filter = req->filter_link->data;
	GSourceFunc next_cb;

	if (result == OFONO_VOICECALL_FILTER_DIAL_BLOCK) {
		ofono_info("%s is refusing to dial %s", filter->name,
					phone_number_to_string(dial->number));
		next_cb = voicecall_filter_request_dial_block_cb;
	} else {
		/* OFONO_VOICECALL_FILTER_DIAL_CONTINUE */
		DBG("%s is ok with dialing %s", filter->name,
					phone_number_to_string(dial->number));
		next_cb = voicecall_filter_request_continue_cb;
	}

	voicecall_filter_request_next(req, next_cb);
}

static gboolean voicecall_filter_request_dial_can_process
				(const struct ofono_voicecall_filter *f)
{
	return f->filter_dial != NULL;
}

static guint voicecall_filter_request_dial_process
				(const struct ofono_voicecall_filter *f,
					struct voicecall_filter_request *req)
{
	struct voicecall_filter_request_dial *dial =
				voicecall_filter_request_dial_cast(req);

	return f->filter_dial(req->chain->vc, dial->number, dial->clir,
				voicecall_filter_request_dial_cb, dial);
}

static void voicecall_filter_request_dial_allow
					(struct voicecall_filter_request *req)
{
	struct voicecall_filter_request_dial *dial =
				voicecall_filter_request_dial_cast(req);

	dial->cb(OFONO_VOICECALL_FILTER_DIAL_CONTINUE, req->user_data);
}

static void voicecall_filter_request_dial_free
					(struct voicecall_filter_request *req)
{
	g_slice_free1(sizeof(struct voicecall_filter_request_dial), req);
}

static struct voicecall_filter_request *voicecall_filter_request_dial_new
	(struct voicecall_filter_chain *chain,
		const struct ofono_phone_number *number,
		enum ofono_clir_option clir,
		ofono_voicecall_filter_dial_cb_t cb,
		ofono_destroy_func destroy, void *data)
{
	static const struct voicecall_filter_request_fn fn = {
		.name = "dial",
		.can_process = voicecall_filter_request_dial_can_process,
		.process = voicecall_filter_request_dial_process,
		.allow = voicecall_filter_request_dial_allow,
		.free = voicecall_filter_request_dial_free
	};

	struct voicecall_filter_request_dial *dial =
		g_slice_new0(struct voicecall_filter_request_dial);
	struct voicecall_filter_request *req = &dial->req;

	voicecall_filter_request_init(req, &fn, chain, NULL, destroy, data);
	dial->number = number;
	dial->clir = clir;
	dial->cb = cb;
	return req;
}

/*==========================================================================*
 * voicecall_filter_request_incoming
 *==========================================================================*/

static struct voicecall_filter_request_incoming *
			voicecall_filter_request_incoming_cast
					(struct voicecall_filter_request *req)
{
	return (struct voicecall_filter_request_incoming *)req;
}

static void voicecall_filter_request_incoming_hangup_complete_cb
					(struct voicecall_filter_request *req)
{
	struct voicecall_filter_request_incoming *in =
				voicecall_filter_request_incoming_cast(req);

	in->cb(OFONO_VOICECALL_FILTER_INCOMING_HANGUP, req->user_data);
}

static gboolean voicecall_filter_request_incoming_hangup_cb(gpointer data)
{
	struct voicecall_filter_request_incoming *in = data;
	struct voicecall_filter_request *req = &in->req;

	req->next_id = 0;
	voicecall_filter_request_complete(req,
		voicecall_filter_request_incoming_hangup_complete_cb);
	return G_SOURCE_REMOVE;
}

static void voicecall_filter_request_incoming_ignore_complete_cb
					(struct voicecall_filter_request *req)
{
	struct voicecall_filter_request_incoming *in =
				voicecall_filter_request_incoming_cast(req);

	in->cb(OFONO_VOICECALL_FILTER_INCOMING_IGNORE, req->user_data);
}

static gboolean voicecall_filter_request_incoming_ignore_cb(gpointer data)
{
	struct voicecall_filter_request_incoming *in = data;
	struct voicecall_filter_request *req = &in->req;

	req->next_id = 0;
	voicecall_filter_request_complete(req,
			voicecall_filter_request_incoming_ignore_complete_cb);
	return G_SOURCE_REMOVE;
}

static void voicecall_filter_request_incoming_cb
		(enum ofono_voicecall_filter_incoming_result result, void *data)
{
	struct voicecall_filter_request_incoming *in = data;
	struct voicecall_filter_request *req = &in->req;
	const struct ofono_voicecall_filter *filter = req->filter_link->data;
	GSourceFunc next_cb;

	if (result == OFONO_VOICECALL_FILTER_INCOMING_HANGUP) {
		ofono_info("%s hangs up incoming call from %s", filter->name,
			phone_number_to_string(&req->call->phone_number));
		next_cb = voicecall_filter_request_incoming_hangup_cb;
	} else if (result == OFONO_VOICECALL_FILTER_INCOMING_IGNORE) {
		ofono_info("%s ignores incoming call from %s", filter->name,
			phone_number_to_string(&req->call->phone_number));
		next_cb = voicecall_filter_request_incoming_ignore_cb;
	} else {
		/* OFONO_VOICECALL_FILTER_INCOMING_CONTINUE */
		DBG("%s is ok with accepting %s", filter->name,
			phone_number_to_string(&req->call->phone_number));
		next_cb = voicecall_filter_request_continue_cb;
	}

	voicecall_filter_request_next(req, next_cb);
}

static gboolean voicecall_filter_request_incoming_can_process
				(const struct ofono_voicecall_filter *f)
{
	return f->filter_incoming != NULL;
}

static guint voicecall_filter_request_incoming_process
				(const struct ofono_voicecall_filter *f,
					struct voicecall_filter_request *req)
{
	return f->filter_incoming(req->chain->vc, req->call,
				voicecall_filter_request_incoming_cb,
				voicecall_filter_request_incoming_cast(req));
}

static void voicecall_filter_request_incoming_allow
					(struct voicecall_filter_request *req)
{
	struct voicecall_filter_request_incoming *in =
				voicecall_filter_request_incoming_cast(req);

	in->cb(OFONO_VOICECALL_FILTER_INCOMING_CONTINUE, req->user_data);
}

static void voicecall_filter_request_incoming_free
					(struct voicecall_filter_request *req)
{
	g_slice_free1(sizeof(struct voicecall_filter_request_incoming), req);
}

static struct voicecall_filter_request *voicecall_filter_request_incoming_new
	(struct voicecall_filter_chain *chain, const struct ofono_call *call,
		ofono_voicecall_filter_incoming_cb_t cb,
		ofono_destroy_func destroy, void *data)
{
	static const struct voicecall_filter_request_fn fn = {
		.name = "incoming",
		.can_process = voicecall_filter_request_incoming_can_process,
		.process = voicecall_filter_request_incoming_process,
		.allow = voicecall_filter_request_incoming_allow,
		.free = voicecall_filter_request_incoming_free
	};

	struct voicecall_filter_request_incoming *in =
		g_slice_new0(struct voicecall_filter_request_incoming);
	struct voicecall_filter_request *req = &in->req;

	voicecall_filter_request_init(req, &fn, chain, call, destroy, data);
	in->cb = cb;
	return req;
}

/*==========================================================================*
 * voicecall_filter_chain
 *==========================================================================*/

struct voicecall_filter_chain *__ofono_voicecall_filter_chain_new
						(struct ofono_voicecall *vc)
{
	struct voicecall_filter_chain *chain = NULL;

	if (vc) {
		chain = g_new0(struct voicecall_filter_chain, 1);
		chain->vc = vc;
	}

	return chain;
}

void __ofono_voicecall_filter_chain_free(struct voicecall_filter_chain *chain)
{
	if (chain) {
		__ofono_voicecall_filter_chain_cancel(chain, NULL);
		g_free(chain);
	}
}

static GSList *voicecall_filter_chain_select(struct voicecall_filter_chain *c,
						const struct ofono_call *call)
{
	if (c) {
		GSList *selected;

		/* Move selected requests to a separate list */
		if (call) {
			GSList *prev = NULL;
			GSList *l = c->req_list;

			selected = NULL;
			while (l) {
				GSList *next = l->next;
				struct voicecall_filter_request *req = l->data;

				if (req->call == call) {
					/* This one will get canceled */
					l->next = selected;
					selected = l;
					if (prev) {
						prev->next = next;
					} else {
						c->req_list = next;
					}
				} else {
					/* This one survives */
					prev = l;
				}
				l = next;
			}
		} else {
			/* Select everything */
			selected = c->req_list;
			c->req_list = NULL;
		}

		return selected;
	} else {
		return NULL;
	}
}

void __ofono_voicecall_filter_chain_restart(struct voicecall_filter_chain *c,
				const struct ofono_call *call)
{
	GSList *l, *canceled = voicecall_filter_chain_select(c, call);

	/* Cancel and resubmit each request */
	for (l = canceled; l; l = l->next) {
		struct voicecall_filter_request *req = l->data;

		voicecall_filter_request_cancel(req);
		voicecall_filter_request_process(req);
	}

}

void __ofono_voicecall_filter_chain_cancel(struct voicecall_filter_chain *c,
						const struct ofono_call *call)
{
	GSList *l, *canceled = voicecall_filter_chain_select(c, call);

	/* Cancel and deallocate each request */
	for (l = canceled; l; l = l->next) {
		struct voicecall_filter_request *req = l->data;

		voicecall_filter_request_cancel(req);
		voicecall_filter_request_done(req);
	}
}

void __ofono_voicecall_filter_chain_dial(struct voicecall_filter_chain *chain,
				const struct ofono_phone_number *number,
				enum ofono_clir_option clir,
				ofono_voicecall_filter_dial_cb_t cb,
				ofono_destroy_func destroy, void *user_data)
{
	if (chain && voicecall_filters && number && cb) {
		voicecall_filter_request_process
			(voicecall_filter_request_dial_new(chain, number,
						clir, cb, destroy, user_data));
	} else {
		if (cb) {
			cb(OFONO_VOICECALL_FILTER_DIAL_CONTINUE, user_data);
		}
		if (destroy) {
			destroy(user_data);
		}
	}
}

void __ofono_voicecall_filter_chain_dial_check(struct voicecall_filter_chain *c,
				const struct ofono_call *call,
				ofono_voicecall_filter_dial_cb_t cb,
				ofono_destroy_func destroy, void *user_data)
{
	if (c && voicecall_filters && call && cb) {
		struct voicecall_filter_request *req =
			voicecall_filter_request_dial_new(c,
				&call->phone_number, OFONO_CLIR_OPTION_DEFAULT,
				cb, destroy, user_data);

		req->call = call;
		voicecall_filter_request_process(req);
	} else {
		if (cb) {
			cb(OFONO_VOICECALL_FILTER_DIAL_CONTINUE, user_data);
		}
		if (destroy) {
			destroy(user_data);
		}
	}
}

void __ofono_voicecall_filter_chain_incoming(struct voicecall_filter_chain *fc,
				const struct ofono_call *call,
				ofono_voicecall_filter_incoming_cb_t cb,
				ofono_destroy_func destroy, void *user_data)
{
	if (fc && voicecall_filters && call && cb) {
		voicecall_filter_request_process
			(voicecall_filter_request_incoming_new(fc, call,
						cb, destroy, user_data));
	} else {
		if (cb) {
			cb(OFONO_VOICECALL_FILTER_INCOMING_CONTINUE, user_data);
		}
		if (destroy) {
			destroy(user_data);
		}
	}
}

/*==========================================================================*
 * ofono_voicecall_filter
 *==========================================================================*/

/**
 * Returns 0 if both are equal;
 * <0 if a comes before b;
 * >0 if a comes after b.
 */
static gint voicecall_filter_sort(gconstpointer a, gconstpointer b)
{
	const struct ofono_voicecall_filter *a_filter = a;
	const struct ofono_voicecall_filter *b_filter = b;

	if (a_filter->priority > b_filter->priority) {
		/* a comes before b */
		return -1;
	} else if (a_filter->priority < b_filter->priority) {
		/* a comes after b */
		return 1;
	} else {
		/* Whatever, as long as the sort is stable */
		return strcmp(a_filter->name, b_filter->name);
	}
}

int ofono_voicecall_filter_register(const struct ofono_voicecall_filter *f)
{
	if (!f || !f->name) {
		return -EINVAL;
	}

	DBG("%s", f->name);
	voicecall_filters = g_slist_insert_sorted(voicecall_filters, (void*)f,
							voicecall_filter_sort);
	return 0;
}

void ofono_voicecall_filter_unregister(const struct ofono_voicecall_filter *f)
{
	if (f) {
		DBG("%s", f->name);
		voicecall_filters = g_slist_remove(voicecall_filters, f);
	}
}

/*
 * Local Variables:
 * mode: C
 * c-basic-offset: 8
 * indent-tabs-mode: t
 * End:
 */
