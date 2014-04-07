/*
 *
 *  oFono - Open Source Telephony
 *
 *  Copyright (C) 2014 Jolla Ltd
 *  Contact: Miia Leinonen
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

#include <glib.h>

#include <ofono/log.h>
#include <ofono/modem.h>

#include "common.h"
#include "gril.h"
#include "call-barring.h"
#include "rilmodem.h"
#include "ril_constants.h"

/* See 3GPP 27.007 7.4 for possible values */
#define RIL_MAX_SERVICE_LENGTH 3

/*
 * ril.h does not state that string count must be given, but that is
 * still expected by the modem
 */
#define RIL_QUERY_STRING_COUNT 4
#define RIL_SET_STRING_COUNT 5
#define RIL_SET_PW_STRING_COUNT 3

#define RIL_LENGTH_ZERO 0

struct barring_data {
	GRil *ril;
	guint timer_id;
};

static void ril_call_barring_query_cb(struct ril_msg *message,
					gpointer user_data)
{
	struct cb_data *cbd = user_data;
	struct parcel rilp;
	struct ofono_error error;
	ofono_call_barring_query_cb_t cb = cbd->cb;
	int bearer_class = 0;

	if (message->error != RIL_E_SUCCESS) {
		ofono_error("Call Barring query failed, err: %i",
			message->error);
		decode_ril_error(&error, "FAIL");
		goto out;
	}

	ril_util_init_parcel(message, &rilp);

	/*
	 * Services for which the specified barring facility is active.
	 * "0" means "disabled for all, -1 if unknown"
	 */
	parcel_r_int32(&rilp); /* count - we know there is only 1 */
	bearer_class = parcel_r_int32(&rilp);
	DBG("Active services: %i", bearer_class);

	decode_ril_error(&error, "OK");

out:
	cb(&error, bearer_class, cbd->data);
}

static void ril_call_barring_query(struct ofono_call_barring *cb,
					const char *lock, int cls,
					ofono_call_barring_query_cb_t callback,
					void *data)
{
	struct barring_data *bd = ofono_call_barring_get_data(cb);
	struct cb_data *cbd = cb_data_new(callback, data);
	struct parcel rilp;
	int ret = 0;
	char cls_textual[RIL_MAX_SERVICE_LENGTH];

	DBG("lock: %s, services to query: %i", lock, cls);

	/*
	 * RIL modems do not support 7 as default bearer class. According to
	 * the 22.030 Annex C: When service code is not given it corresponds to
	 * "All tele and bearer services"
	 */
	if (cls == BEARER_CLASS_DEFAULT)
		cls = SERVICE_CLASS_NONE;

	sprintf(cls_textual, "%d", cls);

	/*
	 * See 3GPP 27.007 7.4 for parameter descriptions.
	 * According to ril.h password should be empty string "" when not
	 * needed, but in reality we only need to give string length as 0
	 */
	parcel_init(&rilp);
	parcel_w_int32(&rilp, RIL_QUERY_STRING_COUNT);	/* Nbr of strings */
	parcel_w_string(&rilp, (char *) lock);		/* Facility code */
	parcel_w_int32(&rilp, RIL_LENGTH_ZERO);		/* Password length */
	parcel_w_string(&rilp, (char *) cls_textual);
	parcel_w_string(&rilp, NULL); /* AID (for FDN, not yet supported) */

	ret = g_ril_send(bd->ril, RIL_REQUEST_QUERY_FACILITY_LOCK,
			rilp.data, rilp.size, ril_call_barring_query_cb,
			cbd, g_free);

	parcel_free(&rilp);

	if (ret <= 0) {
		ofono_error("Sending Call Barring query failed, err: %i", ret);
		g_free(cbd);
		CALLBACK_WITH_FAILURE(callback, -1, data);
	}
}

static void ril_call_barring_set_cb(struct ril_msg *message, gpointer user_data)
{
	struct cb_data *cbd = user_data;
	struct ofono_error error;
	ofono_call_barring_set_cb_t cb = cbd->cb;

	if (message->error != RIL_E_SUCCESS) {
		ofono_error("Call Barring Set request failed, err: %i",
				message->error);
		decode_ril_error(&error, "FAIL");
		goto out;
	}

	decode_ril_error(&error, "OK");

out:
	cb(&error, cbd->data);
}

