/*
 *  oFono - Open Source Telephony - RIL-based devices
 *
 *  Copyright (C) 2015-2018 Jolla Ltd.
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
#include "ril_log.h"

#include <gutil_strv.h>

#include <arpa/inet.h>

#include "ofono.h"
#include "common.h"
#include "mtu-watch.h"

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
	gulong calls_changed_id;
	struct mtu_watch *mtu_watch;
	struct ril_data_call *active_call;
	struct ril_gprs_context_call activate;
	struct ril_gprs_context_call deactivate;
};

static inline struct ril_gprs_context *ril_gprs_context_get_data(
					struct ofono_gprs_context *gprs)
{
	return ofono_gprs_context_get_data(gprs);
}

static char *ril_gprs_context_netmask(const char *bits)
{
	if (bits) {
		int nbits = atoi(bits);
		if (nbits > 0 && nbits < 33) {
			const char* str;
			struct in_addr in;
			in.s_addr = htonl((nbits == 32) ? 0xffffffff :
					((1u << nbits)-1) << (32-nbits));
			str = inet_ntoa(in);
			if (str) {
				return g_strdup(str);
			}
		}
	}
	return NULL;
}

static int ril_gprs_context_address_family(const char *addr)
{
	if (strchr(addr, ':')) {
		return AF_INET6;
	} else if (strchr(addr, '.')) {
		return AF_INET;
	} else {
		return AF_UNSPEC;
	}
}

static void ril_gprs_context_free_active_call(struct ril_gprs_context *gcd)
{
	if (gcd->active_call) {
		ril_data_call_release(gcd->data, gcd->active_call->cid, gcd);
		ril_data_call_free(gcd->active_call);
		gcd->active_call = NULL;
	}
	if (gcd->calls_changed_id) {
		ril_data_remove_handler(gcd->data, gcd->calls_changed_id);
		gcd->calls_changed_id = 0;
	}
	if (gcd->mtu_watch) {
		mtu_watch_free(gcd->mtu_watch);
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
			gcd->mtu_watch = mtu_watch_new(MAX_MTU);
		}
		mtu_watch_set_ifname(gcd->mtu_watch, call->ifname);
		ril_data_call_grab(gcd->data, call->cid, gcd);
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

static void ril_gprs_context_set_address(struct ofono_gprs_context *gc,
					const struct ril_data_call *call)
{
	const char *ip_addr = NULL;
	char *ip_mask = NULL;
	const char *ipv6_addr = NULL;
	unsigned char ipv6_prefix_length = 0;
	char *tmp_ip_addr = NULL;
	char *tmp_ipv6_addr = NULL;
	char * const *list = call->addresses;
	const int n = gutil_strv_length(list);
	int i;

	for (i = 0; i < n && (!ipv6_addr || !ip_addr); i++) {
		const char *addr = list[i];
		switch (ril_gprs_context_address_family(addr)) {
		case AF_INET:
			if (!ip_addr) {
				const char* s = strstr(addr, "/");
				if (s) {
					const gsize len = s - addr;
					tmp_ip_addr = g_strndup(addr, len);
					ip_addr = tmp_ip_addr;
					ip_mask = ril_gprs_context_netmask(s+1);
				} else {
					ip_addr = addr;
				}
				if (!ip_mask) {
					ip_mask = g_strdup("255.255.255.0");
				}
			}
			break;
		case AF_INET6:
			if (!ipv6_addr) {
				const char* s = strstr(addr, "/");
				if (s) {
					const gsize len = s - addr;
					const int prefix = atoi(s + 1);
					tmp_ipv6_addr = g_strndup(addr, len);
					ipv6_addr = tmp_ipv6_addr;
					if (prefix >= 0 && prefix <= 128) {
						ipv6_prefix_length = prefix;
					}
				} else {
					ipv6_addr = addr;
				}
			}
		}
	}

	ofono_gprs_context_set_ipv4_address(gc, ip_addr, TRUE);
	ofono_gprs_context_set_ipv4_netmask(gc, ip_mask);
	ofono_gprs_context_set_ipv6_address(gc, ipv6_addr);
	ofono_gprs_context_set_ipv6_prefix_length(gc, ipv6_prefix_length);

	if (!ip_addr && !ipv6_addr) {
		ofono_error("GPRS context: No IP address");
	}

	/* Allocate temporary strings */
	g_free(ip_mask);
	g_free(tmp_ip_addr);
	g_free(tmp_ipv6_addr);
}

