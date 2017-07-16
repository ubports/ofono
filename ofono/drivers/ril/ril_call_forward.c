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
#include "ril_util.h"
#include "ril_log.h"

#include "common.h"

struct ril_call_forward {
	GRilIoQueue *q;
	guint timer_id;
};

enum ril_call_forward_action {
	CF_ACTION_DISABLE,
	CF_ACTION_ENABLE,
	CF_ACTION_INTERROGATE,
	CF_ACTION_REGISTRATION,
	CF_ACTION_ERASURE
};

#define CF_TIME_DEFAULT (0)

struct ril_call_forward_cbd {
	struct ril_call_forward *fd;
	union _ofono_call_forward_cb {
		ofono_call_forwarding_query_cb_t query;
		ofono_call_forwarding_set_cb_t set;
		gpointer ptr;
	} cb;
	gpointer data;
};

static inline struct ril_call_forward *ril_call_forward_get_data(
					struct ofono_call_forwarding *cf)
{
	return ofono_call_forwarding_get_data(cf);
}

static void ril_call_forward_cbd_free(gpointer cbd)
{
	g_slice_free(struct ril_call_forward_cbd, cbd);
}

static struct ril_call_forward_cbd *ril_call_forward_cbd_new(void *cb,
								void *data)
{
	struct ril_call_forward_cbd *cbd;

	cbd = g_slice_new0(struct ril_call_forward_cbd);
	cbd->cb.ptr = cb;
	cbd->data = data;
	return cbd;
}

static GRilIoRequest *ril_call_forward_req(enum ril_call_forward_action action,
	int type, int cls, const struct ofono_phone_number *number, int time)
{
	GRilIoRequest *req = grilio_request_new();

	/*
	 * Modem seems to respond with error to all requests
	 * made with bearer class BEARER_CLASS_DEFAULT.
	 */
	if (cls == BEARER_CLASS_DEFAULT) {
		cls = SERVICE_CLASS_NONE;
	}

	grilio_request_append_int32(req, action);
	grilio_request_append_int32(req, type);
	grilio_request_append_int32(req, cls);          /* Service class */
	if (number) {
		grilio_request_append_int32(req, number->type);
		grilio_request_append_utf8(req, number->number);
	} else {
		grilio_request_append_int32(req, 0x81); /* TOA unknown */
		grilio_request_append_utf8(req, NULL);  /* No number */
	}
	grilio_request_append_int32(req, time);

	return req;
}

static void ril_call_forward_set_cb(GRilIoChannel *io, int status,
				const void *data, guint len, void *user_data)
{
	struct ofono_error error;
	struct ril_call_forward_cbd *cbd = user_data;
	ofono_call_forwarding_set_cb_t cb = cbd->cb.set;

	if (status == RIL_E_SUCCESS) {
		cb(ril_error_ok(&error), cbd->data);
	} else {
		ofono_error("CF setting failed");
		cb(ril_error_failure(&error), cbd->data);
	}
}

static void ril_call_forward_set(struct ofono_call_forwarding *cf,
		enum ril_call_forward_action cmd, int type, int cls,
		const struct ofono_phone_number *number, int time,
		ofono_call_forwarding_set_cb_t cb, void *data)
{
	struct ril_call_forward *fd = ril_call_forward_get_data(cf);
	GRilIoRequest *req = ril_call_forward_req(cmd, type, cls, number, time);

	grilio_queue_send_request_full(fd->q, req, RIL_REQUEST_SET_CALL_FORWARD,
			ril_call_forward_set_cb, ril_call_forward_cbd_free,
			ril_call_forward_cbd_new(cb, data));
	grilio_request_unref(req);
}

static void ril_call_forward_registration(struct ofono_call_forwarding *cf,
		int type, int cls, const struct ofono_phone_number *number,
		int time, ofono_call_forwarding_set_cb_t cb, void *data)
{
	ofono_info("cf registration");
	ril_call_forward_set(cf, CF_ACTION_REGISTRATION, type, cls,
						number, time, cb, data);
}

static void ril_call_forward_erasure(struct ofono_call_forwarding *cf,
	int type, int cls, ofono_call_forwarding_set_cb_t cb, void *data)
{
	ofono_info("cf erasure");
	ril_call_forward_set(cf, CF_ACTION_ERASURE, type, cls,
					NULL, CF_TIME_DEFAULT, cb, data);
}

static void ril_call_forward_deactivate(struct ofono_call_forwarding *cf,
	int type, int cls, ofono_call_forwarding_set_cb_t cb, void *data)
{
	ofono_info("cf disable");
	ril_call_forward_set(cf, CF_ACTION_DISABLE, type, cls,
					NULL, CF_TIME_DEFAULT, cb, data);
}

