/*
 *
 *  oFono - Open Source Telephony
 *
 *  Copyright (C) 2020 Sergey Matyukevich. All rights reserved.
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
#include <stdlib.h>
#include <stdio.h>
#include <errno.h>
#include <sys/stat.h>

#include <glib.h>

#include <ofono/log.h>
#include <ofono/modem.h>
#include <ofono/gprs-context.h>

#include "gatchat.h"
#include "gatresult.h"
#include "gattty.h"

#include "gemaltomodem.h"

static const char *none_prefix[] = { NULL };

struct gprs_context_data {
	GAtChat *chat;
	unsigned int active_context;
	char username[OFONO_GPRS_MAX_USERNAME_LENGTH + 1];
	char password[OFONO_GPRS_MAX_PASSWORD_LENGTH + 1];
	int auth_type;
	enum ofono_gprs_proto proto;
	ofono_gprs_context_cb_t cb;
	void *cb_data;
};

static void set_gprs_context_interface(struct ofono_gprs_context *gc)
{
	struct ofono_modem *modem;
	const char *interface;

	modem = ofono_gprs_context_get_modem(gc);
	interface = ofono_modem_get_string(modem, "NetworkInterface");
	ofono_gprs_context_set_interface(gc, interface);

	/* Use DHCP */
	ofono_gprs_context_set_ipv4_address(gc, NULL, 0);
}

static void swwan_cb(gboolean ok, GAtResult *result, gpointer user_data)
{
	struct ofono_gprs_context *gc = user_data;
	struct gprs_context_data *gcd = ofono_gprs_context_get_data(gc);
	struct ofono_error error;

	DBG("ok %d", ok);

	if (!ok) {
		ofono_error("Unable to activate context");
		ofono_gprs_context_deactivated(gc, gcd->active_context);
		gcd->active_context = 0;
		decode_at_error(&error, g_at_result_final_response(result));
		gcd->cb(&error, gcd->cb_data);
		return;
	}
}

static void sgauth_enable_cb(gboolean ok, GAtResult *result,
			gpointer user_data)
{
	struct ofono_gprs_context *gc = user_data;
	struct gprs_context_data *gcd = ofono_gprs_context_get_data(gc);
	struct ofono_error error;
	char buf[64];

	DBG("ok %d", ok);

	if (!ok) {
		gcd->active_context = 0;
		decode_at_error(&error, g_at_result_final_response(result));
		gcd->cb(&error, gcd->cb_data);
		return;
	}

	snprintf(buf, sizeof(buf), "AT^SWWAN=1,%u", gcd->active_context);

	if (g_at_chat_send(gcd->chat, buf, none_prefix, swwan_cb, gc, NULL)) {
		set_gprs_context_interface(gc);

		CALLBACK_WITH_SUCCESS(gcd->cb, gcd->cb_data);
		return;
	}

	CALLBACK_WITH_FAILURE(gcd->cb, gcd->cb_data);
}

static void cgdcont_enable_cb(gboolean ok, GAtResult *result,
				gpointer user_data)
{
	struct ofono_gprs_context *gc = user_data;
	struct gprs_context_data *gcd = ofono_gprs_context_get_data(gc);
	struct ofono_error error;
	char buf[384];

	DBG("ok %d", ok);

	if (!ok) {
		gcd->active_context = 0;
		decode_at_error(&error, g_at_result_final_response(result));
		gcd->cb(&error, gcd->cb_data);
		return;
	}

	if (gcd->username[0] && gcd->password[0])
		sprintf(buf, "AT^SGAUTH=%u,%u,\"%s\",\"%s\"",
			gcd->active_context, gcd->auth_type,
			gcd->username, gcd->password);
	else
		sprintf(buf, "AT^SGAUTH=%u,%u,\"\",\"\"",
			gcd->active_context, gcd->auth_type);

	if (g_at_chat_send(gcd->chat, buf, none_prefix,
				sgauth_enable_cb, gc, NULL) > 0)
		return;

	CALLBACK_WITH_FAILURE(gcd->cb, gcd->cb_data);
}

static void gemalto_gprs_activate_primary(struct ofono_gprs_context *gc,
				const struct ofono_gprs_primary_context *ctx,
				ofono_gprs_context_cb_t cb, void *data)
{
	struct gprs_context_data *gcd = ofono_gprs_context_get_data(gc);
	char buf[OFONO_GPRS_MAX_APN_LENGTH + 128];
	int len = 0;

	DBG("cid %u", ctx->cid);

	gcd->active_context = ctx->cid;
	gcd->cb_data = data;
	gcd->cb = cb;

	memcpy(gcd->username, ctx->username, sizeof(ctx->username));
	memcpy(gcd->password, ctx->password, sizeof(ctx->password));
	gcd->proto = ctx->proto;

	switch (ctx->auth_method) {
	case OFONO_GPRS_AUTH_METHOD_PAP:
		gcd->auth_type = 1;
		break;
	case OFONO_GPRS_AUTH_METHOD_CHAP:
		gcd->auth_type = 2;
		break;
	case OFONO_GPRS_AUTH_METHOD_NONE:
	default:
		gcd->auth_type = 0;
		break;
	}

	switch (ctx->proto) {
	case OFONO_GPRS_PROTO_IP:
		len = snprintf(buf, sizeof(buf), "AT+CGDCONT=%u,\"IP\"",
				ctx->cid);
		break;
	case OFONO_GPRS_PROTO_IPV6:
		len = snprintf(buf, sizeof(buf), "AT+CGDCONT=%u,\"IPV6\"",
				ctx->cid);
		break;
	case OFONO_GPRS_PROTO_IPV4V6:
		len = snprintf(buf, sizeof(buf), "AT+CGDCONT=%u,\"IPV4V6\"",
				ctx->cid);
		break;
	}

	if (ctx->apn)
		snprintf(buf + len, sizeof(buf) - len - 3, ",\"%s\"", ctx->apn);

	if (g_at_chat_send(gcd->chat, buf, none_prefix,
				cgdcont_enable_cb, gc, NULL))
		return;

	CALLBACK_WITH_FAILURE(cb, data);
}

