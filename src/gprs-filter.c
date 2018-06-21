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

#include <errno.h>
#include <string.h>

struct gprs_filter_request;
struct gprs_filter_request_fn {
	const char *name;
	gboolean (*can_process)(const struct ofono_gprs_filter *filter);
	guint (*process)(const struct ofono_gprs_filter *filter,
					struct gprs_filter_request *req);
	void (*complete)(struct gprs_filter_request *req, gboolean allow);
	void (*free)(struct gprs_filter_request *req);
};

struct gprs_filter_request {
	int refcount;
	struct gprs_filter_chain *chain;
	struct ofono_gprs_context *gc;
	const struct gprs_filter_request_fn *fn;
	GSList *filter_link;
	guint pending_id;
	guint next_id;
	ofono_destroy_func destroy;
	void* user_data;
};

struct gprs_filter_request_activate {
	struct gprs_filter_request req;
	struct ofono_gprs_primary_context ctx;
	gprs_filter_activate_cb_t cb;
};

struct gprs_filter_request_check {
	struct gprs_filter_request req;
	ofono_gprs_filter_check_cb_t cb;
};

struct gprs_filter_chain {
	struct ofono_gprs *gprs;
	GSList *req_list;
};

static GSList *gprs_filter_list = NULL;

static void gprs_filter_request_init(struct gprs_filter_request *req,
		const struct gprs_filter_request_fn *fn,
		struct gprs_filter_chain *chain, struct ofono_gprs_context *gc,
		ofono_destroy_func destroy, void *user_data)
{
	req->chain = chain;
	req->fn = fn;
	req->gc = gc;
	req->filter_link = gprs_filter_list;
	req->destroy = destroy;
	req->user_data = user_data;

	/*
	 * The list holds an implicit reference to the message. The reference
	 * is released by gprs_filter_request_free when the message is removed
	 * from the list.
	 */
	req->refcount = 1;
	chain->req_list = g_slist_append(chain->req_list, req);
}

static void gprs_filter_request_cancel(struct gprs_filter_request *req)
{
	if (req->pending_id) {
		const struct ofono_gprs_filter *f = req->filter_link->data;

		/*
		 * If the filter returns id of the pending operation,
		 * then it must provide the cancel callback
		 */
		f->cancel(req->pending_id);
		req->pending_id = 0;
	}
	if (req->next_id) {
		g_source_remove(req->next_id);
		req->next_id = 0;
	}
}

static void gprs_filter_request_dispose(struct gprs_filter_request *req)
{
	/* May be invoked several times per request */
	if (req->destroy) {
		ofono_destroy_func destroy = req->destroy;

		req->destroy = NULL;
		destroy(req->user_data);
	}
}

static void gprs_filter_request_free(struct gprs_filter_request *req)
{
	gprs_filter_request_dispose(req);
	req->fn->free(req);
}

#define gprs_filter_request_ref(req) ((req)->refcount++, req)

static int gprs_filter_request_unref(struct gprs_filter_request *req)
{
	const int refcount = --(req->refcount);

	if (!refcount) {
		gprs_filter_request_free(req);
	}
	return refcount;
}

static void gprs_filter_request_free1(gpointer data)
{
	struct gprs_filter_request *req = data;

	/*
	 * This is a g_slist_free_full() callback for use by
	 * __ofono_gprs_filter_chain_free(), meaning that the
	 * chain is no more. Zero the pointer to it in case if
	 * this is not the last reference.
	 */
	req->chain = NULL;
	gprs_filter_request_unref(req);
}

static void gprs_filter_request_dequeue(struct gprs_filter_request *req)
{
	struct gprs_filter_chain *chain = req->chain;
	GSList *l;

	/*
	 * Single-linked list is not particularly good at searching
	 * and removing the elements but since it should be pretty
	 * short (typically just one request), it's not worth optimization.
	 */
	if (chain && (l = g_slist_find(chain->req_list, req)) != NULL) {
		gprs_filter_request_free1(l->data);
		chain->req_list = g_slist_delete_link(chain->req_list, l);
	}
}

