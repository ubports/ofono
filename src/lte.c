/*
 *
 *  oFono - Open Source Telephony
 *
 *  Copyright (C) 2016  Endocode AG. All rights reserved.
 *  Copyright (C) 2018 Gemalto M2M
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

#define SETTINGS_STORE "lte"
#define SETTINGS_GROUP "Settings"
#define LTE_APN "DefaultAccessPointName"
#define LTE_PROTO "Protocol"
#define LTE_USERNAME "Username"
#define LTE_PASSWORD "Password"
#define LTE_AUTH_METHOD "AuthenticationMethod"

struct ofono_lte {
	const struct ofono_lte_driver *driver;
	void *driver_data;
	struct ofono_atom *atom;
	char *imsi;
	GKeyFile *settings;
	DBusMessage *pending;
	struct ofono_lte_default_attach_info pending_info;
	struct ofono_lte_default_attach_info info;
};

static GSList *g_drivers = NULL;

static void lte_load_settings(struct ofono_lte *lte)
{
	char *apn;
	char *proto_str;
	char *auth_method_str;
	char *username;
	char *password;

	if (lte->imsi == NULL)
		return;

	lte->settings = storage_open(lte->imsi, SETTINGS_STORE);

	if (lte->settings == NULL) {
		ofono_error("LTE: Can't open settings file, "
				"changes won't be persistent");
		return;
	}

	apn = g_key_file_get_string(lte->settings, SETTINGS_GROUP,
				LTE_APN, NULL);
	proto_str = g_key_file_get_string(lte->settings, SETTINGS_GROUP,
				LTE_PROTO, NULL);
	auth_method_str = g_key_file_get_string(lte->settings, SETTINGS_GROUP,
				LTE_AUTH_METHOD, NULL);
	username = g_key_file_get_string(lte->settings, SETTINGS_GROUP,
				LTE_USERNAME, NULL);
	password = g_key_file_get_string(lte->settings, SETTINGS_GROUP,
				LTE_PASSWORD, NULL);
	if (apn && is_valid_apn(apn))
		strcpy(lte->info.apn, apn);

	if (proto_str == NULL)
		proto_str = g_strdup("ip");

	/* this must have a valid default */
	if (!gprs_proto_from_string(proto_str, &lte->info.proto))
		lte->info.proto = OFONO_GPRS_PROTO_IP;

	if (auth_method_str == NULL)
		auth_method_str = g_strdup("none");

	/* this must have a valid default */
	if (!gprs_auth_method_from_string(auth_method_str,
							&lte->info.auth_method))
		lte->info.auth_method = OFONO_GPRS_AUTH_METHOD_NONE;

	if (username && strlen(username) <= OFONO_GPRS_MAX_USERNAME_LENGTH)
		strcpy(lte->info.username, username);

	if (password && strlen(password) <= OFONO_GPRS_MAX_PASSWORD_LENGTH)
		strcpy(lte->info.password, password);

	g_free(apn);
	g_free(proto_str);
	g_free(auth_method_str);
	g_free(username);
	g_free(password);
}

static DBusMessage *lte_get_properties(DBusConnection *conn,
					DBusMessage *msg, void *data)
{
	struct ofono_lte *lte = data;
	const char *proto = gprs_proto_to_string(lte->info.proto);
	const char *apn = lte->info.apn;
	const char* auth_method =
			gprs_auth_method_to_string(lte->info.auth_method);
	const char *username = lte->info.username;
	const char *password = lte->info.password;
	DBusMessage *reply;
	DBusMessageIter iter;
	DBusMessageIter dict;

	reply = dbus_message_new_method_return(msg);
	if (reply == NULL)
		return NULL;

	dbus_message_iter_init_append(reply, &iter);

	dbus_message_iter_open_container(&iter, DBUS_TYPE_ARRAY,
					OFONO_PROPERTIES_ARRAY_SIGNATURE,
					&dict);
	ofono_dbus_dict_append(&dict, LTE_APN, DBUS_TYPE_STRING, &apn);
	ofono_dbus_dict_append(&dict, LTE_PROTO, DBUS_TYPE_STRING, &proto);
	ofono_dbus_dict_append(&dict, LTE_AUTH_METHOD, DBUS_TYPE_STRING,
					&auth_method);
	ofono_dbus_dict_append(&dict, LTE_USERNAME, DBUS_TYPE_STRING,
					&username);
	ofono_dbus_dict_append(&dict, LTE_PASSWORD, DBUS_TYPE_STRING,
					&password);
	dbus_message_iter_close_container(&iter, &dict);

	return reply;
}

