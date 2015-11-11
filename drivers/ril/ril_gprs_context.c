/*
 *  oFono - Open Source Telephony - RIL-based devices
 *
 *  Copyright (C) 2015 Jolla Ltd.
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License version 2 as
 *  published by the Free Software Foundation.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 */

#include "ril_plugin.h"
#include "ril_constants.h"
#include "ril_util.h"
#include "ril_log.h"

#include <gutil_strv.h>

#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#include "common.h"

#define PROTO_IP_STR "IP"
#define PROTO_IPV6_STR "IPV6"
#define PROTO_IPV4V6_STR "IPV4V6"

#define MIN_DATA_CALL_LIST_SIZE 8
#define MIN_DATA_CALL_REPLY_SIZE 36

#define SETUP_DATA_CALL_PARAMS 7
#define DATA_PROFILE_DEFAULT_STR "0"
#define DEACTIVATE_DATA_CALL_PARAMS 2

#define CTX_ID_NONE ((unsigned int)(-1))

enum data_call_state {
	DATA_CALL_INACTIVE,
	DATA_CALL_LINK_DOWN,
	DATA_CALL_ACTIVE,
};

enum ril_gprs_context_state {
	STATE_IDLE,
	STATE_ACTIVATING,
	STATE_DEACTIVATING,
	STATE_ACTIVE,
};

struct ril_gprs_context {
	struct ofono_gprs_context *gc;
	struct ril_modem *modem;
	GRilIoChannel *io;
	GRilIoQueue *q;
	guint active_ctx_cid;
	enum ril_gprs_context_state state;
	gulong regid;
	struct ril_gprs_context_data_call *active_call;
	struct ril_gprs_context_deactivate_req *deactivate_req;
};

struct ril_gprs_context_data_call {
	guint status;
	gint cid;
	guint active;
	int retry_time;
	int prot;
	gint mtu;
	gchar *ifname;
	gchar **dnses;
	gchar **gateways;
	gchar **addresses;
};

struct ril_gprs_context_data_call_list {
	guint version;
	guint num;
	GSList *calls;
};

struct ril_gprs_context_cbd {
	struct ril_gprs_context *gcd;
	ofono_gprs_context_cb_t cb;
	gpointer data;
};

struct ril_gprs_context_deactivate_req {
	struct ril_gprs_context_cbd cbd;
	gint cid;
};

#define ril_gprs_context_cbd_free g_free
#define ril_gprs_context_deactivate_req_free g_free

static inline struct ril_gprs_context *ril_gprs_context_get_data(
					struct ofono_gprs_context *gprs)
{
	return ofono_gprs_context_get_data(gprs);
}

static struct ril_gprs_context_cbd *ril_gprs_context_cbd_new(
	struct ril_gprs_context *gcd, ofono_gprs_context_cb_t cb, void *data)
{
	struct ril_gprs_context_cbd *cbd =
		g_new0(struct ril_gprs_context_cbd, 1);

	cbd->gcd = gcd;
	cbd->cb = cb;
	cbd->data = data;
	return cbd;
}

static struct ril_gprs_context_deactivate_req *
	ril_gprs_context_deactivate_req_new(struct ril_gprs_context *gcd,
	ofono_gprs_context_cb_t cb, void *data)
{
	struct ril_gprs_context_deactivate_req *req =
		g_new0(struct ril_gprs_context_deactivate_req, 1);

	req->cbd.gcd = gcd;
	req->cbd.cb = cb;
	req->cbd.data = data;
	req->cid = gcd->active_call->cid;
	return req;
}


static char *ril_gprs_context_netmask(const char *address)
{
	if (address) {
		const char *suffix = strchr(address, '/');
		if (suffix) {
			int nbits = atoi(suffix + 1);
			if (nbits > 0 && nbits < 33) {
				const char* str;
				struct in_addr in;
				in.s_addr = htonl((nbits == 32) ? 0xffffffff :
					((1 << nbits)-1) << (32-nbits));
				str = inet_ntoa(in);
				if (str) {
					return g_strdup(str);
				}
			}
		}
	}
	return g_strdup("255.255.255.0");
}

