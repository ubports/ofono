/*
 *
 *  oFono - Open Source Telephony
 *
 *  Copyright (C) 2008-2011  Intel Corporation. All rights reserved.
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

#include <glib.h>
#include <errno.h>
#include <unistd.h>
#include <gdbus.h>
#include <string.h>
#include <stdio.h>

#include "ofono.h"

#include "simutil.h"
#include "util.h"

#define SIM_AUTH_MAX_RANDS	3

static GSList *g_drivers = NULL;

/*
 * Temporary handle used for the command authentication sequence.
 */
struct auth_request {
	/* DBus values for GSM authentication */
	DBusMessage *msg;
	DBusMessage *reply;
	DBusMessageIter iter;
	DBusMessageIter dict;
	/* ID from open_channel */
	int session_id;
	/* list of rands to calculate key (1 if umts == 1) */
	void *rands[SIM_AUTH_MAX_RANDS];
	int num_rands;
	/* number of keys that have been returned */
	int cb_count;
	void *autn;
	uint8_t umts : 1;
};

struct ofono_sim_auth {
	const struct ofono_sim_auth_driver *driver;
	void *driver_data;
	struct ofono_atom *atom;
	GSList *aid_list;
	uint8_t gsm_access : 1;
	uint8_t gsm_context : 1;
	struct auth_request *pending;
};

/*
 * Find an application by path. 'path' should be a DBusMessage object path.
 */
static struct sim_app_record *find_aid_by_path(GSList *aid_list,
		const char *path)
{
	GSList *iter = aid_list;
	const char *aid = strrchr(path, '/') + 1;

	while (iter) {
		struct sim_app_record *app = iter->data;
		char str[33];

		encode_hex_own_buf(app->aid, 16, 0, str);

		if (!strcmp(aid, str))
			return app;

		iter = g_slist_next(iter);
	}

	return NULL;
}

/*
 * Free all discovered AID's
 */
static void free_apps(struct ofono_sim_auth *sa)
{
	DBusConnection *conn = ofono_dbus_get_connection();
	struct ofono_modem *modem = __ofono_atom_get_modem(sa->atom);
	const char *path = __ofono_atom_get_path(sa->atom);
	GSList *iter = sa->aid_list;

	while (iter) {
		struct sim_app_record *app = iter->data;
		char object[strlen(path) + 33];
		int ret;

		ret = sprintf(object, "%s/", path);
		encode_hex_own_buf(app->aid, 16, 0, object + ret);

		if (app->type == SIM_APP_TYPE_USIM) {
			g_dbus_unregister_interface(conn, object,
					OFONO_USIM_APPLICATION_INTERFACE);
		} else if (app->type == SIM_APP_TYPE_ISIM) {
			g_dbus_unregister_interface(conn, object,
					OFONO_ISIM_APPLICATION_INTERFACE);
		}

		iter = g_slist_next(iter);
	}

	g_dbus_unregister_interface(conn, path,
			OFONO_SIM_AUTHENTICATION_INTERFACE);
	ofono_modem_remove_interface(modem,
			OFONO_SIM_AUTHENTICATION_INTERFACE);


	g_slist_free(sa->aid_list);
}

int ofono_sim_auth_driver_register(const struct ofono_sim_auth_driver *d)
{
	DBG("driver: %p, name: %s", d, d->name);

	if (d->probe == NULL)
		return -EINVAL;

	g_drivers = g_slist_prepend(g_drivers, (void *) d);

	return 0;
}

void ofono_sim_auth_driver_unregister(const struct ofono_sim_auth_driver *d)
{
	DBG("driver: %p, name: %s", d, d->name);

	g_drivers = g_slist_remove(g_drivers, (void *) d);
}

static void sim_auth_unregister(struct ofono_atom *atom)
{
	struct ofono_sim_auth *sa = __ofono_atom_get_data(atom);

	free_apps(sa);

	g_free(sa->pending);
}

