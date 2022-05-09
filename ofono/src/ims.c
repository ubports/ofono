/*
 *
 *  oFono - Open Source Telephony
 *
 *  Copyright (C) 2017  Intel Corporation. All rights reserved.
 *  Copyright (C) 2022  Jolla Ltd.
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
#include "storage.h"
#include "dbus-queue.h"

#define VOICE_CAPABLE_FLAG OFONO_IMS_VOICE_CAPABLE
#define SMS_CAPABLE_FLAG OFONO_IMS_SMS_CAPABLE

#define RECHECK_TIMEOUT_SEC (10)

enum ims_reg_strategy {
	IMS_REG_DISABLED,
	IMS_REG_ENABLED,
	IMS_REG_AUTO
#define IMS_REG_DEFAULT IMS_REG_AUTO
};

enum ims_watch_events {
	WATCH_EVENT_REG_TECH,
	WATCH_EVENT_IMSI,
	WATCH_EVENT_COUNT
};

struct ims_call;

struct ofono_ims {
	int reg_info;
	int ext_info;
	const struct ofono_ims_driver *driver;
	void *driver_data;
	struct ofono_atom *atom;
	struct ofono_watch *watch;
	struct ofono_dbus_queue *q;
	struct ims_call *pending;
	struct ims_call *tail;
	enum ims_reg_strategy reg_strategy;
	gboolean reg_check_pending;
	gulong watch_id[WATCH_EVENT_COUNT];
	char *imsi;
	GKeyFile *settings;
	guint recheck_timeout_id;
};

/* Calls to the driver are serialized */

typedef void (*ims_cb_t)(void);
typedef void (*ims_submit_cb_t)(struct ims_call *call);

struct ims_call {
	struct ims_call *next;
	struct ofono_ims *ims;
	ims_submit_cb_t submit;
	union {
		ofono_ims_register_cb_t register_cb;
		ofono_ims_status_cb_t status_cb;
		ims_cb_t fn;
	} cb;
	void *data;
};

#define CALLBACK(f) ((ims_cb_t)(f))

#define REGISTRATION_PROP "Registration"

#define SETTINGS_STORE "ims"
#define SETTINGS_GROUP "Settings"
#define REGISTRATION_KEY REGISTRATION_PROP

static GSList *g_drivers = NULL;

static const char *reg_strategy_name[] = { "disabled", "enabled", "auto" };

static gboolean ims_registration_recheck_cb(gpointer user_data);

static gboolean ims_ret_strategy_from_string(const char *str,
					enum ims_reg_strategy *value)
{
	if (str) {
		int i;

		for (i = 0; i < G_N_ELEMENTS(reg_strategy_name); i++) {
			if (!g_strcmp0(str, reg_strategy_name[i])) {
				*value = i;
				return TRUE;
			}
		}
	}
	return FALSE;
}

static inline gboolean ims_dbus_access_allowed(DBusMessage *msg,
				enum ofono_dbus_access_ims_method method)
{
	return ofono_dbus_access_method_allowed(dbus_message_get_sender(msg),
				OFONO_DBUS_ACCESS_INTF_IMS, method, NULL);
}

static void ims_call_done(struct ims_call *call)
{
	struct ofono_ims *ims = call->ims;

	ims->pending = call->next;
	g_slice_free(struct ims_call, call);

	if (ims->pending) {
		ims->pending->submit(ims->pending);
	} else {
		ims->tail = NULL;
	}
}

static void ims_call_submit(struct ofono_ims *ims, ims_submit_cb_t submit,
						ims_cb_t cb, void *data)
{
	struct ims_call *call = g_slice_new0(struct ims_call);

	call->ims = ims;
	call->submit = submit;
	call->cb.fn = cb;
	call->data = data;

	if (ims->pending) {
		ims->tail->next = call;
		ims->tail = call;
	} else {
		ims->pending = ims->tail = call;
		submit(call);
	}
}

static void ims_call_register_cb(const struct ofono_error *error, void *data)
{
	struct ims_call *call = data;

	if (call->cb.register_cb)
		call->cb.register_cb(error, call->data);

	ims_call_done(call);
}

