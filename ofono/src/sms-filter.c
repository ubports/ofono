/*
 *  oFono - Open Source Telephony
 *
 *  Copyright (C) 2017 Jolla Ltd.
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

#include "smsutil.h"

#define CAST(address,type,field) \
    ((type *)((guint8*)(address) - G_STRUCT_OFFSET(type,field)))

/* We don't convert enums, assert that they match each other */
#define ASSERT_ENUM_(x) G_STATIC_ASSERT((int)x == (int)OFONO_##x)

/* ofono_sms_number_type vs sms_number_type */
ASSERT_ENUM_(SMS_NUMBER_TYPE_UNKNOWN);
ASSERT_ENUM_(SMS_NUMBER_TYPE_INTERNATIONAL);
ASSERT_ENUM_(SMS_NUMBER_TYPE_NATIONAL);
ASSERT_ENUM_(SMS_NUMBER_TYPE_NETWORK_SPECIFIC);
ASSERT_ENUM_(SMS_NUMBER_TYPE_SUBSCRIBER);
ASSERT_ENUM_(SMS_NUMBER_TYPE_ALPHANUMERIC);
ASSERT_ENUM_(SMS_NUMBER_TYPE_ABBREVIATED);
ASSERT_ENUM_(SMS_NUMBER_TYPE_RESERVED);

/* ofono_sms_numbering_plan vs sms_numbering_plan */
ASSERT_ENUM_(SMS_NUMBERING_PLAN_UNKNOWN);
ASSERT_ENUM_(SMS_NUMBERING_PLAN_ISDN);
ASSERT_ENUM_(SMS_NUMBERING_PLAN_DATA);
ASSERT_ENUM_(SMS_NUMBERING_PLAN_TELEX);
ASSERT_ENUM_(SMS_NUMBERING_PLAN_SC1);
ASSERT_ENUM_(SMS_NUMBERING_PLAN_SC2);
ASSERT_ENUM_(SMS_NUMBERING_PLAN_NATIONAL);
ASSERT_ENUM_(SMS_NUMBERING_PLAN_PRIVATE);
ASSERT_ENUM_(SMS_NUMBERING_PLAN_ERMES);
ASSERT_ENUM_(SMS_NUMBERING_PLAN_RESERVED);

/* ofono_sms_class vs sms_class */
ASSERT_ENUM_(SMS_CLASS_0);
ASSERT_ENUM_(SMS_CLASS_1);
ASSERT_ENUM_(SMS_CLASS_2);
ASSERT_ENUM_(SMS_CLASS_3);
ASSERT_ENUM_(SMS_CLASS_UNSPECIFIED);

struct sms_filter_message;
struct sms_filter_message_fn {
	const char *name;
	gboolean (*can_process)(const struct ofono_sms_filter *filter);
	guint (*process)(const struct ofono_sms_filter *filter,
					struct sms_filter_message *msg);
	void (*passthrough)(struct sms_filter_message *msg);
	void (*destroy)(struct sms_filter_message *msg);
	void (*free)(struct sms_filter_message *msg);
};

struct sms_filter_message {
	int refcount;
	gboolean destroyed;
	const struct sms_filter_message_fn *fn;
	struct sms_filter_chain *chain;
	GSList *filter_link;
	guint pending_id;
	guint continue_id;
};

struct sms_filter_chain_send_text {
	struct sms_filter_message message;
	sms_send_text_cb_t send;
	ofono_destroy_func destroy;
	void *data;
	char *text;
	struct ofono_sms_address addr;
};

struct sms_filter_chain_send_datagram {
	struct sms_filter_message message;
	sms_send_datagram_cb_t send;
	ofono_destroy_func destroy;
	void *data;
	int dst_port;
	int src_port;
	unsigned char *bytes;
	unsigned int len;
	int flags;
	struct ofono_sms_address addr;
};

struct sms_filter_chain_recv_text {
	struct sms_filter_message message;
	sms_dispatch_recv_text_cb_t default_handler;
	struct ofono_uuid uuid;
	char *text;
	enum ofono_sms_class cls;
	struct ofono_sms_address addr;
	struct ofono_sms_scts scts;
};