static const char *ril_gprs_ofono_protocol_to_ril(guint protocol)
{
	switch (protocol) {
	case OFONO_GPRS_PROTO_IPV6:
		return PROTO_IPV6_STR;
	case OFONO_GPRS_PROTO_IPV4V6:
		return PROTO_IPV4V6_STR;
	case OFONO_GPRS_PROTO_IP:
		return PROTO_IP_STR;
	default:
		return NULL;
	}
}

static int ril_gprs_protocol_to_ofono(gchar *protocol_str)
{
	if (protocol_str) {
		if (!strcmp(protocol_str, PROTO_IPV6_STR)) {
			return OFONO_GPRS_PROTO_IPV6;
		} else if (!strcmp(protocol_str, PROTO_IPV4V6_STR)) {
			return OFONO_GPRS_PROTO_IPV4V6;
		} else if (!strcmp(protocol_str, PROTO_IP_STR)) {
			return OFONO_GPRS_PROTO_IP;
		}
	}
	return -1;
}

static void ril_gprs_context_data_call_free(
				struct ril_gprs_context_data_call *call)
{
	if (call) {
		g_free(call->ifname);
		g_strfreev(call->dnses);
		g_strfreev(call->addresses);
		g_strfreev(call->gateways);
		g_free(call);
	}
}

static void ril_gprs_context_set_disconnected(struct ril_gprs_context *gcd)
{
	gcd->state = STATE_IDLE;
	if (gcd->active_call) {
		if (gcd->deactivate_req &&
			gcd->deactivate_req->cid == gcd->active_call->cid) {
			/* Mark this request as done */
			gcd->deactivate_req->cbd.gcd = NULL;
			gcd->deactivate_req = NULL;
		}
		ril_gprs_context_data_call_free(gcd->active_call);
		gcd->active_call = NULL;
	}
	if (gcd->active_ctx_cid != CTX_ID_NONE) {
		guint id = gcd->active_ctx_cid;
		gcd->active_ctx_cid = CTX_ID_NONE;
		ofono_gprs_context_deactivated(gcd->gc, id);
	}
}

static void ril_gprs_split_ip_by_protocol(char **ip_array,
						char ***split_ip_addr,
						char ***split_ipv6_addr)
{
	const int n = gutil_strv_length(ip_array);
	int i;

	*split_ipv6_addr = *split_ip_addr = NULL;
	for (i = 0; i < n && (!*split_ipv6_addr || !*split_ip_addr); i++) {
		const char *addr = ip_array[i];
		switch (ril_address_family(addr)) {
		case AF_INET:
			if (!*split_ip_addr) {
				char *mask = ril_gprs_context_netmask(addr);
				*split_ip_addr = g_strsplit(addr, "/", 2);
				if (gutil_strv_length(*split_ip_addr) == 2) {
					g_free((*split_ip_addr)[1]);
					(*split_ip_addr)[1] = mask;
				} else {
					/* This is rather unlikely to happen */
					*split_ip_addr =
						gutil_strv_add(*split_ip_addr,
									mask);
					g_free(mask);
				}
			}
			break;
		case AF_INET6:
			if (!*split_ipv6_addr) {
				*split_ipv6_addr = g_strsplit(addr, "/", 2);
			}
		}
	}
}

static void ril_gprs_split_gw_by_protocol(char **gw_array, char **ip_gw,
								char **ipv6_gw)
{
	const int n = gutil_strv_length(gw_array);
	int i;

	*ip_gw = *ipv6_gw = NULL;
	for (i = 0; i < n && (!*ipv6_gw || !*ip_gw); i++) {
		const char *gw_addr = gw_array[i];
		switch (ril_address_family(gw_addr)) {
		case AF_INET:
			if (!*ip_gw) *ip_gw = g_strdup(gw_addr);
			break;
		case AF_INET6:
			if (!*ipv6_gw) *ipv6_gw = g_strdup(gw_addr);
			break;
		}
	}
}

