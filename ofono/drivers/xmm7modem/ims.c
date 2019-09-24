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

#define _GNU_SOURCE
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>

#include <glib.h>

#include <ofono/modem.h>
#include <ofono/log.h>
#include <ofono/ims.h>

#include "gatchat.h"
#include "gatresult.h"

#include "xmm7modem.h"

static const char *none_prefix[] = { NULL };
static const char *cireg_prefix[] = { "+CIREG:", NULL };

struct ims_driver_data {
	GAtChat *chat;
};

static void xmm_cireg_cb(gboolean ok, GAtResult *result,
							gpointer user_data)
{
	struct cb_data *cbd = user_data;
	ofono_ims_status_cb_t cb = cbd->cb;
	struct ofono_error error;
	GAtResultIter iter;
	int reg_info, ext_info;

	DBG("ok %d", ok);

	decode_at_error(&error, g_at_result_final_response(result));

	if (!ok) {
		cb(&error, -1, -1, cbd->data);
		return;
	}

	g_at_result_iter_init(&iter, result);

	if (g_at_result_iter_next(&iter, "+CIREG:") == FALSE)
		goto error;

	/* skip value of n */
	g_at_result_iter_skip_next(&iter);

	if (g_at_result_iter_next_number(&iter, &reg_info) == FALSE)
		goto error;

	if (reg_info == 0)
		ext_info =  -1;
	else
		if (g_at_result_iter_next_number(&iter, &ext_info) == FALSE)
			goto error;

	cb(&error, reg_info, ext_info, cbd->data);

	return;

error:
	CALLBACK_WITH_FAILURE(cb, -1, -1, cbd->data);
}

static void xmm_ims_registration_status(struct ofono_ims *ims,
					ofono_ims_status_cb_t cb, void *data)
{
	struct ims_driver_data *idd = ofono_ims_get_data(ims);
	struct cb_data *cbd = cb_data_new(cb, data);

	if (g_at_chat_send(idd->chat, "AT+CIREG?", cireg_prefix,
					xmm_cireg_cb, cbd, g_free) > 0)
		return;

	CALLBACK_WITH_FAILURE(cb, -1, -1, data);
	g_free(cbd);
}

static void xmm_ims_register_cb(gboolean ok, GAtResult *result,
					gpointer user_data)
{
	struct cb_data *cbd = user_data;
	ofono_ims_register_cb_t cb = cbd->cb;
	struct ofono_error error;

	DBG("ok %d", ok);

	decode_at_error(&error, g_at_result_final_response(result));
	cb(&error, cbd->data);
}

static void xmm_ims_register(struct ofono_ims *ims,
					ofono_ims_register_cb_t cb, void *data)
{
	struct ims_driver_data *idd = ofono_ims_get_data(ims);
	struct cb_data *cbd = cb_data_new(cb, data);

	if (g_at_chat_send(idd->chat, "AT+XIREG=1", none_prefix,
			xmm_ims_register_cb, cbd, g_free) > 0)
		return;

	CALLBACK_WITH_FAILURE(cb, data);
	g_free(cbd);
}

static void xmm_ims_unregister(struct ofono_ims *ims,
					ofono_ims_register_cb_t cb, void *data)
{
	struct ims_driver_data *idd = ofono_ims_get_data(ims);
	struct cb_data *cbd = cb_data_new(cb, data);

	if (g_at_chat_send(idd->chat, "AT+XIREG=0", none_prefix,
			xmm_ims_register_cb, cbd, g_free) > 0)
		return;

	CALLBACK_WITH_FAILURE(cb, data);
	g_free(cbd);
}

static void ciregu_notify(GAtResult *result, gpointer user_data)
{
	struct ofono_ims *ims = user_data;
	int reg_info, ext_info;
	GAtResultIter iter;

	g_at_result_iter_init(&iter, result);

	if (!g_at_result_iter_next(&iter, "+CIREGU:"))
		return;

	if (!g_at_result_iter_next_number(&iter, &reg_info))
		return;

	if (reg_info == 0)
		ext_info =  -1;
	else
		if (!g_at_result_iter_next_number(&iter, &ext_info))
			return;

	DBG("reg_info:%d, ext_info:%d", reg_info, ext_info);

	ofono_ims_status_notify(ims, reg_info, ext_info);
}

static void xmm_cireg_set_cb(gboolean ok, GAtResult *result,
						gpointer user_data)
{
	struct ofono_ims *ims = user_data;

	if (!ok) {
		ofono_ims_remove(ims);
		return;
	}

	ofono_ims_register(ims);
}

static void cireg_support_cb(gboolean ok, GAtResult *result,
						gpointer user_data)
{
	struct ofono_ims *ims = user_data;
	struct ims_driver_data *idd = ofono_ims_get_data(ims);

	if (!ok) {
		ofono_ims_remove(ims);
		return;
	}

	g_at_chat_register(idd->chat, "+CIREGU:", ciregu_notify,
					FALSE, ims, NULL);

	g_at_chat_send(idd->chat, "AT+CIREG=2", none_prefix,
				xmm_cireg_set_cb, ims, NULL);
}

static int xmm_ims_probe(struct ofono_ims *ims, void *data)
{
	GAtChat *chat = data;
	struct ims_driver_data *idd;

	DBG("at ims probe");

	idd = g_try_new0(struct ims_driver_data, 1);
	if (!idd)
		return -ENOMEM;

	idd->chat = g_at_chat_clone(chat);

	ofono_ims_set_data(ims, idd);

	g_at_chat_send(idd->chat, "AT+CIREG=?", cireg_prefix,
				cireg_support_cb, ims, NULL);

	return 0;
}

static void xmm_ims_remove(struct ofono_ims *ims)
{
	struct ims_driver_data *idd = ofono_ims_get_data(ims);

	DBG("at ims remove");

	g_at_chat_unref(idd->chat);

	ofono_ims_set_data(ims, NULL);

	g_free(idd);
}

static struct ofono_ims_driver driver = {
	.name				= "xmm7modem",
	.probe				= xmm_ims_probe,
	.remove				= xmm_ims_remove,
	.ims_register			= xmm_ims_register,
	.ims_unregister			= xmm_ims_unregister,
	.registration_status		= xmm_ims_registration_status,
};

void xmm_ims_init(void)
{
	ofono_ims_driver_register(&driver);
}

void xmm_ims_exit(void)
{
	ofono_ims_driver_unregister(&driver);
}