static void gprs_filter_request_complete(struct gprs_filter_request *req,
							gboolean allow)
{
	gprs_filter_request_ref(req);
	req->fn->complete(req, allow);
	gprs_filter_request_dispose(req);
	gprs_filter_request_dequeue(req);
	gprs_filter_request_unref(req);
}

static void gprs_filter_request_process(struct gprs_filter_request *req)
{
	GSList *l = req->filter_link;
	const struct ofono_gprs_filter *f = l->data;
	const struct gprs_filter_request_fn *fn = req->fn;

	while (f && !fn->can_process(f)) {
		l = l->next;
		f = l ? l->data : NULL;
	}

	gprs_filter_request_ref(req);
	if (f) {
		req->filter_link = l;
		req->pending_id = fn->process(f, req);
	} else {
		gprs_filter_request_complete(req, TRUE);
	}
	gprs_filter_request_unref(req);
}

static void gprs_filter_request_next(struct gprs_filter_request *req,
							GSourceFunc fn)
{
	req->pending_id = 0;
	req->next_id = g_idle_add(fn, req);
}

static gboolean gprs_filter_request_continue_cb(gpointer data)
{
	struct gprs_filter_request *req = data;

	req->next_id = 0;
	req->filter_link = req->filter_link->next;
	if (req->filter_link) {
		gprs_filter_request_process(req);
	} else {
		gprs_filter_request_complete(req, TRUE);
	}
	return G_SOURCE_REMOVE;
}

static gboolean gprs_filter_request_disallow_cb(gpointer data)
{
	struct gprs_filter_request *req = data;

	req->next_id = 0;
	gprs_filter_request_complete(req, FALSE);
	return G_SOURCE_REMOVE;
}

/*==========================================================================*
 * gprs_filter_request_activate
 *==========================================================================*/

static void gprs_filter_copy_context(struct ofono_gprs_primary_context *dest,
				const struct ofono_gprs_primary_context *src)
{
	dest->cid = src->cid;
	dest->proto = src->proto;
	dest->auth_method = src->auth_method;
	strncpy(dest->apn, src->apn, OFONO_GPRS_MAX_APN_LENGTH);
	strncpy(dest->username, src->username, OFONO_GPRS_MAX_USERNAME_LENGTH);
	strncpy(dest->password, src->password, OFONO_GPRS_MAX_PASSWORD_LENGTH);
	dest->apn[OFONO_GPRS_MAX_APN_LENGTH] = 0;
	dest->username[OFONO_GPRS_MAX_USERNAME_LENGTH] = 0;
	dest->password[OFONO_GPRS_MAX_PASSWORD_LENGTH] = 0;
}

static struct gprs_filter_request_activate *gprs_filter_request_activate_cast
					(struct gprs_filter_request *req)
{
	return (struct gprs_filter_request_activate *)req;
}

static gboolean gprs_filter_request_activate_can_process
					(const struct ofono_gprs_filter *f)
{
	return f->filter_activate != NULL;
}

static void gprs_filter_request_activate_cb
		(const struct ofono_gprs_primary_context *ctx, void *data)
{
	struct gprs_filter_request_activate *act = data;
	struct gprs_filter_request *req = &act->req;
	const struct ofono_gprs_filter *filter = req->filter_link->data;

	if (ctx) {
		if (ctx != &act->ctx) {
			/* The filter may have updated context settings */
			gprs_filter_copy_context(&act->ctx, ctx);
		}
		gprs_filter_request_next(req, gprs_filter_request_continue_cb);
	} else {
		DBG("%s not allowing to activate mobile data", filter->name);
		gprs_filter_request_next(req, gprs_filter_request_disallow_cb);
	}
}