static void ril_gprs_split_dns_by_protocol(char **dns_array, char ***dns_addr,
							char ***dns_ipv6_addr)
{
	const int n = gutil_strv_length(dns_array);
	int i;

	*dns_ipv6_addr = *dns_addr = 0;
	for (i = 0; i < n; i++) {
		const char *addr = dns_array[i];
		switch (ril_address_family(addr)) {
		case AF_INET:
			*dns_addr = gutil_strv_add(*dns_addr, addr);
			break;
		case AF_INET6:
			*dns_ipv6_addr = gutil_strv_add(*dns_ipv6_addr, addr);
			break;
		}
	}
}

static gint ril_gprs_context_parse_data_call_compare(gconstpointer a,
							gconstpointer b)
{
	const struct ril_gprs_context_data_call *ca = a;
	const struct ril_gprs_context_data_call *cb = b;

	if (ca->cid < cb->cid) {
		return -1;
	} else if (ca->cid > cb->cid) {
		return 1;
	} else {
		return 0;
	}
}

static void ril_gprs_context_data_call_free1(gpointer data)
{
	ril_gprs_context_data_call_free(data);
}

static void ril_gprs_context_data_call_list_free(
			struct ril_gprs_context_data_call_list *list)
{
	if (list) {
		g_slist_free_full(list->calls, ril_gprs_context_data_call_free1);
		g_free(list);
	}
}

static struct ril_gprs_context_data_call *ril_gprs_context_data_call_find(
		struct ril_gprs_context_data_call_list *list, gint cid)
{
	if (list) {
		GSList *entry;

		for (entry = list->calls; entry; entry = entry->next) {
			struct ril_gprs_context_data_call *call = entry->data;

			if (call->cid == cid) {
				return call;
			}
		}
	}

	return NULL;
}

/* Only compares the stuff that's important to us */
static gboolean ril_gprs_context_data_call_equal(
			const struct ril_gprs_context_data_call *c1,
			const struct ril_gprs_context_data_call *c2)
{
	if (!c1 && !c2) {
		return TRUE;
	} else if (c1 && c2) {
		return c1->cid == c2->cid &&
			c1->active == c2->active && c1->prot == c2->prot &&
			!g_strcmp0(c1->ifname, c2->ifname) &&
			gutil_strv_equal(c1->dnses, c2->dnses) &&
			gutil_strv_equal(c1->gateways, c2->gateways) &&
			gutil_strv_equal(c1->addresses, c2->addresses);
	} else {
		return FALSE;
	}
}

static struct ril_gprs_context_data_call *
	ril_gprs_context_parse_data_call(int version, GRilIoParser *rilp)
{
	char *prot;
	struct ril_gprs_context_data_call *call =
		g_new0(struct ril_gprs_context_data_call, 1);

	grilio_parser_get_uint32(rilp, &call->status);
	grilio_parser_get_int32(rilp, &call->retry_time);
	grilio_parser_get_int32(rilp, &call->cid);
	grilio_parser_get_uint32(rilp, &call->active);
	prot = grilio_parser_get_utf8(rilp);
	call->ifname = grilio_parser_get_utf8(rilp);
	call->addresses = grilio_parser_split_utf8(rilp, " ");
	call->dnses = grilio_parser_split_utf8(rilp, " ");
	call->gateways = grilio_parser_split_utf8(rilp, " ");

	call->prot = ril_gprs_protocol_to_ofono(prot);
	if (call->prot < 0) {
		ofono_error("Invalid type(protocol) specified: %s", prot);
	}

	g_free(prot);

	if (version >= 9) {
		/* PCSCF */
		grilio_parser_skip_string(rilp);
		if (version >= 11) {
			/* MTU */
			grilio_parser_get_int32(rilp, &call->mtu);
		}
	}

	return call;
}