static void deactivate_cb(gboolean ok, GAtResult *result,
		gpointer user_data)
{
	struct ofono_gprs_context *gc = user_data;
	struct gprs_context_data *gcd = ofono_gprs_context_get_data(gc);

	DBG("ok %d", ok);

	gcd->active_context = 0;

	if (!ok) {
		CALLBACK_WITH_FAILURE(gcd->cb, gcd->cb_data);
		return;
	}

	CALLBACK_WITH_SUCCESS(gcd->cb, gcd->cb_data);
}

static void gemalto_gprs_deactivate_primary(struct ofono_gprs_context *gc,
						unsigned int cid,
						ofono_gprs_context_cb_t cb,
						void *data)
{
	struct gprs_context_data *gcd = ofono_gprs_context_get_data(gc);
	char buf[64];

	DBG("cid %u", cid);

	gcd->cb = cb;
	gcd->cb_data = data;

	snprintf(buf, sizeof(buf), "AT^SWWAN=0,%u", gcd->active_context);

	if (g_at_chat_send(gcd->chat, buf, none_prefix,
				deactivate_cb, gc, NULL))
		return;

	CALLBACK_WITH_FAILURE(cb, data);
}

static void gemalto_gprs_read_settings(struct ofono_gprs_context *gc,
					unsigned int cid,
					ofono_gprs_context_cb_t cb, void *data)
{
	struct gprs_context_data *gcd = ofono_gprs_context_get_data(gc);
	char buf[64];

	DBG("cid %u", cid);

	gcd->active_context = cid;
	gcd->cb = cb;
	gcd->cb_data = data;

	/*
	 * AT^SWWAN command activates PDP context unless it has been already
	 * activated automatically, and then starts DHCP server in the ME.
	 * So AT^SWWAN command should be run in both cases:
	 * - when activate context and then obtain IP address from the ME
	 * - when obtain IP address from the automatically activated context
	 *
	 * Note that the ME waits until DHCP negotiation has finished before
	 * sending the "OK" or "ERROR" result code. So success is reported
	 * to the core before AT^SWWAN response.
	 */
	snprintf(buf, sizeof(buf), "AT^SWWAN=1,%u", gcd->active_context);

	if (g_at_chat_send(gcd->chat, buf, none_prefix, swwan_cb, gc, NULL)) {
		set_gprs_context_interface(gc);

		CALLBACK_WITH_SUCCESS(gcd->cb, gcd->cb_data);
		return;
	}

	CALLBACK_WITH_FAILURE(gcd->cb, gcd->cb_data);
}

static void cgev_notify(GAtResult *result, gpointer user_data)
{
	struct ofono_gprs_context *gc = user_data;
	struct gprs_context_data *gcd = ofono_gprs_context_get_data(gc);
	GAtResultIter iter;
	const char *event;
	gint cid;

	g_at_result_iter_init(&iter, result);

	if (!g_at_result_iter_next(&iter, "+CGEV:"))
		return;

	if (!g_at_result_iter_next_unquoted_string(&iter, &event))
		return;

	if (g_str_has_prefix(event, "NW PDN DEACT"))
		sscanf(event, "%*s %*s %*s %u", &cid);
	else if (g_str_has_prefix(event, "ME PDN DEACT"))
		sscanf(event, "%*s %*s %*s %u", &cid);
	else if (g_str_has_prefix(event, "NW DEACT"))
		sscanf(event, "%*s %*s %u", &cid);
	else
		return;

	DBG("cid %d, active cid: %d", cid, gcd->active_context);

	if ((unsigned int) cid != gcd->active_context)
		return;

	ofono_gprs_context_deactivated(gc, gcd->active_context);
	gcd->active_context = 0;
}

static int gemalto_gprs_context_probe(struct ofono_gprs_context *gc,
					unsigned int vendor, void *data)
{
	GAtChat *chat = data;
	struct gprs_context_data *gcd;

	DBG("");

	gcd = g_new0(struct gprs_context_data, 1);

	gcd->chat = g_at_chat_clone(chat);

	ofono_gprs_context_set_data(gc, gcd);
	g_at_chat_register(chat, "+CGEV:", cgev_notify, FALSE, gc, NULL);

	return 0;
}

static void gemalto_gprs_context_remove(struct ofono_gprs_context *gc)
{
	struct gprs_context_data *gcd = ofono_gprs_context_get_data(gc);

	DBG("");

	ofono_gprs_context_set_data(gc, NULL);
	g_at_chat_unref(gcd->chat);
	g_free(gcd);
}

static const struct ofono_gprs_context_driver driver = {
	.name			= "gemaltomodem",
	.probe			= gemalto_gprs_context_probe,
	.remove			= gemalto_gprs_context_remove,
	.activate_primary	= gemalto_gprs_activate_primary,
	.deactivate_primary	= gemalto_gprs_deactivate_primary,
	.read_settings		= gemalto_gprs_read_settings,
};

void gemalto_gprs_context_init(void)
{
	ofono_gprs_context_driver_register(&driver);
}

void gemalto_gprs_context_exit(void)
{
	ofono_gprs_context_driver_unregister(&driver);
}
