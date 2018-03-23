/*
 *  oFono - Open Source Telephony
 *
 *  Copyright (C) 2017 Jolla Ltd. All rights reserved.
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

#include "dbus-queue.h"

#include <gdbus.h>

#include "ofono.h"

struct ofono_dbus_queue {
	struct ofono_dbus_queue_request *requests;
};

struct ofono_dbus_queue_request {
	struct ofono_dbus_queue_request *next;
	ofono_dbus_cb_t fn;
	DBusMessage *msg;
	void *data;
};

struct ofono_dbus_queue *__ofono_dbus_queue_new()
{
	return g_new0(struct ofono_dbus_queue, 1);
}

static struct ofono_dbus_queue_request *__ofono_dbus_queue_req_new
			(ofono_dbus_cb_t fn, DBusMessage *msg, void *data)
{
	struct ofono_dbus_queue_request *req =
		g_slice_new0(struct ofono_dbus_queue_request);

	req->msg = dbus_message_ref(msg);
	req->data = data;
	req->fn = fn;
	return req;
}

static void __ofono_dbus_queue_req_free(struct ofono_dbus_queue_request *req)
{
	g_slice_free1(sizeof(*req), req);
}

static void __ofono_dbus_queue_req_complete
				(struct ofono_dbus_queue_request *req,
					ofono_dbus_cb_t fn, void *param)
{
	DBusMessage *reply = fn ? fn(req->msg, param) : NULL;

	if (!reply)
		reply = __ofono_error_failed(req->msg);

	__ofono_dbus_pending_reply(&req->msg, reply);
	__ofono_dbus_queue_req_free(req);
}

void __ofono_dbus_queue_free(struct ofono_dbus_queue *q)
{
	if (q) {
		while (q->requests) {
			struct ofono_dbus_queue_request *req = q->requests;
			DBusMessage *reply = __ofono_error_canceled(req->msg);

			__ofono_dbus_pending_reply(&req->msg, reply);
			q->requests = req->next;
			__ofono_dbus_queue_req_free(req);
		}

		g_free(q);
	}
}

ofono_bool_t __ofono_dbus_queue_pending(struct ofono_dbus_queue *q)
{
	return q && q->requests;
}

ofono_bool_t __ofono_dbus_queue_set_pending(struct ofono_dbus_queue *q,
						DBusMessage *msg)
{
	if (!q || q->requests)
		return FALSE;

	q->requests = __ofono_dbus_queue_req_new(NULL, msg, NULL);
	return TRUE;
}

void __ofono_dbus_queue_request(struct ofono_dbus_queue *q,
			ofono_dbus_cb_t fn, DBusMessage *msg, void *data)
{
	struct ofono_dbus_queue_request *req =
		__ofono_dbus_queue_req_new(fn, msg, data);

	if (q->requests) {
		struct ofono_dbus_queue_request *prev = q->requests;

		while (prev->next)
			prev = prev->next;

		prev->next = req;
	} else {
		DBusMessage *reply;

		q->requests = req;
		reply = req->fn(req->msg, req->data);
		if (reply) {
			/* The request has completed synchronously */
			__ofono_dbus_queue_reply_msg(q, reply);
		}
	}
}

/* Consumes one reference to the reply */
void __ofono_dbus_queue_reply_msg(struct ofono_dbus_queue *q,
							DBusMessage *reply)
{
	struct ofono_dbus_queue_request *done, *next;

	if (!q || !q->requests) {
		/* This should never happen */
		if (reply) {
			dbus_message_unref(reply);
		}
		return;
	}

	/* De-queue one request */
	done = q->requests;
	next = done->next;
	q->requests = next;
	done->next = NULL;

	/* Interpret NULL reply as a cancel */
	if (!reply)
		reply = __ofono_error_canceled(done->msg);

	/* Send the reply */
	__ofono_dbus_pending_reply(&done->msg, reply);
	__ofono_dbus_queue_req_free(done);

	/* Submit the next request if there is any */
	while (next && reply) {
		reply = next->fn(next->msg, next->data);
		if (reply) {
			/* The request has completed synchronously */
			done = next;
			next = done->next;
			q->requests = next;
			done->next = NULL;

			/* Send the reply */
			__ofono_dbus_pending_reply(&done->msg, reply);
			__ofono_dbus_queue_req_free(done);
		}
	}
}

void __ofono_dbus_queue_reply_ok(struct ofono_dbus_queue *q)
{
	__ofono_dbus_queue_reply_fn(q, dbus_message_new_method_return);
}

void __ofono_dbus_queue_reply_failed(struct ofono_dbus_queue *q)
{
	__ofono_dbus_queue_reply_fn(q, __ofono_error_failed);
}

void __ofono_dbus_queue_reply_fn(struct ofono_dbus_queue *q,
						ofono_dbus_reply_cb_t fn)
{
	if (q && q->requests)
		__ofono_dbus_queue_reply_msg(q, fn(q->requests->msg));
}

void __ofono_dbus_queue_reply_all_ok(struct ofono_dbus_queue *q)
{
	__ofono_dbus_queue_reply_all_fn(q, dbus_message_new_method_return);
}

void __ofono_dbus_queue_reply_all_failed(struct ofono_dbus_queue *q)
{
	__ofono_dbus_queue_reply_all_fn(q, __ofono_error_failed);
}

static DBusMessage * __ofono_dbus_queue_reply_all_wrapper(DBusMessage *msg,
								void *data)
{
	 return ((ofono_dbus_reply_cb_t)data)(msg);
}

void __ofono_dbus_queue_reply_all_fn(struct ofono_dbus_queue *q,
						ofono_dbus_reply_cb_t fn)
{
	__ofono_dbus_queue_reply_all_fn_param(q,
				__ofono_dbus_queue_reply_all_wrapper,
				fn ? fn : __ofono_error_failed);
}

void __ofono_dbus_queue_reply_all_fn_param(struct ofono_dbus_queue *q,
					ofono_dbus_cb_t fn, void *param)
{
	struct ofono_dbus_queue_request *prev, *req;
	ofono_dbus_cb_t handler;
	void *data;

	if (!q || !q->requests)
		return;

	/* Store handler and data so that we can compare against them */
	req = q->requests;
	handler = req->fn;
	data = req->data;

	/* De-queue the first request */
	q->requests = req->next;
	req->next = NULL;

	/* Send the reply and free the request */
	__ofono_dbus_queue_req_complete(req, fn, param);

	/*
	 * Find all other requests with the same handler and the same data
	 * and complete those too (except when the handler is NULL)
	 */
	if (!handler)
		return;

	prev = NULL;
	req = q->requests;
	while (req) {
		struct ofono_dbus_queue_request *next = req->next;

		if (req->fn == handler && req->data == data) {
			/* Found a match */
			if (prev) {
				prev->next = next;
			} else {
				q->requests = next;
			}

			__ofono_dbus_queue_req_complete(req, fn, param);
		} else {
			/* Keep this one */
			prev = req;
		}
		
		req = next;
	}
}
