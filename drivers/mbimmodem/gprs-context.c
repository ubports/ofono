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

#include <stdint.h>
#include <stdbool.h>
#include <errno.h>
#include <stdlib.h>
#include <arpa/inet.h>

#include <ofono/log.h>
#include <ofono/modem.h>
#include <ofono/gprs-context.h>

#include "drivers/mbimmodem/mbim.h"
#include "drivers/mbimmodem/mbim-message.h"
#include "drivers/mbimmodem/mbimmodem.h"

enum state {
	STATE_IDLE,
	STATE_ENABLING,
	STATE_DISABLING,
	STATE_ACTIVE,
};

struct gprs_context_data {
	struct mbim_device *device;
	unsigned int active_context;
	enum ofono_gprs_proto proto;
	enum state state;
	ofono_gprs_context_cb_t cb;
	void *cb_data;
};

static uint32_t proto_to_context_ip_type(enum ofono_gprs_proto proto)
{
	switch (proto) {
	case OFONO_GPRS_PROTO_IP:
		return 1; /* MBIMContextIPTypeIPv4 */
	case OFONO_GPRS_PROTO_IPV6:
		return 2; /* MBIMContextIPTypeIPv6 */
	case OFONO_GPRS_PROTO_IPV4V6:
		return 3; /* MBIMContextIPTypeIPv4v6 */
	}

	return 0;
}

static uint32_t auth_method_to_auth_protocol(enum ofono_gprs_auth_method method)
{
	switch (method) {
	case OFONO_GPRS_AUTH_METHOD_CHAP:
		return 2; /* MBIMAuthProtocolChap */
	case OFONO_GPRS_AUTH_METHOD_PAP:
		return 1; /* MBIMAuthProtocolPap */
	case OFONO_GPRS_AUTH_METHOD_NONE:
		return 0; /* MBIMAUthProtocolNone */
	}

	return 0; /* MBIMAUthProtocolNone */
}

static void mbim_deactivate_cb(struct mbim_message *message, void *user)
{
	struct ofono_gprs_context *gc = user;
	struct gprs_context_data *gcd = ofono_gprs_context_get_data(gc);

	DBG("");

	gcd->active_context = 0;
	gcd->state = STATE_IDLE;

	if (!gcd->cb)
		return;

	if (mbim_message_get_error(message) != 0)
		CALLBACK_WITH_FAILURE(gcd->cb, gcd->cb_data);
	else
		CALLBACK_WITH_SUCCESS(gcd->cb, gcd->cb_data);
}

static void mbim_gprs_deactivate_primary(struct ofono_gprs_context *gc,
					unsigned int cid,
					ofono_gprs_context_cb_t cb, void *data)
{
	struct gprs_context_data *gcd = ofono_gprs_context_get_data(gc);
	struct mbim_message *message;

	DBG("cid %u", cid);

	gcd->state = STATE_DISABLING;
	gcd->cb = cb;
	gcd->cb_data = data;

	message = mbim_message_new(mbim_uuid_basic_connect,
					MBIM_CID_CONNECT,
					MBIM_COMMAND_TYPE_SET);
	mbim_message_set_arguments(message, "uusssuuu16y",
					cid, 0, NULL, NULL, NULL, 0, 0, 0,
					mbim_context_type_internet);

	if (mbim_device_send(gcd->device, GPRS_CONTEXT_GROUP, message,
				mbim_deactivate_cb, gc, NULL) > 0)
		return;

	mbim_message_unref(message);

	if (cb)
		CALLBACK_WITH_FAILURE(cb, data);
}

