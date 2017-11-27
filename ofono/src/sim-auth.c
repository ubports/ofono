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
	unsigned int watch_id;
	struct ofono_sim_aid_session *session;
};

struct aid_object {
	uint8_t aid[16];
	char *path;
	enum sim_app_type type;
};

struct ofono_sim_auth {
	struct ofono_sim *sim;
	const struct ofono_sim_auth_driver *driver;
	void *driver_data;
	struct ofono_atom *atom;
	GSList *aid_objects;
	uint8_t gsm_access : 1;
	uint8_t gsm_context : 1;
	struct auth_request *pending;
	char *nai;
};

/*
 * Find an application by path. 'path' should be a DBusMessage object path.
 */
static uint8_t *find_aid_by_path(GSList *aid_objects,
		const char *path)
{
	GSList *iter = aid_objects;

	while (iter) {
		struct aid_object *obj = iter->data;

		if (!strcmp(path, obj->path))
			return obj->aid;

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
	GSList *iter = sa->aid_objects;

	while (iter) {
		struct aid_object *obj = iter->data;

		if (obj->type == SIM_APP_TYPE_USIM)
			g_dbus_unregister_interface(conn, obj->path,
					OFONO_USIM_APPLICATION_INTERFACE);
		else if (obj->type == SIM_APP_TYPE_ISIM)
			g_dbus_unregister_interface(conn, obj->path,
					OFONO_ISIM_APPLICATION_INTERFACE);

		g_free(obj->path);
		g_free(obj);

		iter = g_slist_next(iter);
	}

	g_dbus_unregister_interface(conn, path,
			OFONO_SIM_AUTHENTICATION_INTERFACE);
	ofono_modem_remove_interface(modem,
			OFONO_SIM_AUTHENTICATION_INTERFACE);


	g_slist_free(sa->aid_objects);
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

	g_free(sa);
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

static void handle_umts(struct ofono_sim_auth *sa, const uint8_t *resp,
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

	reply = dbus_message_new_method_return(sa->pending->msg);

	dbus_message_iter_init_append(reply, &iter);

	dbus_message_iter_open_container(&iter, DBUS_TYPE_ARRAY,
			"{say}", &dict);

	if (auts) {
		append_dict_byte_array(&dict, "AUTS", auts, 14);
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
		reply = __ofono_error_not_supported(sa->pending->msg);

	__ofono_dbus_pending_reply(&sa->pending->msg, reply);

	__ofono_sim_remove_session_watch(sa->pending->session,
			sa->pending->watch_id);

	g_free(sa->pending);
	sa->pending = NULL;
}

static void handle_gsm(struct ofono_sim_auth *sa, const uint8_t *resp,
		uint16_t len)
{
	DBusMessageIter iter;
	const uint8_t *sres = NULL;
	const uint8_t *kc = NULL;

	if (!sim_parse_gsm_authenticate(resp, len, &sres, &kc))
		goto gsm_end;

	/* initial iteration, setup the reply message */
	if (sa->pending->cb_count == 0) {
		sa->pending->reply = dbus_message_new_method_return(
				sa->pending->msg);

		dbus_message_iter_init_append(sa->pending->reply,
				&sa->pending->iter);

		dbus_message_iter_open_container(&sa->pending->iter,
				DBUS_TYPE_ARRAY, "a{say}", &sa->pending->dict);
	}

	/* append the Nth sres/kc byte arrays */
	dbus_message_iter_open_container(&sa->pending->dict, DBUS_TYPE_ARRAY,
			"{say}", &iter);
	append_dict_byte_array(&iter, "SRES", sres, 4);
	append_dict_byte_array(&iter, "Kc", kc, 8);
	dbus_message_iter_close_container(&sa->pending->dict, &iter);

	sa->pending->cb_count++;

	/* calculated the number of keys requested, close container */
	if (sa->pending->cb_count == sa->pending->num_rands) {
		dbus_message_iter_close_container(&sa->pending->iter,
				&sa->pending->dict);
		goto gsm_end;
	}

	return;

gsm_end:
	if (!sa->pending->reply)
		sa->pending->reply = __ofono_error_not_supported(
				sa->pending->msg);

	__ofono_dbus_pending_reply(&sa->pending->msg, sa->pending->reply);

	__ofono_sim_remove_session_watch(sa->pending->session,
			sa->pending->watch_id);

	g_free(sa->pending);

	sa->pending = NULL;
}

static void logical_access_cb(const struct ofono_error *error,
		const unsigned char *resp, unsigned int len, void *data)
{
	struct ofono_sim_auth *sa = data;

	/* error must have occurred in a previous CB */
	if (!sa->pending)
		return;

	if (error->type != OFONO_ERROR_TYPE_NO_ERROR) {
		__ofono_dbus_pending_reply(&sa->pending->msg,
				__ofono_error_failed(sa->pending->msg));
		g_free(sa->pending);
		sa->pending = NULL;
		return;
	}

	if (sa->pending->umts)
		handle_umts(sa, resp, len);
	else
		handle_gsm(sa, resp, len);
}

static void get_session_cb(ofono_bool_t active, int session_id,
		void *data)
{
	struct ofono_sim_auth *sa = data;
	int i;

	if (!active)
		goto error;

	/* save session ID for close_channel() */
	sa->pending->session_id = session_id;

	/*
	 * This will do the logical access num_rand times, providing a new
	 * RAND seed each time. In the UMTS case, num_rands should be 1.
	 */
	for (i = 0; i < sa->pending->num_rands; i++) {
		uint8_t auth_cmd[40];
		int len = 0;

		if (sa->pending->umts)
			len = sim_build_umts_authenticate(auth_cmd, 40,
					sa->pending->rands[i],
					sa->pending->autn);
		else
			len = sim_build_gsm_authenticate(auth_cmd, 40,
					sa->pending->rands[i]);

		if (!len)
			goto error;

		ofono_sim_logical_access(sa->sim, session_id, auth_cmd, len,
				logical_access_cb, sa);
	}

	return;

error:
	__ofono_dbus_pending_reply(&sa->pending->msg,
			__ofono_error_failed(sa->pending->msg));
	g_free(sa->pending);
	sa->pending = NULL;
}

static DBusMessage *usim_gsm_authenticate(DBusConnection *conn,
		DBusMessage *msg, void *data)
{
	struct ofono_sim_auth *sa = data;
	DBusMessageIter iter;
	DBusMessageIter array;
	uint8_t *aid;

	if (sa->pending)
		return __ofono_error_busy(msg);

	dbus_message_iter_init(msg, &iter);

	if (dbus_message_iter_get_arg_type(&iter) != DBUS_TYPE_ARRAY)
		return __ofono_error_invalid_format(msg);

	sa->pending = g_new0(struct auth_request, 1);

	dbus_message_iter_recurse(&iter, &array);

	while (dbus_message_iter_get_arg_type(&array) == DBUS_TYPE_ARRAY) {
		int nelement;
		DBusMessageIter in;

		dbus_message_iter_recurse(&array, &in);

		if (dbus_message_iter_get_arg_type(&in) != DBUS_TYPE_BYTE ||
				sa->pending->num_rands == SIM_AUTH_MAX_RANDS)
			goto format_error;

		dbus_message_iter_get_fixed_array(&in,
				&sa->pending->rands[sa->pending->num_rands++],
				&nelement);

		if (nelement != 16)
			goto format_error;

		dbus_message_iter_next(&array);
	}

	if (sa->pending->num_rands < 2)
		goto format_error;

	/*
	 * retrieve session from SIM
	 */
	aid = find_aid_by_path(sa->aid_objects, dbus_message_get_path(msg));
	sa->pending->session = __ofono_sim_get_session_by_aid(sa->sim, aid);
	sa->pending->msg = dbus_message_ref(msg);
	sa->pending->watch_id = __ofono_sim_add_session_watch(
			sa->pending->session, get_session_cb, sa, NULL);

	return NULL;

format_error:
	g_free(sa->pending);
	sa->pending = NULL;
	return __ofono_error_invalid_format(msg);
}

static DBusMessage *umts_common(DBusConnection *conn, DBusMessage *msg,
					void *data, enum sim_app_type type)
{
	uint8_t *rand = NULL;
	uint8_t *autn = NULL;
	uint32_t rlen;
	uint32_t alen;
	struct ofono_sim_auth *sa = data;
	uint8_t *aid;

	if (sa->pending)
		return __ofono_error_busy(msg);

	/* get RAND/AUTN and setup handle args */
	if (!dbus_message_get_args(msg, NULL, DBUS_TYPE_ARRAY,
			DBUS_TYPE_BYTE, &rand, &rlen, DBUS_TYPE_ARRAY,
			DBUS_TYPE_BYTE, &autn, &alen,
			DBUS_TYPE_INVALID))
		return __ofono_error_invalid_format(msg);

	if (rlen != 16 || alen != 16)
		return __ofono_error_invalid_format(msg);

	sa->pending = g_new0(struct auth_request, 1);
	sa->pending->msg = dbus_message_ref(msg);
	sa->pending->rands[0] = rand;
	sa->pending->num_rands = 1;
	sa->pending->autn = autn;
	sa->pending->umts = 1;

	/*
	 * retrieve session from SIM
	 */
	aid = find_aid_by_path(sa->aid_objects, dbus_message_get_path(msg));
	sa->pending->session = __ofono_sim_get_session_by_aid(sa->sim, aid);

	sa->pending->watch_id = __ofono_sim_add_session_watch(
			sa->pending->session, get_session_cb, sa, NULL);

	return NULL;
}

static DBusMessage *get_applications(DBusConnection *conn,
		DBusMessage *msg, void *data)
{
	struct ofono_sim_auth *sa = data;
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
	if (!sa->aid_objects)
		goto apps_end;

	aid_iter = sa->aid_objects;

	while (aid_iter) {
		struct aid_object *obj = aid_iter->data;

		switch (obj->type) {
		case SIM_APP_TYPE_ISIM:
			dbus_message_iter_open_container(&array,
					DBUS_TYPE_DICT_ENTRY, NULL, &dict);
			append_dict_application(&dict, obj->path, "Ims",
					"ISim");
			dbus_message_iter_close_container(&array, &dict);

			break;
		case SIM_APP_TYPE_USIM:
			dbus_message_iter_open_container(&array,
					DBUS_TYPE_DICT_ENTRY, NULL, &dict);
			append_dict_application(&dict, obj->path, "Umts",
					"USim");
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

static DBusMessage *get_sim_auth_properties(DBusConnection *conn,
		DBusMessage *msg, void *data)
{
	struct ofono_sim_auth *sa = data;
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

	if (sa->nai)
		ofono_dbus_dict_append(&dict, "NetworkAccessIdentity",
				DBUS_TYPE_STRING, &sa->nai);

	dbus_message_iter_close_container(&iter, &dict);

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
	{ GDBUS_METHOD("GetProperties",
			NULL,
			GDBUS_ARGS({"properties", "a{sv}"}),
			get_sim_auth_properties) },
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

/*
 * Build NAI according to TS 23.003. This should only be used as an NAI
 * if the SimManager interface could not find an NAI from the ISim.
 */
static char *build_nai(const char *imsi)
{
	char mcc[3];
	char mnc[3];
	char *nai;

	strncpy(mcc, imsi, 3);

	if (strlen(imsi) == 16) {
		strncpy(mnc, imsi + 3, 3);
	} else {
		mnc[0] = '0';
		strncpy(mnc + 1, imsi + 3, 2);
	}

	nai = g_strdup_printf("%s@ims.mnc%.3s.mcc%.3s.3gppnetwork.org",
			imsi, mnc, mcc);

	return nai;
}

static void sim_auth_register(struct ofono_sim_auth *sa)
{
	DBusConnection *conn = ofono_dbus_get_connection();
	const char *path = __ofono_atom_get_path(sa->atom);
	struct ofono_modem *modem = __ofono_atom_get_modem(sa->atom);
	struct ofono_sim *sim = __ofono_atom_find(OFONO_ATOM_TYPE_SIM, modem);
	GSList *iter = __ofono_sim_get_aid_list(sim);
	int ret;

	sa->sim = sim;

	if (!iter) {
		DBG("No AID list");
		return;
	}

	while (iter) {
		struct sim_app_record *r = iter->data;
		struct aid_object *new = g_new0(struct aid_object, 1);

		new->type = r->type;

		switch (r->type) {
		case SIM_APP_TYPE_USIM:
			new->path = g_new0(char, strlen(path) + 34);

			ret = sprintf(new->path, "%s/", path);

			encode_hex_own_buf(r->aid, 16, 0, new->path + ret);

			g_dbus_register_interface(conn, new->path,
					OFONO_USIM_APPLICATION_INTERFACE,
					sim_auth_usim_app, NULL, NULL,
					sa, NULL);

			memcpy(new->aid, r->aid, 16);

			break;
		case SIM_APP_TYPE_ISIM:
			new->path = g_new0(char, strlen(path) + 34);

			ret = sprintf(new->path, "%s/", path);

			encode_hex_own_buf(r->aid, 16, 0, new->path + ret);

			g_dbus_register_interface(conn, new->path,
					OFONO_ISIM_APPLICATION_INTERFACE,
					sim_auth_isim_app, NULL, NULL,
					sa, NULL);

			memcpy(new->aid, r->aid, 16);

			break;
		default:
			DBG("Unknown SIM application '%04x'", r->type);
			/*
			 * If we get here, the SIM application was not ISIM
			 * or USIM, skip.
			 */
			g_free(new);

			goto loop_end;
		}

		sa->aid_objects = g_slist_prepend(sa->aid_objects, new);

loop_end:
		iter = g_slist_next(iter);
	}

	/* if IMPI is not available, build the NAI */
	if (!__ofono_sim_get_impi(sa->sim))
		sa->nai = build_nai(ofono_sim_get_imsi(sa->sim));
	else
		sa->nai = g_strdup(__ofono_sim_get_impi(sa->sim));

	g_dbus_register_interface(conn, path,
			OFONO_SIM_AUTHENTICATION_INTERFACE,
			sim_authentication, NULL, NULL,
			sa, NULL);
	ofono_modem_add_interface(modem, OFONO_SIM_AUTHENTICATION_INTERFACE);

	__ofono_atom_register(sa->atom, sim_auth_unregister);

	sa->gsm_access = __ofono_sim_ust_service_available(sim,
			SIM_UST_SERVICE_GSM_ACCESS);
	sa->gsm_context = __ofono_sim_ust_service_available(sim,
			SIM_UST_SERVICE_GSM_SECURITY_CONTEXT);
}

struct ofono_sim_auth *ofono_sim_auth_create(struct ofono_modem *modem)
{
	struct ofono_sim_auth *sa;

	sa = g_new0(struct ofono_sim_auth, 1);

	if (sa == NULL)
		return NULL;

	sa->atom = __ofono_modem_add_atom(modem, OFONO_ATOM_TYPE_SIM_AUTH,
						sim_auth_remove, sa);

	sim_auth_register(sa);

	return sa;
}

void ofono_sim_auth_remove(struct ofono_sim_auth *sa)
{
	__ofono_atom_free(sa->atom);
}