static void sim_auth_remove(struct ofono_atom *atom)
{
	struct ofono_sim_auth *sa = __ofono_atom_get_data(atom);

	DBG("atom: %p", atom);

	if (sa == NULL)
		return;

	if (sa->driver && sa->driver->remove)
		sa->driver->remove(sa);

	g_free(sa);
}

struct ofono_sim_auth *ofono_sim_auth_create(struct ofono_modem *modem,
						unsigned int vendor,
						const char *driver, void *data)
{
	struct ofono_sim_auth *sa;
	GSList *l;

	if (driver == NULL)
		return NULL;

	sa = g_try_new0(struct ofono_sim_auth, 1);

	if (sa == NULL)
		return NULL;

	sa->atom = __ofono_modem_add_atom(modem, OFONO_ATOM_TYPE_SIM_AUTH,
						sim_auth_remove, sa);

	for (l = g_drivers; l; l = l->next) {
		const struct ofono_sim_auth_driver *drv = l->data;

		if (g_strcmp0(drv->name, driver))
			continue;

		if (drv->probe(sa, vendor, data) < 0)
			continue;

		sa->driver = drv;
		break;
	}

	return sa;
}

/*
 * appends {oa{sv}} into an existing dict array
 */
static void append_dict_application(DBusMessageIter *iter, const char *path,
		const char *type, const char *name)
{
	DBusMessageIter array;

	dbus_message_iter_append_basic(iter, DBUS_TYPE_OBJECT_PATH, &path);

	dbus_message_iter_open_container(iter, DBUS_TYPE_ARRAY, "{sv}", &array);

	ofono_dbus_dict_append(&array, "Type", DBUS_TYPE_STRING, &type);
	ofono_dbus_dict_append(&array, "Name", DBUS_TYPE_STRING, &name);

	dbus_message_iter_close_container(iter, &array);
}

/*
 * appends a{say} onto an existing dict array
 */
static void append_dict_byte_array(DBusMessageIter *iter, const char *key,
		const void *arr, uint32_t len)
{
	DBusMessageIter keyiter;
	DBusMessageIter valueiter;

	dbus_message_iter_open_container(iter, DBUS_TYPE_DICT_ENTRY, NULL,
			&keyiter);
	dbus_message_iter_append_basic(&keyiter, DBUS_TYPE_STRING, &key);
	dbus_message_iter_open_container(&keyiter, DBUS_TYPE_ARRAY,
			"y", &valueiter);
	dbus_message_iter_append_fixed_array(&valueiter, DBUS_TYPE_BYTE, &arr,
			len);
	dbus_message_iter_close_container(&keyiter, &valueiter);
	dbus_message_iter_close_container(iter, &keyiter);
}

static void handle_umts(struct ofono_sim_auth *sim, const uint8_t *resp,
		uint16_t len)
{
	DBusMessage *reply = NULL;
	DBusMessageIter iter;
	DBusMessageIter dict;
	const uint8_t *res = NULL;
	const uint8_t *ck = NULL;
	const uint8_t *ik = NULL;
	const uint8_t *auts = NULL;
	const uint8_t *kc = NULL;

	if (!sim_parse_umts_authenticate(resp, len, &res, &ck, &ik,
			&auts, &kc))
		goto umts_end;

	reply = dbus_message_new_method_return(sim->pending->msg);

	dbus_message_iter_init_append(reply, &iter);

	dbus_message_iter_open_container(&iter, DBUS_TYPE_ARRAY,
			"{say}", &dict);

	if (auts) {
		append_dict_byte_array(&dict, "AUTS", auts, 16);
	} else {
		append_dict_byte_array(&dict, "RES", res, 8);
		append_dict_byte_array(&dict, "CK", ck, 16);
		append_dict_byte_array(&dict, "IK", ik, 16);
		if (kc)
			append_dict_byte_array(&dict, "Kc", kc, 8);
	}

	dbus_message_iter_close_container(&iter, &dict);

umts_end:
	if (!reply)
		reply = __ofono_error_not_supported(sim->pending->msg);

	__ofono_dbus_pending_reply(&sim->pending->msg, reply);

	sim->driver->close_channel(sim, sim->pending->session_id, NULL, NULL);

	g_free(sim->pending);
	sim->pending = NULL;
}