struct sms_filter_chain_recv_datagram {
	struct sms_filter_message message;
	sms_dispatch_recv_datagram_cb_t default_handler;
	struct ofono_uuid uuid;
	int dst_port;
	int src_port;
	unsigned char *buf;
	unsigned int len;
	struct ofono_sms_address addr;
	struct ofono_sms_scts scts;
};

struct sms_filter_chain {
	struct ofono_sms *sms;
	struct ofono_modem *modem;
	GSList *msg_list;
};

static GSList *sms_filter_list = NULL;

static void sms_filter_convert_sms_address(struct ofono_sms_address *dest,
						const struct sms_address *src)
{
	dest->number_type = (enum ofono_sms_number_type)src->number_type;
	dest->numbering_plan = (enum ofono_sms_numbering_plan)
		src->numbering_plan;
	strncpy(dest->address, src->address, sizeof(dest->address));
};

static void sms_filter_convert_sms_address_back(struct sms_address *dest,
					const struct ofono_sms_address *src)
{
	dest->number_type = (enum sms_number_type)src->number_type;
	dest->numbering_plan = (enum sms_numbering_plan)src->numbering_plan;
	strncpy(dest->address, src->address, sizeof(dest->address));
};

static void sms_filter_convert_sms_scts(struct ofono_sms_scts *dest,
						const struct sms_scts *src)
{
	dest->year = src->year;
	dest->month = src->month;
	dest->day = src->day;
	dest->hour = src->hour;
	dest->minute = src->minute;
	dest->second = src->second;
	dest->has_timezone = src->has_timezone;
	dest->timezone = src->timezone;
}

static void sms_filter_convert_sms_scts_back(struct sms_scts *dest,
					const struct ofono_sms_scts *src)
{
	dest->year = src->year;
	dest->month = src->month;
	dest->day = src->day;
	dest->hour = src->hour;
	dest->minute = src->minute;
	dest->second = src->second;
	dest->has_timezone = src->has_timezone;
	dest->timezone = src->timezone;
}

static void sms_filter_message_init(struct sms_filter_message *msg,
	struct sms_filter_chain *chain, const struct sms_filter_message_fn *fn)
{
	/* The caller has zeroed the structure for us */
	msg->fn = fn;
	msg->chain = chain;
	msg->filter_link = sms_filter_list;

	/*
	 * The list holds an implicit reference to the message. The reference
	 * is released by sms_filter_message_free when the message is removed
	 * from the list.
	 */
	msg->refcount = 1;
	chain->msg_list = g_slist_append(chain->msg_list, msg);
}

static void sms_filter_message_process(struct sms_filter_message *msg)
{
	GSList *filter_link = msg->filter_link;
	const struct ofono_sms_filter *filter = filter_link->data;
	const struct sms_filter_message_fn *fn = msg->fn;

	while (filter && !fn->can_process(filter)) {
		filter_link = filter_link->next;
		filter = filter_link ? filter_link->data : NULL;
	}

	if (filter) {
		guint id;

		/*
		 * If fn->process returns zero, the message may have
		 * already been deallocated. It's only guaranteed to
		 * be alive if fn->process returns non-zero id.
		 */
		msg->filter_link = filter_link;
		id = fn->process(filter, msg);
		if (id) {
			msg->pending_id = id;
		}
	} else {
		fn->passthrough(msg);
	}
}

static void sms_filter_message_destroy(struct sms_filter_message *msg)
{
	/*
	 * It's ok to call this function several times for one message.
	 * And it could be called twice if the callback deletes the
	 * filter chain. The reference count makes sure that we don't
	 * deallocate it more than once.
	 */
	if (msg->pending_id) {
		const struct ofono_sms_filter *filter = msg->filter_link->data;
		guint id = msg->pending_id;

		msg->pending_id = 0;
		filter->cancel(id);
	}
	if (msg->continue_id) {
		g_source_remove(msg->continue_id);
		msg->continue_id = 0;
	}
	if (!msg->destroyed) {
		const struct sms_filter_message_fn *fn = msg->fn;

		msg->destroyed = TRUE;
		if (fn->destroy) {
			fn->destroy(msg);
		}
	}
}

