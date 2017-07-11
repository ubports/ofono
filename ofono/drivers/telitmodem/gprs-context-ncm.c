/*
 *
 *  oFono - Open Source Telephony
 *
 *  Copyright (C) 2017 Piotr Haber. All rights reserved.
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
 */

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#define _GNU_SOURCE
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

#include "telitmodem.h"

static const char *none_prefix[] = { NULL };
static const char *cgpaddr_prefix[] = { "+CGPADDR:", NULL };
static const char *cgcontrdp_prefix[] = { "+CGCONTRDP:", NULL };

enum state {
	STATE_IDLE,
	STATE_ENABLING,
	STATE_DISABLING,
	STATE_ACTIVE,
};

enum auth_method {
	AUTH_METHOD_NONE,
	AUTH_METHOD_PAP,
	AUTH_METHOD_CHAP,
};

struct gprs_context_data {
	GAtChat *chat;
	unsigned int active_context;
	char username[OFONO_GPRS_MAX_USERNAME_LENGTH + 1];
	char password[OFONO_GPRS_MAX_PASSWORD_LENGTH + 1];
	enum auth_method auth_method;
	enum state state;
	enum ofono_gprs_proto proto;
	char address[64];
	char netmask[64];
	char gateway[64];
	char dns1[64];
	char dns2[64];
	ofono_gprs_context_cb_t cb;
	void *cb_data;
};

static void failed_setup(struct ofono_gprs_context *gc,
				GAtResult *result, gboolean deactivate)
{
	struct gprs_context_data *gcd = ofono_gprs_context_get_data(gc);
	struct ofono_error error;
	char buf[64];

	DBG("deactivate %d", deactivate);

	if (deactivate == TRUE) {
		sprintf(buf, "AT+CGACT=0,%u", gcd->active_context);
		g_at_chat_send(gcd->chat, buf, none_prefix, NULL, NULL, NULL);
	}

	gcd->active_context = 0;
	gcd->state = STATE_IDLE;

	if (result == NULL) {
		CALLBACK_WITH_FAILURE(gcd->cb, gcd->cb_data);
		return;
	}

	decode_at_error(&error, g_at_result_final_response(result));
	gcd->cb(&error, gcd->cb_data);
}

static void session_cb(gboolean ok, GAtResult *result, gpointer user_data)
{
	struct ofono_gprs_context *gc = user_data;
	struct gprs_context_data *gcd = ofono_gprs_context_get_data(gc);
	struct ofono_modem *modem;
	const char *interface;
	const char *dns[3];

	DBG("ok %d", ok);

	if (!ok) {
		ofono_error("Failed to establish session");
		failed_setup(gc, result, TRUE);
		return;
	}

	gcd->state = STATE_ACTIVE;

	dns[0] = gcd->dns1;
	dns[1] = gcd->dns2;
	dns[2] = 0;

	modem = ofono_gprs_context_get_modem(gc);
	interface = ofono_modem_get_string(modem, "NetworkInterface");

	ofono_gprs_context_set_interface(gc, interface);
	ofono_gprs_context_set_ipv4_address(gc, gcd->address, TRUE);
	ofono_gprs_context_set_ipv4_netmask(gc, gcd->netmask);
	ofono_gprs_context_set_ipv4_gateway(gc, gcd->gateway);
	ofono_gprs_context_set_ipv4_dns_servers(gc, dns);

	CALLBACK_WITH_SUCCESS(gcd->cb, gcd->cb_data);
}

