/*
 *
 *  oFono - Open Source Telephony
 *
 *  Copyright (C) 2008-2011  Intel Corporation. All rights reserved.
 *  Copyright (C) 2013 Jolla Ltd
 *  Contact: Jussi Kangas <jussi.kangas@tieto.com>
 *  Copyright (C) 2014 Canonical Ltd.
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

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

#include <glib.h>

#include <ofono/log.h>
#include <ofono/modem.h>
#include <ofono/call-forwarding.h>
#include "common.h"

#if __GNUC__ > 7
#pragma GCC diagnostic ignored "-Wrestrict"
#endif

#include "gril.h"

#include "rilmodem.h"

struct forw_data {
	GRil *ril;
	int last_cls;
};

static void ril_query_call_fwd_cb(struct ril_msg *message, gpointer user_data)
{
	struct cb_data *cbd = user_data;
	struct forw_data *fd = ofono_call_forwarding_get_data(cbd->user);
	ofono_call_forwarding_query_cb_t cb = cbd->cb;
	struct ofono_call_forwarding_condition *list;
	struct parcel rilp;
	unsigned int list_size;
	unsigned int i;

	if (message->error != RIL_E_SUCCESS) {
		ofono_error("%s: rild error: %s", __func__,
				ril_error_to_string(message->error));
		goto error;
	}

	g_ril_init_parcel(message, &rilp);

	if (rilp.size < sizeof(int32_t))
		goto error;

	list_size = parcel_r_int32(&rilp);
	if (list_size == 0) {
		list = g_new0(struct ofono_call_forwarding_condition, 1);
		list_size = 1;

		list->status = 0;
		list->cls = fd->last_cls;
		goto done;
	}

	list = g_new0(struct ofono_call_forwarding_condition, list_size);

	g_ril_append_print_buf(fd->ril, "{");

	for (i = 0; i < list_size; i++) {
		char *str;

		list[i].status =  parcel_r_int32(&rilp);

		parcel_r_int32(&rilp); /* skip reason */

		list[i].cls = parcel_r_int32(&rilp);
		list[i].phone_number.type = parcel_r_int32(&rilp);

		str = parcel_r_string(&rilp);

		if (str != NULL) {
			strncpy(list[i].phone_number.number, str,
				OFONO_MAX_PHONE_NUMBER_LENGTH);
			g_free(str);

			list[i].phone_number.number[
				OFONO_MAX_PHONE_NUMBER_LENGTH] = '\0';
		}

		list[i].time = parcel_r_int32(&rilp);

		if (rilp.malformed) {
			ofono_error("%s: malformed parcel", __func__);
			g_free(list);
			goto error;
		}

		g_ril_append_print_buf(fd->ril, "%s [%d,%d,%d,%s,%d]",
					print_buf,
					list[i].status,
					list[i].cls,
					list[i].phone_number.type,
					list[i].phone_number.number,
					list[i].time);

	}

	g_ril_append_print_buf(fd->ril, "%s}", print_buf);
	g_ril_print_response(fd->ril, message);

done:
	CALLBACK_WITH_SUCCESS(cb, (int) list_size, list, cbd->data);
	g_free(list);
	return;

error:
	CALLBACK_WITH_FAILURE(cb, 0, NULL, cbd->data);
}

static void ril_set_forward_cb(struct ril_msg *message, gpointer user_data)
{
	struct cb_data *cbd = user_data;
	ofono_call_forwarding_set_cb_t cb = cbd->cb;
	struct forw_data *fd = ofono_call_forwarding_get_data(cbd->user);

	if (message->error != RIL_E_SUCCESS) {
		ofono_error("%s: failed; rild error: %s", __func__,
					ril_error_to_string(message->error));
		CALLBACK_WITH_FAILURE(cb, cbd->data);
	}

	g_ril_print_response_no_args(fd->ril, message);
	CALLBACK_WITH_SUCCESS(cb, cbd->data);
}


/*
 * Modem seems to respond with error to all queries or settings made with
 * bearer class BEARER_CLASS_DEFAULT. Design decision: If given class is
 * BEARER_CLASS_DEFAULT let's map it to SERVICE_CLASS_NONE as with it e.g.
 * ./send-ussd '*21*<phone_number>#' returns cls:53 i.e. 1+4+16+32 as
 * service class.
*/
#define FIXUP_CLS() \
	if (cls == BEARER_CLASS_DEFAULT)	\
		cls = SERVICE_CLASS_NONE	\

