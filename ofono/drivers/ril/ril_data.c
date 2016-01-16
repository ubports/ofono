/*
 *  oFono - Open Source Telephony - RIL-based devices
 *
 *  Copyright (C) 2016 Jolla Ltd.
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

#include "ril_data.h"
#include "ril_log.h"

#include <grilio_queue.h>
#include <grilio_request.h>

typedef GObjectClass RilDataClass;
typedef struct ril_data RilData;

struct ril_data_manager {
	gint ref_count;
	struct ril_data *selected;
	guint pending_id;
	GSList *data_list;
};

struct ril_data {
	GObject object;
	GRilIoQueue *q;
	const char *log_prefix;
	char *custom_log_prefix;
	struct ril_data_manager *dm;
	gboolean allowed;
};

enum ril_data_signal {
	SIGNAL_ALLOW_CHANGED,
	SIGNAL_COUNT
};

#define SIGNAL_ALLOW_CHANGED_NAME    "ril-data-allow-changed"

static guint ril_data_signals[SIGNAL_COUNT] = { 0 };

G_DEFINE_TYPE(RilData, ril_data, G_TYPE_OBJECT)
#define RIL_DATA_TYPE (ril_data_get_type())
#define RIL_DATA(obj) (G_TYPE_CHECK_INSTANCE_CAST(obj, RIL_DATA_TYPE,RilData))

static void ril_data_manager_check(struct ril_data_manager *self);

/*==========================================================================*
 * ril_data
 *==========================================================================*/

gulong ril_data_add_allow_changed_handler(struct ril_data *self,
						ril_data_cb_t cb, void *arg)
{
	return (G_LIKELY(self) && G_LIKELY(cb)) ? g_signal_connect(self,
		SIGNAL_ALLOW_CHANGED_NAME, G_CALLBACK(cb), arg) : 0;
}

void ril_data_remove_handler(struct ril_data *self, gulong id)
{
	if (G_LIKELY(self) && G_LIKELY(id)) {
		g_signal_handler_disconnect(self, id);
	}
}

struct ril_data *ril_data_new(struct ril_data_manager *dm, GRilIoChannel *io)
{
	GASSERT(dm);
	if (G_LIKELY(dm)) {
		struct ril_data *self = g_object_new(RIL_DATA_TYPE, NULL);
		self->q = grilio_queue_new(io);
		self->dm = ril_data_manager_ref(dm);
		dm->data_list = g_slist_append(dm->data_list, self);
		return self;
	}
	return NULL;
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

G_INLINE_FUNC void ril_data_signal(struct ril_data *self)
{
	g_signal_emit(self, ril_data_signals[SIGNAL_ALLOW_CHANGED], 0);
}

void ril_data_allow(struct ril_data *self, gboolean allow)
{
	if (G_LIKELY(self)) {
		struct ril_data_manager *dm = self->dm;
		DBG("%s%s", self->log_prefix, allow ? "yes" : "no");
		if (allow) {
			if (!self->allowed) {
				self->allowed = TRUE;
				ril_data_manager_check(dm);
			}
		} else {
			if (self->allowed) {
				self->allowed = FALSE;
				if (dm->selected == self) {
					ril_data_manager_check(dm);
				}
			}
		}
	}
}

gboolean ril_data_allowed(struct ril_data *self)
{
	return G_LIKELY(self) && self->allowed && self->dm->selected == self;
}

void ril_data_set_name(struct ril_data *self, const char *name)
{
	if (G_LIKELY(self)) {
		g_free(self->custom_log_prefix);
		if (name) {
			self->custom_log_prefix = g_strconcat(name, " ", NULL);
			self->log_prefix = self->custom_log_prefix;
		} else {
			self->custom_log_prefix = NULL;
			self->log_prefix = "";
		}
	}
}

static void ril_data_init(struct ril_data *self)
{
	self->log_prefix = "";
}

static void ril_data_dispose(GObject *object)
{
	struct ril_data *self = RIL_DATA(object);
	struct ril_data_manager	*dm = self->dm;

	dm->data_list = g_slist_remove(dm->data_list, self);
	grilio_queue_cancel_all(self->q, FALSE);
	ril_data_manager_check(dm);
	G_OBJECT_CLASS(ril_data_parent_class)->dispose(object);
}

static void ril_data_finalize(GObject *object)
{
	struct ril_data *self = RIL_DATA(object);

	g_free(self->custom_log_prefix);
	grilio_queue_unref(self->q);
	ril_data_manager_unref(self->dm);
	G_OBJECT_CLASS(ril_data_parent_class)->finalize(object);
}

static void ril_data_class_init(RilDataClass *klass)
{
	GObjectClass *object_class = G_OBJECT_CLASS(klass);

	object_class->dispose = ril_data_dispose;
	object_class->finalize = ril_data_finalize;
	ril_data_signals[SIGNAL_ALLOW_CHANGED] =
		g_signal_new(SIGNAL_ALLOW_CHANGED_NAME,
			G_OBJECT_CLASS_TYPE(klass), G_SIGNAL_RUN_FIRST,
			0, NULL, NULL, NULL, G_TYPE_NONE, 0);
}

/*==========================================================================*
 * ril_data_manager
 *==========================================================================*/

struct ril_data_manager *ril_data_manager_new()
{
	struct ril_data_manager *self = g_new0(struct ril_data_manager, 1);
	self->ref_count = 1;
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
			GASSERT(!self->selected);
			g_free(self);
		}
	}
}