static struct ril_gprs_context_data_call_list *
	ril_gprs_context_parse_data_call_list(const void *data, guint len)
{
	struct ril_gprs_context_data_call_list *reply =
		g_new0(struct ril_gprs_context_data_call_list, 1);
	GRilIoParser rilp;
	unsigned int i, n;

	grilio_parser_init(&rilp, data, len);
	grilio_parser_get_uint32(&rilp, &reply->version);
	grilio_parser_get_uint32(&rilp, &n);
	DBG("version=%d,num=%d", reply->version, n);

	for (i = 0; i < n && !grilio_parser_at_end(&rilp); i++) {
		struct ril_gprs_context_data_call *call =
			ril_gprs_context_parse_data_call(reply->version, &rilp);

		DBG("%d [status=%d,retry=%d,cid=%d,"
			"active=%d,type=%s,ifname=%s,mtu=%d,"
			"address=%s, dns=%s %s,gateways=%s]",
				i, call->status, call->retry_time,
				call->cid, call->active,
				ril_gprs_ofono_protocol_to_ril(call->prot),
				call->ifname, call->mtu, call->addresses[0],
				call->dnses[0],
				(call->dnses[0] && call->dnses[1]) ?
				call->dnses[1] : "",
				call->gateways[0]);

		reply->num++;
		reply->calls = g_slist_insert_sorted(reply->calls, call,
				ril_gprs_context_parse_data_call_compare);
	}

	return reply;
}

static void ril_gprs_context_call_list_changed(GRilIoChannel *io, guint event,
				const void *data, guint len, void *user_data)
{
	struct ril_gprs_context *gcd = user_data;
	struct ofono_gprs_context *gc = gcd->gc;
	struct ril_gprs_context_data_call *call = NULL;
	struct ril_gprs_context_data_call *prev_call;
	struct ril_gprs_context_data_call_list *unsol =
		ril_gprs_context_parse_data_call_list(data, len);

	if (gcd->active_call) {
		/* Find our call */
		call = ril_gprs_context_data_call_find(unsol,
							gcd->active_call->cid);
		if (call) {
			/* Check if the call have been disconnected */
			if (call->active == DATA_CALL_INACTIVE) {
				ofono_error("Clearing active context");
				ril_gprs_context_set_disconnected(gcd);
				call = NULL;

				/* Compare it agains the last known state */
			} else if (ril_gprs_context_data_call_equal(call,
							gcd->active_call)) {
				DBG("call %u didn't change", call->cid);
				call = NULL;
			} else {
				/* Steal it from the list */
				DBG("call %u changed", call->cid);
				unsol->calls = g_slist_remove(unsol->calls,
									call);
			}
		} else {
			ofono_error("Clearing active context");
			ril_gprs_context_set_disconnected(gcd);
		}
	}

	/* We don't need the rest of the list anymore */
	ril_gprs_context_data_call_list_free(unsol);

	if (!call) {
		/* We are not interested */
		return;
	}

	/* Store the updated call data */
	prev_call = gcd->active_call;
	gcd->active_call = call;

	if (call->status != 0) {
		ofono_info("data call status: %d", call->status);
	}

	if (call->active == DATA_CALL_ACTIVE) {
		gboolean signal = FALSE;

		if (call->ifname && g_strcmp0(call->ifname, prev_call->ifname)) {
			DBG("interface changed");
			signal = TRUE;
			ofono_gprs_context_set_interface(gc, call->ifname);
		}

		if (!gutil_strv_equal(call->addresses, prev_call->addresses)) {
			char **split_ip_addr = NULL;
			char **split_ipv6_addr = NULL;

			DBG("address changed");
			signal = TRUE;

			/* Pick 1 address of each protocol */
			ril_gprs_split_ip_by_protocol(call->addresses,
					&split_ip_addr, &split_ipv6_addr);

			if ((call->prot == OFONO_GPRS_PROTO_IPV4V6 ||
					call->prot == OFONO_GPRS_PROTO_IPV6) &&
						split_ipv6_addr) {
				ofono_gprs_context_set_ipv6_address(gc,
							split_ipv6_addr[0]);
			}

			if ((call->prot == OFONO_GPRS_PROTO_IPV4V6 ||
					call->prot == OFONO_GPRS_PROTO_IP) &&
						split_ip_addr) {
				ofono_gprs_context_set_ipv4_netmask(gc,
							split_ip_addr[1]);
				ofono_gprs_context_set_ipv4_address(gc,
							split_ip_addr[0], TRUE);
			}

			g_strfreev(split_ip_addr);
			g_strfreev(split_ipv6_addr);
		}

		if (!gutil_strv_equal(call->gateways, prev_call->gateways)){
			char *ip_gw = NULL;
			char *ipv6_gw = NULL;

			DBG("gateway changed");
			signal = TRUE;

			/* Pick 1 gw for each protocol*/
			ril_gprs_split_gw_by_protocol(call->gateways,
							&ip_gw, &ipv6_gw);

			if ((call->prot == OFONO_GPRS_PROTO_IPV4V6 ||
					call->prot == OFONO_GPRS_PROTO_IPV6) &&
						ipv6_gw) {
				ofono_gprs_context_set_ipv6_gateway(gc, ipv6_gw);
			}

			if ((call->prot == OFONO_GPRS_PROTO_IPV4V6 ||
					call->prot == OFONO_GPRS_PROTO_IP) &&
						ip_gw) {
				ofono_gprs_context_set_ipv4_gateway(gc, ip_gw);
			}

			g_free(ip_gw);
			g_free(ipv6_gw);
		}

		if (!gutil_strv_equal(call->dnses, prev_call->dnses)){
			char **dns_ip = NULL;
			char **dns_ipv6 = NULL;

			DBG("name server(s) changed");
			signal = TRUE;

			/* split based on protocol*/
			ril_gprs_split_dns_by_protocol(call->dnses,
							&dns_ip, &dns_ipv6);

			if ((call->prot == OFONO_GPRS_PROTO_IPV4V6 ||
					call->prot == OFONO_GPRS_PROTO_IPV6) &&
						dns_ipv6) {
				ofono_gprs_context_set_ipv6_dns_servers(gc,
						(const char **) dns_ipv6);
			}

			if ((call->prot == OFONO_GPRS_PROTO_IPV4V6 ||
				call->prot == OFONO_GPRS_PROTO_IP) && dns_ip) {
				ofono_gprs_context_set_ipv4_dns_servers(gc,
							(const char**)dns_ip);
			}

			g_strfreev(dns_ip);
			g_strfreev(dns_ipv6);
		}

		if (signal) {
			ofono_gprs_context_signal_change(gc, call->cid);
		}
	}

	ril_gprs_context_data_call_free(prev_call);
}