static void ims_call_status_cb(const struct ofono_error *error,
						int reg_info, int ext_info,
						void *data)
{
	struct ims_call *call = data;

	if (call->cb.status_cb)
		call->cb.status_cb(error, reg_info, ext_info, call->data);

	ims_call_done(call);
}

static void ims_call_submit_registration_status(struct ims_call *call)
{
	struct ofono_ims *ims = call->ims;

	ims->driver->registration_status(ims, ims_call_status_cb, call);
}

static void ims_call_submit_register(struct ims_call *call)
{
	struct ofono_ims *ims = call->ims;

	ims->driver->ims_register(ims, ims_call_register_cb, call);
}

static void ims_call_submit_unregister(struct ims_call *call)
{
	struct ofono_ims *ims = call->ims;

	ims->driver->ims_unregister(ims, ims_call_register_cb, call);
}

static void ims_call_registration_status(struct ofono_ims *ims,
				ofono_ims_status_cb_t cb, void *data)
{
	ims_call_submit(ims, ims_call_submit_registration_status,
						CALLBACK(cb), data);
}

static void ims_call_register(struct ofono_ims *ims,
				ofono_ims_register_cb_t cb, void *data)
{
	ims_call_submit(ims, ims_call_submit_register, CALLBACK(cb), data);
}

static void ims_call_unregister(struct ofono_ims *ims,
				ofono_ims_register_cb_t cb, void *data)
{
	ims_call_submit(ims, ims_call_submit_unregister, CALLBACK(cb), data);
}

static gboolean ims_supported_reg_tech(struct ofono_ims *ims)
{
	return ims->watch &&
		ims->watch->reg_tech >= OFONO_ACCESS_TECHNOLOGY_EUTRAN;
}

static void ims_registration_check(struct ofono_ims *ims)
{
	if (!ims->reg_check_pending)
		return;

	ims->reg_check_pending = FALSE;
	if (ims->recheck_timeout_id) {
		g_source_remove(ims->recheck_timeout_id);
		ims->recheck_timeout_id = 0;
	}

	DBG("checking ims state");
	switch (ims->reg_strategy) {
	case IMS_REG_DISABLED:
		/* Keep registration off */
		if (ims->reg_info && ims->driver &&
					ims->driver->ims_unregister) {
			DBG("auto-unregistering");
			ims_call_unregister(ims, NULL, NULL);
			ims->recheck_timeout_id =
				g_timeout_add_seconds(RECHECK_TIMEOUT_SEC,
					ims_registration_recheck_cb, ims);
		} else {
			DBG("ims is disabled, leaving it unregistered");
		}
		return;
	case IMS_REG_ENABLED:
		/* Any state is acceptable */
		DBG("ims is enabled, no action needed");
		return;
	case IMS_REG_AUTO:
		break;
	}

	/* Keep registration on (default behavior) */
	if (!ims->reg_info && ims_supported_reg_tech(ims) &&
				ims->driver && ims->driver->ims_register) {
		DBG("auto-registering");
		ims_call_register(ims, NULL, NULL);
		ims->recheck_timeout_id =
			g_timeout_add_seconds(RECHECK_TIMEOUT_SEC,
				ims_registration_recheck_cb, ims);
	} else {
		DBG("leaving ims registered");
	}
}

static gboolean ims_registration_recheck_cb(gpointer user_data)
{
	struct ofono_ims *ims = user_data;

	ims->recheck_timeout_id = 0;
	ims_registration_check(ims);
	return G_SOURCE_REMOVE;
}

static void ims_reg_tech_changed(struct ofono_watch *watch, void *data)
{
	struct ofono_ims *ims = data;

	ims->reg_check_pending = TRUE;
	ims_registration_check(ims);
}

