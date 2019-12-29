/*
 *
 *  oFono - Open Source Telephony
 *
 *  Copyright (C) 2017  Intel Corporation. All rights reserved.
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

#include <string.h>
#include <stdio.h>
#include <stdint.h>
#include <stdarg.h>
#include <errno.h>

#include <glib.h>
#include <gdbus.h>

#include "ofono.h"

#include "common.h"

#define VOICE_CAPABLE_FLAG 0x1
#define SMS_CAPABLE_FLAG 0x4

struct ofono_ims {
	int reg_info;
	int ext_info;
	const struct ofono_ims_driver *driver;
	void *driver_data;
	struct ofono_atom *atom;
	DBusMessage *pending;
};

static GSList *g_drivers = NULL;

static DBusMessage *ims_get_properties(DBusConnection *conn,
					DBusMessage *msg, void *data)
{
	struct ofono_ims *ims = data;
	DBusMessage *reply;
	DBusMessageIter iter;
	DBusMessageIter dict;
	dbus_bool_t value;

	reply = dbus_message_new_method_return(msg);
	if (reply == NULL)
		return NULL;

	dbus_message_iter_init_append(reply, &iter);

	dbus_message_iter_open_container(&iter, DBUS_TYPE_ARRAY,
					OFONO_PROPERTIES_ARRAY_SIGNATURE,
					&dict);

	value = ims->reg_info ? TRUE : FALSE;
	ofono_dbus_dict_append(&dict, "Registered", DBUS_TYPE_BOOLEAN, &value);

	if (ims->ext_info != -1) {
		value = ims->ext_info & VOICE_CAPABLE_FLAG ? TRUE : FALSE;
		ofono_dbus_dict_append(&dict, "VoiceCapable",
					DBUS_TYPE_BOOLEAN, &value);

		value = ims->ext_info & SMS_CAPABLE_FLAG ? TRUE : FALSE;
		ofono_dbus_dict_append(&dict, "SmsCapable",
					DBUS_TYPE_BOOLEAN, &value);
	}

	dbus_message_iter_close_container(&iter, &dict);

	return reply;
}

static void ims_set_sms_capable(struct ofono_ims *ims, ofono_bool_t status)
{
	const char *path = __ofono_atom_get_path(ims->atom);
	DBusConnection *conn = ofono_dbus_get_connection();
	dbus_bool_t new_value = status;
	dbus_bool_t old_value = ims->ext_info & SMS_CAPABLE_FLAG ? TRUE :
								FALSE;

	if (old_value == new_value)
		return;

	ofono_dbus_signal_property_changed(conn, path,
						OFONO_IMS_INTERFACE,
						"SmsCapable",
						DBUS_TYPE_BOOLEAN,
						&new_value);
}

static void ims_set_voice_capable(struct ofono_ims *ims, ofono_bool_t status)
{
	const char *path = __ofono_atom_get_path(ims->atom);
	DBusConnection *conn = ofono_dbus_get_connection();
	dbus_bool_t new_value = status;
	dbus_bool_t old_value = ims->ext_info & VOICE_CAPABLE_FLAG ? TRUE :
								FALSE;

	if (old_value == new_value)
		return;

	ofono_dbus_signal_property_changed(conn, path,
						OFONO_IMS_INTERFACE,
						"VoiceCapable",
						DBUS_TYPE_BOOLEAN,
						&new_value);
}

static void ims_set_registered(struct ofono_ims *ims, ofono_bool_t status)
{
	const char *path = __ofono_atom_get_path(ims->atom);
	DBusConnection *conn = ofono_dbus_get_connection();
	dbus_bool_t new_value = status;
	dbus_bool_t old_value = ims->reg_info ? TRUE : FALSE;

	if (old_value == new_value)
		return;

	ofono_dbus_signal_property_changed(conn, path,
						OFONO_IMS_INTERFACE,
						"Registered",
						DBUS_TYPE_BOOLEAN,
						&new_value);
}

void ofono_ims_status_notify(struct ofono_ims *ims, int reg_info, int ext_info)
{
	dbus_bool_t new_reg_info;
	dbus_bool_t new_voice_capable, new_sms_capable;

	if (ims == NULL)
		return;

	DBG("%s reg_info:%d ext_info:%d", __ofono_atom_get_path(ims->atom),
						reg_info, ext_info);

	if (ims->ext_info == ext_info && ims->reg_info == reg_info)
		return;

	new_reg_info = reg_info ? TRUE : FALSE;
	ims_set_registered(ims, new_reg_info);

	if (ext_info < 0)
		goto skip;

	new_voice_capable = ext_info & VOICE_CAPABLE_FLAG ? TRUE : FALSE;
	ims_set_voice_capable(ims, new_voice_capable);

	new_sms_capable = ext_info & SMS_CAPABLE_FLAG ? TRUE: FALSE;
	ims_set_sms_capable(ims, new_sms_capable);

skip:
	ims->reg_info = reg_info;
	ims->ext_info = ext_info;
}

static void registration_status_cb(const struct ofono_error *error,
						int reg_info, int ext_info,
						void *data)
{
	struct ofono_ims *ims = data;

	if (error->type != OFONO_ERROR_TYPE_NO_ERROR) {
		DBG("Error during IMS registration/unregistration");
		return;
	}

	ofono_ims_status_notify(ims, reg_info, ext_info);
}

static void register_cb(const struct ofono_error *error, void *data)
{
	struct ofono_ims *ims = data;
	DBusMessage *reply;

	if (error->type == OFONO_ERROR_TYPE_NO_ERROR)
		reply = dbus_message_new_method_return(ims->pending);
	else
		reply = __ofono_error_failed(ims->pending);

	__ofono_dbus_pending_reply(&ims->pending, reply);

	if (ims->driver->registration_status == NULL)
		return;

	ims->driver->registration_status(ims, registration_status_cb, ims);
}

static DBusMessage *ofono_ims_send_register(DBusConnection *conn,
						DBusMessage *msg, void *data)
{
	struct ofono_ims *ims = data;

	if (ims->pending)
		return __ofono_error_busy(msg);

	if (ims->driver->ims_register == NULL)
		return __ofono_error_not_implemented(msg);

	ims->pending = dbus_message_ref(msg);

	ims->driver->ims_register(ims, register_cb, ims);

	return NULL;
}

static DBusMessage *ofono_ims_unregister(DBusConnection *conn,
						DBusMessage *msg, void *data)
{
	struct ofono_ims *ims = data;

	if (ims->pending)
		return __ofono_error_busy(msg);

	if (ims->driver->ims_unregister == NULL)
		return __ofono_error_not_implemented(msg);

	ims->pending = dbus_message_ref(msg);

	ims->driver->ims_unregister(ims, register_cb, ims);

	return NULL;
}

static const GDBusMethodTable ims_methods[] = {
	{ GDBUS_METHOD("GetProperties",
			NULL, GDBUS_ARGS({ "properties", "a{sv}" }),
			ims_get_properties) },
	{ GDBUS_ASYNC_METHOD("Register", NULL, NULL,
			ofono_ims_send_register) },
	{ GDBUS_ASYNC_METHOD("Unregister", NULL, NULL,
			ofono_ims_unregister) },
	{ }
};

static const GDBusSignalTable ims_signals[] = {
	{ GDBUS_SIGNAL("PropertyChanged",
			GDBUS_ARGS({ "name", "s" }, { "value", "v" })) },
	{ }
};

static void ims_atom_remove(struct ofono_atom *atom)
{
	struct ofono_ims *ims = __ofono_atom_get_data(atom);

	DBG("atom: %p", atom);

	if (ims == NULL)
		return;

	if (ims->driver && ims->driver->remove)
		ims->driver->remove(ims);

	g_free(ims);
}

struct ofono_ims *ofono_ims_create(struct ofono_modem *modem,
					const char *driver, void *data)
{
	struct ofono_ims *ims;
	GSList *l;

	if (driver == NULL)
		return NULL;

	ims = g_try_new0(struct ofono_ims, 1);

	if (ims == NULL)
		return NULL;

	ims->atom = __ofono_modem_add_atom(modem, OFONO_ATOM_TYPE_IMS,
						ims_atom_remove, ims);

	ims->reg_info = 0;
	ims->ext_info = -1;

	for (l = g_drivers; l; l = l->next) {
		const struct ofono_ims_driver *drv = l->data;

		if (g_strcmp0(drv->name, driver))
			continue;

		if (drv->probe(ims, data) < 0)
			continue;

		ims->driver = drv;
		break;
	}

	DBG("IMS atom created");

	return ims;
}

int ofono_ims_driver_register(const struct ofono_ims_driver *d)
{
	DBG("driver: %p, name: %s", d, d->name);

	if (d->probe == NULL)
		return -EINVAL;

	g_drivers = g_slist_prepend(g_drivers, (void *) d);

	return 0;
}

void ofono_ims_driver_unregister(const struct ofono_ims_driver *d)
{
	DBG("driver: %p, name: %s", d, d->name);

	g_drivers = g_slist_remove(g_drivers, (void *) d);
}

static void ims_atom_unregister(struct ofono_atom *atom)
{
	DBusConnection *conn = ofono_dbus_get_connection();
	struct ofono_modem *modem = __ofono_atom_get_modem(atom);
	const char *path = __ofono_atom_get_path(atom);

	ofono_modem_remove_interface(modem, OFONO_IMS_INTERFACE);
	g_dbus_unregister_interface(conn, path, OFONO_IMS_INTERFACE);
}

static void ofono_ims_finish_register(struct ofono_ims *ims)
{
	DBusConnection *conn = ofono_dbus_get_connection();
	struct ofono_modem *modem = __ofono_atom_get_modem(ims->atom);
	const char *path = __ofono_atom_get_path(ims->atom);

	if (!g_dbus_register_interface(conn, path,
				OFONO_IMS_INTERFACE,
				ims_methods, ims_signals, NULL,
				ims, NULL)) {
		ofono_error("could not create %s interface",
				OFONO_IMS_INTERFACE);
		return;
	}

	ofono_modem_add_interface(modem, OFONO_IMS_INTERFACE);
	__ofono_atom_register(ims->atom, ims_atom_unregister);
}

static void registration_init_cb(const struct ofono_error *error,
						int reg_info, int ext_info,
						void *data)
{
	struct ofono_ims *ims = data;

	if (error->type == OFONO_ERROR_TYPE_NO_ERROR) {
		ims->reg_info = reg_info;
		ims->ext_info = ext_info;
	}

	ofono_ims_finish_register(ims);
}

void ofono_ims_register(struct ofono_ims *ims)
{
	if (!ims->driver->registration_status) {
		ofono_ims_finish_register(ims);
		return;
	}

	ims->driver->registration_status(ims, registration_init_cb, ims);
}

void ofono_ims_remove(struct ofono_ims *ims)
{
	__ofono_atom_free(ims->atom);
}

void ofono_ims_set_data(struct ofono_ims *ims, void *data)
{
	ims->driver_data = data;
}

void *ofono_ims_get_data(const struct ofono_ims *ims)
{
	return ims->driver_data;
}
