/*
 *
 *  oFono - Open Source Telephony
 *
 *  Copyright (C) 2016  EndoCode AG. All rights reserved.
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
#include <stdbool.h>
#include <errno.h>

#include <glib.h>

#include <ofono/log.h>
#include <ofono/modem.h>
#include <ofono/gprs-context.h>

#include "gatchat.h"
#include "gatresult.h"

#include "ubloxmodem.h"

static const char *none_prefix[] = { NULL };
static const char *cgcontrdp_prefix[] = { "+CGCONTRDP:", NULL };
static const char *uipaddr_prefix[] = { "+UIPADDR:", NULL };

struct gprs_context_data {
	GAtChat *chat;
	unsigned int active_context;
	ofono_gprs_context_cb_t cb;
	void *cb_data;
};

static void uipaddr_cb(gboolean ok, GAtResult *result, gpointer user_data)
{
	struct ofono_gprs_context *gc = user_data;
	struct gprs_context_data *gcd = ofono_gprs_context_get_data(gc);
	GAtResultIter iter;

	const char *gw = NULL;
	const char *netmask = NULL;

	DBG("ok %d", ok);

	if (!ok) {
		CALLBACK_WITH_SUCCESS(gcd->cb, gcd->cb_data);
		return;
	}

	g_at_result_iter_init(&iter, result);

	while (g_at_result_iter_next(&iter, "+UIPADDR:")) {
		g_at_result_iter_skip_next(&iter);
		g_at_result_iter_skip_next(&iter);

		if (!g_at_result_iter_next_string(&iter, &gw))
			break;

		if (!g_at_result_iter_next_string(&iter, &netmask))
			break;
	}

	if (gw)
		ofono_gprs_context_set_ipv4_gateway(gc, gw);

	if (netmask)
		ofono_gprs_context_set_ipv4_netmask(gc, netmask);

	CALLBACK_WITH_SUCCESS(gcd->cb, gcd->cb_data);
}

/*
 * CGCONTRDP returns addr + netmask in the same string in the form
 * of "a.b.c.d.m.m.m.m" for IPv4. IPv6 is not supported so we ignore it.
 */
static int set_address_and_netmask(struct ofono_gprs_context *gc,
				const char *addrnetmask)
{
	char *dup = strdup(addrnetmask);
	char *s = dup;

	const char *addr = s;
	const char *netmask = NULL;

	int ret = -EINVAL;
	int i;

	/* Count 7 dots for ipv4, less or more means error. */
	for (i = 0; i < 8; i++, s++) {
		s = strchr(s, '.');

		if (!s)
			break;

		if (i == 3) {
			/* set netmask ptr and break the string */
			netmask = s + 1;
			s[0] = 0;
		}
	}

	if (i == 7) {
		ofono_gprs_context_set_ipv4_address(gc, addr, 1);
		ofono_gprs_context_set_ipv4_netmask(gc, netmask);

		ret = 0;
	}

	free(dup);

	return ret;
}

static void set_gprs_context_interface(struct ofono_gprs_context *gc)
{
	struct ofono_modem *modem;
	const char *interface;

	/* read interface name read at detection time */
	modem = ofono_gprs_context_get_modem(gc);
	interface = ofono_modem_get_string(modem, "NetworkInterface");
	ofono_gprs_context_set_interface(gc, interface);
}

static void cgcontrdp_cb(gboolean ok, GAtResult *result, gpointer user_data)
{
	struct ofono_gprs_context *gc = user_data;
	struct gprs_context_data *gcd = ofono_gprs_context_get_data(gc);
	GAtResultIter iter;

	const char *laddrnetmask = NULL;
	const char *gw = NULL;
	const char *dns[3] = { NULL, NULL, NULL };
	char buf[64];

	DBG("ok %d", ok);

	if (!ok) {
		struct ofono_error error;

		decode_at_error(&error, g_at_result_final_response(result));
		gcd->cb(&error, gcd->cb_data);

		return;
	}

	g_at_result_iter_init(&iter, result);

	while (g_at_result_iter_next(&iter, "+CGCONTRDP:")) {
		/* skip cid, bearer_id, apn */
		g_at_result_iter_skip_next(&iter);
		g_at_result_iter_skip_next(&iter);
		g_at_result_iter_skip_next(&iter);

		if (!g_at_result_iter_next_string(&iter, &laddrnetmask))
			break;

		if (!g_at_result_iter_next_string(&iter, &gw))
			break;

		if (!g_at_result_iter_next_string(&iter, &dns[0]))
			break;

		if (!g_at_result_iter_next_string(&iter, &dns[1]))
			break;
	}

	set_gprs_context_interface(gc);

	if (!laddrnetmask || set_address_and_netmask(gc, laddrnetmask) < 0) {
		CALLBACK_WITH_FAILURE(gcd->cb, gcd->cb_data);
		return;
	}

	if (gw)
		ofono_gprs_context_set_ipv4_gateway(gc, gw);

	if (dns[0])
		ofono_gprs_context_set_ipv4_dns_servers(gc, dns);

	/*
	 * Some older versions of Toby L2 need to issue AT+UIPADDR to get the
	 * the correct gateway and netmask. The newer version will return an
	 * empty ok reply.
	 */
	snprintf(buf, sizeof(buf), "AT+UIPADDR=%u", gcd->active_context);
	if (g_at_chat_send(gcd->chat, buf, uipaddr_prefix,
				uipaddr_cb, gc, NULL) > 0)
		return;

	/* Even if UIPADDR failed, we still have enough data. */
	CALLBACK_WITH_SUCCESS(gcd->cb, gcd->cb_data);
}