static void ims_set_reg_strategy(struct ofono_ims *ims,
					enum ims_reg_strategy value)
{
	if (ims->reg_strategy != value) {
		const char *path = __ofono_atom_get_path(ims->atom);
		DBusConnection *conn = ofono_dbus_get_connection();

		DBG("ims %s", reg_strategy_name[value]);
		ims->reg_strategy = value;
		ims->reg_check_pending = TRUE;

		if (ims->settings) {
			g_key_file_set_string(ims->settings, SETTINGS_GROUP,
				REGISTRATION_KEY, reg_strategy_name[value]);
			storage_sync(ims->imsi, SETTINGS_STORE, ims->settings);
		}

		ofono_dbus_signal_property_changed(conn, path,
				OFONO_IMS_INTERFACE,
				REGISTRATION_PROP, DBUS_TYPE_STRING,
				reg_strategy_name + ims->reg_strategy);
	}
}

static gboolean ims_imsi_check(struct ofono_ims *ims)
{
	const char* imsi = ims->watch ? ims->watch->imsi : NULL;

	if (g_strcmp0(ims->imsi, imsi)) {
		if (ims->imsi) {
			storage_close(ims->imsi, SETTINGS_STORE,
						ims->settings, TRUE);
			g_free(ims->imsi);
		}
		if (imsi) {
			ims->settings = storage_open(imsi, SETTINGS_STORE);
			ims->imsi = g_strdup(imsi);
		} else {
			ims->settings = NULL;
			ims->imsi = NULL;
		}
		return TRUE;
	}
	return FALSE;
}

static void ims_apply_settings(struct ofono_ims *ims)
{
	char* str;

	if (!ims->settings)
		return;

	str = g_key_file_get_string(ims->settings, SETTINGS_GROUP,
						REGISTRATION_KEY, NULL);

	if (str) {
		enum ims_reg_strategy ims_reg = IMS_REG_DEFAULT;

		if (ims_ret_strategy_from_string(str, &ims_reg))
			ims_set_reg_strategy(ims, ims_reg);

		g_free(str);
	}
}

static void ims_imsi_changed(struct ofono_watch *watch, void *data)
{
	struct ofono_ims *ims = data;

	if (ims_imsi_check(ims)) {
		ims_apply_settings(ims);
		ims_registration_check(ims);
	}
}

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
	ofono_dbus_dict_append(&dict, REGISTRATION_PROP, DBUS_TYPE_STRING,
					reg_strategy_name + ims->reg_strategy);

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

static DBusMessage *ims_set_property(DBusConnection *conn,
					DBusMessage *msg, void *data)
{
	struct ofono_ims *ims = data;
	DBusMessageIter iter;
	DBusMessageIter var;
	const char *property;

	if (!ims_dbus_access_allowed(msg, OFONO_DBUS_ACCESS_IMS_SET_PROPERTY))
		return __ofono_error_access_denied(msg);

	if (!dbus_message_iter_init(msg, &iter))
		return __ofono_error_invalid_args(msg);

	if (dbus_message_iter_get_arg_type(&iter) != DBUS_TYPE_STRING)
		return __ofono_error_invalid_args(msg);

	dbus_message_iter_get_basic(&iter, &property);
	dbus_message_iter_next(&iter);

	if (dbus_message_iter_get_arg_type(&iter) != DBUS_TYPE_VARIANT)
		return __ofono_error_invalid_args(msg);

	dbus_message_iter_recurse(&iter, &var);

	if (!g_strcmp0(property, REGISTRATION_PROP)) {
		const char *str = NULL;
		enum ims_reg_strategy value = IMS_REG_DEFAULT;

		if (dbus_message_iter_get_arg_type(&var) != DBUS_TYPE_STRING)
			return __ofono_error_invalid_args(msg);

		dbus_message_iter_get_basic(&var, &str);

		if (ims_ret_strategy_from_string(str, &value)) {
			ims_set_reg_strategy(ims, value);
			ims_registration_check(ims);
			return dbus_message_new_method_return(msg);
		}
	}

	return __ofono_error_invalid_args(msg);
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

	ims->reg_check_pending = TRUE;
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
	ims_registration_check(ims);
}

