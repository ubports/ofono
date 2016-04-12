/*
 *  oFono - Open Source Telephony - RIL-based devices
 *
 *  Copyright (C) 2015-2016 Jolla Ltd.
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License version 2 as
 *  published by the Free Software Foundation.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 *  GNU General Public License for more details.
 */

#include "ril_plugin.h"
#include "ril_network.h"
#include "ril_data.h"
#include "ril_util.h"
#include "ril_mtu.h"
#include "ril_log.h"

#include <gutil_strv.h>

#include <arpa/inet.h>

#include "common.h"

#define CTX_ID_NONE ((unsigned int)(-1))

#define MAX_MTU 1280

struct ril_gprs_context_call {
	struct ril_data_request *req;
	ofono_gprs_context_cb_t cb;
	gpointer data;
};

struct ril_gprs_context {
	struct ofono_gprs_context *gc;
	struct ril_modem *modem;
	struct ril_network *network;
	struct ril_data *data;
	guint active_ctx_cid;
	gulong calls_changed_event_id;
	struct ril_mtu_watch *mtu_watch;
	struct ril_data_call *active_call;
	struct ril_gprs_context_call activate;
	struct ril_gprs_context_call deactivate;
};

static inline struct ril_gprs_context *ril_gprs_context_get_data(
					struct ofono_gprs_context *gprs)
{
	return ofono_gprs_context_get_data(gprs);
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

static void ril_gprs_context_set_ipv4(struct ofono_gprs_context *gc,
						char * const *ip_addr)
{
	const guint n = gutil_strv_length(ip_addr);

	if (n > 0) {
		ofono_gprs_context_set_ipv4_address(gc, ip_addr[0], TRUE);
		if (n > 1) {
			ofono_gprs_context_set_ipv4_netmask(gc, ip_addr[1]);
		}
	}
}

static void ril_gprs_context_set_ipv6(struct ofono_gprs_context *gc,
						char * const *ipv6_addr)
{
	const guint n = gutil_strv_length(ipv6_addr);

	if (n > 0) {
		ofono_gprs_context_set_ipv6_address(gc, ipv6_addr[0]);
		if (n > 1) {
			const int p = atoi(ipv6_addr[1]);
			if (p > 0 && p <= 128) {
				ofono_gprs_context_set_ipv6_prefix_length(gc, p);
			}
		}
	}
}

static void ril_gprs_context_free_active_call(struct ril_gprs_context *gcd)
{
	if (gcd->active_call) {
		ril_data_call_free(gcd->active_call);
		gcd->active_call = NULL;
	}
	if (gcd->calls_changed_event_id) {
		ril_data_remove_handler(gcd->data, gcd->calls_changed_event_id);
		gcd->calls_changed_event_id = 0;
	}
	if (gcd->mtu_watch) {
		ril_mtu_watch_free(gcd->mtu_watch);
		gcd->mtu_watch = NULL;
	}
}

static void ril_gprs_context_set_active_call(struct ril_gprs_context *gcd,
					const struct ril_data_call *call)
{
	if (call) {
		ril_data_call_free(gcd->active_call);
		gcd->active_call = ril_data_call_dup(call);
		if (!gcd->mtu_watch) {
			gcd->mtu_watch = ril_mtu_watch_new(MAX_MTU);
		}
		ril_mtu_watch_set_ifname(gcd->mtu_watch, call->ifname);
	} else {
		ril_gprs_context_free_active_call(gcd);
	}
}

static void ril_gprs_context_set_disconnected(struct ril_gprs_context *gcd)
{
	if (gcd->active_call) {
		ril_gprs_context_free_active_call(gcd);
		if (gcd->deactivate.req) {
			struct ril_gprs_context_call deact = gcd->deactivate;

			/*
			 * Complete the deactivate request. We need to
			 * clear gcd->deactivate first because cancelling
			 * the deactivation request will probably result
			 * in ril_gprs_context_deactivate_primary_cb() being
			 * invoked with GRILIO_CANCELLED status. And we don't
			 * want to fail the disconnect request because this
			 * is a success (we wanted to disconnect the data
			 * call and it's gone).
			 *
			 * Additionally, we need to make sure that we don't
			 * complete the same request twice - that would crash
			 * the core.
			 */
			memset(&gcd->deactivate, 0, sizeof(gcd->deactivate));
			ril_data_request_cancel(deact.req);
			if (deact.cb) {
				struct ofono_error error;
				ofono_info("Deactivated data call");
				deact.cb(ril_error_ok(&error), deact.data);
			}
		}
	}
	if (gcd->active_ctx_cid != CTX_ID_NONE) {
		guint id = gcd->active_ctx_cid;
		gcd->active_ctx_cid = CTX_ID_NONE;
		DBG("ofono context %u deactivated", id);
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

/* Only compares the stuff that's important to us */
static gboolean ril_gprs_context_data_call_equal(
			const struct ril_data_call *c1,
			const struct ril_data_call *c2)
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

static void ril_gprs_context_call_list_changed(struct ril_data *data, void *arg)
{
	struct ril_gprs_context *gcd = arg;
	struct ofono_gprs_context *gc = gcd->gc;

	/*
	 * gcd->active_call can't be NULL here because this callback
	 * is only registered when we have the active call and released
	 * when active call is dropped.
	 */
	struct ril_data_call *prev_call = gcd->active_call;
	const struct ril_data_call *call =
		ril_data_call_find(data->data_calls, prev_call->cid);

	if (call) {
		/* Check if the call has been disconnected */
		if (call->active == RIL_DATA_CALL_INACTIVE) {
			ofono_error("Clearing active context");
			ril_gprs_context_set_disconnected(gcd);
			call = NULL;

		/* Compare it against the last known state */
		} else if (ril_gprs_context_data_call_equal(call, prev_call)) {
			DBG("call %u didn't change", call->cid);
			call = NULL;

		} else {
			DBG("call %u changed", call->cid);
		}
	} else {
		ofono_error("Clearing active context");
		ril_gprs_context_set_disconnected(gcd);
	}

	if (!call) {
		/* We are not interested */
		return;
	}

	/*
	 * prev_call points to the previous active call, and it will
	 * be deallocated at the end of the this function. Clear the
	 * gcd->active_call pointer so that we don't deallocate it twice.
	 */
	gcd->active_call = NULL;
	ril_gprs_context_set_active_call(gcd, call);

	if (call->status != PDP_FAIL_NONE) {
		ofono_info("data call status: %d", call->status);
	}

	if (call->active == RIL_DATA_CALL_ACTIVE) {
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
				ril_gprs_context_set_ipv6(gc, split_ipv6_addr);
			}

			if ((call->prot == OFONO_GPRS_PROTO_IPV4V6 ||
					call->prot == OFONO_GPRS_PROTO_IP) &&
						split_ip_addr) {
				ril_gprs_context_set_ipv4(gc, split_ip_addr);
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

	ril_data_call_free(prev_call);
}

static void ril_gprs_context_activate_primary_cb(struct ril_data *data,
			int ril_status, const struct ril_data_call *call,
			void *user_data)
{
	struct ril_gprs_context *gcd = user_data;
	struct ofono_gprs_context *gc = gcd->gc;
	struct ofono_error error;
	char **split_ip_addr = NULL;
	char **split_ipv6_addr = NULL;
	char* ip_gw = NULL;
	char* ipv6_gw = NULL;
	char** dns_addr = NULL;
	char** dns_ipv6_addr = NULL;
	ofono_gprs_context_cb_t cb;
	gpointer cb_data;

	ofono_info("setting up data call");

	ril_error_init_failure(&error);
	if (ril_status != RIL_E_SUCCESS) {
		ofono_error("GPRS context: Reply failure: %s",
					ril_error_to_string(ril_status));
		goto done;
	}

	if (call->status != PDP_FAIL_NONE) {
		ofono_error("Unexpected data call status %d", call->status);
		error.type = OFONO_ERROR_TYPE_CMS;
		error.error = call->status;
		goto done;
	}

	/* Must have interface */
	if (!call->ifname) {
		ofono_error("GPRS context: No interface");
		goto done;
	}

	/* Check the ip address */
	ril_gprs_split_ip_by_protocol(call->addresses, &split_ip_addr,
						&split_ipv6_addr);
	if (!split_ip_addr && !split_ipv6_addr) {
		ofono_error("GPRS context: No IP address");
		goto done;
	}

	ril_error_init_ok(&error);
	ril_gprs_context_set_active_call(gcd, call);

	GASSERT(!gcd->calls_changed_event_id);
	ril_data_remove_handler(gcd->data, gcd->calls_changed_event_id);
	gcd->calls_changed_event_id =
		ril_data_add_calls_changed_handler(gcd->data,
			ril_gprs_context_call_list_changed, gcd);

	ofono_gprs_context_set_interface(gc, call->ifname);
	ril_gprs_split_gw_by_protocol(call->gateways, &ip_gw, &ipv6_gw);
	ril_gprs_split_dns_by_protocol(call->dnses, &dns_addr, &dns_ipv6_addr);

	if (split_ipv6_addr &&
			(call->prot == OFONO_GPRS_PROTO_IPV6 ||
			call->prot == OFONO_GPRS_PROTO_IPV4V6)) {
		ril_gprs_context_set_ipv6(gc, split_ipv6_addr);
		ofono_gprs_context_set_ipv6_gateway(gc, ipv6_gw);
		ofono_gprs_context_set_ipv6_dns_servers(gc,
						(const char **) dns_ipv6_addr);
	}

	if (split_ip_addr &&
			(call->prot == OFONO_GPRS_PROTO_IP ||
			call->prot == OFONO_GPRS_PROTO_IPV4V6)) {
		ril_gprs_context_set_ipv4(gc, split_ip_addr);
		ofono_gprs_context_set_ipv4_gateway(gc, ip_gw);
		ofono_gprs_context_set_ipv4_dns_servers(gc,
						(const char **) dns_addr);
	}

done:
	g_strfreev(split_ip_addr);
	g_strfreev(split_ipv6_addr);
	g_strfreev(dns_addr);
	g_strfreev(dns_ipv6_addr);
	g_free(ip_gw);
	g_free(ipv6_gw);

	cb = gcd->activate.cb;
	cb_data = gcd->activate.data;
	GASSERT(gcd->activate.req);
	memset(&gcd->activate, 0, sizeof(gcd->activate));

	if (cb) {
		if (error.type != OFONO_ERROR_TYPE_NO_ERROR) {
			gcd->active_ctx_cid = CTX_ID_NONE;
		}
		cb(&error, cb_data);
	}
}

static void ril_gprs_context_activate_primary(struct ofono_gprs_context *gc,
				const struct ofono_gprs_primary_context *ctx,
				ofono_gprs_context_cb_t cb, void *data)
{
	struct ril_gprs_context *gcd = ril_gprs_context_get_data(gc);
	struct ofono_netreg *netreg = ril_modem_ofono_netreg(gcd->modem);
	const int rs = ofono_netreg_get_status(netreg);

	/* Let's make sure that we aren't connecting when roaming not allowed */
	if (rs == NETWORK_REGISTRATION_STATUS_ROAMING) {
		struct ofono_gprs *gprs = ril_modem_ofono_gprs(gcd->modem);
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
	GASSERT(!gcd->activate.req);
	GASSERT(ctx->cid != CTX_ID_NONE);

	gcd->active_ctx_cid = ctx->cid;
	gcd->activate.cb = cb;
	gcd->activate.data = data;
	gcd->activate.req = ril_data_call_setup(gcd->data, ctx,
				ril_gprs_context_activate_primary_cb, gcd);
}

static void ril_gprs_context_deactivate_primary_cb(struct ril_data *data,
					int ril_status, void *user_data)
{
	struct ril_gprs_context *gcd = user_data;

	/*
	 * Data call list may change before the completion of the deactivate
	 * request, in that case ril_gprs_context_set_disconnected will be
	 * invoked and gcd->deactivate.req will be NULL.
	 */
	if (gcd->deactivate.req) {
		struct ofono_error error;
		ofono_gprs_context_cb_t cb = gcd->deactivate.cb;
		gpointer cb_data = gcd->deactivate.data;

		if (ril_status == RIL_E_SUCCESS) {
			GASSERT(gcd->active_call);
			ril_error_init_ok(&error);
			ofono_info("Deactivated data call");
		} else {
			ril_error_init_failure(&error);
			ofono_error("Deactivate failure: %s",
					ril_error_to_string(ril_status));
		}

		memset(&gcd->deactivate, 0, sizeof(gcd->deactivate));
		if (cb) {
			ril_gprs_context_free_active_call(gcd);
			cb(&error, cb_data);
			return;
		}
	}

	/* Make sure we are in the disconnected state */
	ril_gprs_context_set_disconnected(gcd);
}

static void ril_gprs_context_deactivate_primary(struct ofono_gprs_context *gc,
		unsigned int id, ofono_gprs_context_cb_t cb, void *data)
{
	struct ril_gprs_context *gcd = ril_gprs_context_get_data(gc);

	GASSERT(gcd->active_call && gcd->active_ctx_cid == id);
	ofono_info("Deactivate primary");

	if (gcd->active_call && gcd->active_ctx_cid == id) {
		gcd->deactivate.cb = cb;
		gcd->deactivate.data = data;
		gcd->deactivate.req = ril_data_call_deactivate(gcd->data,
				gcd->active_call->cid,
				ril_gprs_context_deactivate_primary_cb, gcd);
	} else if (cb) {
		struct ofono_error error;
		cb(ril_error_ok(&error), data);
	}
}

static void ril_gprs_context_detach_shutdown(struct ofono_gprs_context *gc,
						unsigned int id)
{
	DBG("%d", id);
	ril_gprs_context_deactivate_primary(gc, id, NULL, NULL);
}

static int ril_gprs_context_probe(struct ofono_gprs_context *gc,
					unsigned int vendor, void *data)
{
	struct ril_modem *modem = data;
	struct ril_gprs_context *gcd = g_new0(struct ril_gprs_context, 1);

	DBG("");
	gcd->gc = gc;
	gcd->modem = modem;
	gcd->network = ril_network_ref(modem->network);
	gcd->data = ril_data_ref(modem->data);
	gcd->active_ctx_cid = CTX_ID_NONE;
	ofono_gprs_context_set_data(gc, gcd);
	return 0;
}

static void ril_gprs_context_remove(struct ofono_gprs_context *gc)
{
	struct ril_gprs_context *gcd = ril_gprs_context_get_data(gc);

	DBG("");
	ofono_gprs_context_set_data(gc, NULL);

	ril_data_request_cancel(gcd->activate.req);

	if (gcd->deactivate.req) {
		/* Let it complete but we won't be around to be notified. */
		ril_data_request_detach(gcd->deactivate.req);
	} else if (gcd->active_call) {
		ril_data_call_deactivate(gcd->data, gcd->active_call->cid,
								NULL, NULL);
	}

	ril_data_remove_handler(gcd->data, gcd->calls_changed_event_id);
	ril_data_unref(gcd->data);
	ril_network_unref(gcd->network);
	ril_data_call_free(gcd->active_call);
	ril_mtu_watch_free(gcd->mtu_watch);
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