static int sms_filter_message_unref(struct sms_filter_message *msg)
{
	const int refcount = --(msg->refcount);

	if (!refcount) {
		sms_filter_message_destroy(msg);
		msg->fn->free(msg);
	}
	return refcount;
}

static void sms_filter_message_free1(gpointer data)
{
	struct sms_filter_message *msg = data;

	/*
	 * This is a g_slist_free_full() callback for use by
	 * __ofono_sms_filter_chain_free(), so we know that the
	 * message is (was) on the list and therefore we have to
	 * release the reference. Also, make sure that the message
	 * is destroyed even if we are not releasing the last reference.
	 */
	if (sms_filter_message_unref(msg)) {
		sms_filter_message_destroy(msg);
		/* The chain is no more */
		msg->chain = NULL;
	}
}

static void sms_filter_message_free(struct sms_filter_message *msg)
{
	struct sms_filter_chain *chain = msg->chain;

	/*
	 * Single-linked list is not particularly good at searching
	 * and removing the elements but since it should be pretty
	 * short (typically just one message), it's not worth optimization.
	 */
	if (chain && g_slist_find(chain->msg_list, msg)) {
		chain->msg_list = g_slist_remove(chain->msg_list, msg);
		/*
		 * The message has to be destroyed even if we are not
		 * releasing the last reference.
		 */
		if (sms_filter_message_unref(msg)) {
			sms_filter_message_destroy(msg);
		}
	}
}

static void sms_filter_message_next(struct sms_filter_message *msg,
							GSourceFunc fn)
{
	msg->pending_id = 0;
	msg->continue_id = g_idle_add(fn, msg);
}

static gboolean sms_filter_message_continue(gpointer data)
{
	struct sms_filter_message *msg = data;
	const struct sms_filter_message_fn *fn = msg->fn;

	msg->continue_id = 0;
	msg->filter_link = msg->filter_link->next;
	if (msg->filter_link) {
		sms_filter_message_process(msg);
	} else {
		msg->refcount++;
		fn->passthrough(msg);
		sms_filter_message_free(msg);
		sms_filter_message_unref(msg);
	}
	return G_SOURCE_REMOVE;
}

static gboolean sms_filter_message_drop(gpointer data)
{
	struct sms_filter_message *msg = data;

	msg->continue_id = 0;
	sms_filter_message_free(msg);
	return G_SOURCE_REMOVE;
}

static void sms_filter_message_processed(struct sms_filter_message *msg,
					enum ofono_sms_filter_result result)
{
	const struct ofono_sms_filter *filter = msg->filter_link->data;

	switch (result) {
	case OFONO_SMS_FILTER_DROP:
		DBG("%s dropping %s", filter->name, msg->fn->name);
		sms_filter_message_next(msg, sms_filter_message_drop);
		break;
	default:
		DBG("unexpected result %d from %s", result, filter->name);
		/* fall through */
	case OFONO_SMS_FILTER_CONTINUE:
		sms_filter_message_next(msg, sms_filter_message_continue);
		break;
	}
}

/* sms_filter_chain_send_text */

static inline struct sms_filter_chain_send_text *sms_filter_chain_send_text_cast
					(struct sms_filter_message *msg)
{
	return CAST(msg, struct sms_filter_chain_send_text, message);
}

static gboolean sms_filter_chain_send_text_can_process
				(const struct ofono_sms_filter *filter)
{
	return filter->filter_send_text != NULL;
}

static void sms_filter_chain_send_text_process_cb
				(enum ofono_sms_filter_result res,
					const struct ofono_sms_address *addr,
					const char *text, void *data)
{
	struct sms_filter_chain_send_text *msg = data;

	if (res != OFONO_SMS_FILTER_DROP) {
		/* Update the message */
		if (&msg->addr != addr) {
			msg->addr = *addr;
		}
		if (msg->text != text) {
			g_free(msg->text);
			msg->text = g_strdup(text);
		}
	}

	sms_filter_message_processed(&msg->message, res);
}