static int ublox_send_cgcontrdp(struct ofono_gprs_context *gc)
{
	struct gprs_context_data *gcd = ofono_gprs_context_get_data(gc);
	char buf[64];

	/* read ip configuration info */
	snprintf(buf, sizeof(buf), "AT+CGCONTRDP=%u", gcd->active_context);
	return g_at_chat_send(gcd->chat, buf, cgcontrdp_prefix,
				cgcontrdp_cb, gc, NULL);
}

static void ublox_read_settings(struct ofono_gprs_context *gc)
{
	struct gprs_context_data *gcd = ofono_gprs_context_get_data(gc);

	if (ublox_send_cgcontrdp(gc) < 0)
		CALLBACK_WITH_FAILURE(gcd->cb, gcd->cb_data);
}

static void ublox_gprs_read_settings(struct ofono_gprs_context *gc,
					unsigned int cid,
					ofono_gprs_context_cb_t cb, void *data)
{
	struct gprs_context_data *gcd = ofono_gprs_context_get_data(gc);

	DBG("cid %u", cid);

	gcd->active_context = cid;
	gcd->cb = cb;
	gcd->cb_data = data;

	ublox_read_settings(gc);
}

static void cgact_enable_cb(gboolean ok, GAtResult *result, gpointer user_data)
{
	struct ofono_gprs_context *gc = user_data;
	struct gprs_context_data *gcd = ofono_gprs_context_get_data(gc);

	DBG("ok %d", ok);

	if (!ok) {
		struct ofono_error error;

		gcd->active_context = 0;

		decode_at_error(&error, g_at_result_final_response(result));
		gcd->cb(&error, gcd->cb_data);

		return;
	}

	ublox_read_settings(gc);
}

static void cgdcont_cb(gboolean ok, GAtResult *result, gpointer user_data)
{
	struct ofono_gprs_context *gc = user_data;
	struct gprs_context_data *gcd = ofono_gprs_context_get_data(gc);
	char buf[64];

	DBG("ok %d", ok);

	if (!ok) {
		struct ofono_error error;

		gcd->active_context = 0;

		decode_at_error(&error, g_at_result_final_response(result));
		gcd->cb(&error, gcd->cb_data);

		return;
	}

	snprintf(buf, sizeof(buf), "AT+CGACT=1,%u", gcd->active_context);

	if (g_at_chat_send(gcd->chat, buf, none_prefix,
				cgact_enable_cb, gc, NULL))
		return;

	CALLBACK_WITH_FAILURE(gcd->cb, gcd->cb_data);
}

#define UBLOX_MAX_USER_LEN 50
#define UBLOX_MAX_PASS_LEN 50

static void ublox_send_uauthreq(struct ofono_gprs_context *gc,
				const char *username, const char *password,
				enum ofono_gprs_auth_method auth_method)

{
	struct gprs_context_data *gcd = ofono_gprs_context_get_data(gc);
	char buf[UBLOX_MAX_USER_LEN + UBLOX_MAX_PASS_LEN + 32];
	unsigned auth;

	switch (auth_method) {
	case OFONO_GPRS_AUTH_METHOD_PAP:
		auth = 1;
		break;
	case OFONO_GPRS_AUTH_METHOD_ANY:
	case OFONO_GPRS_AUTH_METHOD_CHAP:
		auth = 2;
		break;
	default:
		ofono_error("Unsupported auth type %u", auth_method);
		return;
	}

	snprintf(buf, sizeof(buf), "AT+UAUTHREQ=%u,%u,\"%s\",\"%s\"",
			gcd->active_context, auth, username, password);

	/* If this failed, we will see it during context activation. */
	g_at_chat_send(gcd->chat, buf, none_prefix, NULL, NULL, NULL);
}

static void ublox_send_cgdcont(struct ofono_gprs_context *gc, const char *apn,
				const char *username, const char *password,
				enum ofono_gprs_auth_method auth_method)
{
	struct gprs_context_data *gcd = ofono_gprs_context_get_data(gc);
	char buf[OFONO_GPRS_MAX_APN_LENGTH + 128];
	size_t u_len, p_len;
	int len;