static void ril_gprs_context_activate_primary_cb(GRilIoChannel *io, int status,
				const void *data, guint len, void *user_data)
{
	struct ril_gprs_context_cbd *cbd = user_data;
	ofono_gprs_context_cb_t cb = cbd->cb;
	struct ril_gprs_context *gcd = cbd->gcd;
	struct ofono_gprs_context *gc = gcd->gc;
	struct ofono_error error;
	struct ril_gprs_context_data_call_list *reply = NULL;
	struct ril_gprs_context_data_call *call;
	char **split_ip_addr = NULL;
	char **split_ipv6_addr = NULL;
	char* ip_gw = NULL;
	char* ipv6_gw = NULL;
	char** dns_addr = NULL;
	char** dns_ipv6_addr = NULL;

	ofono_info("setting up data call");

	ril_error_init_ok(&error);
	if (status != RIL_E_SUCCESS) {
		ofono_error("GPRS context: Reply failure: %s",
						ril_error_to_string(status));
		error.type = OFONO_ERROR_TYPE_FAILURE;
		error.error = status;
		ril_gprs_context_set_disconnected(gcd);
		goto done;
	}

	reply = ril_gprs_context_parse_data_call_list(data, len);
	if (reply->num != 1) {
		ofono_error("Number of data calls: %u", reply->num);
		ril_error_init_failure(&error);
		ril_gprs_context_set_disconnected(gcd);
		goto done;
	}

	call = reply->calls->data;

	if (call->status != 0) {
		ofono_error("Unexpected data call status %d", call->status);
		error.type = OFONO_ERROR_TYPE_FAILURE;
		error.error = call->status;
		goto done;
	}

	/* Must have interface */
	if (!call->ifname) {
		ofono_error("GPRS context: No interface");
		error.type = OFONO_ERROR_TYPE_FAILURE;
		error.error = EINVAL;
		ril_gprs_context_set_disconnected(gcd);
		goto done;
	}

	/* Check the ip address */
	ril_gprs_split_ip_by_protocol(call->addresses, &split_ip_addr,
						&split_ipv6_addr);
	if (!split_ip_addr && !split_ipv6_addr) {
		ofono_error("GPRS context: No IP address");
		error.type = OFONO_ERROR_TYPE_FAILURE;
		error.error = EINVAL;
		ril_gprs_context_set_disconnected(gcd);
		goto done;
	}

	/* Steal the call data from the list */
	g_slist_free(reply->calls);
	reply->calls = NULL;
	ril_gprs_context_data_call_free(gcd->active_call);
	gcd->active_call = call;
	gcd->state = STATE_ACTIVE;

	ofono_gprs_context_set_interface(gc, call->ifname);
	ril_gprs_split_gw_by_protocol(call->gateways, &ip_gw, &ipv6_gw);
	ril_gprs_split_dns_by_protocol(call->dnses, &dns_addr, &dns_ipv6_addr);

	/* TODO:
	 * RILD can return multiple addresses; oFono only supports setting
	 * a single IPv4 and single IPV6 address. At this time, we only use
	 * the first address. It's possible that a RIL may just specify
	 * the end-points of the point-to-point connection, in which case this
	 * code will need to changed to handle such a device.
	 */

	if (split_ipv6_addr &&
			(call->prot == OFONO_GPRS_PROTO_IPV6 ||
			call->prot == OFONO_GPRS_PROTO_IPV4V6)) {

		ofono_gprs_context_set_ipv6_address(gc, split_ipv6_addr[0]);
		ofono_gprs_context_set_ipv6_gateway(gc, ipv6_gw);
		ofono_gprs_context_set_ipv6_dns_servers(gc,
						(const char **) dns_ipv6_addr);
	}

	if (split_ip_addr &&
			(call->prot == OFONO_GPRS_PROTO_IP ||
			call->prot == OFONO_GPRS_PROTO_IPV4V6)) {
		ofono_gprs_context_set_ipv4_netmask(gc, split_ip_addr[1]);
		ofono_gprs_context_set_ipv4_address(gc, split_ip_addr[0], TRUE);
		ofono_gprs_context_set_ipv4_gateway(gc, ip_gw);
		ofono_gprs_context_set_ipv4_dns_servers(gc,
						(const char **) dns_addr);
	}

done:
	ril_gprs_context_data_call_list_free(reply);
	g_strfreev(split_ip_addr);
	g_strfreev(split_ipv6_addr);
	g_strfreev(dns_addr);
	g_strfreev(dns_ipv6_addr);
	g_free(ip_gw);
	g_free(ipv6_gw);

	cb(&error, cbd->data);
}

