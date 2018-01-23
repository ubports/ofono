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
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <errno.h>

#include <glib.h>

#include <ofono/log.h>
#include <ofono/modem.h>
#include <ofono/radio-settings.h>

#include "gatchat.h"
#include "gatresult.h"

#include "xmm7modem.h"

static const char *none_prefix[] = { NULL };
static const char *xact_prefix[] = { "+XACT:", NULL };

struct radio_settings_data {
	GAtChat *chat;
};

static void xact_query_cb(gboolean ok, GAtResult *result, gpointer user_data)
{
	struct cb_data *cbd = user_data;
	ofono_radio_settings_rat_mode_query_cb_t cb = cbd->cb;
	enum ofono_radio_access_mode mode;
	struct ofono_error error;
	GAtResultIter iter;
	int value, preferred;

	decode_at_error(&error, g_at_result_final_response(result));

	if (!ok) {
		cb(&error, -1, cbd->data);
		return;
	}

	g_at_result_iter_init(&iter, result);

	if (g_at_result_iter_next(&iter, "+XACT:") == FALSE)
		goto error;

	if (g_at_result_iter_next_number(&iter, &value) == FALSE)
		goto error;

	if (g_at_result_iter_next_number(&iter, &preferred) == FALSE)
		goto error;

	switch (value) {
	case 0:
		mode = OFONO_RADIO_ACCESS_MODE_GSM;
		break;
	case 1:
		mode = OFONO_RADIO_ACCESS_MODE_UMTS;
		break;
	case 2:
		mode = OFONO_RADIO_ACCESS_MODE_LTE;
		break;
	case 3:
		mode = OFONO_RADIO_ACCESS_MODE_UMTS;
		break;
	case 4:
		mode = OFONO_RADIO_ACCESS_MODE_LTE;
		break;
	case 5:
		mode = OFONO_RADIO_ACCESS_MODE_LTE;
		break;
	case 6:
		mode = OFONO_RADIO_ACCESS_MODE_ANY;
		break;
	default:
		CALLBACK_WITH_FAILURE(cb, -1, cbd->data);
		return;
	}

	cb(&error, mode, cbd->data);

	return;

error:
	CALLBACK_WITH_FAILURE(cb, -1, cbd->data);
}

static void xmm_query_rat_mode(struct ofono_radio_settings *rs,
				ofono_radio_settings_rat_mode_query_cb_t cb,
				void *data)
{
	struct radio_settings_data *rsd = ofono_radio_settings_get_data(rs);
	struct cb_data *cbd = cb_data_new(cb, data);

	if (g_at_chat_send(rsd->chat, "AT+XACT?", xact_prefix,
					xact_query_cb, cbd, g_free) > 0)
		return;

	CALLBACK_WITH_FAILURE(cb, -1, data);
	g_free(cbd);
}

static void xact_modify_cb(gboolean ok, GAtResult *result, gpointer user_data)
{
	struct cb_data *cbd = user_data;
	ofono_radio_settings_rat_mode_set_cb_t cb = cbd->cb;
	struct ofono_error error;

	decode_at_error(&error, g_at_result_final_response(result));
	cb(&error, cbd->data);
}

static void xmm_set_rat_mode(struct ofono_radio_settings *rs,
				enum ofono_radio_access_mode mode,
				ofono_radio_settings_rat_mode_set_cb_t cb,
				void *data)
{
	struct radio_settings_data *rsd = ofono_radio_settings_get_data(rs);
	struct cb_data *cbd = cb_data_new(cb, data);
	char buf[20];
	int value = 6, preferred = 2;

	switch (mode) {
	case OFONO_RADIO_ACCESS_MODE_ANY:
		value = 6;
		break;
	case OFONO_RADIO_ACCESS_MODE_GSM:
		value = 0;
		break;
	case OFONO_RADIO_ACCESS_MODE_UMTS:
		value = 1;
		break;
	case OFONO_RADIO_ACCESS_MODE_LTE:
		value = 2;
		break;
	}

	if (value == 6)
		snprintf(buf, sizeof(buf), "AT+XACT=%u,%u", value, preferred);
	else
		snprintf(buf, sizeof(buf), "AT+XACT=%u", value);

	if (g_at_chat_send(rsd->chat, buf, none_prefix,
					xact_modify_cb, cbd, g_free) > 0)
		return;

	CALLBACK_WITH_FAILURE(cb, data);
	g_free(cbd);
}

static void xact_support_cb(gboolean ok, GAtResult *result, gpointer user_data)
{
	struct ofono_radio_settings *rs = user_data;

	if (!ok) {
		ofono_radio_settings_remove(rs);
		return;
	}

	ofono_radio_settings_register(rs);
}

static int xmm_radio_settings_probe(struct ofono_radio_settings *rs,
					unsigned int vendor, void *user)
{
	GAtChat *chat = user;
	struct radio_settings_data *rsd;

	rsd = g_try_new0(struct radio_settings_data, 1);
	if (rsd == NULL)
		return -ENOMEM;

	rsd->chat = g_at_chat_clone(chat);

	ofono_radio_settings_set_data(rs, rsd);

	g_at_chat_send(rsd->chat, "AT+XACT=?", xact_prefix,
					xact_support_cb, rs, NULL);

	return 0;
}

static void xmm_radio_settings_remove(struct ofono_radio_settings *rs)
{
	struct radio_settings_data *rsd = ofono_radio_settings_get_data(rs);

	ofono_radio_settings_set_data(rs, NULL);

	g_at_chat_unref(rsd->chat);
	g_free(rsd);
}

static struct ofono_radio_settings_driver driver = {
	.name			= "xmm7modem",
	.probe			= xmm_radio_settings_probe,
	.remove			= xmm_radio_settings_remove,
	.query_rat_mode		= xmm_query_rat_mode,
	.set_rat_mode		= xmm_set_rat_mode
};

void xmm_radio_settings_init(void)
{
	ofono_radio_settings_driver_register(&driver);
}

void xmm_radio_settings_exit(void)
{
	ofono_radio_settings_driver_unregister(&driver);
}