static void register_cb(const struct ofono_error *error, void *data)
{
	struct ofono_ims *ims = data;

	if (error->type == OFONO_ERROR_TYPE_NO_ERROR)
		__ofono_dbus_queue_reply_ok(ims->q);
	else
		__ofono_dbus_queue_reply_failed(ims->q);
}

static DBusMessage *ofono_ims_register_fn(DBusMessage *msg, void *data)
{
	struct ofono_ims *ims = data;

	ims_call_register(ims, register_cb, ims);

	return NULL;
}

static DBusMessage *ofono_ims_send_register(DBusConnection *conn,
						DBusMessage *msg, void *data)
{
	struct ofono_ims *ims = data;

	if (!ims_dbus_access_allowed(msg, OFONO_DBUS_ACCESS_IMS_REGISTER))
		return __ofono_error_access_denied(msg);

	if (!ims->driver || !ims->driver->ims_register)
		return __ofono_error_not_implemented(msg);

	if (ims->reg_strategy == IMS_REG_DISABLED)
		return __ofono_error_not_allowed(msg);

	__ofono_dbus_queue_request(ims->q, ofono_ims_register_fn, msg, ims);

	return NULL;
}

static DBusMessage *ofono_ims_unregister_fn(DBusMessage *msg, void *data)
{
	struct ofono_ims *ims = data;

	ims_call_unregister(ims, register_cb, ims);

	return NULL;
}

static DBusMessage *ofono_ims_unregister(DBusConnection *conn,
						DBusMessage *msg, void *data)
{
	struct ofono_ims *ims = data;

	if (!ims_dbus_access_allowed(msg, OFONO_DBUS_ACCESS_IMS_UNREGISTER))
		return __ofono_error_access_denied(msg);

	if (!ims->driver || !ims->driver->ims_unregister)
		return __ofono_error_not_implemented(msg);

	__ofono_dbus_queue_request(ims->q, ofono_ims_unregister_fn, msg, ims);

	return NULL;
}

static const GDBusMethodTable ims_methods[] = {
	{ GDBUS_METHOD("GetProperties",
			NULL, GDBUS_ARGS({ "properties", "a{sv}" }),
			ims_get_properties) },
	{ GDBUS_METHOD("SetProperty",
			GDBUS_ARGS({ "property", "s" }, { "value", "v" }),
			NULL, ims_set_property) },
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

	while (ims->pending) {
		struct ims_call *call = ims->pending;

		ims->pending = call->next;
		g_slice_free(struct ims_call, call);
	}

	if (ims->imsi) {
		storage_close(ims->imsi, SETTINGS_STORE, ims->settings, TRUE);
		g_free(ims->imsi);
	}

	if (ims->recheck_timeout_id) {
		g_source_remove(ims->recheck_timeout_id);
	}

	__ofono_dbus_queue_free(ims->q);
	ofono_watch_remove_all_handlers(ims->watch, ims->watch_id);
	ofono_watch_unref(ims->watch);
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
	ims->reg_strategy = IMS_REG_DEFAULT;
	ims->reg_check_pending = TRUE;
	ims->q = __ofono_dbus_queue_new();

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

	ims->watch = ofono_watch_new(path);
	ims->watch_id[WATCH_EVENT_REG_TECH] =
		ofono_watch_add_reg_tech_changed_handler(ims->watch,
				ims_reg_tech_changed, ims);
	ims->watch_id[WATCH_EVENT_IMSI] =
		ofono_watch_add_imsi_changed_handler(ims->watch,
				ims_imsi_changed, ims);

	ofono_modem_add_interface(modem, OFONO_IMS_INTERFACE);
	__ofono_atom_register(ims->atom, ims_atom_unregister);

	ims->reg_check_pending = TRUE;
	ims_imsi_check(ims);
	ims_apply_settings(ims);
	ims_registration_check(ims);
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
	if (!ims->driver || !ims->driver->registration_status) {
		ofono_ims_finish_register(ims);
		return;
	}

	ims_call_registration_status(ims, registration_init_cb, ims);
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