static void ril_gprs_context_activate_primary(struct ofono_gprs_context *gc,
				const struct ofono_gprs_primary_context *ctx,
				ofono_gprs_context_cb_t cb, void *data)
{
	struct ril_gprs_context *gcd = ril_gprs_context_get_data(gc);
	struct ofono_netreg *netreg = ril_modem_ofono_netreg(gcd->modem);
	struct ofono_gprs *gprs = ril_modem_ofono_gprs(gcd->modem);
	const int rs = ofono_netreg_get_status(netreg);
	const gchar *protocol_str;
	GRilIoRequest* req;
	int tech, auth;

	/* Let's make sure that we aren't connecting when roaming not allowed */
	if (rs == NETWORK_REGISTRATION_STATUS_ROAMING) {
		if (!ofono_gprs_get_roaming_allowed(gprs) &&
			ril_netreg_check_if_really_roaming(netreg, rs) ==
					NETWORK_REGISTRATION_STATUS_ROAMING) {
			struct ofono_error error;
			ofono_info("Can't activate context %d (roaming)",
								ctx->cid);
			cb(ril_error_failure(&error), data);
			return;
		}
	}

	ofono_info("Activating context: %d", ctx->cid);
	protocol_str = ril_gprs_ofono_protocol_to_ril(ctx->proto);
	GASSERT(protocol_str);

	/* ril.h has this to say about the radio tech parameter:
	 *
	 * ((const char **)data)[0] Radio technology to use: 0-CDMA,
	 *                          1-GSM/UMTS, 2... for values above 2
	 *                          this is RIL_RadioTechnology + 2.
	 *
	 * Makes little sense but it is what it is.
	 */
	tech = ril_gprs_ril_data_tech(gprs);
	if (tech > 2) {
		tech += 2;
	} else {
		/*
		 * This value used to be hardcoded, let's keep using it
		 * as the default.
		 */
		tech = RADIO_TECH_HSPA;
	}

        /*
         * We do the same as in $AOSP/frameworks/opt/telephony/src/java/com/
         * android/internal/telephony/dataconnection/DataConnection.java,
         * onConnect(), and use authentication or not depending on whether
         * the user field is empty or not.
         */
	auth = (ctx->username && ctx->username[0]) ?
					RIL_AUTH_BOTH : RIL_AUTH_NONE;

	/*
	 * TODO: add comments about tethering, other non-public
	 * profiles...
	 */
	req = grilio_request_new();
	grilio_request_append_int32(req, SETUP_DATA_CALL_PARAMS);
	grilio_request_append_format(req, "%d", tech);
	grilio_request_append_utf8(req, DATA_PROFILE_DEFAULT_STR);
	grilio_request_append_utf8(req, ctx->apn);
	grilio_request_append_utf8(req, ctx->username);
	grilio_request_append_utf8(req, ctx->password);
	grilio_request_append_format(req, "%d", auth);
	grilio_request_append_utf8(req, protocol_str);

	GASSERT(ctx->cid != CTX_ID_NONE);
	gcd->active_ctx_cid = ctx->cid;
	gcd->state = STATE_ACTIVATING;

	grilio_queue_send_request_full(gcd->q, req, RIL_REQUEST_SETUP_DATA_CALL,
		ril_gprs_context_activate_primary_cb, ril_gprs_context_cbd_free,
		ril_gprs_context_cbd_new(gcd, cb, data));
	grilio_request_unref(req);
}