static void ril_call_forward_activate(struct ofono_call_forwarding *cf,
	int type, int cls, ofono_call_forwarding_set_cb_t cb, void *data)
{
	ofono_info("cf enable");
	ril_call_forward_set(cf, CF_ACTION_ENABLE, type, cls,
					NULL, CF_TIME_DEFAULT, cb, data);
}

static void ril_call_forward_query_cb(GRilIoChannel *io, int status,
				const void *data, guint len, void *user_data)
{
	struct ofono_error error;
	struct ril_call_forward_cbd *cbd = user_data;
	ofono_call_forwarding_query_cb_t cb = cbd->cb.query;

	if (status == RIL_E_SUCCESS) {
		struct ofono_call_forwarding_condition *list = NULL;
		GRilIoParser rilp;
		int count = 0;
		int i;

		grilio_parser_init(&rilp, data, len);
		grilio_parser_get_int32(&rilp, &count);

		list = g_new0(struct ofono_call_forwarding_condition, count);
		for (i = 0; i < count; i++) {
			struct ofono_call_forwarding_condition *fw = list + i;
			char *str;

			grilio_parser_get_int32(&rilp, &fw->status);
			grilio_parser_get_int32(&rilp, NULL);
			grilio_parser_get_int32(&rilp, &fw->cls);
			grilio_parser_get_int32(&rilp, &fw->phone_number.type);
			str = grilio_parser_get_utf8(&rilp);
			if (str) {
				strncpy(fw->phone_number.number, str,
					OFONO_MAX_PHONE_NUMBER_LENGTH);
				fw->phone_number.number[
					OFONO_MAX_PHONE_NUMBER_LENGTH] = 0;
				g_free(str);
			}
			grilio_parser_get_int32(&rilp, &fw->time);
		}

		cb(ril_error_ok(&error), count, list, cbd->data);
		g_free(list);
	} else {
		ofono_error("CF query failed");
		cb(ril_error_failure(&error), 0, NULL, cbd->data);
	}
}

static void ril_call_forward_query(struct ofono_call_forwarding *cf, int type,
		int cls, ofono_call_forwarding_query_cb_t cb, void *data)
{
	struct ril_call_forward *fd = ril_call_forward_get_data(cf);
	GRilIoRequest *req = ril_call_forward_req(CF_ACTION_INTERROGATE,
					type, cls, NULL, CF_TIME_DEFAULT);

	ofono_info("cf query");
	grilio_queue_send_request_full(fd->q, req,
			RIL_REQUEST_QUERY_CALL_FORWARD_STATUS,
			ril_call_forward_query_cb, ril_call_forward_cbd_free,
			ril_call_forward_cbd_new(cb, data));
	grilio_request_unref(req);
}

static gboolean ril_call_forward_register(gpointer user_data)
{
	struct ofono_call_forwarding *cf = user_data;
	struct ril_call_forward *fd = ril_call_forward_get_data(cf);

	fd->timer_id = 0;
	ofono_call_forwarding_register(cf);
	return FALSE;
}

static int ril_call_forward_probe(struct ofono_call_forwarding *cf,
					unsigned int vendor, void *data)
{
	struct ril_modem *modem = data;
	struct ril_call_forward *fd = g_try_new0(struct ril_call_forward, 1);

	DBG("");
	fd->q = grilio_queue_new(ril_modem_io(modem));
	fd->timer_id = g_idle_add(ril_call_forward_register, cf);
	ofono_call_forwarding_set_data(cf, fd);
	return 0;
}

static void ril_call_forward_remove(struct ofono_call_forwarding *cf)
{
	struct ril_call_forward *fd = ril_call_forward_get_data(cf);

	DBG("");
	ofono_call_forwarding_set_data(cf, NULL);

	if (fd->timer_id) {
		g_source_remove(fd->timer_id);
	}

	grilio_queue_cancel_all(fd->q, FALSE);
	grilio_queue_unref(fd->q);
	g_free(fd);
}

const struct ofono_call_forwarding_driver ril_call_forwarding_driver = {
	.name           = RILMODEM_DRIVER,
	.probe          = ril_call_forward_probe,
	.remove         = ril_call_forward_remove,
	.erasure        = ril_call_forward_erasure,
	.deactivation	= ril_call_forward_deactivate,
	.query          = ril_call_forward_query,
	.registration   = ril_call_forward_registration,
	.activation     = ril_call_forward_activate
};

/*
 * Local Variables:
 * mode: C
 * c-basic-offset: 8
 * indent-tabs-mode: t
 * End:
 */