static void handle_gsm(struct ofono_sim_auth *sim, const uint8_t *resp,
		uint16_t len)
{
	DBusMessageIter iter;
	const uint8_t *sres = NULL;
	const uint8_t *kc = NULL;

	if (!sim_parse_gsm_authenticate(resp, len, &sres, &kc))
		goto gsm_end;

	/* initial iteration, setup the reply message */
	if (sim->pending->cb_count == 0) {
		sim->pending->reply = dbus_message_new_method_return(
				sim->pending->msg);

		dbus_message_iter_init_append(sim->pending->reply,
				&sim->pending->iter);

		dbus_message_iter_open_container(&sim->pending->iter,
				DBUS_TYPE_ARRAY, "a{say}", &sim->pending->dict);
	}

	/* append the Nth sres/kc byte arrays */
	dbus_message_iter_open_container(&sim->pending->dict, DBUS_TYPE_ARRAY,
			"{say}", &iter);
	append_dict_byte_array(&iter, "SRES", sres, 4);
	append_dict_byte_array(&iter, "Kc", kc, 8);
	dbus_message_iter_close_container(&sim->pending->dict, &iter);

	sim->pending->cb_count++;

	/* calculated the number of keys requested, close container */
	if (sim->pending->cb_count == sim->pending->num_rands) {
		dbus_message_iter_close_container(&sim->pending->iter,
				&sim->pending->dict);
		goto gsm_end;
	}

	return;

gsm_end:
	if (!sim->pending->reply)
		sim->pending->reply = __ofono_error_not_supported(
				sim->pending->msg);

	__ofono_dbus_pending_reply(&sim->pending->msg, sim->pending->reply);

	sim->driver->close_channel(sim, sim->pending->session_id, NULL, NULL);

	g_free(sim->pending);

	sim->pending = NULL;
}

static void logical_access_cb(const struct ofono_error *error,
		const uint8_t *resp, uint16_t len, void *data)
{
	struct ofono_sim_auth *sim = data;

	/* error must have occurred in a previous CB */
	if (!sim->pending)
		return;

	if (error->type != OFONO_ERROR_TYPE_NO_ERROR) {
		__ofono_dbus_pending_reply(&sim->pending->msg,
				__ofono_error_failed(sim->pending->msg));
		g_free(sim->pending);
		sim->pending = NULL;
		return;
	}

	if (sim->pending->umts)
		handle_umts(sim, resp, len);
	else
		handle_gsm(sim, resp, len);
}

static void open_channel_cb(const struct ofono_error *error, int session_id,
		void *data)
{
	struct ofono_sim_auth *sim = data;
	int i;

	if (error->type != OFONO_ERROR_TYPE_NO_ERROR)
		goto error;

	if (session_id == -1)
		goto error;

	/* save session ID for close_channel() */
	sim->pending->session_id = session_id;

	/*
	 * This will do the logical access num_rand times, providing a new
	 * RAND seed each time. In the UMTS case, num_rands should be 1.
	 */
	for (i = 0; i < sim->pending->num_rands; i++) {
		uint8_t auth_cmd[40];
		int len = 0;

		if (sim->pending->umts)
			len = sim_build_umts_authenticate(auth_cmd, 40,
					sim->pending->rands[i],
					sim->pending->autn);
		else
			len = sim_build_gsm_authenticate(auth_cmd, 40,
					sim->pending->rands[i]);

		if (!len)
			goto error;

		sim->driver->logical_access(sim, session_id, auth_cmd, len,
				logical_access_cb, sim);
	}

	return;

error:
	__ofono_dbus_pending_reply(&sim->pending->msg,
			__ofono_error_failed(sim->pending->msg));
	g_free(sim->pending);
	sim->pending = NULL;
}