static guint sms_filter_chain_send_text_process
				(const struct ofono_sms_filter *filter,
					struct sms_filter_message *msg)
{
	struct sms_filter_chain_send_text *send_msg =
		sms_filter_chain_send_text_cast(msg);
	struct sms_filter_chain *chain = msg->chain;

	return filter->filter_send_text(chain->modem, &send_msg->addr,
			send_msg->text, sms_filter_chain_send_text_process_cb,
			send_msg);
}

static void sms_filter_chain_send_text_passthrough
					(struct sms_filter_message *msg)
{
	struct sms_filter_chain_send_text *send_msg =
		sms_filter_chain_send_text_cast(msg);

	if (send_msg->send) {
		struct sms_filter_chain *chain = msg->chain;
		struct sms_address addr;

		sms_filter_convert_sms_address_back(&addr, &send_msg->addr);
		send_msg->send(chain->sms, &addr, send_msg->text,
							send_msg->data);
	}
}

static void sms_filter_chain_send_text_destroy(struct sms_filter_message *msg)
{
	struct sms_filter_chain_send_text *send_msg =
		sms_filter_chain_send_text_cast(msg);

	if (send_msg->destroy) {
		send_msg->destroy(send_msg->data);
	}
}

static void sms_filter_chain_send_text_free(struct sms_filter_message *msg)
{
	struct sms_filter_chain_send_text *send_msg =
		sms_filter_chain_send_text_cast(msg);

	g_free(send_msg->text);
	g_free(send_msg);
}

static struct sms_filter_message *sms_filter_send_text_new
	(struct sms_filter_chain *chain, const struct sms_address *addr,
		const char *text, sms_send_text_cb_t send,
		void *data, ofono_destroy_func destroy)
{
	static const struct sms_filter_message_fn send_text_fn = {
		.name = "outgoing SMS text message",
		.can_process = sms_filter_chain_send_text_can_process,
		.process = sms_filter_chain_send_text_process,
		.passthrough = sms_filter_chain_send_text_passthrough,
		.destroy = sms_filter_chain_send_text_destroy,
		.free = sms_filter_chain_send_text_free
	};

	struct sms_filter_chain_send_text *send_msg =
		g_new0(struct sms_filter_chain_send_text, 1);

	sms_filter_message_init(&send_msg->message, chain, &send_text_fn);
	sms_filter_convert_sms_address(&send_msg->addr, addr);
	send_msg->send = send;
	send_msg->destroy = destroy;
	send_msg->data = data;
	send_msg->text = g_strdup(text);
	return &send_msg->message;
}

/* sms_filter_chain_send_datagram */

static inline struct sms_filter_chain_send_datagram
				*sms_filter_chain_send_datagram_cast
					(struct sms_filter_message *msg)
{
	return CAST(msg, struct sms_filter_chain_send_datagram, message);
}

static gboolean sms_filter_chain_send_datagram_can_process
				(const struct ofono_sms_filter *filter)
{
	return filter->filter_send_datagram != NULL;
}

static void sms_datagram_set_bytes(
				struct sms_filter_chain_send_datagram *msg,
				const unsigned char *bytes, unsigned int len)
{
	msg->bytes = g_malloc0(sizeof(unsigned char) * len);
	memcpy(msg->bytes, bytes, len);
	msg->len = len;
}

static void sms_filter_chain_send_datagram_process_cb
				(enum ofono_sms_filter_result res,
					const struct ofono_sms_address *addr,
					int dst_port, int src_port,
					const unsigned char *bytes,
					unsigned int len, void *data)
{
	struct sms_filter_chain_send_datagram *msg = data;

	if (res != OFONO_SMS_FILTER_DROP) {
		/* Update the message */
		if (&msg->addr != addr) {
			msg->addr = *addr;
		}
		if (msg->bytes != bytes) {
			g_free(msg->bytes);
			sms_datagram_set_bytes(msg, bytes, len);
		}

		msg->dst_port = dst_port;
		msg->src_port = src_port;
	}

	sms_filter_message_processed(&msg->message, res);
}

