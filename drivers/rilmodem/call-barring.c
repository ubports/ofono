/*
 *
 *  oFono - Open Source Telephony
 *
 *  Copyright (C) 2014 Jolla Ltd
 *  Contact: Miia Leinonen
 *  Copyright (C) 2014  Canonical Ltd
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

#include <errno.h>

#include <glib.h>

#include <ofono/log.h>
#include <ofono/modem.h>
#include <ofono/call-barring.h>
#include "common.h"

#include "gril.h"

#include "rilmodem.h"

struct barring_data {
	GRil *ril;
};

/*
 * RIL modems do not support 7 as default bearer class. According to TS 22.030
 * Annex C: When service code is not given it corresponds to "All tele and
 * bearer services"
 */
#define FIXUP_CLS() \
	if (cls == BEARER_CLASS_DEFAULT)	\
		cls = SERVICE_CLASS_NONE	\

static void ril_call_barring_query_cb(struct ril_msg *message,
					gpointer user_data)
{
	struct cb_data *cbd = user_data;
	ofono_call_barring_query_cb_t cb = cbd->cb;
	struct barring_data *bd = cbd->user;
	struct parcel rilp;
	int bearer_class;

	if (message->error != RIL_E_SUCCESS)
		goto error;

	g_ril_init_parcel(message, &rilp);

	/* TODO: infineon returns two integers, use a quirk here */
	if (parcel_r_int32(&rilp) < 1)
		goto error;

	bearer_class = parcel_r_int32(&rilp);

	if (bearer_class < 0 || rilp.malformed)
		goto error;

	g_ril_append_print_buf(bd->ril, "{%d}", bearer_class);
	g_ril_print_response(bd->ril, message);

	CALLBACK_WITH_SUCCESS(cb, bearer_class, cbd->data);
	return;

error:
	CALLBACK_WITH_FAILURE(cb, -1, cbd->data);
}

static void ril_call_barring_query(struct ofono_call_barring *cb,
					const char *lock, int cls,
					ofono_call_barring_query_cb_t callback,
					void *data)
{
	struct barring_data *bd = ofono_call_barring_get_data(cb);
	struct cb_data *cbd = cb_data_new(callback, data, bd);
	struct parcel rilp;
	char svcs_str[4];

	DBG("lock: %s, services to query: %d", lock, cls);

	FIXUP_CLS();

	parcel_init(&rilp);
	parcel_w_int32(&rilp, 4);	/* # of strings */
	parcel_w_string(&rilp, lock);
	parcel_w_string(&rilp, "");	/* Password is empty when not needed */
	snprintf(svcs_str, sizeof(svcs_str), "%d", cls);
	parcel_w_string(&rilp, svcs_str);
	parcel_w_string(&rilp, NULL);	/* AID (for FDN, not yet supported) */

	g_ril_append_print_buf(bd->ril, "(%s,\"\",%s,(null))",
				lock, svcs_str);

	if (g_ril_send(bd->ril, RIL_REQUEST_QUERY_FACILITY_LOCK, &rilp,
				ril_call_barring_query_cb, cbd, g_free) > 0)
		return;

	g_free(cbd);
	CALLBACK_WITH_FAILURE(callback, -1, data);
}

static void ril_call_barring_set_cb(struct ril_msg *message, gpointer user_data)
{
	struct cb_data *cbd = user_data;
	ofono_call_barring_set_cb_t cb = cbd->cb;
	struct barring_data *bd = cbd->user;
	struct parcel rilp;
	int retries = -1;

	if (message->error != RIL_E_SUCCESS)
		goto error;

	g_ril_init_parcel(message, &rilp);

	/* mako reply has no payload for call barring */
	if (parcel_data_avail(&rilp) == 0)
		goto done;

	if (parcel_r_int32(&rilp) != 1)
		goto error;

	retries = parcel_r_int32(&rilp);

	if (rilp.malformed)
		goto error;

done:
	g_ril_append_print_buf(bd->ril, "{%d}", retries);
	g_ril_print_response(bd->ril, message);

	CALLBACK_WITH_SUCCESS(cb, cbd->data);
	return;

error:
	CALLBACK_WITH_FAILURE(cb, cbd->data);
}