	len = snprintf(buf, sizeof(buf), "AT+CGDCONT=%u,\"IP\"",
				gcd->active_context);

	if (apn)
		snprintf(buf + len, sizeof(buf) - len - 3, ",\"%s\"", apn);

	if (g_at_chat_send(gcd->chat, buf, none_prefix,
				cgdcont_cb, gc, NULL) == 0)
		goto error;

	u_len = strlen(username);
	p_len = strlen(password);

	if (u_len && p_len) {
		if (u_len >= UBLOX_MAX_USER_LEN ||
			p_len >= UBLOX_MAX_PASS_LEN) {
			ofono_error("Toby L2: user or password length too big");

			goto error;
		}

		ublox_send_uauthreq(gc, username, password, auth_method);
	}

	return;

error:
	CALLBACK_WITH_FAILURE(gcd->cb, gcd->cb_data);
}

static void ublox_gprs_activate_primary(struct ofono_gprs_context *gc,
				const struct ofono_gprs_primary_context *ctx,
				ofono_gprs_context_cb_t cb, void *data)
{
	struct gprs_context_data *gcd = ofono_gprs_context_get_data(gc);

	/* IPv6 support not implemented */
	if (ctx->proto != OFONO_GPRS_PROTO_IP) {
		CALLBACK_WITH_FAILURE(cb, data);
		return;
	}

	DBG("cid %u", ctx->cid);

	gcd->active_context = ctx->cid;

	if (!gcd->active_context) {
		ofono_error("can't activate more contexts");
		CALLBACK_WITH_FAILURE(cb, data);
		return;
	}

	gcd->cb = cb;
	gcd->cb_data = data;

	ublox_send_cgdcont(gc, ctx->apn, ctx->username, ctx->password,
				ctx->auth_method);
}

static void cgact_disable_cb(gboolean ok, GAtResult *result, gpointer user_data)
{
	struct ofono_gprs_context *gc = user_data;
	struct gprs_context_data *gcd = ofono_gprs_context_get_data(gc);

	DBG("ok %d", ok);

	if (!ok) {
		CALLBACK_WITH_FAILURE(gcd->cb, gcd->cb_data);
		return;
	}

	gcd->active_context = 0;

	CALLBACK_WITH_SUCCESS(gcd->cb, gcd->cb_data);
}

static void ublox_gprs_deactivate_primary(struct ofono_gprs_context *gc,
					unsigned int cid,
					ofono_gprs_context_cb_t cb, void *data)
{
	struct gprs_context_data *gcd = ofono_gprs_context_get_data(gc);
	char buf[64];

	DBG("cid %u", cid);

	gcd->cb = cb;
	gcd->cb_data = data;

	snprintf(buf, sizeof(buf), "AT+CGACT=0,%u", gcd->active_context);
	g_at_chat_send(gcd->chat, buf, none_prefix,
			cgact_disable_cb, gc, NULL);
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
	else if (g_str_has_prefix(event, "NW DEACT"))
		sscanf(event, "%*s %*s %u", &cid);
	else
		return;

	DBG("cid %d", cid);

	if ((unsigned int) cid != gcd->active_context)
		return;

	ofono_gprs_context_deactivated(gc, gcd->active_context);
	gcd->active_context = 0;
}

static int ublox_gprs_context_probe(struct ofono_gprs_context *gc,
					unsigned int vendor, void *data)
{
	GAtChat *chat = data;
	struct gprs_context_data *gcd;

	DBG("");

	gcd = g_try_new0(struct gprs_context_data, 1);
	if (gcd == NULL)
		return -ENOMEM;

	gcd->chat = g_at_chat_clone(chat);

	ofono_gprs_context_set_data(gc, gcd);

	g_at_chat_register(chat, "+CGEV:", cgev_notify, FALSE, gc, NULL);

	return 0;
}

static void ublox_gprs_context_remove(struct ofono_gprs_context *gc)
{
	struct gprs_context_data *gcd = ofono_gprs_context_get_data(gc);

	DBG("");

	ofono_gprs_context_set_data(gc, NULL);

	g_at_chat_unref(gcd->chat);

	memset(gcd, 0, sizeof(*gcd));
	g_free(gcd);
}

static struct ofono_gprs_context_driver driver = {
	.name			= "ubloxmodem",
	.probe			= ublox_gprs_context_probe,
	.remove			= ublox_gprs_context_remove,
	.activate_primary	= ublox_gprs_activate_primary,
	.deactivate_primary	= ublox_gprs_deactivate_primary,
	.read_settings		= ublox_gprs_read_settings,
};

void ublox_gprs_context_init(void)
{
	ofono_gprs_context_driver_register(&driver);
}

void ublox_gprs_context_exit(void)
{
	ofono_gprs_context_driver_unregister(&driver);
}