static guint sms_filter_chain_send_datagram_process
				(const struct ofono_sms_filter *filter,
					struct sms_filter_message *msg)
{
	struct sms_filter_chain_send_datagram *send_msg =
		sms_filter_chain_send_datagram_cast(msg);
	struct sms_filter_chain *chain = msg->chain;

	return filter->filter_send_datagram(chain->modem, &send_msg->addr,
				send_msg->dst_port, send_msg->src_port,
				send_msg->bytes, send_msg->len,
				sms_filter_chain_send_datagram_process_cb,
				send_msg);
}

static void sms_filter_chain_send_datagram_passthrough
					(struct sms_filter_message *msg)
{
	struct sms_filter_chain_send_datagram *send_msg =
		sms_filter_chain_send_datagram_cast(msg);

	if (send_msg->send) {
		struct sms_filter_chain *chain = msg->chain;
		struct sms_address addr;

		sms_filter_convert_sms_address_back(&addr, &send_msg->addr);
		send_msg->send(chain->sms, &addr, send_msg->dst_port,
					send_msg->src_port, send_msg->bytes,
					send_msg->len, send_msg->flags,
					send_msg->data);
	}
}

static void sms_filter_chain_send_datagram_destroy
					(struct sms_filter_message *msg)
{
	struct sms_filter_chain_send_datagram *send_msg =
		sms_filter_chain_send_datagram_cast(msg);

	if (send_msg->destroy) {
		send_msg->destroy(send_msg->data);
	}
}

static void sms_filter_chain_send_datagram_free
					(struct sms_filter_message *msg)
{
	struct sms_filter_chain_send_datagram *send_msg =
		sms_filter_chain_send_datagram_cast(msg);

	g_free(send_msg->bytes);
	g_free(send_msg);
}

static struct sms_filter_message *sms_filter_send_datagram_new
	(struct sms_filter_chain *chain, const struct sms_address *addr,
		int dst_port, int src_port, unsigned char *bytes,
		unsigned int len, int flags, sms_send_datagram_cb_t send,
		void *data, ofono_destroy_func destroy)
{
	static const struct sms_filter_message_fn send_datagram_fn = {
		.name = "outgoing SMS data message",
		.can_process = sms_filter_chain_send_datagram_can_process,
		.process = sms_filter_chain_send_datagram_process,
		.passthrough = sms_filter_chain_send_datagram_passthrough,
		.destroy = sms_filter_chain_send_datagram_destroy,
		.free = sms_filter_chain_send_datagram_free
	};

	struct sms_filter_chain_send_datagram *send_msg =
		g_new0(struct sms_filter_chain_send_datagram, 1);

	sms_filter_message_init(&send_msg->message, chain, &send_datagram_fn);
	sms_filter_convert_sms_address(&send_msg->addr, addr);
	send_msg->send = send;
	send_msg->destroy = destroy;
	send_msg->data = data;
	sms_datagram_set_bytes(send_msg, bytes, len);
	send_msg->dst_port = dst_port;
	send_msg->src_port = src_port;
	send_msg->flags = flags;
	return &send_msg->message;
}

/* sms_filter_chain_recv_text */

static inline struct sms_filter_chain_recv_text *
	sms_filter_chain_recv_text_cast(struct sms_filter_message *msg)
{
	return CAST(msg, struct sms_filter_chain_recv_text, message);
}

static gboolean sms_filter_chain_recv_text_can_process
				(const struct ofono_sms_filter *filter)
{
	return filter->filter_recv_text != NULL;
}