static void ril_gprs_context_set_gateway(struct ofono_gprs_context *gc,
					const struct ril_data_call *call)
{
	const char *ip_gw = NULL;
	const char *ipv6_gw = NULL;
	char * const *list = call->gateways;
	const int n = gutil_strv_length(list);
	int i;

	/* Pick 1 gw for each protocol*/
	for (i = 0; i < n && (!ipv6_gw || !ip_gw); i++) {
		const char *addr = list[i];
		switch (ril_gprs_context_address_family(addr)) {
		case AF_INET:
			if (!ip_gw) ip_gw = addr;
			break;
		case AF_INET6:
			if (!ipv6_gw) ipv6_gw = addr;
			break;
		}
	}

	ofono_gprs_context_set_ipv4_gateway(gc, ip_gw);
	ofono_gprs_context_set_ipv6_gateway(gc, ipv6_gw);
}

static void ril_gprs_context_set_dns_servers(struct ofono_gprs_context *gc,
					const struct ril_data_call *call)
{
	int i;
	char * const *list = call->dnses;
	const int n = gutil_strv_length(list);
	const char **ip_dns = g_new0(const char *, n+1);
	const char **ipv6_dns = g_new0(const char *, n+1);
	const char **ip_ptr = ip_dns;
	const char **ipv6_ptr = ipv6_dns;

	for (i = 0; i < n; i++) {
		const char *addr = list[i];
		switch (ril_gprs_context_address_family(addr)) {
		case AF_INET:
			*ip_ptr++ = addr;
			break;
		case AF_INET6:
			*ipv6_ptr++ = addr;
			break;
		}
	}

	ofono_gprs_context_set_ipv4_dns_servers(gc, ip_dns);
	ofono_gprs_context_set_ipv6_dns_servers(gc, ipv6_dns);

	g_free(ip_dns);
	g_free(ipv6_dns);
}