static void mbim_ip_configuration_cb(struct mbim_message *message, void *user)
{
	struct ofono_gprs_context *gc = user;
	struct gprs_context_data *gcd = ofono_gprs_context_get_data(gc);
	struct ofono_modem *modem = ofono_gprs_context_get_modem(gc);
	const char *interface;
	uint32_t session_id;
	uint32_t ipv4_config_available;
	uint32_t ipv6_config_available;
	uint32_t n_ipv4_addr;
	uint32_t ipv4_addr_offset;
	uint32_t n_ipv6_addr;
	uint32_t ipv6_addr_offset;
	uint32_t ipv4_gw_offset;
	uint32_t ipv6_gw_offset;
	uint32_t n_ipv4_dns;
	uint32_t ipv4_dns_offset;
	uint32_t n_ipv6_dns;
	uint32_t ipv6_dns_offset;
	uint32_t ipv4_mtu;
	uint32_t ipv6_mtu;

	struct in6_addr ipv6;
	struct in_addr ipv4;
	char buf[INET6_ADDRSTRLEN];

	DBG("%u", mbim_message_get_error(message));

	if (mbim_message_get_error(message) != 0)
		goto error;

	if (!mbim_message_get_arguments(message, "uuuuuuuuuuuuuuu",
				&session_id,
				&ipv4_config_available, &ipv6_config_available,
				&n_ipv4_addr, &ipv4_addr_offset,
				&n_ipv6_addr, &ipv6_addr_offset,
				&ipv4_gw_offset, &ipv6_gw_offset,
				&n_ipv4_dns, &ipv4_dns_offset,
				&n_ipv6_dns, &ipv6_dns_offset,
				&ipv4_mtu, &ipv6_mtu))
		goto error;

	if (gcd->proto == OFONO_GPRS_PROTO_IPV6)
		goto ipv6;

	if (ipv4_config_available & 0x1) { /* Address Info present */
		uint32_t prefix;

		if (!mbim_message_get_ipv4_element(message, ipv4_addr_offset,
							&prefix, &ipv4))
			goto error;

		inet_ntop(AF_INET, &ipv4, buf, sizeof(buf));
		ofono_gprs_context_set_ipv4_address(gc, buf, TRUE);
		ofono_gprs_context_set_ipv4_prefix_length(gc, prefix);
	} else
		ofono_gprs_context_set_ipv4_address(gc, NULL, FALSE);

	if (ipv4_config_available & 0x2) { /* IPv4 Gateway info */
		if (!mbim_message_get_ipv4_address(message,
							ipv4_gw_offset, &ipv4))
			goto error;

		inet_ntop(AF_INET, &ipv4, buf, sizeof(buf));

		ofono_gprs_context_set_ipv4_gateway(gc, buf);
	}

	if (ipv4_config_available & 0x3) { /* IPv4 DNS Info */
		const char *dns[3];
		char dns1[INET_ADDRSTRLEN];
		char dns2[INET_ADDRSTRLEN];

		memset(dns, 0, sizeof(dns));

		if (n_ipv4_dns > 1) { /* Grab second DNS */
			if (!mbim_message_get_ipv4_address(message,
							ipv4_dns_offset + 4,
							&ipv4))
				goto error;

			inet_ntop(AF_INET, &ipv4, dns2, sizeof(dns2));
			dns[1] = dns2;
		}

		if (n_ipv4_dns > 0) { /* Grab first DNS */
			if (!mbim_message_get_ipv4_address(message,
							ipv4_dns_offset,
							&ipv4))
				goto error;

			inet_ntop(AF_INET, &ipv4, dns1, sizeof(dns1));
			dns[0] = dns1;

			ofono_gprs_context_set_ipv4_dns_servers(gc, dns);
		}
	}

	if (gcd->proto == OFONO_GPRS_PROTO_IP)
		goto done;
ipv6:
	if (ipv6_config_available & 0x1) { /* Address Info present */
		uint32_t prefix;

		if (!mbim_message_get_ipv6_element(message, ipv6_addr_offset,
							&prefix, &ipv6))
			goto error;

		inet_ntop(AF_INET6, &ipv6, buf, sizeof(buf));
		ofono_gprs_context_set_ipv6_address(gc, buf);
		ofono_gprs_context_set_ipv6_prefix_length(gc, prefix);
	}

	if (ipv6_config_available & 0x2) { /* IPv6 Gateway info */
		if (!mbim_message_get_ipv6_address(message,
							ipv6_gw_offset, &ipv6))
			goto error;

		inet_ntop(AF_INET6, &ipv6, buf, sizeof(buf));

		ofono_gprs_context_set_ipv6_gateway(gc, buf);
	}

	if (ipv6_config_available & 0x3) { /* IPv6 DNS Info */
		const char *dns[3];
		char dns1[INET6_ADDRSTRLEN];
		char dns2[INET6_ADDRSTRLEN];

		memset(dns, 0, sizeof(dns));

		if (n_ipv6_dns > 1) { /* Grab second DNS */
			if (!mbim_message_get_ipv6_address(message,
							ipv6_dns_offset + 16,
							&ipv6))
				goto error;

			inet_ntop(AF_INET6, &ipv6, dns2, sizeof(dns2));
			dns[1] = dns2;
		}

		if (n_ipv6_dns > 0) { /* Grab first DNS */
			if (!mbim_message_get_ipv6_address(message,
							ipv6_dns_offset,
							&ipv6))
				goto error;

			inet_ntop(AF_INET6, &ipv6, dns1, sizeof(dns1));
			dns[0] = dns1;

			ofono_gprs_context_set_ipv6_dns_servers(gc, dns);
		}
	}
done:

	gcd->state = STATE_ACTIVE;
	interface = ofono_modem_get_string(modem, "NetworkInterface");
	ofono_gprs_context_set_interface(gc, interface);

	CALLBACK_WITH_SUCCESS(gcd->cb, gcd->cb_data);
	gcd->cb = NULL;
	gcd->cb_data = NULL;
	return;

error:
	CALLBACK_WITH_FAILURE(gcd->cb, gcd->cb_data);
	gcd->state = STATE_IDLE;
	gcd->cb = NULL;
	gcd->cb_data = NULL;

	message = mbim_message_new(mbim_uuid_basic_connect,
					MBIM_CID_CONNECT,
					MBIM_COMMAND_TYPE_SET);
	mbim_message_set_arguments(message, "uusssuuu16y",
					gcd->active_context, 0,
					NULL, NULL, NULL, 0, 0, 0,
					mbim_context_type_internet);

	if (!mbim_device_send(gcd->device, GPRS_CONTEXT_GROUP, message,
				NULL, NULL, NULL))
		mbim_message_unref(message);
}