static void lte_set_default_attach_info_cb(const struct ofono_error *error,
								void *data)
{
	struct ofono_lte *lte = data;
	const char *path = __ofono_atom_get_path(lte->atom);
	DBusConnection *conn = ofono_dbus_get_connection();
	DBusMessage *reply;
	char *key;
	char *value;
	const char *str;
	DBusMessageIter iter;
	DBusMessageIter var;

	DBG("%s error %d", path, error->type);

	if (error->type != OFONO_ERROR_TYPE_NO_ERROR) {
		__ofono_dbus_pending_reply(&lte->pending,
				__ofono_error_failed(lte->pending));
		return;
	}

	/*
	 * Reparsing of the message to extract the key and value
	 * No error checking needed since we already validated pending
	 */
	dbus_message_iter_init(lte->pending, &iter);
	dbus_message_iter_get_basic(&iter, &str);
	key = strdup(str);

	dbus_message_iter_next(&iter);
	dbus_message_iter_recurse(&iter, &var);
	dbus_message_iter_get_basic(&var, &str);
	value = strdup(str);

	memcpy(&lte->info, &lte->pending_info, sizeof(lte->info));

	reply = dbus_message_new_method_return(lte->pending);
	__ofono_dbus_pending_reply(&lte->pending, reply);

	if (lte->settings) {
		/*
		 * the following code removes from storage empty APN, user, pwd
		 * for proto and auth_method, given that they always
		 * have defaults, it will not do anything.
		 */
		if (!*value)
			/* Clear entry on empty string. */
			g_key_file_remove_key(lte->settings,
				SETTINGS_GROUP, key, NULL);
		else
			g_key_file_set_string(lte->settings,
				SETTINGS_GROUP, key, value);

		storage_sync(lte->imsi, SETTINGS_STORE, lte->settings);
	}

	ofono_dbus_signal_property_changed(conn, path,
					OFONO_LTE_INTERFACE,
					key,
					DBUS_TYPE_STRING, &value);

	g_free(value);
	g_free(key);
}

static DBusMessage *lte_set_property(DBusConnection *conn,
					DBusMessage *msg, void *data)
{
	struct ofono_lte *lte = data;
	DBusMessageIter iter;
	DBusMessageIter var;
	const char *property;
	const char *str;
	enum ofono_gprs_auth_method auth_method;
	enum ofono_gprs_proto proto;

	if (lte->driver->set_default_attach_info == NULL)
		return __ofono_error_not_implemented(msg);

	if (lte->pending)
		return __ofono_error_busy(msg);

	if (!dbus_message_iter_init(msg, &iter))
		return __ofono_error_invalid_args(msg);

	if (dbus_message_iter_get_arg_type(&iter) != DBUS_TYPE_STRING)
		return __ofono_error_invalid_args(msg);

	dbus_message_iter_get_basic(&iter, &property);
	dbus_message_iter_next(&iter);

	if (dbus_message_iter_get_arg_type(&iter) != DBUS_TYPE_VARIANT)
		return __ofono_error_invalid_args(msg);

	dbus_message_iter_recurse(&iter, &var);

	if (dbus_message_iter_get_arg_type(&var) != DBUS_TYPE_STRING)
		return __ofono_error_invalid_args(msg);

	dbus_message_iter_get_basic(&var, &str);

	memcpy(&lte->pending_info, &lte->info, sizeof(lte->info));

	if ((strcmp(property, LTE_APN) == 0)) {
		if (g_str_equal(str, lte->info.apn))
			return dbus_message_new_method_return(msg);

		/* We do care about empty value: it can be used for reset. */
		if (is_valid_apn(str) == FALSE && str[0] != '\0')
			return __ofono_error_invalid_format(msg);

		g_strlcpy(lte->pending_info.apn, str,
					OFONO_GPRS_MAX_APN_LENGTH + 1);
	} else if ((strcmp(property, LTE_PROTO) == 0)) {
		if (!gprs_proto_from_string(str, &proto))
			return __ofono_error_invalid_format(msg);

		if (proto == lte->info.proto)
			return dbus_message_new_method_return(msg);

		lte->pending_info.proto = proto;
	} else if (strcmp(property, LTE_AUTH_METHOD) == 0) {
		if (!gprs_auth_method_from_string(str, &auth_method))
			return __ofono_error_invalid_format(msg);

		if (auth_method == lte->info.auth_method)
			return dbus_message_new_method_return(msg);

		lte->pending_info.auth_method = auth_method;
	} else if (strcmp(property, LTE_USERNAME) == 0) {
		if (strlen(str) > OFONO_GPRS_MAX_USERNAME_LENGTH)
			return __ofono_error_invalid_format(msg);

		if (g_str_equal(str, lte->info.username))
			return dbus_message_new_method_return(msg);

		g_strlcpy(lte->pending_info.username, str,
					OFONO_GPRS_MAX_USERNAME_LENGTH + 1);
	} else if (strcmp(property, LTE_PASSWORD) == 0) {
		if (strlen(str) > OFONO_GPRS_MAX_PASSWORD_LENGTH)
			return __ofono_error_invalid_format(msg);

		if (g_str_equal(str, lte->info.password))
			return dbus_message_new_method_return(msg);

		g_strlcpy(lte->pending_info.password, str,
					OFONO_GPRS_MAX_PASSWORD_LENGTH + 1);
	} else
		return __ofono_error_invalid_args(msg);

	lte->pending = dbus_message_ref(msg);
	lte->driver->set_default_attach_info(lte, &lte->pending_info,
					lte_set_default_attach_info_cb, lte);

	return NULL;
}