/*
 * Activation/deactivation/erasure actions, have no number associated with them,
 * but apparently rild expects a number anyway.  So fields need to be filled.
 * Otherwise there is no response.
 */
#define APPEND_DUMMY_NUMBER() \
	parcel_w_int32(&rilp, 0x81);		\
	parcel_w_string(&rilp, "1234567890")	\

/*
 * Time has no real meaing for action commands other then registration, so
 * if not needed, set arbitrary 60s time so rild doesn't return an error.
 */
#define APPEND_DUMMY_TIME() \
	parcel_w_int32(&rilp, 60);

static void ril_activate(struct ofono_call_forwarding *cf,
				int type, int cls,
				ofono_call_forwarding_set_cb_t cb, void *data)
{
	struct forw_data *fd = ofono_call_forwarding_get_data(cf);
	struct cb_data *cbd = cb_data_new(cb, data, cf);
	struct parcel rilp;

	FIXUP_CLS();

	parcel_init(&rilp);

	parcel_w_int32(&rilp, 1);	/* Activation: 1 */
	parcel_w_int32(&rilp, type);
	parcel_w_int32(&rilp, cls);
	APPEND_DUMMY_NUMBER();
	APPEND_DUMMY_TIME();

	g_ril_append_print_buf(fd->ril, "(action: 1, type: %d cls: %d "
					"number type: %d number: %s time: %d)",
					type, cls, 0x81, "1234567890", 60);

	if (g_ril_send(fd->ril, RIL_REQUEST_SET_CALL_FORWARD,
				&rilp, ril_set_forward_cb, cbd, g_free) > 0)
		return;

	CALLBACK_WITH_FAILURE(cb, cbd->data);
	g_free(cbd);
}

static void ril_erasure(struct ofono_call_forwarding *cf,
				int type, int cls,
				ofono_call_forwarding_set_cb_t cb, void *data)
{
	struct forw_data *fd = ofono_call_forwarding_get_data(cf);
	struct cb_data *cbd = cb_data_new(cb, data, cf);
	struct parcel rilp;

	FIXUP_CLS();

	parcel_init(&rilp);

	parcel_w_int32(&rilp, 4);	/* Erasure: 4 */
	parcel_w_int32(&rilp, type);
	parcel_w_int32(&rilp, cls);
	APPEND_DUMMY_NUMBER();
	APPEND_DUMMY_TIME();

	g_ril_append_print_buf(fd->ril, "(action: 4, type: %d cls: %d "
					"number type: %d number: %s time: %d)",
					type, cls, 0x81, "1234567890", 60);

	if (g_ril_send(fd->ril, RIL_REQUEST_SET_CALL_FORWARD,
				&rilp, ril_set_forward_cb, cbd, g_free) > 0)
		return;

	CALLBACK_WITH_FAILURE(cb, cbd->data);
	g_free(cbd);
}

static void ril_deactivate(struct ofono_call_forwarding *cf,
				int type, int cls,
				ofono_call_forwarding_set_cb_t cb, void *data)
{
	struct forw_data *fd = ofono_call_forwarding_get_data(cf);
	struct cb_data *cbd = cb_data_new(cb, data, cf);
	struct parcel rilp;

	FIXUP_CLS();

	parcel_init(&rilp);

	parcel_w_int32(&rilp, 0);	/* Deactivation: 0 */
	parcel_w_int32(&rilp, type);
	parcel_w_int32(&rilp, cls);
	APPEND_DUMMY_NUMBER();
	APPEND_DUMMY_TIME();

	g_ril_append_print_buf(fd->ril, "(action: 0, type: %d cls: %d "
					"number type: %d number: %s time: %d)",
					type, cls, 0x81, "1234567890", 60);

	if (g_ril_send(fd->ril, RIL_REQUEST_SET_CALL_FORWARD,
				&rilp, ril_set_forward_cb, cbd, g_free) > 0)
		return;

	CALLBACK_WITH_FAILURE(cb, cbd->data);
	g_free(cbd);
}

