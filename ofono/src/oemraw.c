/*
 *
 *  oFono - Open Source Telephony
 *
 *  Copyright (C) 2013 Jolla Ltd
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

#define _GNU_SOURCE

#include <errno.h>
#include <glib.h>
#include <gdbus.h>
#include <dbus/dbus.h>
#include "ofono.h"
#include "common.h"
#include "ofono/oemraw.h"

static GSList *g_drivers;

struct ofono_oem_raw {
	struct ofono_atom *atom;
	const struct ofono_oem_raw_driver *driver;
	void *driver_data;
};

static void ofono_oem_raw_query_cb(const struct ofono_error *error,
		const struct ofono_oem_raw_results *res, void *data)
{
	char *ptr;
	char byte;
	int i;
	DBusMessage *reply;
	DBusMessageIter iter;
	DBusMessageIter subiter;
	struct ofono_oem_raw_request *req = data;

	if (error && error->type == OFONO_ERROR_TYPE_NO_ERROR) {
		reply = dbus_message_new_method_return(req->pending);
	} else {
		/*
		 * Log error messages in driver when completing a request,
		 * logging here provides no extra information.
		 */
		goto error;
	}

	dbus_message_iter_init_append(reply, &iter);

	if (!dbus_message_iter_open_container(&iter, DBUS_TYPE_ARRAY,
		"y", &subiter)) {
		DBG("Failed to open a dbus iterator");
		goto error;
	}

	ptr = (char *)res->data;

	for (i = 0; i < res->length; i++) {
		byte = ptr[i];
		dbus_message_iter_append_basic(&subiter, DBUS_TYPE_BYTE,
					       &byte);
	}

	dbus_message_iter_close_container(&iter, &subiter);

	goto end;

error:
	reply = __ofono_error_failed(req->pending);

end:
	__ofono_dbus_pending_reply(&req->pending, reply);
	g_free(req);

	return;
}

static DBusMessage *oem_raw_make_request(DBusConnection *conn,
					 DBusMessage *msg, void *data)
{
	char *array; /* Byte array containing client request*/
	int array_len; /* Length of request byte array */
	DBusMessageIter iter;
	DBusMessageIter subiter;
	struct ofono_oem_raw_request *req;
	struct ofono_oem_raw *raw;
	raw = data;
	req = 0;

	if (raw && raw->driver->request == NULL)
		return __ofono_error_not_implemented(msg);

	dbus_message_iter_init(msg, &iter);

	if (dbus_message_iter_get_arg_type(&iter) != DBUS_TYPE_ARRAY)
		goto error_arg;

	if (dbus_message_iter_get_element_type(&iter) != DBUS_TYPE_BYTE) {
		DBG("Ignoring request because dbus request element type=%c",
		    dbus_message_iter_get_element_type(&iter));
		goto error_arg;
	}

	dbus_message_iter_recurse(&iter, &subiter);

	dbus_message_iter_get_fixed_array(&subiter, &array, &array_len);

	req = g_new0(struct ofono_oem_raw_request, 1);
	req->data = array;
	req->length = array_len;
	/* Store msg to request struct to allow multiple parallel requests */
	req->pending = dbus_message_ref(msg);
	raw->driver->request(raw, req, ofono_oem_raw_query_cb, req);

	return NULL;

error_arg:
	DBG("DBus arg type=%c, msg signature: %s",
		dbus_message_iter_get_arg_type(&iter),
		dbus_message_get_signature(msg));
	return __ofono_error_invalid_args(msg);
}

static const GDBusMethodTable oem_raw_methods[] = {
	{ GDBUS_ASYNC_METHOD("Send",
			GDBUS_ARGS({ "req", "ay" }),
			GDBUS_ARGS({ "response", "ay"}),
			oem_raw_make_request) },
	{ }
};

static const GDBusSignalTable oem_raw_signals[] = {
	{ }
};

static void oem_raw_dbus_unregister(struct ofono_atom *atom)
{
	DBG("");
	struct ofono_oem_raw *oemraw = __ofono_atom_get_data(atom);
	const char *path = __ofono_atom_get_path(oemraw->atom);
	DBusConnection *conn = ofono_dbus_get_connection();
	struct ofono_modem *modem = __ofono_atom_get_modem(oemraw->atom);

	ofono_modem_remove_interface(modem, OFONO_OEM_RAW_INTERFACE);

	if (!g_dbus_unregister_interface(conn, path, OFONO_OEM_RAW_INTERFACE))
		ofono_error("Failed to unregister interface %s",
				OFONO_OEM_RAW_INTERFACE);
}

void ofono_oem_raw_dbus_register(struct ofono_oem_raw *oemraw)
{
	DBusConnection *conn;
	DBG("");
	conn = ofono_dbus_get_connection();
	struct ofono_modem *modem = __ofono_atom_get_modem(oemraw->atom);
	const char *path = __ofono_atom_get_path(oemraw->atom);

	if (!g_dbus_register_interface(conn, path,
						OFONO_OEM_RAW_INTERFACE,
						oem_raw_methods,
						oem_raw_signals,
						NULL, oemraw, NULL)) {
		ofono_error("Could not create interface %s",
				OFONO_OEM_RAW_INTERFACE);
		return;
	}

	ofono_modem_add_interface(modem, OFONO_OEM_RAW_INTERFACE);
	__ofono_atom_register(oemraw->atom, oem_raw_dbus_unregister);
}

int ofono_oem_raw_driver_register(struct ofono_oem_raw_driver *driver)
{
	if (driver->probe == NULL)
		return -EINVAL;

	g_drivers = g_slist_prepend(g_drivers, (void *) driver);
	return 0;
}

void ofono_oem_raw_driver_unregister(struct ofono_oem_raw_driver *driver)
{
	g_drivers = g_slist_remove(g_drivers, (void *) driver);
}

void ofono_oem_raw_remove(struct ofono_oem_raw *oemraw)
{
	__ofono_atom_free(oemraw->atom);
}

void *ofono_oem_raw_get_data(struct ofono_oem_raw *raw)
{
	return raw->driver_data;
}

void ofono_oem_raw_set_data(struct ofono_oem_raw *raw, void *cid)
{
	raw->driver_data = cid;
}

static void oem_raw_remove(struct ofono_atom *atom)
{
	struct ofono_oem_raw *oemraw = __ofono_atom_get_data(atom);

	if (oemraw == NULL)
		return;

	if (oemraw->driver && oemraw->driver->remove)
		oemraw->driver->remove(oemraw);

	g_free(oemraw);
}

struct ofono_oem_raw *ofono_oem_raw_create(struct ofono_modem *modem,
					unsigned int vendor,
					const char *driver,
					void *data)
{
	struct ofono_oem_raw *oemraw = 0;
	GSList *l;

	if (driver == NULL)
		return NULL;

	oemraw = g_try_new0(struct ofono_oem_raw, 1);
	if (oemraw == NULL)
		return NULL;

	oemraw->atom = __ofono_modem_add_atom(modem,
					OFONO_ATOM_TYPE_OEM_RAW,
					oem_raw_remove, oemraw);

	for (l = g_drivers; l; l = l->next) {
		const struct ofono_oem_raw_driver *drv = l->data;

		if (g_strcmp0(drv->name, driver))
			continue;

		if (drv->probe(oemraw, vendor, data) < 0)
			continue;

		oemraw->driver = drv;
		break;
	}

	return oemraw;
}