static void mbim_activate_cb(struct mbim_message *message, void *user)
{
	struct ofono_gprs_context *gc = user;
	struct gprs_context_data *gcd = ofono_gprs_context_get_data(gc);

	DBG("");

	if (mbim_message_get_error(message) != 0)
		goto error;

	message = mbim_message_new(mbim_uuid_basic_connect,
					MBIM_CID_IP_CONFIGURATION,
					MBIM_COMMAND_TYPE_QUERY);
	mbim_message_set_arguments(message, "uuuuuuuuuuuuuuu",
				gcd->active_context,
				0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0);

	if (mbim_device_send(gcd->device, GPRS_CONTEXT_GROUP, message,
				mbim_ip_configuration_cb, gc, NULL) > 0)
		return;

error:
	CALLBACK_WITH_FAILURE(gcd->cb, gcd->cb_data);
	gcd->state = STATE_IDLE;
	gcd->cb = NULL;
	gcd->cb_data = NULL;
}

static void mbim_gprs_activate_primary(struct ofono_gprs_context *gc,
				const struct ofono_gprs_primary_context *ctx,
				ofono_gprs_context_cb_t cb, void *data)
{
	struct gprs_context_data *gcd = ofono_gprs_context_get_data(gc);
	struct mbim_message *message;
	const char *username = NULL;
	const char *password = NULL;

	DBG("cid %u", ctx->cid);

	gcd->state = STATE_ENABLING;
	gcd->cb = cb;
	gcd->cb_data = data;
	gcd->active_context = ctx->cid;
	gcd->proto = ctx->proto;

	if (ctx->auth_method != OFONO_GPRS_AUTH_METHOD_NONE && ctx->username[0])
		username = ctx->username;

	if (ctx->auth_method != OFONO_GPRS_AUTH_METHOD_NONE && ctx->password[0])
		password = ctx->password;

	message = mbim_message_new(mbim_uuid_basic_connect,
					MBIM_CID_CONNECT,
					MBIM_COMMAND_TYPE_SET);
	mbim_message_set_arguments(message, "uusssuuu16y",
				ctx->cid,
				1, /* MBIMActivationCommandActivate */
				ctx->apn,
				username,
				password,
				0, /*MBIMCompressionNone */
				auth_method_to_auth_protocol(ctx->auth_method),
				proto_to_context_ip_type(ctx->proto),
				mbim_context_type_internet);

	if (mbim_device_send(gcd->device, GPRS_CONTEXT_GROUP, message,
				mbim_activate_cb, gc, NULL) > 0)
		return;

	mbim_message_unref(message);
	CALLBACK_WITH_FAILURE(cb, data);
}