static void ril_gprs_context_deactivate_data_call_cb(GRilIoChannel *io, int err,
				const void *data, guint len, void *user_data)
{
	struct ofono_error error;
	struct ril_gprs_context_deactivate_req *req = user_data;
	struct ril_gprs_context *gcd = req->cbd.gcd;

	if (!gcd) {
		/*
		 * ril_gprs_context_remove() zeroes gcd pointer for the
		 * pending ril_gprs_context_deactivate_req. Or we may have
		 * received RIL_UNSOL_DATA_CALL_LIST_CHANGED event before
		 * RIL_REQUEST_DEACTIVATE_DATA_CALL completes, in which
		 * case gcd will also be NULL. In any case, it means that
		 * there's nothing left for us to do here. Just ignore it.
		 */
		DBG("late completion, cid: %d err: %d", req->cid, err);
	} else {
		ofono_gprs_context_cb_t cb = req->cbd.cb;

		/* Mark it as done */
		if (gcd->deactivate_req == req) {
			gcd->deactivate_req = NULL;
		}

		if (err == RIL_E_SUCCESS) {
			GASSERT(gcd->active_call &&
					gcd->active_call->cid == req->cid);
			ril_gprs_context_set_disconnected(gcd);
			ofono_info("Deactivated data call");
			if (cb) {
				cb(ril_error_ok(&error), req->cbd.data);
			}
		} else {
			ofono_error("Deactivate failure: %s",
						ril_error_to_string(err));
			if (cb) {
				cb(ril_error_failure(&error), req->cbd.data);
			}
		}
	}
}