static DBusMessage *usim_gsm_authenticate(DBusConnection *conn,
		DBusMessage *msg, void *data)
{
	struct ofono_sim_auth *sim = data;
	DBusMessageIter iter;
	DBusMessageIter array;
	int i;
	struct sim_app_record *app;
	int rands;

	if (sim->pending)
		return __ofono_error_busy(msg);

	dbus_message_iter_init(msg, &iter);

	if (dbus_message_iter_get_arg_type(&iter) != DBUS_TYPE_ARRAY)
		return __ofono_error_invalid_format(msg);

	rands = dbus_message_iter_get_element_count(&iter);

	if (rands > 3 || rands < 2)
		return __ofono_error_invalid_format(msg);

	sim->pending = malloc(sizeof(struct auth_request));
	sim->pending->msg = dbus_message_ref(msg);
	sim->pending->umts = 0;
	sim->pending->cb_count = 0;
	sim->pending->num_rands = rands;

	dbus_message_iter_recurse(&iter, &array);

	for (i = 0; i < sim->pending->num_rands; i++) {
		int nelement;
		DBusMessageIter in;

		dbus_message_iter_recurse(&array, &in);

		if (dbus_message_iter_get_arg_type(&in) != DBUS_TYPE_BYTE)
			goto format_error;

		dbus_message_iter_get_fixed_array(&in, &sim->pending->rands[i],
				&nelement);

		if (nelement != 16)
			goto format_error;

		dbus_message_iter_next(&array);
	}

	app = find_aid_by_path(sim->aid_list, dbus_message_get_path(msg));

	sim->driver->open_channel(sim, app->aid, open_channel_cb, sim);

	return NULL;

format_error:
	g_free(sim->pending);
	sim->pending = NULL;
	return __ofono_error_invalid_format(msg);
}

static DBusMessage *umts_common(DBusConnection *conn, DBusMessage *msg,
					void *data, enum sim_app_type type)
{
	uint8_t *rand = NULL;
	uint8_t *autn = NULL;
	uint32_t rlen;
	uint32_t alen;
	struct ofono_sim_auth *sim = data;
	struct sim_app_record *app;

	if (sim->pending)
		return __ofono_error_busy(msg);

	/* get RAND/AUTN and setup handle args */
	if (!dbus_message_get_args(msg, NULL, DBUS_TYPE_ARRAY,
			DBUS_TYPE_BYTE, &rand, &rlen, DBUS_TYPE_ARRAY,
			DBUS_TYPE_BYTE, &autn, &alen,
			DBUS_TYPE_INVALID))
		return __ofono_error_invalid_format(msg);

	if (rlen != 16 || alen != 16)
		return __ofono_error_invalid_format(msg);

	sim->pending = g_new0(struct auth_request, 1);
	sim->pending->msg = dbus_message_ref(msg);
	sim->pending->rands[0] = rand;
	sim->pending->num_rands = 1;
	sim->pending->autn = autn;
	sim->pending->umts = 1;

	app = find_aid_by_path(sim->aid_list, dbus_message_get_path(msg));

	sim->driver->open_channel(sim, app->aid, open_channel_cb, sim);

	return NULL;
}