static const GDBusMethodTable lte_methods[] = {
	{ GDBUS_METHOD("GetProperties",
			NULL, GDBUS_ARGS({ "properties", "a{sv}" }),
			lte_get_properties) },
	{ GDBUS_ASYNC_METHOD("SetProperty",
			GDBUS_ARGS({ "property", "s" }, { "value", "v" }),
			NULL, lte_set_property) },
	{ }
};

static const GDBusSignalTable lte_signals[] = {
	{ GDBUS_SIGNAL("PropertyChanged",
			GDBUS_ARGS({ "name", "s" }, { "value", "v" })) },
	{ }
};

static void lte_atom_remove(struct ofono_atom *atom)
{
	struct ofono_lte *lte = __ofono_atom_get_data(atom);

	DBG("atom: %p", atom);

	if (lte == NULL)
		return;

	if (lte->settings) {
		storage_close(lte->imsi, SETTINGS_STORE, lte->settings, TRUE);
		lte->settings = NULL;
	}

	if (lte->driver && lte->driver->remove)
		lte->driver->remove(lte);

	g_free(lte->imsi);
	lte->imsi = NULL;

	g_free(lte);
}

struct ofono_lte *ofono_lte_create(struct ofono_modem *modem,
					unsigned int vendor,
					const char *driver, void *data)
{
	struct ofono_lte *lte;
	GSList *l;

	if (driver == NULL)
		return NULL;

	lte = g_try_new0(struct ofono_lte, 1);

	if (lte == NULL)
		return NULL;

	lte->atom = __ofono_modem_add_atom(modem, OFONO_ATOM_TYPE_LTE,
						lte_atom_remove, lte);

	for (l = g_drivers; l; l = l->next) {
		const struct ofono_lte_driver *drv = l->data;

		if (g_strcmp0(drv->name, driver))
			continue;

		if (drv->probe(lte, vendor, data) < 0)
			continue;

		lte->driver = drv;
		break;
	}

	DBG("LTE atom created");

	return lte;
}

int ofono_lte_driver_register(const struct ofono_lte_driver *d)
{
	DBG("driver: %p, name: %s", d, d->name);

	if (d->probe == NULL)
		return -EINVAL;

	g_drivers = g_slist_prepend(g_drivers, (void *) d);

	return 0;
}

void ofono_lte_driver_unregister(const struct ofono_lte_driver *d)
{
	DBG("driver: %p, name: %s", d, d->name);

	g_drivers = g_slist_remove(g_drivers, (void *) d);
}

static void lte_atom_unregister(struct ofono_atom *atom)
{
	DBusConnection *conn = ofono_dbus_get_connection();
	struct ofono_modem *modem = __ofono_atom_get_modem(atom);
	const char *path = __ofono_atom_get_path(atom);

	ofono_modem_remove_interface(modem, OFONO_LTE_INTERFACE);
	g_dbus_unregister_interface(conn, path, OFONO_LTE_INTERFACE);
}

static void ofono_lte_finish_register(struct ofono_lte *lte)
{
	DBusConnection *conn = ofono_dbus_get_connection();
	struct ofono_modem *modem = __ofono_atom_get_modem(lte->atom);
	const char *path = __ofono_atom_get_path(lte->atom);

	if (!g_dbus_register_interface(conn, path,
				OFONO_LTE_INTERFACE,
				lte_methods, lte_signals, NULL,
				lte, NULL)) {
		ofono_error("could not create %s interface",
				OFONO_LTE_INTERFACE);
		return;
	}

	ofono_modem_add_interface(modem, OFONO_LTE_INTERFACE);

	__ofono_atom_register(lte->atom, lte_atom_unregister);
}

static void lte_init_default_attach_info_cb(const struct ofono_error *error,
						void *data)
{
	struct ofono_lte *lte = data;

	ofono_lte_finish_register(lte);
}

void ofono_lte_register(struct ofono_lte *lte)
{
	struct ofono_modem *modem = __ofono_atom_get_modem(lte->atom);
	struct ofono_sim *sim = __ofono_atom_find(OFONO_ATOM_TYPE_SIM, modem);
	const char *imsi = ofono_sim_get_imsi(sim);

	if (imsi == NULL) {
		ofono_error("No sim atom found. It is required for registering LTE atom.");
		return;
	}

	lte->imsi = g_strdup(imsi);

	lte_load_settings(lte);
	if (lte->driver->set_default_attach_info) {
		lte->driver->set_default_attach_info(lte, &lte->info,
					lte_init_default_attach_info_cb, lte);
		return;
	}

	ofono_lte_finish_register(lte);
}

void ofono_lte_remove(struct ofono_lte *lte)
{
	__ofono_atom_free(lte->atom);
}

void ofono_lte_set_data(struct ofono_lte *lte, void *data)
{
	lte->driver_data = data;
}

void *ofono_lte_get_data(const struct ofono_lte *lte)
{
	return lte->driver_data;
}

struct ofono_modem *ofono_lte_get_modem(const struct ofono_lte *lte)
{
	return __ofono_atom_get_modem(lte->atom);
}