static void ril_gprs_context_deactivate_data_call(struct ril_gprs_context *gcd,
					ofono_gprs_context_cb_t cb, void *data)
{
	GRilIoRequest *req = grilio_request_new();

	/* Overlapping deactivate requests make no sense */
	GASSERT(!gcd->deactivate_req);
	if (gcd->deactivate_req) {
		gcd->deactivate_req->cbd.gcd = NULL;
	}
	gcd->deactivate_req =
		ril_gprs_context_deactivate_req_new(gcd, cb, data);

	/* Caller is responsible for checking gcd->active_call */
	GASSERT(gcd->active_call);
	grilio_request_append_int32(req, DEACTIVATE_DATA_CALL_PARAMS);
	grilio_request_append_format(req, "%d", gcd->active_call->cid);
	grilio_request_append_format(req, "%d",
					RIL_DEACTIVATE_DATA_CALL_NO_REASON);

	/*
	 * Send it to GRilIoChannel so that it doesn't get cancelled
	 * by ril_gprs_context_remove()
	 */
	grilio_channel_send_request_full(gcd->io, req,
		RIL_REQUEST_DEACTIVATE_DATA_CALL,
		ril_gprs_context_deactivate_data_call_cb,
		ril_gprs_context_deactivate_req_free,
		gcd->deactivate_req);
	grilio_request_unref(req);
	gcd->state = STATE_DEACTIVATING;
}

static void ril_gprs_context_deactivate_primary(struct ofono_gprs_context *gc,
		unsigned int id, ofono_gprs_context_cb_t cb, void *data)
{
	struct ril_gprs_context *gcd = ril_gprs_context_get_data(gc);

	GASSERT(cb);
	GASSERT(gcd->active_call && gcd->active_ctx_cid == id);
	ofono_info("Deactivate primary");

	if (gcd->active_call && gcd->active_ctx_cid == id) {
		ril_gprs_context_deactivate_data_call(gcd, cb, data);
	} else {
		struct ofono_error error;
		cb(ril_error_ok(&error), data);
	}
}

static void ril_gprs_context_detach_shutdown(struct ofono_gprs_context *gc,
						unsigned int id)
{
	struct ril_gprs_context *gcd = ril_gprs_context_get_data(gc);

	DBG("%d", id);
	GASSERT(gcd->active_ctx_cid == id);
	if (gcd->active_call && !gcd->deactivate_req) {
		ril_gprs_context_deactivate_data_call(gcd, NULL, NULL);
	}
}

static int ril_gprs_context_probe(struct ofono_gprs_context *gc,
					unsigned int vendor, void *data)
{
	struct ril_modem *modem = data;
	struct ril_gprs_context *gcd = g_new0(struct ril_gprs_context, 1);

	DBG("");
	gcd->gc = gc;
	gcd->modem = modem;
	gcd->io = grilio_channel_ref(ril_modem_io(modem));
	gcd->q = grilio_queue_new(gcd->io);
	gcd->regid = grilio_channel_add_unsol_event_handler(gcd->io,
			ril_gprs_context_call_list_changed,
			RIL_UNSOL_DATA_CALL_LIST_CHANGED, gcd);
	ril_gprs_context_set_disconnected(gcd);
	ofono_gprs_context_set_data(gc, gcd);
	return 0;
}

static void ril_gprs_context_remove(struct ofono_gprs_context *gc)
{
	struct ril_gprs_context *gcd = ril_gprs_context_get_data(gc);

	DBG("");
	ofono_gprs_context_set_data(gc, NULL);

	if (gcd->active_call && !gcd->deactivate_req) {
		ril_gprs_context_deactivate_data_call(gcd, NULL, NULL);
	}

	if (gcd->deactivate_req) {
		gcd->deactivate_req->cbd.gcd = NULL;
	}

	grilio_channel_remove_handler(gcd->io, gcd->regid);
	grilio_channel_unref(gcd->io);
	grilio_queue_cancel_all(gcd->q, FALSE);
	grilio_queue_unref(gcd->q);
	ril_gprs_context_data_call_free(gcd->active_call);
	g_free(gcd);
}

const struct ofono_gprs_context_driver ril_gprs_context_driver = {
	.name                   = RILMODEM_DRIVER,
	.probe                  = ril_gprs_context_probe,
	.remove                 = ril_gprs_context_remove,
	.activate_primary       = ril_gprs_context_activate_primary,
	.deactivate_primary     = ril_gprs_context_deactivate_primary,
	.detach_shutdown        = ril_gprs_context_detach_shutdown,
};

/*
 * Local Variables:
 * mode: C
 * c-basic-offset: 8
 * indent-tabs-mode: t
 * End:
 */