static void sms_filter_chain_recv_text_process_cb
	(enum ofono_sms_filter_result res, const struct ofono_uuid *uuid,
			const char *text, enum ofono_sms_class cls,
			const struct ofono_sms_address *addr,
			const struct ofono_sms_scts *scts, void *data)
{
	struct sms_filter_chain_recv_text *msg = data;

	if (res != OFONO_SMS_FILTER_DROP) {
		/* Update the message */
		if (&msg->uuid != uuid) {
			msg->uuid = *uuid;
		}
		if (msg->text != text) {
			g_free(msg->text);
			msg->text = g_strdup(text);
		}
		msg->cls = cls;
		if (&msg->addr != addr) {
			msg->addr = *addr;
		}
		if (&msg->scts != scts) {
			msg->scts = *scts;
		}
	}

	sms_filter_message_processed(&msg->message, res);
}

static guint sms_filter_chain_recv_text_process
				(const struct ofono_sms_filter *filter,
					struct sms_filter_message *msg)
{
	struct sms_filter_chain_recv_text *recv_msg =
		sms_filter_chain_recv_text_cast(msg);
	struct sms_filter_chain *chain = msg->chain;

	return filter->filter_recv_text(chain->modem, &recv_msg->uuid,
			recv_msg->text, recv_msg->cls, &recv_msg->addr,
			&recv_msg->scts, sms_filter_chain_recv_text_process_cb,
			recv_msg);
}

static void sms_filter_chain_recv_text_passthrough
					(struct sms_filter_message *msg)
{
	struct sms_filter_chain_recv_text *recv_msg =
		sms_filter_chain_recv_text_cast(msg);

	if (recv_msg->default_handler) {
		struct sms_filter_chain *chain = msg->chain;
		struct sms_address addr;
		struct sms_scts scts;

		sms_filter_convert_sms_address_back(&addr, &recv_msg->addr);
		sms_filter_convert_sms_scts_back(&scts, &recv_msg->scts);
		recv_msg->default_handler(chain->sms, &recv_msg->uuid,
				recv_msg->text, recv_msg->cls, &addr, &scts);
	}
}

static void sms_filter_chain_recv_text_free(struct sms_filter_message *msg)
{
	struct sms_filter_chain_recv_text *recv_msg =
		sms_filter_chain_recv_text_cast(msg);

	g_free(recv_msg->text);
	g_free(recv_msg);
}

static struct sms_filter_message *sms_filter_chain_recv_text_new
	(struct sms_filter_chain *chain, const struct ofono_uuid *uuid,
		char *text, enum sms_class cls,
		const struct sms_address *addr, const struct sms_scts *scts,
		sms_dispatch_recv_text_cb_t default_handler)
{
	static const struct sms_filter_message_fn recv_text_fn = {
		.name = "incoming SMS text message",
		.can_process = sms_filter_chain_recv_text_can_process,
		.process = sms_filter_chain_recv_text_process,
		.passthrough = sms_filter_chain_recv_text_passthrough,
		.free = sms_filter_chain_recv_text_free
	};

	struct sms_filter_chain_recv_text *recv_msg =
		g_new0(struct sms_filter_chain_recv_text, 1);

	sms_filter_message_init(&recv_msg->message, chain, &recv_text_fn);
	sms_filter_convert_sms_address(&recv_msg->addr, addr);
	sms_filter_convert_sms_scts(&recv_msg->scts, scts);
	recv_msg->default_handler = default_handler;
	recv_msg->uuid = *uuid;
	recv_msg->text = text;
	recv_msg->cls = (enum ofono_sms_class)cls;
	return &recv_msg->message;
}

/* sms_filter_send_datagram */

static inline struct sms_filter_chain_recv_datagram *
	sms_filter_chain_recv_datagram_cast(struct sms_filter_message *msg)
{
	return CAST(msg, struct sms_filter_chain_recv_datagram, message);
}

static gboolean sms_filter_chain_recv_datagram_can_process
				(const struct ofono_sms_filter *filter)
{
	return filter->filter_recv_datagram != NULL;
}