static void contrdp_cb(gboolean ok, GAtResult *result, gpointer user_data)
{
	struct ofono_gprs_context *gc = user_data;
	struct gprs_context_data *gcd = ofono_gprs_context_get_data(gc);
	char buf[64];
	int cid, bearer_id;
	const char *apn, *ip_mask, *gw;
	const char *dns1, *dns2;
	GAtResultIter iter;
	gboolean found = FALSE;

	DBG("ok %d", ok);

	if (!ok) {
		ofono_error("Unable to get context dynamic paramerers");
		failed_setup(gc, result, TRUE);
		return;
	}

	g_at_result_iter_init(&iter, result);

	while (g_at_result_iter_next(&iter, "+CGCONTRDP:")) {
		if (!g_at_result_iter_next_number(&iter, &cid))
			goto error;
		if (!g_at_result_iter_next_number(&iter, &bearer_id))
			goto error;
		if (!g_at_result_iter_next_string(&iter, &apn))
			goto error;
		if (!g_at_result_iter_next_string(&iter, &ip_mask))
			goto error;
		if (!g_at_result_iter_next_string(&iter, &gw))
			goto error;
		if (!g_at_result_iter_next_string(&iter, &dns1))
			goto error;
		if (!g_at_result_iter_next_string(&iter, &dns2))
			goto error;

		if ((unsigned int) cid == gcd->active_context) {
			found = TRUE;

			if (strcmp(gcd->address, "") != 0)
				strncpy(gcd->netmask,
					&ip_mask[strlen(gcd->address) + 1],
					sizeof(gcd->netmask));

			strncpy(gcd->gateway, gw, sizeof(gcd->gateway));
			strncpy(gcd->dns1, dns1, sizeof(gcd->dns1));
			strncpy(gcd->dns2, dns2, sizeof(gcd->dns2));
		}
	}

	if (found == FALSE)
		goto error;

	ofono_info("IP: %s", gcd->address);
	ofono_info("MASK: %s", gcd->netmask);
	ofono_info("GW: %s", gcd->gateway);
	ofono_info("DNS: %s, %s", gcd->dns1, gcd->dns2);

	sprintf(buf, "AT+CGDATA=\"M-RAW_IP\",%d", gcd->active_context);
	if (g_at_chat_send(gcd->chat, buf, none_prefix,
					session_cb, gc, NULL) > 0)
		return;

error:
	failed_setup(gc, NULL, TRUE);
}

static void address_cb(gboolean ok, GAtResult *result, gpointer user_data)
{
	struct ofono_gprs_context *gc = user_data;
	struct gprs_context_data *gcd = ofono_gprs_context_get_data(gc);
	int cid;
	const char *address;
	char buf[64];
	GAtResultIter iter;

	DBG("ok %d", ok);

	if (!ok) {
		ofono_error("Unable to get context address");
		failed_setup(gc, result, TRUE);
		return;
	}

	g_at_result_iter_init(&iter, result);

	if (!g_at_result_iter_next(&iter, "+CGPADDR:"))
		goto error;

	if (!g_at_result_iter_next_number(&iter, &cid))
		goto error;

	if ((unsigned int) cid != gcd->active_context)
		goto error;

	if (!g_at_result_iter_next_string(&iter, &address))
		goto error;

	strncpy(gcd->address, address, sizeof(gcd->address));

	sprintf(buf, "AT+CGCONTRDP=%d", gcd->active_context);
	if (g_at_chat_send(gcd->chat, buf, cgcontrdp_prefix,
					contrdp_cb, gc, NULL) > 0)
		return;

error:
	failed_setup(gc, NULL, TRUE);
}

static void activate_cb(gboolean ok, GAtResult *result, gpointer user_data)
{
	struct ofono_gprs_context *gc = user_data;
	struct gprs_context_data *gcd = ofono_gprs_context_get_data(gc);
	char buf[64];

	DBG("ok %d", ok);

	if (!ok) {
		ofono_error("Unable to activate context");
		failed_setup(gc, result, FALSE);
		return;
	}

	sprintf(buf, "AT+CGPADDR=%u", gcd->active_context);
	if (g_at_chat_send(gcd->chat, buf, cgpaddr_prefix,
					address_cb, gc, NULL) > 0)
		return;

	failed_setup(gc, NULL, TRUE);
}

static void setup_cb(gboolean ok, GAtResult *result, gpointer user_data)
{
	struct ofono_gprs_context *gc = user_data;
	struct gprs_context_data *gcd = ofono_gprs_context_get_data(gc);
	char buf[384];

	DBG("ok %d", ok);

	if (!ok) {
		ofono_error("Failed to setup context");
		failed_setup(gc, result, FALSE);
		return;
	}

	if (gcd->username[0] && gcd->password[0])
		sprintf(buf, "AT#PDPAUTH=%u,%u,\"%s\",\"%s\"",
			gcd->active_context, gcd->auth_method,
			gcd->username, gcd->password);
	else
		sprintf(buf, "AT#PDPAUTH=%u,0", gcd->active_context);

	if (g_at_chat_send(gcd->chat, buf, none_prefix, NULL, NULL, NULL) == 0)
		goto error;

	sprintf(buf, "AT#NCM=1,%u", gcd->active_context);

	if (g_at_chat_send(gcd->chat, buf, none_prefix, NULL, NULL, NULL) == 0)
		goto error;

	sprintf(buf, "AT+CGACT=1,%u", gcd->active_context);

	if (g_at_chat_send(gcd->chat, buf, none_prefix,
				activate_cb, gc, NULL) > 0)
		return;

error:
	failed_setup(gc, NULL, FALSE);
}

