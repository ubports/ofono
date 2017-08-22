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

#include "gdbus.h"
#include "ofono.h"

#define RIL_OEM_RAW_INTERFACE "org.ofono.OemRaw"
#define RIL_OEM_RAW_TIMEOUT   (60*1000) /* 60 sec */

struct ril_oem_raw {
	GRilIoQueue *q;
	DBusConnection *conn;
	char *path;
	char *log_prefix;
};

#define DBG_(oem,fmt,args...) DBG("%s" fmt, (oem)->log_prefix, ##args)

static void ril_oem_raw_send_cb(GRilIoChannel *io, int ril_status,
			const void *data, guint len, void *user_data)
{
	DBusMessage *msg = user_data;
	DBusMessage *reply;

	if (ril_status == RIL_E_SUCCESS) {
		DBusMessageIter it, array;
		const guchar* bytes = data;
		guint i;

		reply = dbus_message_new_method_return(msg);
		dbus_message_iter_init_append(reply, &it);
		dbus_message_iter_open_container(&it, DBUS_TYPE_ARRAY,
					DBUS_TYPE_BYTE_AS_STRING, &array);

		for (i = 0; i < len; i++) {
			guchar byte = bytes[i];
			dbus_message_iter_append_basic(&array, DBUS_TYPE_BYTE,
									&byte);
		}

		dbus_message_iter_close_container(&it, &array);
	} else if (ril_status == GRILIO_STATUS_TIMEOUT) {
		DBG("Timed out");
		reply = __ofono_error_timed_out(msg);
	} else {
		DBG("Error %s", ril_error_to_string(ril_status));
		reply = __ofono_error_failed(msg);
	}

	__ofono_dbus_pending_reply(&msg, reply);
}

static DBusMessage *ril_oem_raw_send(DBusConnection *conn, DBusMessage *msg,
							void *user_data)
{
	DBusMessageIter it;
	struct ril_oem_raw *oem = user_data;

	dbus_message_iter_init(msg, &it);
	if (dbus_message_iter_get_arg_type(&it) == DBUS_TYPE_ARRAY &&
		dbus_message_iter_get_element_type(&it) == DBUS_TYPE_BYTE) {
		char *data;
		int data_len;
		DBusMessageIter array;
		GRilIoRequest *req;

		/* Fetch the data */
		dbus_message_iter_recurse(&it, &array);
		dbus_message_iter_get_fixed_array(&array, &data, &data_len);
		DBG_(oem, "%d bytes", data_len);

		/*
		 * And forward it to rild. Set a timeout because rild may
		 * never respond to invalid requests.
		 */
		req = grilio_request_sized_new(data_len);
		grilio_request_set_timeout(req, RIL_OEM_RAW_TIMEOUT);
		grilio_request_append_bytes(req, data, data_len);
		grilio_queue_send_request_full(oem->q, req,
				RIL_REQUEST_OEM_HOOK_RAW, ril_oem_raw_send_cb,
				NULL, dbus_message_ref(msg));
		grilio_request_unref(req);
		return NULL;
	} else {
		DBG_(oem, "Unexpected signature");
		return __ofono_error_invalid_args(msg);
	}
}

static const GDBusMethodTable ril_oem_raw_methods[] = {
	{ GDBUS_ASYNC_METHOD("Send",
			GDBUS_ARGS({ "request", "ay" }),
			GDBUS_ARGS({ "response", "ay" }),
			ril_oem_raw_send) },
	{ }
};

struct ril_oem_raw *ril_oem_raw_new(struct ril_modem *modem,
						const char *log_prefix)
{
	struct ril_oem_raw *oem = g_new0(struct ril_oem_raw, 1);

	DBG("%s", ril_modem_get_path(modem));
	oem->path = g_strdup(ril_modem_get_path(modem));
	oem->conn = dbus_connection_ref(ofono_dbus_get_connection());
	oem->q = grilio_queue_new(ril_modem_io(modem));
	oem->log_prefix = (log_prefix && log_prefix[0]) ?
			g_strconcat(log_prefix, " ", NULL) : g_strdup("");

	/* Register D-Bus interface */
	if (g_dbus_register_interface(oem->conn, oem->path,
			RIL_OEM_RAW_INTERFACE, ril_oem_raw_methods,
			NULL, NULL, oem, NULL)) {
		ofono_modem_add_interface(modem->ofono, RIL_OEM_RAW_INTERFACE);
		return oem;
	} else {
		ofono_error("OemRaw D-Bus register failed");
		ril_oem_raw_free(oem);
		return NULL;
	}
}

void ril_oem_raw_free(struct ril_oem_raw *oem)
{
	if (oem) {
		DBG("%s", oem->path);
		g_dbus_unregister_interface(oem->conn, oem->path,
						RIL_OEM_RAW_INTERFACE);
		dbus_connection_unref(oem->conn);

		grilio_queue_cancel_all(oem->q, TRUE);
		grilio_queue_unref(oem->q);

		g_free(oem->log_prefix);
		g_free(oem->path);
		g_free(oem);
	}
}

/*
 * Local Variables:
 * mode: C
 * c-basic-offset: 8
 * indent-tabs-mode: t
 * End:
 */