static void ril_registration(struct ofono_call_forwarding *cf, int type,
				int cls,
				const struct ofono_phone_number *number,
				int time, ofono_call_forwarding_set_cb_t cb,
				void *data)
{
	struct forw_data *fd = ofono_call_forwarding_get_data(cf);
	struct cb_data *cbd = cb_data_new(cb, data, cf);
	struct parcel rilp;

	FIXUP_CLS();

	parcel_init(&rilp);

	parcel_w_int32(&rilp, 3);	/* Registration: 3 */
	parcel_w_int32(&rilp, type);
	parcel_w_int32(&rilp, cls);
	parcel_w_int32(&rilp, number->type);
	parcel_w_string(&rilp, number->number);
	parcel_w_int32(&rilp, time);

	g_ril_append_print_buf(fd->ril, "(action: 3, type: %d cls: %d "
					"number type: %d number: %s time: %d)",
					type, cls, number->type, number->number,
					time);

	if (g_ril_send(fd->ril, RIL_REQUEST_SET_CALL_FORWARD,
				&rilp, ril_set_forward_cb, cbd, g_free) > 0)
		return;

	CALLBACK_WITH_FAILURE(cb, cbd->data);
	g_free(cbd);
}

static void ril_query(struct ofono_call_forwarding *cf, int type, int cls,
				ofono_call_forwarding_query_cb_t cb,
				void *data)
{
	struct forw_data *fd = ofono_call_forwarding_get_data(cf);
	struct cb_data *cbd = cb_data_new(cb, data, cf);
	struct parcel rilp;

	FIXUP_CLS();

	parcel_init(&rilp);

	parcel_w_int32(&rilp, 2);	/* Interrogation: 2 */
	parcel_w_int32(&rilp, type);
	parcel_w_int32(&rilp, cls);
	APPEND_DUMMY_NUMBER();
	APPEND_DUMMY_TIME();

	g_ril_append_print_buf(fd->ril, "(action: 2, type: %d cls: %d "
					"number type: %d number: %s time: %d)",
					type, cls, 0x81, "1234567890", 60);

	fd->last_cls = cls;

	if (g_ril_send(fd->ril, RIL_REQUEST_QUERY_CALL_FORWARD_STATUS,
				&rilp, ril_query_call_fwd_cb, cbd, g_free) > 0)
		return;

	CALLBACK_WITH_FAILURE(cb, 0, NULL, cbd->data);
	g_free(cbd);
}

static gboolean ril_delayed_register(gpointer user_data)
{
	struct ofono_call_forwarding *cf = user_data;

	ofono_call_forwarding_register(cf);
	return FALSE;
}

static int ril_call_forwarding_probe(struct ofono_call_forwarding *cf,
					unsigned int vendor, void *user)
{
	GRil *ril = user;
	struct forw_data *fd;

	fd = g_try_new0(struct forw_data, 1);
	if (fd == NULL)
		return -ENOMEM;

	fd->ril = g_ril_clone(ril);
	ofono_call_forwarding_set_data(cf, fd);

	/*
	 * ofono_call_forwarding_register() needs to be called after
	 * the driver has been set in ofono_call_forwarding_create(),
	 * which calls this function.  Most other drivers make
	 * some kind of capabilities query to the modem, and then
	 * call register in the callback; we use an idle event instead.
	 */
	g_idle_add(ril_delayed_register, cf);

	return 0;
}

static void ril_call_forwarding_remove(struct ofono_call_forwarding *cf)
{
	struct forw_data *data = ofono_call_forwarding_get_data(cf);
	ofono_call_forwarding_set_data(cf, NULL);

	g_ril_unref(data->ril);
	g_free(data);
}

static const struct ofono_call_forwarding_driver driver = {
	.name			= RILMODEM,
	.probe			= ril_call_forwarding_probe,
	.remove			= ril_call_forwarding_remove,
	.erasure		= ril_erasure,
	.deactivation		= ril_deactivate,
	.query			= ril_query,
	.registration		= ril_registration,
	.activation		= ril_activate
};

void ril_call_forwarding_init(void)
{
	ofono_call_forwarding_driver_register(&driver);
}

void ril_call_forwarding_exit(void)
{
	ofono_call_forwarding_driver_unregister(&driver);
}