static void sms_filter_chain_recv_datagram_process_cb
	(enum ofono_sms_filter_result result, const struct ofono_uuid *uuid,
		int dst_port, int src_port, const unsigned char *buf,
		unsigned int len, const struct ofono_sms_address *addr,
		const struct ofono_sms_scts *scts, void *data)
{
	struct sms_filter_chain_recv_datagram *dg = data;

	if (result != OFONO_SMS_FILTER_DROP) {
		/* Update the datagram */
		if (&dg->uuid != uuid) {
			dg->uuid = *uuid;
		}
		dg->dst_port = dst_port;
		dg->src_port = src_port;
		dg->len = len;
		if (dg->buf != buf) {
			g_free(dg->buf);
			dg->buf = g_memdup(buf, len);
		}
		if (&dg->addr != addr) {
			dg->addr = *addr;
		}
		if (&dg->scts != scts) {
			dg->scts = *scts;
		}
	}

	sms_filter_message_processed(&dg->message, result);
}

static guint sms_filter_chain_recv_datagram_process
				(const struct ofono_sms_filter *filter,
					struct sms_filter_message *msg)
{
	struct sms_filter_chain *chain = msg->chain;
	struct sms_filter_chain_recv_datagram *recv_dg =
		sms_filter_chain_recv_datagram_cast(msg);

	return filter->filter_recv_datagram(chain->modem, &recv_dg->uuid,
			recv_dg->dst_port, recv_dg->src_port, recv_dg->buf,
			recv_dg->len, &recv_dg->addr, &recv_dg->scts,
			sms_filter_chain_recv_datagram_process_cb, recv_dg);
}

static void sms_filter_chain_recv_datagram_passthrough
					(struct sms_filter_message *msg)
{
	struct sms_filter_chain_recv_datagram *recv_dg =
		sms_filter_chain_recv_datagram_cast(msg);

	if (recv_dg->default_handler) {
		struct sms_filter_chain *chain = msg->chain;
		struct sms_address addr;
		struct sms_scts scts;

		sms_filter_convert_sms_address_back(&addr, &recv_dg->addr);
		sms_filter_convert_sms_scts_back(&scts, &recv_dg->scts);
		recv_dg->default_handler(chain->sms, &recv_dg->uuid,
				recv_dg->dst_port, recv_dg->src_port,
				recv_dg->buf, recv_dg->len, &addr, &scts);
	}
}

static void sms_filter_chain_recv_datagram_free(struct sms_filter_message *msg)
{
	struct sms_filter_chain_recv_datagram *recv_dg =
		sms_filter_chain_recv_datagram_cast(msg);

	g_free(recv_dg->buf);
	g_free(recv_dg);
}

static struct sms_filter_message *sms_filter_chain_recv_datagram_new
	(struct sms_filter_chain *chain, const struct ofono_uuid *uuid,
		int dst, int src, unsigned char *buf, unsigned int len,
		const struct sms_address *addr, const struct sms_scts *scts,
		sms_dispatch_recv_datagram_cb_t default_handler)
{
	static const struct sms_filter_message_fn recv_datagram_fn = {
		.name = "incoming SMS datagram",
		.can_process = sms_filter_chain_recv_datagram_can_process,
		.process = sms_filter_chain_recv_datagram_process,
		.passthrough = sms_filter_chain_recv_datagram_passthrough,
		.free = sms_filter_chain_recv_datagram_free
	};

	struct sms_filter_chain_recv_datagram *recv_dg =
		g_new0(struct sms_filter_chain_recv_datagram, 1);

	sms_filter_message_init(&recv_dg->message, chain, &recv_datagram_fn);
	sms_filter_convert_sms_address(&recv_dg->addr, addr);
	sms_filter_convert_sms_scts(&recv_dg->scts, scts);
	recv_dg->default_handler = default_handler;
	recv_dg->uuid = *uuid;
	recv_dg->dst_port = dst;
	recv_dg->src_port = src;
	recv_dg->buf = buf;
	recv_dg->len = len;
	return &recv_dg->message;
}

struct sms_filter_chain *__ofono_sms_filter_chain_new(struct ofono_sms *sms,
						struct ofono_modem *modem)
{
	struct sms_filter_chain *chain = g_new0(struct sms_filter_chain, 1);