static DBusMessage *get_applications(DBusConnection *conn,
		DBusMessage *msg, void *data)
{
	struct ofono_sim_auth *sim = data;
	const char *path = __ofono_atom_get_path(sim->atom);
	int ret;
	char object[strlen(path) + 33];
	DBusMessage *reply;
	DBusMessageIter iter;
	DBusMessageIter array;
	DBusMessageIter dict;
	GSList *aid_iter;

	reply = dbus_message_new_method_return(msg);
	if (reply == NULL)
		return NULL;

	dbus_message_iter_init_append(reply, &iter);

	dbus_message_iter_open_container(&iter, DBUS_TYPE_ARRAY, "{oa{sv}}",
				&array);

	/* send empty array */
	if (!sim->aid_list)
		goto apps_end;

	aid_iter = sim->aid_list;

	while (aid_iter) {
		struct sim_app_record *app = aid_iter->data;

		ret = sprintf(object, "%s/", path);
		encode_hex_own_buf(app->aid, 16, 0, object + ret);

		switch (app->type) {
		case SIM_APP_TYPE_ISIM:
			dbus_message_iter_open_container(&array,
					DBUS_TYPE_DICT_ENTRY, NULL, &dict);
			append_dict_application(&dict, object, "Ims", "ISim");
			dbus_message_iter_close_container(&array, &dict);

			break;
		case SIM_APP_TYPE_USIM:
			dbus_message_iter_open_container(&array,
					DBUS_TYPE_DICT_ENTRY, NULL, &dict);
			append_dict_application(&dict, object, "Umts", "USim");
			dbus_message_iter_close_container(&array, &dict);

			break;
		default:
			break;
		}

		aid_iter = g_slist_next(aid_iter);
	}

apps_end:
	dbus_message_iter_close_container(&iter, &array);

	return reply;
}

static DBusMessage *send_properties(DBusConnection *conn, DBusMessage *msg,
		void *data, const char *type, const char *name)
{
	DBusMessage *reply;
	DBusMessageIter iter;
	DBusMessageIter array;

	reply = dbus_message_new_method_return(msg);
	if (reply == NULL)
		return NULL;

	dbus_message_iter_init_append(reply, &iter);

	dbus_message_iter_open_container(&iter, DBUS_TYPE_ARRAY, "{sv}",
				&array);

	ofono_dbus_dict_append(&array, "Type", DBUS_TYPE_STRING, &type);
	ofono_dbus_dict_append(&array, "Name", DBUS_TYPE_STRING, &name);

	dbus_message_iter_close_container(&iter, &array);

	return reply;
}

static DBusMessage *usim_get_properties(DBusConnection *conn,
		DBusMessage *msg, void *data)
{
	return send_properties(conn, msg, data, "Umts", "USim");
}

static DBusMessage *isim_get_properties(DBusConnection *conn,
		DBusMessage *msg, void *data)
{
	return send_properties(conn, msg, data, "Ims", "ISim");
}

static DBusMessage *isim_ims_authenticate(DBusConnection *conn,
		DBusMessage *msg, void *data)
{
	return umts_common(conn, msg, data, SIM_APP_TYPE_ISIM);
}

static DBusMessage *usim_umts_authenticate(DBusConnection *conn,
		DBusMessage *msg, void *data)
{
	return umts_common(conn, msg, data, SIM_APP_TYPE_USIM);
}

static const GDBusMethodTable sim_authentication[] = {
	{ GDBUS_METHOD("GetApplications",
			NULL,
			GDBUS_ARGS({"applications", "a{oa{sv}}"}),
			get_applications) },
	{ }
};

static const GDBusMethodTable sim_auth_usim_app[] = {
	{ GDBUS_ASYNC_METHOD("GetProperties",
			NULL,
			GDBUS_ARGS({"properties", "a{sv}"}),
			usim_get_properties) },
	{ GDBUS_ASYNC_METHOD("GsmAuthenticate",
			GDBUS_ARGS({"rands", "aay"}),
			GDBUS_ARGS({"keys", "a{say}"}),
			usim_gsm_authenticate) },
	{ GDBUS_ASYNC_METHOD("UmtsAuthenticate",
			GDBUS_ARGS({"rand", "ay"}, {"autn", "ay"}),
			GDBUS_ARGS({"return", "a{sv}"}),
			usim_umts_authenticate) },
	{ }
};