static void telitncm_gprs_activate_primary(struct ofono_gprs_context *gc,
				const struct ofono_gprs_primary_context *ctx,
				ofono_gprs_context_cb_t cb, void *data)
{
	struct gprs_context_data *gcd = ofono_gprs_context_get_data(gc);
	char buf[OFONO_GPRS_MAX_APN_LENGTH + 128];
	int len = 0;

	DBG("cid %u", ctx->cid);

	gcd->active_context = ctx->cid;
	gcd->cb = cb;
	gcd->cb_data = data;
	memcpy(gcd->username, ctx->username, sizeof(ctx->username));
	memcpy(gcd->password, ctx->password, sizeof(ctx->password));
	gcd->state = STATE_ENABLING;
	gcd->proto = ctx->proto;

	/* We only support CHAP and PAP */
	switch (ctx->auth_method) {
	case OFONO_GPRS_AUTH_METHOD_CHAP:
		gcd->auth_method = AUTH_METHOD_CHAP;
		break;
	case OFONO_GPRS_AUTH_METHOD_PAP:
		gcd->auth_method = AUTH_METHOD_PAP;
		break;
	default:
		goto error;
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
		snprintf(buf + len, sizeof(buf) - len - 3,
					",\"%s\"", ctx->apn);

	if (g_at_chat_send(gcd->chat, buf, none_prefix,
				setup_cb, gc, NULL) > 0)
		return;

error:
	CALLBACK_WITH_FAILURE(cb, data);
}

static void deactivate_cb(gboolean ok, GAtResult *result, gpointer user_data)
{
	struct ofono_gprs_context *gc = user_data;
	struct gprs_context_data *gcd = ofono_gprs_context_get_data(gc);

	DBG("ok %d", ok);

	gcd->active_context = 0;
	gcd->state = STATE_IDLE;

	CALLBACK_WITH_SUCCESS(gcd->cb, gcd->cb_data);
}

static void telitncm_gprs_deactivate_primary(struct ofono_gprs_context *gc,
					unsigned int cid,
					ofono_gprs_context_cb_t cb, void *data)
{
	struct gprs_context_data *gcd = ofono_gprs_context_get_data(gc);
	char buf[64];

	DBG("cid %u", cid);

	gcd->state = STATE_DISABLING;
	gcd->cb = cb;
	gcd->cb_data = data;

	sprintf(buf, "AT+CGACT=0,%u", gcd->active_context);

	if (g_at_chat_send(gcd->chat, buf, none_prefix,
				deactivate_cb, gc, NULL) > 0)
		return;

	CALLBACK_WITH_SUCCESS(cb, data);
}

static void cgev_notify(GAtResult *result, gpointer user_data)
{
	struct ofono_gprs_context *gc = user_data;
	struct gprs_context_data *gcd = ofono_gprs_context_get_data(gc);
	const char *event;
	int cid;
	GAtResultIter iter;

	g_at_result_iter_init(&iter, result);

	if (!g_at_result_iter_next(&iter, "+CGEV:"))
		return;

	if (!g_at_result_iter_next_unquoted_string(&iter, &event))
		return;

	if (g_str_has_prefix(event, "NW DEACT") == FALSE)
		return;

	if (!g_at_result_iter_skip_next(&iter))
		return;

	if (!g_at_result_iter_next_number(&iter, &cid))
		return;

	DBG("cid %d", cid);

	if ((unsigned int) cid != gcd->active_context)
		return;

	ofono_gprs_context_deactivated(gc, gcd->active_context);

	gcd->active_context = 0;
	gcd->state = STATE_IDLE;
}

static int telitncm_gprs_context_probe(struct ofono_gprs_context *gc,
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

static void telitncm_gprs_context_remove(struct ofono_gprs_context *gc)
{
	struct gprs_context_data *gcd = ofono_gprs_context_get_data(gc);

	DBG("");

	ofono_gprs_context_set_data(gc, NULL);

	g_at_chat_unref(gcd->chat);
	g_free(gcd);
}

static struct ofono_gprs_context_driver driver = {
	.name			= "telitncmmodem",
	.probe			= telitncm_gprs_context_probe,
	.remove			= telitncm_gprs_context_remove,
	.activate_primary	= telitncm_gprs_activate_primary,
	.deactivate_primary	= telitncm_gprs_deactivate_primary,
};

void telitncm_gprs_context_init(void)
{
	ofono_gprs_context_driver_register(&driver);
}

void telitncm_gprs_context_exit(void)
{
	ofono_gprs_context_driver_unregister(&driver);
}