	chain->sms = sms;
	chain->modem = modem;
	return chain;
}

void __ofono_sms_filter_chain_free(struct sms_filter_chain *chain)
{
	if (chain) {
		g_slist_free_full(chain->msg_list, sms_filter_message_free1);
		g_free(chain);
	}
}

void __ofono_sms_filter_chain_send_text(struct sms_filter_chain *chain,
		const struct sms_address *addr, const char *text,
		sms_send_text_cb_t sender, ofono_destroy_func destroy,
		void *data)
{
	if (chain) {
		if (sms_filter_list) {
			sms_filter_message_process
				(sms_filter_send_text_new(chain, addr,
					text, sender, data, destroy));
			return;
		}
		if (sender) {
			sender(chain->sms, addr, text, data);
		}
	}
	if (destroy) {
		destroy(data);
	}
}

void __ofono_sms_filter_chain_send_datagram(struct sms_filter_chain *chain,
			const struct sms_address *addr, int dstport,
			int srcport, unsigned char *bytes, int len,
			int flags, sms_send_datagram_cb_t sender,
			ofono_destroy_func destroy, void *data)
{
	if (chain) {
		if (sms_filter_list) {
			sms_filter_message_process
				(sms_filter_send_datagram_new(chain, addr,
						dstport, srcport, bytes, len,
						flags, sender, data, destroy));
			return;
		}
		if (sender) {
			sender(chain->sms, addr, dstport, srcport, bytes, len,
								flags, data);
		}
	}
	if (destroy) {
		destroy(data);
	}
}

/* Does g_free(buf) when done */
void __ofono_sms_filter_chain_recv_datagram(struct sms_filter_chain *chain,
		const struct ofono_uuid *uuid, int dst_port, int src_port,
		unsigned char *buf, unsigned int len,
		const struct sms_address *addr, const struct sms_scts *scts,
		sms_dispatch_recv_datagram_cb_t default_handler)
{
	if (chain) {
		if (sms_filter_list) {
			sms_filter_message_process
				(sms_filter_chain_recv_datagram_new(chain,
					uuid, dst_port, src_port, buf, len,
					addr, scts, default_handler));
			return;
		}
		if (default_handler) {
			default_handler(chain->sms, uuid, dst_port,
					src_port, buf, len, addr, scts);
		}
	}
	g_free(buf);
}

/* Does g_free(message) when done */
void __ofono_sms_filter_chain_recv_text(struct sms_filter_chain *chain,
		const struct ofono_uuid *uuid, char *message,
		enum sms_class cls, const struct sms_address *addr,
		const struct sms_scts *scts,
		sms_dispatch_recv_text_cb_t default_handler)
{
	if (chain) {
		if (sms_filter_list) {
			sms_filter_message_process
				(sms_filter_chain_recv_text_new(chain,
					uuid, message, cls, addr, scts,
					default_handler));
			return;
		}
		if (default_handler) {
			default_handler(chain->sms, uuid, message,
						cls, addr, scts);
		}
	}
	g_free(message);
}

/**
 * Returns 0 if both are equal;
 * <0 if a comes before b;
 * >0 if a comes after b.
 */
static gint sms_filter_sort(gconstpointer a, gconstpointer b)
{
	const struct ofono_sms_filter *a_filter = a;
	const struct ofono_sms_filter *b_filter = b;

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

int ofono_sms_filter_register(const struct ofono_sms_filter *filter)
{
	if (!filter || !filter->name) {
		return -EINVAL;
	}

	DBG("%s", filter->name);
	sms_filter_list = g_slist_insert_sorted(sms_filter_list,
					(void*)filter, sms_filter_sort);
	return 0;
}

void ofono_sms_filter_unregister(const struct ofono_sms_filter *filter)
{
	if (filter) {
		DBG("%s", filter->name);
		sms_filter_list = g_slist_remove(sms_filter_list, filter);
	}
}

/*
 * Local Variables:
 * mode: C
 * c-basic-offset: 8
 * indent-tabs-mode: t
 * End:
 */