/* Only compares the stuff that's important to us */
#define DATA_CALL_IFNAME_CHANGED    (0x01)
#define DATA_CALL_ADDRESS_CHANGED   (0x02)
#define DATA_CALL_GATEWAY_CHANGED   (0x04)
#define DATA_CALL_DNS_CHANGED       (0x08)
#define DATA_CALL_ALL_CHANGED       (0x0f)
static int ril_gprs_context_data_call_change(
			const struct ril_data_call *c1,
			const struct ril_data_call *c2)
{
	if (!c1 && !c2) {
		return 0;
	} else if (c1 && c2) {
		int changes = 0;

		if (g_strcmp0(c1->ifname, c2->ifname)) {
			changes |= DATA_CALL_IFNAME_CHANGED;
		}

		if (!gutil_strv_equal(c1->addresses, c2->addresses)) {
			changes |= DATA_CALL_ADDRESS_CHANGED;
		}

		if (!gutil_strv_equal(c1->gateways, c2->gateways)) {
			changes |= DATA_CALL_GATEWAY_CHANGED;
		}

		if (!gutil_strv_equal(c1->dnses, c2->dnses)) {
			changes |= DATA_CALL_DNS_CHANGED;
		}

		return changes;
	} else {
		return DATA_CALL_ALL_CHANGED;
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
	int change = 0;

	if (call && call->active != RIL_DATA_CALL_INACTIVE) {
		/* Compare it against the last known state */
		change = ril_gprs_context_data_call_change(call, prev_call);
	} else {
		ofono_error("Clearing active context");
		ril_gprs_context_set_disconnected(gcd);
		call = NULL;
	}

	if (!call) {
		/* We are not interested */
		return;
	} else if (!change) {
		DBG("call %u didn't change", call->cid);
		return;
	} else {
		DBG("call %u changed", call->cid);
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

	if (change & DATA_CALL_IFNAME_CHANGED) {
		DBG("interface changed");
		ofono_gprs_context_set_interface(gc, call->ifname);
	}

	if (change & DATA_CALL_ADDRESS_CHANGED) {
		DBG("address changed");
		ril_gprs_context_set_address(gc, call);
	}

	if (change & DATA_CALL_GATEWAY_CHANGED) {
		DBG("gateway changed");
		ril_gprs_context_set_gateway(gc, call);
	}

	if (change & DATA_CALL_DNS_CHANGED) {
		DBG("name server(s) changed");
		ril_gprs_context_set_dns_servers(gc, call);
	}

	ofono_gprs_context_signal_change(gc, gcd->active_ctx_cid);
	ril_data_call_free(prev_call);
}

static void ril_gprs_context_activate_primary_cb(struct ril_data *data,
			int ril_status, const struct ril_data_call *call,
			void *user_data)
{
	struct ril_gprs_context *gcd = user_data;
	struct ofono_gprs_context *gc = gcd->gc;
	struct ofono_error error;
	ofono_gprs_context_cb_t cb;
	gpointer cb_data;

	ril_error_init_failure(&error);
	if (ril_status != RIL_E_SUCCESS) {
		ofono_error("GPRS context: Reply failure: %s",
					ril_error_to_string(ril_status));
	} else if (!call) {
		ofono_error("Unexpected data call failure");
	} else if (call->status != PDP_FAIL_NONE) {
		ofono_error("Unexpected data call status %d", call->status);
		error.type = OFONO_ERROR_TYPE_CMS;
		error.error = call->status;
	} else if (!call->ifname) {
		/* Must have interface */
		ofono_error("GPRS context: No interface");
	} else {
		ofono_info("setting up data call");

		GASSERT(!gcd->calls_changed_id);
		ril_data_remove_handler(gcd->data, gcd->calls_changed_id);
		gcd->calls_changed_id =
			ril_data_add_calls_changed_handler(gcd->data,
				ril_gprs_context_call_list_changed, gcd);

		ril_gprs_context_set_active_call(gcd, call);
		ofono_gprs_context_set_interface(gc, call->ifname);
		ril_gprs_context_set_address(gc, call);
		ril_gprs_context_set_gateway(gc, call);
		ril_gprs_context_set_dns_servers(gc, call);
		ril_error_init_ok(&error);
	}

	if (error.type != OFONO_ERROR_TYPE_NO_ERROR) {
		gcd->active_ctx_cid = CTX_ID_NONE;
	}

	cb = gcd->activate.cb;
	cb_data = gcd->activate.data;
	GASSERT(gcd->activate.req);
	memset(&gcd->activate, 0, sizeof(gcd->activate));

	if (cb) {
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
		if (!__ofono_gprs_get_roaming_allowed(gprs) &&
			ril_netreg_check_if_really_roaming(netreg, rs) ==
					NETWORK_REGISTRATION_STATUS_ROAMING) {
			struct ofono_error error;
			ofono_info("Can't activate context %u (roaming)",
								ctx->cid);
			cb(ril_error_failure(&error), data);
			return;
		}
	}

	ofono_info("Activating context: %u", ctx->cid);
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
		ofono_gprs_context_cb_t cb = gcd->deactivate.cb;
		gpointer cb_data = gcd->deactivate.data;

		if (ril_status == RIL_E_SUCCESS) {
			GASSERT(gcd->active_call);
			ofono_info("Deactivated data call");
		} else {
			ofono_error("Deactivate failure: %s",
					ril_error_to_string(ril_status));
		}

		memset(&gcd->deactivate, 0, sizeof(gcd->deactivate));
		if (cb) {
			struct ofono_error error;

			ril_gprs_context_free_active_call(gcd);
			cb(ril_error_ok(&error), cb_data);
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

	GASSERT(gcd->active_ctx_cid == id);
	ofono_info("Deactivating context: %u", id);

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
	DBG("%u", id);
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

	if (gcd->activate.req) {
		/*
		 * The core has already completed its pending D-Bus
		 * request, invoking the completion callback will
		 * cause libdbus to panic.
		 */
		ril_data_request_detach(gcd->activate.req);
		ril_data_request_cancel(gcd->activate.req);
	}

	if (gcd->deactivate.req) {
		/* Let it complete but we won't be around to be notified. */
		ril_data_request_detach(gcd->deactivate.req);
	} else if (gcd->active_call) {
		ril_data_call_deactivate(gcd->data, gcd->active_call->cid,
								NULL, NULL);
	}

	ril_data_remove_handler(gcd->data, gcd->calls_changed_id);
	ril_data_unref(gcd->data);
	ril_network_unref(gcd->network);
	ril_data_call_free(gcd->active_call);
	mtu_watch_free(gcd->mtu_watch);
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