static void ril_call_barring_set(struct ofono_call_barring *cb,
				const char *lock, int enable,
				const char *passwd, int cls,
				ofono_call_barring_set_cb_t callback,
				void *data)
{
	struct barring_data *bd = ofono_call_barring_get_data(cb);
	struct cb_data *cbd = cb_data_new(callback, data);
	struct parcel rilp;
	int ret = 0;
	char cls_textual[RIL_MAX_SERVICE_LENGTH];

	DBG("lock: %s, enable: %i, bearer class: %i", lock, enable, cls);

	/*
	 * RIL modem does not support 7 as default bearer class. According to
	 * the 22.030 Annex C: When service code is not given it corresponds to
	 * "All tele and bearer services"
	 */
	if (cls == BEARER_CLASS_DEFAULT)
		cls = SERVICE_CLASS_NONE;

	sprintf(cls_textual, "%d", cls);

	/* See 3GPP 27.007 7.4 for parameter descriptions */
	parcel_init(&rilp);
	parcel_w_int32(&rilp, RIL_SET_STRING_COUNT);	/* Nbr of strings */
	parcel_w_string(&rilp, (char *) lock);		/* Facility code */

	if (enable)
		parcel_w_string(&rilp, RIL_FACILITY_LOCK);
	else
		parcel_w_string(&rilp, RIL_FACILITY_UNLOCK);

	parcel_w_string(&rilp, (char *) passwd);
	parcel_w_string(&rilp, (char *) cls_textual);
	parcel_w_string(&rilp, NULL);	/* AID (for FDN, not yet supported) */

	ret = g_ril_send(bd->ril, RIL_REQUEST_SET_FACILITY_LOCK,
			rilp.data, rilp.size, ril_call_barring_set_cb,
			cbd, g_free);

	parcel_free(&rilp);

	if (ret <= 0) {
		ofono_error("Sending Call Barring Set request failed, err: %i",
				ret);
		g_free(cbd);
		CALLBACK_WITH_FAILURE(callback, data);
	}
}

static void ril_call_barring_set_passwd_cb(struct ril_msg *message,
						gpointer user_data)
{
	struct cb_data *cbd = user_data;
	struct ofono_error error;
	ofono_call_barring_set_cb_t cb = cbd->cb;

	if (message->error != RIL_E_SUCCESS) {
		ofono_error("Call Barring Set PW req failed, err: %i",
				message->error);
		decode_ril_error(&error, "FAIL");
		goto out;
	}

	decode_ril_error(&error, "OK");

out:
	cb(&error, cbd->data);
}

static void ril_call_barring_set_passwd(struct ofono_call_barring *barr,
					const char *lock,
					const char *old_passwd,
					const char *new_passwd,
					ofono_call_barring_set_cb_t cb,
					void *data)
{
	struct barring_data *bd = ofono_call_barring_get_data(barr);
	struct cb_data *cbd = cb_data_new(cb, data);
	struct parcel rilp;
	int ret = 0;

	DBG("");

	parcel_init(&rilp);
	parcel_w_int32(&rilp, RIL_SET_PW_STRING_COUNT);	/* Nbr of strings */
	parcel_w_string(&rilp, (char *) lock);		/* Facility code */
	parcel_w_string(&rilp, (char *) old_passwd);
	parcel_w_string(&rilp, (char *) new_passwd);

	ret = g_ril_send(bd->ril, RIL_REQUEST_CHANGE_BARRING_PASSWORD,
			rilp.data, rilp.size, ril_call_barring_set_passwd_cb,
			cbd, g_free);

	parcel_free(&rilp);

	if (ret <= 0) {
		ofono_error("Sending Call Barring Set PW req failed, err: %i",
				ret);
		g_free(cbd);
		CALLBACK_WITH_FAILURE(cb, data);
	}
}

static gboolean ril_delayed_register(gpointer user_data)
{
	struct ofono_call_barring *cb = user_data;
	struct barring_data *bd = ofono_call_barring_get_data(cb);

	bd->timer_id = 0;

	ofono_call_barring_register(cb);
	return FALSE;
}

static int ril_call_barring_probe(struct ofono_call_barring *cb,
					unsigned int vendor, void *user)
{
	GRil *ril = user;
	struct barring_data *bd = g_try_new0(struct barring_data, 1);

	bd->ril = g_ril_clone(ril);
	ofono_call_barring_set_data(cb, bd);
	bd->timer_id = g_timeout_add_seconds(2, ril_delayed_register, cb);

	return 0;
}

static void ril_call_barring_remove(struct ofono_call_barring *cb)
{
	struct barring_data *data = ofono_call_barring_get_data(cb);
	ofono_call_barring_set_data(cb, NULL);

	if (data->timer_id > 0)
		g_source_remove(data->timer_id);

	g_ril_unref(data->ril);
	g_free(data);
}

static struct ofono_call_barring_driver driver = {
	.name			= "rilmodem",
	.probe			= ril_call_barring_probe,
	.remove			= ril_call_barring_remove,
	.query			= ril_call_barring_query,
	.set			= ril_call_barring_set,
	.set_passwd		= ril_call_barring_set_passwd
};

void ril_call_barring_init(void)
{
	ofono_call_barring_driver_register(&driver);
}

void ril_call_barring_exit(void)
{
	ofono_call_barring_driver_unregister(&driver);
}