static void ril_data_manager_allow_data_cb(GRilIoChannel *io, int ril_status,
				const void *data, guint len, void *user_data)
{
	struct ril_data_manager *self = user_data;

	GASSERT(self->selected);
	GASSERT(self->pending_id);
	self->pending_id = 0;

	if (ril_status == RIL_E_SUCCESS) {
		DBG("%sselected", self->selected->log_prefix);
	} else {
		DBG("%srequest failed", self->selected->log_prefix);
	}
}

static struct ril_data *ril_data_manager_pick(struct ril_data_manager *self)
{
	GSList *list = self->data_list;
	while (list) {
		struct ril_data *data = list->data;
		if (data->allowed) {
			return data;
		}
		list = list->next;
	}
	return NULL;
}

static GRilIoRequest *ril_data_allow_req(gboolean allow)
{
	GRilIoRequest *req = grilio_request_sized_new(8);

	grilio_request_append_int32(req, 1);
	grilio_request_append_int32(req, allow != FALSE);
	return req;
}

static void ril_data_manager_check(struct ril_data_manager *self)
{
	struct ril_data *data = ril_data_manager_pick(self);

	if (data) {
		if (self->selected != data) {
			GRilIoRequest *req = ril_data_allow_req(TRUE);
			struct ril_data *prev = self->selected;

			/* Cancel pending request, if any */
			GASSERT(prev || !self->pending_id);
			if (prev) {
				grilio_queue_cancel_request(prev->q,
						self->pending_id, FALSE);
			}

			/*
			 * Submit the RIL request. Note that with
			 * some older RILs this request will never
			 * get completed (no reply from rild will
			 * ever come).
			 */
			grilio_request_set_retry(req, RIL_RETRY_MS, -1);
			self->pending_id =
				grilio_queue_send_request_full(data->q, req,
					RIL_REQUEST_ALLOW_DATA,
					ril_data_manager_allow_data_cb,
					NULL, self);
			grilio_request_unref(req);

			DBG("%srequested", data->log_prefix);
			self->selected = data;
			if (prev) {
				ril_data_signal(prev);
			}
			ril_data_signal(data);
		}
	} else {
		if (self->selected) {
			struct ril_data *prev = self->selected;
			if (self->pending_id) {
				grilio_queue_cancel_request(prev->q,
						self->pending_id, FALSE);
				self->pending_id = 0;
			}
			self->selected = NULL;
			ril_data_signal(prev);
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