static const GDBusMethodTable sim_auth_isim_app[] = {
	{ GDBUS_ASYNC_METHOD("GetProperties",
			NULL,
			GDBUS_ARGS({"properties", "a{sv}"}),
			isim_get_properties) },
	{ GDBUS_ASYNC_METHOD("ImsAuthenticate",
			GDBUS_ARGS({"rand", "ay"}, {"autn", "ay"}),
			GDBUS_ARGS({"return", "a{sv}"}),
			isim_ims_authenticate) },
	{ }
};

static void discover_apps_cb(const struct ofono_error *error,
		const unsigned char *dataobj,
		int len, void *data)
{
	DBusConnection *conn = ofono_dbus_get_connection();
	struct ofono_sim_auth *sim = data;
	struct ofono_modem *modem = __ofono_atom_get_modem(sim->atom);
	const char *path = __ofono_atom_get_path(sim->atom);
	GSList *iter;
	char app_path[strlen(path) + 34];
	int ret;

	if (error->type != OFONO_ERROR_TYPE_NO_ERROR)
		goto parse_error;

	sim->aid_list = sim_parse_app_template_entries(dataobj, len);

	if (!sim->aid_list)
		goto parse_error;

	iter = sim->aid_list;

	ret = sprintf(app_path, "%s/", path);

	while (iter) {
		struct sim_app_record *app = iter->data;

		switch (app->type) {
		case SIM_APP_TYPE_USIM:
			encode_hex_own_buf(app->aid, 16, 0, app_path + ret);

			g_dbus_register_interface(conn, app_path,
					OFONO_USIM_APPLICATION_INTERFACE,
					sim_auth_usim_app, NULL, NULL,
					sim, NULL);
			break;
		case SIM_APP_TYPE_ISIM:
			encode_hex_own_buf(app->aid, 16, 0, app_path + ret);

			g_dbus_register_interface(conn, app_path,
					OFONO_ISIM_APPLICATION_INTERFACE,
					sim_auth_isim_app, NULL, NULL,
					sim, NULL);
			break;
		default:
			DBG("Unknown SIM application '%04x'", app->type);
			/*
			 * If we get here, the SIM application was not ISIM
			 * or USIM, skip.
			 */
		}

		iter = g_slist_next(iter);
	}

	/*
	 * Now SimAuthentication interface can be registered since app
	 * discovery has completed
	 */
	g_dbus_register_interface(conn, path,
			OFONO_SIM_AUTHENTICATION_INTERFACE,
			sim_authentication, NULL, NULL,
			sim, NULL);
	ofono_modem_add_interface(modem,
			OFONO_SIM_AUTHENTICATION_INTERFACE);

	return;

parse_error:
	/*
	 * Something went wrong parsing the AID list, it can't be assumed that
	 * any previously parsed AID's are valid so free them all.
	 */
	DBG("Error parsing app list");
}

void ofono_sim_auth_register(struct ofono_sim_auth *sa)
{
	struct ofono_modem *modem = __ofono_atom_get_modem(sa->atom);
	struct ofono_sim *sim = __ofono_atom_find(OFONO_ATOM_TYPE_SIM, modem);

	__ofono_atom_register(sa->atom, sim_auth_unregister);

	/* Do SIM application discovery, the cb will register DBus ifaces */
	sa->driver->list_apps(sa, discover_apps_cb, sa);

	sim = __ofono_atom_find(OFONO_ATOM_TYPE_SIM, modem);

	sa->gsm_access = __ofono_sim_ust_service_available(sim,
			SIM_UST_SERVICE_GSM_ACCESS);
	sa->gsm_context = __ofono_sim_ust_service_available(sim,
			SIM_UST_SERVICE_GSM_SECURITY_CONTEXT);
}

void ofono_sim_auth_remove(struct ofono_sim_auth *sa)
{
	__ofono_atom_free(sa->atom);
}

void ofono_sim_auth_set_data(struct ofono_sim_auth *sa, void *data)
{
	sa->driver_data = data;
}

void *ofono_sim_auth_get_data(struct ofono_sim_auth *sa)
{
	return sa->driver_data;
}