static void mbim_gprs_detach_shutdown(struct ofono_gprs_context *gc,
						unsigned int cid)
{
	DBG("");
	mbim_gprs_deactivate_primary(gc, cid, NULL, NULL);
}

static void mbim_connect_notify(struct mbim_message *message, void *user)
{
	uint32_t session_id;
	uint32_t activation_state;
	uint32_t voice_call_state;
	uint32_t ip_type;
	uint8_t context_type[16];
	uint32_t nw_error;
	char uuidstr[37];

	DBG("");

	if (!mbim_message_get_arguments(message, "uuuu16yu",
					&session_id, &activation_state,
					&voice_call_state, &ip_type,
					context_type, &nw_error))
		return;

	DBG("session_id: %u, activation_state: %u, ip_type: %u",
			session_id, activation_state, ip_type);
	l_uuid_to_string(context_type, uuidstr, sizeof(uuidstr));
	DBG("context_type: %s, nw_error: %u", uuidstr, nw_error);
}

static int mbim_gprs_context_probe(struct ofono_gprs_context *gc,
					unsigned int vendor, void *data)
{
	struct mbim_device *device = data;
	struct gprs_context_data *gcd;

	DBG("");

	if (!mbim_device_register(device, GPRS_CONTEXT_GROUP,
					mbim_uuid_basic_connect,
					MBIM_CID_CONNECT,
					mbim_connect_notify, gc, NULL))
		return -EIO;

	gcd = l_new(struct gprs_context_data, 1);
	gcd->device = mbim_device_ref(device);

	ofono_gprs_context_set_data(gc, gcd);

	return 0;
}

static void mbim_gprs_context_remove(struct ofono_gprs_context *gc)
{
	struct gprs_context_data *gcd = ofono_gprs_context_get_data(gc);

	DBG("");

	ofono_gprs_context_set_data(gc, NULL);

	mbim_device_cancel_group(gcd->device, GPRS_CONTEXT_GROUP);
	mbim_device_unregister_group(gcd->device, GPRS_CONTEXT_GROUP);
	mbim_device_unref(gcd->device);
	gcd->device = NULL;
	l_free(gcd);
}

static const struct ofono_gprs_context_driver driver = {
	.name			= "mbim",
	.probe			= mbim_gprs_context_probe,
	.remove			= mbim_gprs_context_remove,
	.activate_primary	= mbim_gprs_activate_primary,
	.deactivate_primary	= mbim_gprs_deactivate_primary,
	.detach_shutdown	= mbim_gprs_detach_shutdown
};

void mbim_gprs_context_init(void)
{
	ofono_gprs_context_driver_register(&driver);
}

void mbim_gprs_context_exit(void)
{
	ofono_gprs_context_driver_unregister(&driver);
}