static guint gprs_filter_request_activate_process
				(const struct ofono_gprs_filter *f,
					struct gprs_filter_request *req)
{
	struct gprs_filter_request_activate *act =
		gprs_filter_request_activate_cast(req);

	return f->filter_activate(req->gc, &act->ctx,
				gprs_filter_request_activate_cb, act);
}

static void gprs_filter_request_activate_complete
			(struct gprs_filter_request *req, gboolean allow)
{
	struct gprs_filter_request_activate *act =
		gprs_filter_request_activate_cast(req);

	act->cb(allow ? &act->ctx : NULL, req->user_data);
}

static void gprs_filter_request_activate_free(struct gprs_filter_request *req)
{
	g_slice_free1(sizeof(struct gprs_filter_request_activate), req);
}

static struct gprs_filter_request *gprs_filter_request_activate_new
	(struct gprs_filter_chain *chain, struct ofono_gprs_context *gc,
		const struct ofono_gprs_primary_context *ctx,
		gprs_filter_activate_cb_t cb, ofono_destroy_func destroy,
		void *data)
{
	static const struct gprs_filter_request_fn activate_fn = {
		.name = "activate",
		.can_process = gprs_filter_request_activate_can_process,
		.process = gprs_filter_request_activate_process,
		.complete = gprs_filter_request_activate_complete,
		.free = gprs_filter_request_activate_free
	};

	struct gprs_filter_request_activate *act =
		g_slice_new0(struct gprs_filter_request_activate);
	struct gprs_filter_request *req = &act->req;

	gprs_filter_request_init(req, &activate_fn, chain, gc, destroy, data);
	gprs_filter_copy_context(&act->ctx, ctx);
	act->cb = cb;
	return req;
}

/*==========================================================================*
 * gprs_filter_request_check
 *==========================================================================*/

static struct gprs_filter_request_check *gprs_filter_request_check_cast
					(struct gprs_filter_request *req)
{
	return (struct gprs_filter_request_check *)req;
}

static gboolean gprs_filter_request_check_can_process
					(const struct ofono_gprs_filter *f)
{
	return f->api_version >= 1 && f->filter_check != NULL;
}

static void gprs_filter_request_check_cb(ofono_bool_t allow, void *data)
{
	struct gprs_filter_request_check *check = data;
	struct gprs_filter_request *req = &check->req;
	const struct ofono_gprs_filter *filter = req->filter_link->data;

	if (allow) {
		gprs_filter_request_next(req, gprs_filter_request_continue_cb);
	} else {
		DBG("%s not allowing mobile data", filter->name);
		gprs_filter_request_next(req, gprs_filter_request_disallow_cb);
	}
}

static guint gprs_filter_request_check_process
				(const struct ofono_gprs_filter *f,
					struct gprs_filter_request *req)
{
	return f->filter_check(req->chain->gprs, gprs_filter_request_check_cb,
					gprs_filter_request_check_cast(req));
}

static void gprs_filter_request_check_complete
			(struct gprs_filter_request *req, gboolean allow)
{
	gprs_filter_request_check_cast(req)->cb(allow, req->user_data);
}

static void gprs_filter_request_check_free(struct gprs_filter_request *req)
{
	g_slice_free1(sizeof(struct gprs_filter_request_check), req);
}

static struct gprs_filter_request *gprs_filter_request_check_new
	(struct gprs_filter_chain *chain, gprs_filter_check_cb_t cb,
				ofono_destroy_func destroy, void *data)
{
	static const struct gprs_filter_request_fn check_fn = {
		.name = "check",
		.can_process = gprs_filter_request_check_can_process,
		.process = gprs_filter_request_check_process,
		.complete = gprs_filter_request_check_complete,
		.free = gprs_filter_request_check_free
	};

	struct gprs_filter_request_check *check =
		g_slice_new0(struct gprs_filter_request_check);
	struct gprs_filter_request *req = &check->req;

	gprs_filter_request_init(req, &check_fn, chain, NULL, destroy, data);
	check->cb = cb;
	return req;
}