static void ril_call_barring_set(struct ofono_call_barring *cb,
				const char *lock, int enable,
				const char *passwd, int cls,
				ofono_call_barring_set_cb_t callback,
				void *data)
{
	struct barring_data *bd = ofono_call_barring_get_data(cb);
	struct cb_data *cbd = cb_data_new(callback, data, bd);
	struct parcel rilp;
	char svcs_str[4];

	DBG("lock: %s, enable: %d, bearer class: %d", lock, enable, cls);

	FIXUP_CLS();

	parcel_init(&rilp);
	parcel_w_int32(&rilp, 5);	/* # of strings */
	parcel_w_string(&rilp, lock);
	parcel_w_string(&rilp, enable ? "1" : "0");
	parcel_w_string(&rilp, passwd);
	snprintf(svcs_str, sizeof(svcs_str), "%d", cls);
	parcel_w_string(&rilp, svcs_str);
	parcel_w_string(&rilp, NULL);	/* AID (for FDN, not yet supported) */

	g_ril_append_print_buf(bd->ril, "(%s,%s,%s,%s,(null))",
				lock, enable ? "1" : "0", passwd, svcs_str);

	if (g_ril_send(bd->ril, RIL_REQUEST_SET_FACILITY_LOCK, &rilp,
			ril_call_barring_set_cb, cbd, g_free) > 0)
		return;

	g_free(cbd);
	CALLBACK_WITH_FAILURE(callback, data);
}

static void ril_call_barring_set_passwd_cb(struct ril_msg *message,
						gpointer user_data)
{
	struct cb_data *cbd = user_data;
	ofono_call_barring_set_cb_t cb = cbd->cb;
	struct barring_data *bd = cbd->user;

	if (message->error != RIL_E_SUCCESS) {
		ofono_error("%s: set password failed, err: %s", __func__,
				ril_error_to_string(message->error));
		goto error;
	}

	g_ril_print_response_no_args(bd->ril, message);

	CALLBACK_WITH_SUCCESS(cb, cbd->data);
	return;

error:
	CALLBACK_WITH_FAILURE(cb, cbd->data);
}

static void ril_call_barring_set_passwd(struct ofono_call_barring *barr,
					const char *lock,
					const char *old_passwd,
					const char *new_passwd,
					ofono_call_barring_set_cb_t cb,
					void *data)
{
	struct barring_data *bd = ofono_call_barring_get_data(barr);
	struct cb_data *cbd = cb_data_new(cb, data, bd);
	struct parcel rilp;

	DBG("lock %s old %s new %s", lock, old_passwd, new_passwd);

	parcel_init(&rilp);

	parcel_w_int32(&rilp, 3);	/* # of strings */
	parcel_w_string(&rilp, lock);
	parcel_w_string(&rilp, old_passwd);
	parcel_w_string(&rilp, new_passwd);

	g_ril_append_print_buf(bd->ril, "(%s,%s,%s)",
				lock, old_passwd, new_passwd);

	if (g_ril_send(bd->ril, RIL_REQUEST_CHANGE_BARRING_PASSWORD, &rilp,
			ril_call_barring_set_passwd_cb, cbd, g_free) > 0)
		return;

	g_free(cbd);
	CALLBACK_WITH_FAILURE(cb, data);
}

static gboolean ril_delayed_register(gpointer user_data)
{
	struct ofono_call_barring *cb = user_data;

	ofono_call_barring_register(cb);
	return FALSE;
}

static int ril_call_barring_probe(struct ofono_call_barring *cb,
					unsigned int vendor, void *user)
{
	GRil *ril = user;
	struct barring_data *bd = g_try_new0(struct barring_data, 1);
	if (bd == NULL)
		return -ENOMEM;

	bd->ril = g_ril_clone(ril);
	ofono_call_barring_set_data(cb, bd);

	g_idle_add(ril_delayed_register, cb);

	return 0;
}

static void ril_call_barring_remove(struct ofono_call_barring *cb)
{
	struct barring_data *data = ofono_call_barring_get_data(cb);
	ofono_call_barring_set_data(cb, NULL);

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
