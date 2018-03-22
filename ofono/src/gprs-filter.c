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

struct gprs_filter_request {
	struct gprs_filter_chain *chain;
	GSList *filter_link;
	guint pending_id;
	guint next_id;
	struct ofono_gprs_primary_context ctx;
	gprs_filter_activate_cb_t act;
	ofono_destroy_func destroy;
	void* user_data;
};

/* There's no need to support more than one request at a time */

struct gprs_filter_chain {
	struct ofono_gprs_context *gc;
	struct gprs_filter_request *req;
};

static GSList *gprs_filter_list = NULL;

static void gprs_filter_request_process(struct gprs_filter_request *req);

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

static struct gprs_filter_request *gprs_filter_request_new
	(struct gprs_filter_chain *chain,
		const struct ofono_gprs_primary_context *ctx,
				gprs_filter_activate_cb_t act,
				ofono_destroy_func destroy, void *user_data)
{
	struct gprs_filter_request *req = g_new0(struct gprs_filter_request, 1);

	req->chain = chain;
	req->filter_link = gprs_filter_list;
	gprs_filter_copy_context(&req->ctx, ctx);
	req->act = act;
	req->destroy = destroy;
	req->user_data = user_data;
	return req;
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

static void gprs_filter_request_free(struct gprs_filter_request *req)
{
	if (req->destroy) {
		req->destroy(req->user_data);
	}
	g_free(req);
}

static void gprs_filter_request_complete(struct gprs_filter_request *req,
							gboolean allow)
{
	req->chain->req = NULL;
	gprs_filter_request_cancel(req);
	req->act(allow ? &req->ctx : NULL, req->user_data);
	gprs_filter_request_free(req);
}

static void gprs_filter_request_next(struct gprs_filter_request *req,
							GSourceFunc fn)
{
	req->pending_id = 0;
	req->next_id = g_idle_add(fn, req);
}

static gboolean gprs_filter_continue_cb(gpointer data)
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

static gboolean gprs_filter_cancel_cb(gpointer data)
{
	struct gprs_filter_request *req = data;

	req->next_id = 0;
	gprs_filter_request_complete(req, FALSE);
	return G_SOURCE_REMOVE;
}

static void gprs_filter_activate_cb
		(const struct ofono_gprs_primary_context *ctx, void *data)
{
	struct gprs_filter_request *req = data;
	const struct ofono_gprs_filter *filter = req->filter_link->data;

	if (ctx) {
		if (ctx != &req->ctx) {
			/* The filter may have updated context settings */
			gprs_filter_copy_context(&req->ctx, ctx);
		}
		gprs_filter_request_next(req, gprs_filter_continue_cb);
	} else {
		DBG("%s not allowing to activate mobile data", filter->name);
		gprs_filter_request_next(req, gprs_filter_cancel_cb);
	}
}

static void gprs_filter_request_process(struct gprs_filter_request *req)
{
	GSList *l = req->filter_link;
	const struct ofono_gprs_filter *f = l->data;

	while (f && !f->filter_activate) {
		l = l->next;
		f = l ? l->data : NULL;
	}

	if (f) {
		guint id;

		req->filter_link = l;
		id = f->filter_activate(req->chain->gc, &req->ctx,
						gprs_filter_activate_cb, req);
		if (id) {
			/*
			 * If f->filter_activate returns zero, the request
			 * may have already been deallocated. It's only
			 * guaranteed to be alive if f->filter_activate
			 * returns non-zero id.
			 */
			req->pending_id = id;
		}
	} else {
		gprs_filter_request_complete(req, TRUE);
	}
}

void __ofono_gprs_filter_chain_activate(struct gprs_filter_chain *chain,
		const struct ofono_gprs_primary_context *ctx,
		gprs_filter_activate_cb_t act, ofono_destroy_func destroy,
		void *user_data)
{
	if (chain && gprs_filter_list && ctx && act) {
		if (!chain->req) {
			chain->req = gprs_filter_request_new(chain, ctx,
						act, destroy, user_data);
			gprs_filter_request_process(chain->req);
			return;
		} else {
			/*
			 * This shouldn't be happening - ofono core
			 * makes sure that the next context activation
			 * request is not submitted until the previous
			 * has completed.
			 */
			ctx = NULL;
		}
	}
	if (act) {
		act(ctx, user_data);
	}
	if (destroy) {
		destroy(user_data);
	}
}

struct gprs_filter_chain *__ofono_gprs_filter_chain_new
					(struct ofono_gprs_context *gc)
{
	struct gprs_filter_chain *chain = NULL;

	if (gc) {
		chain = g_new0(struct gprs_filter_chain, 1);
		chain->gc = gc;
	}
	return chain;
}

void __ofono_gprs_filter_chain_free(struct gprs_filter_chain *chain)
{
	if (chain) {
		if (chain->req) {
			gprs_filter_request_complete(chain->req, TRUE);
		}
		g_free(chain);
	}
}

void __ofono_gprs_filter_chain_cancel(struct gprs_filter_chain *chain)
{
	if (chain && chain->req) {
		gprs_filter_request_cancel(chain->req);
		gprs_filter_request_free(chain->req);
		chain->req = NULL;
	}
}

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
