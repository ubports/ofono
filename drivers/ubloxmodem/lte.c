/*
 *
 *  oFono - Open Source Telephony
 *
 *  Copyright (C) 2016  Endocode AG. All rights reserved.
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

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>

#include <glib.h>

#include <ofono/modem.h>
#include <ofono/gprs-context.h>
#include <ofono/log.h>
#include <ofono/lte.h>

#include "gatchat.h"
#include "gatresult.h"

#include "ubloxmodem.h"

static const char *none_prefix[] = { NULL };

struct lte_driver_data {
	GAtChat *chat;
	const struct ublox_model *model;
	struct ofono_lte_default_attach_info pending_info;
};

static void at_lte_set_auth_cb(gboolean ok, GAtResult *result,
							gpointer user_data)
{
	struct cb_data *cbd = user_data;
	ofono_lte_cb_t cb = cbd->cb;
	struct ofono_error error;

	decode_at_error(&error, g_at_result_final_response(result));
	cb(&error, cbd->data);
}

static void at_lte_set_default_attach_info_cb(gboolean ok, GAtResult *result,
							gpointer user_data)
{
	struct cb_data *cbd = user_data;
	ofono_lte_cb_t cb = cbd->cb;
	void *data = cbd->data;
	struct lte_driver_data *ldd = cbd->user;
	struct ofono_error error;
	char buf[32 + OFONO_GPRS_MAX_USERNAME_LENGTH +
					OFONO_GPRS_MAX_PASSWORD_LENGTH  + 1];
	enum ofono_gprs_auth_method auth_method;
	int cid;

	if (!ok) {
		decode_at_error(&error, g_at_result_final_response(result));
		cb(&error, data);
		return;
	}

	if (ublox_is_toby_l2(ldd->model)) {
		/* If CGDCONT has already been used to set up cid 4 then
		 * the EPS default bearer will be configured from another
		 * cid (see documentation for how this is selected).  Avoid
		 * doing so as this assumes as much...
		 */
		cid = 4;
	} else if (ublox_is_toby_l4(ldd->model)) {
		cid = 1;
	} else {
		ofono_error("Unknown model; "
			"unable to determine EPS default bearer CID");
		goto out;
	}

	auth_method = ldd->pending_info.auth_method;

	/* change the authentication method if the  parameters are invalid */
	if (!*ldd->pending_info.username || !*ldd->pending_info.password)
		auth_method = OFONO_GPRS_AUTH_METHOD_NONE;

	/* In contrast to CGAUTH, all four parameters are _required_ here;
	 * if auth type is NONE then username and password must be set to
	 * empty strings.
	 */
	sprintf(buf, "AT+UAUTHREQ=%d,%d,\"%s\",\"%s\"",
			cid,
			at_util_gprs_auth_method_to_auth_prot(auth_method),
			ldd->pending_info.username,
			ldd->pending_info.password);

	cbd = cb_data_ref(cbd);
	if (g_at_chat_send(ldd->chat, buf, none_prefix,
			at_lte_set_auth_cb, cbd, cb_data_unref) > 0)
		return;

out:
	cb_data_unref(cbd);
	CALLBACK_WITH_FAILURE(cb, data);
}

static void ublox_lte_set_default_attach_info(const struct ofono_lte *lte,
			const struct ofono_lte_default_attach_info *info,
			ofono_lte_cb_t cb, void *data)
{
	struct lte_driver_data *ldd = ofono_lte_get_data(lte);
	char buf[32 + OFONO_GPRS_MAX_APN_LENGTH + 1];
	struct cb_data *cbd = cb_data_new(cb, data);

	DBG("LTE config with APN: %s", info->apn);

	cbd->user = ldd;
	memcpy(&ldd->pending_info, info, sizeof(ldd->pending_info));

	if (ublox_is_toby_l2(ldd->model)) {
		if (strlen(info->apn) > 0)
			snprintf(buf, sizeof(buf), "AT+UCGDFLT=0,%s,\"%s\"",
				at_util_gprs_proto_to_pdp_type(info->proto),
				info->apn);
		else
			snprintf(buf, sizeof(buf), "AT+UCGDFLT=0");

	} else if (ublox_is_toby_l4(ldd->model)) {
		if (strlen(info->apn) > 0)
			snprintf(buf, sizeof(buf), "AT+CGDCONT=1,%s,\"%s\"",
				at_util_gprs_proto_to_pdp_type(info->proto),
				info->apn);
		else
			snprintf(buf, sizeof(buf), "AT+CGDCONT=1");
	}

	if (g_at_chat_send(ldd->chat, buf, none_prefix,
			at_lte_set_default_attach_info_cb,
			cbd, cb_data_unref) > 0)
		return;

	cb_data_unref(cbd);
	CALLBACK_WITH_FAILURE(cb, data);
}

static gboolean lte_delayed_register(gpointer user_data)
{
	struct ofono_lte *lte = user_data;

	ofono_lte_register(lte);

	return FALSE;
}

static int ublox_lte_probe(struct ofono_lte *lte,
					unsigned int model_id, void *data)
{
	GAtChat *chat = data;
	struct lte_driver_data *ldd;

	DBG("ublox lte probe");

	ldd = g_try_new0(struct lte_driver_data, 1);
	if (!ldd)
		return -ENOMEM;

	ldd->chat = g_at_chat_clone(chat);
	ldd->model = ublox_model_from_id(model_id);

	ofono_lte_set_data(lte, ldd);

	g_idle_add(lte_delayed_register, lte);

	return 0;
}

static void ublox_lte_remove(struct ofono_lte *lte)
{
	struct lte_driver_data *ldd = ofono_lte_get_data(lte);

	DBG("ublox lte remove");

	g_at_chat_unref(ldd->chat);

	ofono_lte_set_data(lte, NULL);

	g_free(ldd);
}

static const struct ofono_lte_driver driver = {
	.name				= UBLOXMODEM,
	.probe				= ublox_lte_probe,
	.remove				= ublox_lte_remove,
	.set_default_attach_info	= ublox_lte_set_default_attach_info,
};

void ublox_lte_init(void)
{
	ofono_lte_driver_register(&driver);
}

void ublox_lte_exit(void)
{
	ofono_lte_driver_unregister(&driver);
}