/*==========================================================================*
 * gprs_filter_chain
 *==========================================================================*/

struct gprs_filter_chain *__ofono_gprs_filter_chain_new(struct ofono_gprs *gp)
{
	struct gprs_filter_chain *chain = NULL;

	if (gp) {
		chain = g_new0(struct gprs_filter_chain, 1);
		chain->gprs = gp;
	}
	return chain;
}

void __ofono_gprs_filter_chain_free(struct gprs_filter_chain *chain)
{
	if (chain) {
		__ofono_gprs_filter_chain_cancel(chain, NULL);
		g_free(chain);
	}
}

void __ofono_gprs_filter_chain_cancel(struct gprs_filter_chain *chain,
					struct ofono_gprs_context *gc)
{
	if (chain) {
		GSList *l, *canceled;

		/* Move canceled requests to a separate list */
		if (gc) {
			GSList *prev = NULL;

			canceled = NULL;
			l = chain->req_list;
			while (l) {
				GSList *next = l->next;
				struct gprs_filter_request *req = l->data;

				if (req->gc == gc) {
					/* This one will get canceled */
					l->next = canceled;
					canceled = l;
					if (prev) {
						prev->next = next;
					} else {
						chain->req_list = next;
					}
				} else {
					/* This one survives */
					prev = l;
				}
				l = next;
			}
		} else {
			/* Everything is getting canceled */
			canceled = chain->req_list;
			chain->req_list = NULL;
		}

		/* Actually cancel each request */
		for (l = canceled; l; l = l->next) {
			gprs_filter_request_cancel(l->data);
		}

		/* And deallocate them */
		g_slist_free_full(canceled, gprs_filter_request_free1);
	}
}

void __ofono_gprs_filter_chain_activate(struct gprs_filter_chain *chain,
		struct ofono_gprs_context *gc,
		const struct ofono_gprs_primary_context *ctx,
		gprs_filter_activate_cb_t cb, ofono_destroy_func destroy,
		void *user_data)
{
	if (chain && gprs_filter_list && ctx && cb) {
		gprs_filter_request_process
			(gprs_filter_request_activate_new(chain, gc, ctx,
						cb, destroy, user_data));
	} else {
		if (cb) {
			cb(ctx, user_data);
		}
		if (destroy) {
			destroy(user_data);
		}
	}
}

void __ofono_gprs_filter_chain_check(struct gprs_filter_chain *chain,
		gprs_filter_check_cb_t cb, ofono_destroy_func destroy,
		void *user_data)
{
	if (chain && gprs_filter_list && cb) {
		gprs_filter_request_process
			(gprs_filter_request_check_new(chain, cb, destroy,
								user_data));
	} else {
		if (cb) {
			cb(TRUE, user_data);
		}
		if (destroy) {
			destroy(user_data);
		}
	}
}

/*==========================================================================*
 * ofono_gprs_filter
 *==========================================================================*/

/**
 * Returns 0 if both are equal;
 * <0 if a comes before b;
 * >0 if a comes after b.
 */
static gint gprs_filter_sort(gconstpointer a, gconstpointer b)
{
	const struct ofono_gprs_filter *a_filter = a;
	const struct ofono_gprs_filter *b_filter = b;

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

int ofono_gprs_filter_register(const struct ofono_gprs_filter *filter)
{
	if (!filter || !filter->name) {
		return -EINVAL;
	}

	DBG("%s", filter->name);
	gprs_filter_list = g_slist_insert_sorted(gprs_filter_list,
					(void*)filter, gprs_filter_sort);
	return 0;
}

void ofono_gprs_filter_unregister(const struct ofono_gprs_filter *filter)
{
	if (filter) {
		DBG("%s", filter->name);
		gprs_filter_list = g_slist_remove(gprs_filter_list, filter);
	}
}

/*
 * Local Variables:
 * mode: C
 * c-basic-offset: 8
 * indent-tabs-mode: t
 * End:
 */
