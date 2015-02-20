/*
 *
 *  oFono - Open Source Telephony
 *
 *  Copyright (C) 2008-2011  Intel Corporation. All rights reserved.
 *  Copyright (C) 2013 Canonical Ltd.
 *  Copyright (C) 2013 Jolla Ltd.
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
#include <sys/stat.h>

#include <glib.h>

#include <ofono/log.h>
#include <ofono/modem.h>
#include <ofono/gprs-context.h>
#include <ofono/types.h>

#include "grilreply.h"
#include "grilrequest.h"
#include "grilunsol.h"

#include "common.h"

#include "rilmodem.h"

enum data_call_state {
	DATA_CALL_INACTIVE,
	DATA_CALL_LINK_DOWN,
	DATA_CALL_ACTIVE,
};

enum state {
	STATE_IDLE,
	STATE_ENABLING,
	STATE_DISABLING,
	STATE_ACTIVE,
};

struct gprs_context_data {
	GRil *ril;
	guint active_ctx_cid;
	gint active_rild_cid;
	enum state state;
	guint regid;
	struct unsol_data_call_list *old_list;
	guint prev_active_status;
};

static void set_context_disconnected(struct gprs_context_data *gcd)
{
	gcd->active_ctx_cid = -1;
	gcd->active_rild_cid = -1;
	gcd->state = STATE_IDLE;
}

static void ril_gprs_split_ip_by_protocol(char **ip_array,
						char ***split_ip_addr,
						char ***split_ipv6_addr,
						char **ip_addr)
{
	const char ipv6_delimiter = ':';
	const char ip_delimiter = '.';
	int i;

	*split_ipv6_addr = *split_ip_addr = NULL;
	for (i=0; i< g_strv_length(ip_array); i++) {
		if (strchr(ip_array[i], ipv6_delimiter)) {
			if (*split_ipv6_addr == NULL) {
				*split_ipv6_addr = g_strsplit(
							ip_array[i], "/",2);
			}
		} else if (strchr(ip_array[i], ip_delimiter)) {
			if (*split_ip_addr == NULL) {
				*ip_addr = g_strdup(ip_array[i]);
				*split_ip_addr = g_strsplit(
							ip_array[i], "/", 2);
			}
		}
	}
}

static void ril_gprs_split_gw_by_protocol(char **gw_array, char **ip_gw,
								char **ipv6_gw)
{
	const char ipv6_delimiter = ':';
	const char ip_delimiter = '.';
	int i;

	*ip_gw = *ipv6_gw = NULL;
	for (i=0; i< g_strv_length(gw_array); i++) {
		if (strchr(gw_array[i],ipv6_delimiter)) {
			if (*ipv6_gw == NULL) {
				*ipv6_gw = g_strdup(gw_array[i]);
			}
		} else if (strchr(gw_array[i],ip_delimiter)) {
			if (*ip_gw == NULL)
				*ip_gw = g_strdup(gw_array[i]);
		}
	}
}

static void ril_gprs_split_dns_by_protocol(char **dns_array, char ***dns_addr,
							char ***dns_ipv6_addr)
{
	const char ipv6_delimiter = ':';
	const char ip_delimiter = '.';
	char *temp = NULL;
	char *temp1 = NULL;
	char *dnsip = NULL;
	char *dnsipv6 = NULL;
	int i, dnsip_len, dnsipv6_len;

	dnsip_len = dnsipv6_len = 0;

	for (i=0; i< g_strv_length(dns_array); i++) {
		if (strchr(dns_array[i],ipv6_delimiter)) {
			if (dnsipv6 == NULL) {
				dnsipv6 = g_strdup(dns_array[i]);
			} else {
				temp = g_strconcat(dnsipv6, ",", NULL);
				g_free(dnsipv6);
				temp1 = g_strconcat(temp, dns_array[i], NULL);
				g_free(temp);
				dnsipv6 = temp1;
			}
			dnsipv6_len++;
		} else if (strchr(dns_array[i],ip_delimiter)) {
			if (dnsip == NULL) {
				dnsip = g_strdup(dns_array[i]);
			} else {
				temp = g_strconcat(dnsip, ",", NULL);
				g_free(dnsip);
				temp1 = g_strconcat(temp, dns_array[i], NULL);
				g_free(temp);
				dnsip = temp1;

			}
			dnsip_len++;
		}
	}

	if (dnsip)
		*dns_addr = g_strsplit(dnsip, ",", dnsip_len);

	if (dnsipv6)
		*dns_ipv6_addr = g_strsplit(dnsipv6, ",", dnsipv6_len);

	g_free(dnsip);
	g_free(dnsipv6);
}

static void ril_gprs_context_call_list_changed(struct ril_msg *message,
						gpointer user_data)
{
	struct ofono_gprs_context *gc = user_data;
	struct gprs_context_data *gcd = ofono_gprs_context_get_data(gc);
	struct data_call *call = NULL;
	struct unsol_data_call_list *unsol;
	gboolean disconnect = FALSE;
	gboolean signal = FALSE;
	GSList *iterator = NULL;
	struct ofono_error error;

	unsol = g_ril_unsol_parse_data_call_list(gcd->ril, message, &error);

	if (error.type != OFONO_ERROR_TYPE_NO_ERROR)
		goto error;

	if (g_ril_unsol_cmp_dcl(unsol,gcd->old_list,gcd->active_rild_cid))
		goto error;

	g_ril_unsol_free_data_call_list(gcd->old_list);
	gcd->old_list = unsol;

	DBG("number of call in call_list_changed is: %d", unsol->num);

	for (iterator = unsol->call_list; iterator; iterator = iterator->next) {
		call = (struct data_call *) iterator->data;

		/*
		 * Every context receives notifications about all data calls
		 * but should only handle its own.
		 */
		if (call->cid != gcd->active_rild_cid)
			continue;

		if (call->active == DATA_CALL_LINK_DOWN)
			gcd->prev_active_status = call->active;

		if (call->status != 0)
			ofono_info("data call status:%d", call->status);

		if (call->active == DATA_CALL_INACTIVE) {
			disconnect = TRUE;
			gcd->prev_active_status = call->active;
			ofono_gprs_context_deactivated(gc, gcd->active_ctx_cid);
			break;
		}

		if (call->active == DATA_CALL_ACTIVE) {
			int protocol = -1;

			if (gcd->prev_active_status != DATA_CALL_LINK_DOWN)
				signal = TRUE;

			gcd->prev_active_status = call->active;

			if (call->type)
				protocol = ril_protocol_string_to_ofono_protocol(call->type);

			if (call->ifname)
				ofono_gprs_context_set_interface(gc,
								call->ifname);

			if (call->addresses) {
				char **split_ip_addr = NULL;
				char **ip_array = NULL;
				char **split_ipv6_addr = NULL;
				char *ip_addr = NULL;

				/*addresses to an array*/
				ip_array = g_strsplit(call->addresses, " ",-1);

				/*pick 1 address of each protocol*/
				ril_gprs_split_ip_by_protocol(ip_array,
								&split_ip_addr,
								&split_ipv6_addr,
								&ip_addr);

				if ((protocol == OFONO_GPRS_PROTO_IPV4V6 ||
						protocol == OFONO_GPRS_PROTO_IPV6)
						&& split_ipv6_addr != NULL){

					ofono_gprs_context_set_ipv6_address(gc,
							split_ipv6_addr[0]);
				}

				if ((protocol == OFONO_GPRS_PROTO_IPV4V6 ||
						protocol == OFONO_GPRS_PROTO_IP)
						&& split_ip_addr != NULL) {

					ofono_gprs_context_set_ipv4_netmask(gc,
						ril_util_get_netmask(ip_addr));
					ofono_gprs_context_set_ipv4_address(gc,
							split_ip_addr[0], TRUE);
				}

				g_strfreev(split_ip_addr);
				g_strfreev(split_ipv6_addr);
				g_strfreev(ip_array);
				g_free(ip_addr);
			}

			if (call->gateways) {
				char **gw_array = NULL;
				char *ip_gw = NULL;
				char *ipv6_gw = NULL;
				/*addresses to an array*/
				gw_array = g_strsplit(call->gateways, " ", -1);

				/*pick 1 gw for each protocol*/
				ril_gprs_split_gw_by_protocol(gw_array, &ip_gw,
								&ipv6_gw);

				if ((protocol == OFONO_GPRS_PROTO_IPV4V6 ||
						protocol == OFONO_GPRS_PROTO_IPV6)
						&& ipv6_gw != NULL)
					ofono_gprs_context_set_ipv6_gateway(gc,
								ipv6_gw);

				if ((protocol == OFONO_GPRS_PROTO_IPV4V6 ||
						protocol == OFONO_GPRS_PROTO_IP)
						&& ip_gw != NULL)
					ofono_gprs_context_set_ipv4_gateway(gc,
								ip_gw);

				g_strfreev(gw_array);
				g_free(ip_gw);
				g_free(ipv6_gw);
			}

			if (call->dnses) {
				char **dns_array = NULL;
				char **dns_ip = NULL;
				char **dns_ipv6 = NULL;

				/*addresses to an array*/
				dns_array = g_strsplit(call->dnses, " ", -1);

				/*split based on protocol*/
				ril_gprs_split_dns_by_protocol(dns_array,
								&dns_ip,
								&dns_ipv6);

				if ((protocol == OFONO_GPRS_PROTO_IPV4V6 ||
						protocol == OFONO_GPRS_PROTO_IPV6)
						&& dns_ipv6 != NULL)
					ofono_gprs_context_set_ipv6_dns_servers(
						gc, (const char **) dns_ipv6);

				if ((protocol == OFONO_GPRS_PROTO_IPV4V6 ||
						protocol == OFONO_GPRS_PROTO_IP)
						&& dns_ip != NULL)
					ofono_gprs_context_set_ipv4_dns_servers(
						gc, (const char**)dns_ip);

				g_strfreev(dns_ip);
				g_strfreev(dns_ipv6);
				g_strfreev(dns_array);
			}
			break;
		}
	}

	if (disconnect) {
		ofono_error("Clearing active context");
		set_context_disconnected(gcd);
		gcd->old_list = NULL;
		goto error;
	}

	if (signal)
		ofono_gprs_context_signal_change(gc, gcd->active_ctx_cid);

	return;

error:
	g_ril_unsol_free_data_call_list(unsol);
}

static void ril_setup_data_call_cb(struct ril_msg *message, gpointer user_data)
{
	struct cb_data *cbd = user_data;
	ofono_gprs_context_cb_t cb = cbd->cb;
	struct ofono_gprs_context *gc = cbd->user;
	struct gprs_context_data *gcd = ofono_gprs_context_get_data(gc);
	struct ofono_error error;
	struct reply_setup_data_call *reply = NULL;
	char **split_ip_addr = NULL;
	char **split_ipv6_addr = NULL;
	char* ip_addr = NULL;
	char* ip_gw = NULL;
	char* ipv6_gw = NULL;
	char** dns_addr = NULL;
	char** dns_ipv6_addr = NULL;

	ofono_info("setting up data call");

	if (message->error != RIL_E_SUCCESS) {
		ofono_error("GPRS context: Reply failure: %s",
			    ril_error_to_string(message->error));

		error.type = OFONO_ERROR_TYPE_FAILURE;
		error.error = message->error;

		set_context_disconnected(gcd);
		goto error;
	}

	reply = g_ril_reply_parse_data_call(gcd->ril, message, &error);

	gcd->active_rild_cid = reply->cid;

	if (error.type != OFONO_ERROR_TYPE_NO_ERROR) {
		ofono_error("no active context. disconnect");
		goto error;
	}

	if (reply->status != 0) {
		ofono_error("%s: reply->status is non-zero: %d",
				__func__,
				reply->status);

		error.type = OFONO_ERROR_TYPE_FAILURE;
		error.error = reply->status;

		goto error;
	}

	/*check the ip address protocol*/
	ril_gprs_split_ip_by_protocol(reply->ip_addrs, &split_ip_addr,
						&split_ipv6_addr, &ip_addr);

	if (split_ip_addr == NULL && split_ipv6_addr == NULL) {
		ofono_error("%s: No IP address returned",
				__func__);

		error.type = OFONO_ERROR_TYPE_FAILURE;
		error.error = EINVAL;

		set_context_disconnected(gcd);
		goto error;
	}

	gcd->state = STATE_ACTIVE;

	ofono_gprs_context_set_interface(gc, reply->ifname);

	ril_gprs_split_gw_by_protocol(reply->gateways, &ip_gw, &ipv6_gw);

	ril_gprs_split_dns_by_protocol(reply->dns_addresses, &dns_addr,
								&dns_ipv6_addr);

	/* TODO:
	 * RILD can return multiple addresses; oFono only supports setting
	 * a single IPv4 and single IPV6 address. At this time, we only use
	 * the first address. It's possible that a RIL may just specify
	 * the end-points of the point-to-point connection, in which case this
	 * code will need to changed to handle such a device.
	 */

	if (split_ipv6_addr != NULL &&
			(reply->protocol == OFONO_GPRS_PROTO_IPV6 ||
			reply->protocol == OFONO_GPRS_PROTO_IPV4V6)) {

		ofono_gprs_context_set_ipv6_address(gc, split_ipv6_addr[0]);
		ofono_gprs_context_set_ipv6_gateway(gc, ipv6_gw);
		ofono_gprs_context_set_ipv6_dns_servers(gc,
						(const char **) dns_ipv6_addr);
	}

	if (split_ip_addr != NULL &&
			(reply->protocol == OFONO_GPRS_PROTO_IP ||
			reply->protocol == OFONO_GPRS_PROTO_IPV4V6)) {
		ofono_gprs_context_set_ipv4_netmask(gc,
					ril_util_get_netmask(ip_addr));
		ofono_gprs_context_set_ipv4_address(gc, split_ip_addr[0], TRUE);
		ofono_gprs_context_set_ipv4_gateway(gc, ip_gw);
		ofono_gprs_context_set_ipv4_dns_servers(gc,
						(const char **) dns_addr);
	}
error:
	g_ril_reply_free_setup_data_call(reply);
	g_strfreev(split_ip_addr);
	g_strfreev(split_ipv6_addr);
	g_strfreev(dns_addr);
	g_strfreev(dns_ipv6_addr);
	g_free(ip_addr);
	g_free(ip_gw);
	g_free(ipv6_gw);

	cb(&error, cbd->data);
}

static void ril_gprs_context_activate_primary(struct ofono_gprs_context *gc,
				const struct ofono_gprs_primary_context *ctx,
				ofono_gprs_context_cb_t cb, void *data)
{
	struct gprs_context_data *gcd = ofono_gprs_context_get_data(gc);
	struct cb_data *cbd = cb_data_new(cb, data);
	struct req_setup_data_call request;
	struct parcel rilp;
	struct ofono_error error;
	int reqid = RIL_REQUEST_SETUP_DATA_CALL;
	int ret = 0;
	int netreg_status;
	int roaming = NETWORK_REGISTRATION_STATUS_ROAMING;

	ofono_info("Activating context: %d", ctx->cid);

	/* Let's make sure that we aren't connecting when roaming not allowed */
	netreg_status = get_current_network_status();
	if (netreg_status == roaming) {
		if (!ril_roaming_allowed() && (roaming
				== check_if_really_roaming(netreg_status)))
			goto exit;
	}

	cbd->user = gc;

	/* TODO: implement radio technology selection. */
	request.tech = RADIO_TECH_HSPA;

	/* TODO: add comments about tethering, other non-public
	 * profiles...
	 */
	request.data_profile = RIL_DATA_PROFILE_DEFAULT;
	request.apn = g_strdup(ctx->apn);
	request.username = g_strdup(ctx->username);
	request.password = g_strdup(ctx->password);
	request.auth_type = RIL_AUTH_BOTH;

	request.protocol = ctx->proto;

	if (g_ril_request_setup_data_call(gcd->ril,
						&request,
						&rilp,
						&error) == FALSE) {
		ofono_error("Couldn't build SETUP_DATA_CALL request.");
		goto error;
	}

	gcd->active_ctx_cid = ctx->cid;
	gcd->state = STATE_ENABLING;

	ret = g_ril_send(gcd->ril,
				reqid,
				rilp.data,
				rilp.size,
				ril_setup_data_call_cb, cbd, g_free);

	/* NOTE - we could make the following function part of g_ril_send? */
	g_ril_print_request(gcd->ril, ret, reqid);

	parcel_free(&rilp);

error:
	g_free(request.apn);
	g_free(request.username);
	g_free(request.password);
exit:
	if (ret <= 0) {
		ofono_error("Send RIL_REQUEST_SETUP_DATA_CALL failed.");

		set_context_disconnected(gcd);

		g_free(cbd);
		CALLBACK_WITH_FAILURE(cb, data);
	}
}

static void ril_deactivate_data_call_cb(struct ril_msg *message,
						gpointer user_data)
{
	struct cb_data *cbd = user_data;
	ofono_gprs_context_cb_t cb = cbd->cb;
	struct ofono_gprs_context *gc = cbd->user;
	struct gprs_context_data *gcd = ofono_gprs_context_get_data(gc);
	gint id = gcd->active_ctx_cid;

	ofono_info("deactivating data call");

	/* Reply has no data... */
	if (message->error == RIL_E_SUCCESS) {

		g_ril_print_response_no_args(gcd->ril, message);

		set_context_disconnected(gcd);

		/* If the deactivate was a result of a shutdown,
		 * there won't be call back, so _deactivated()
		 * needs to be called directly.
		 */
		if (cb)
			CALLBACK_WITH_SUCCESS(cb, cbd->data);
		else
			ofono_gprs_context_deactivated(gc, id);

	} else {
		ofono_error("%s: replay failure: %s",
				__func__,
				ril_error_to_string(message->error));

		if (cb)
			CALLBACK_WITH_FAILURE(cb, cbd->data);
	}
}

static void ril_gprs_context_deactivate_primary(struct ofono_gprs_context *gc,
						unsigned int id,
						ofono_gprs_context_cb_t cb,
						void *data)
{
	struct gprs_context_data *gcd = ofono_gprs_context_get_data(gc);
	struct cb_data *cbd = NULL;
	struct parcel rilp;
	struct req_deactivate_data_call request;
	struct ofono_error error;
	int reqid = RIL_REQUEST_DEACTIVATE_DATA_CALL;
	int ret = 0;

	ofono_info("deactivate primary");

	if (gcd->active_rild_cid == -1) {
		set_context_disconnected(gcd);

		if (cb) {
			CALLBACK_WITH_SUCCESS(cb, data);
			g_free(cbd);
		}

		return;
	}


	cbd = cb_data_new(cb, data);
	cbd->user = gc;

	gcd->state = STATE_DISABLING;

	request.cid = gcd->active_rild_cid;
	request.reason = RIL_DEACTIVATE_DATA_CALL_NO_REASON;

	if (g_ril_request_deactivate_data_call(gcd->ril, &request,
						&rilp, &error) == FALSE) {
		ofono_error("Couldn't build DEACTIVATE_DATA_CALL request.");
		goto error;
	}

	ret = g_ril_send(gcd->ril,
				reqid,
				rilp.data,
				rilp.size,
				ril_deactivate_data_call_cb, cbd, g_free);

	g_ril_append_print_buf(gcd->ril, "(%d,0)", request.cid);
	g_ril_print_request(gcd->ril, ret, reqid);

	parcel_free(&rilp);

error:
	if (ret <= 0) {
		ofono_error("Send RIL_REQUEST_DEACTIVATE_DATA_CALL failed.");
		g_free(cbd);

		if (cb)
			CALLBACK_WITH_FAILURE(cb, data);
	}
}

static void ril_gprs_context_detach_shutdown(struct ofono_gprs_context *gc,
						unsigned int id)
{
	DBG("cid: %d", id);

	ril_gprs_context_deactivate_primary(gc, 0, NULL, NULL);
}

static int ril_gprs_context_probe(struct ofono_gprs_context *gc,
					unsigned int vendor, void *data)
{
	GRil *ril = data;
	struct gprs_context_data *gcd;

	gcd = g_try_new0(struct gprs_context_data, 1);
	if (gcd == NULL)
		return -ENOMEM;

	gcd->ril = g_ril_clone(ril);
	set_context_disconnected(gcd);

	ofono_gprs_context_set_data(gc, gcd);

	gcd->regid = g_ril_register(gcd->ril, RIL_UNSOL_DATA_CALL_LIST_CHANGED,
					ril_gprs_context_call_list_changed, gc);
	return 0;
}

static void ril_gprs_context_remove(struct ofono_gprs_context *gc)
{
	struct gprs_context_data *gcd = ofono_gprs_context_get_data(gc);

	DBG("");

	g_ril_unsol_free_data_call_list(gcd->old_list);

	if (gcd->state != STATE_IDLE)
		ril_gprs_context_detach_shutdown(gc, 0);

	ofono_gprs_context_set_data(gc, NULL);

	if (gcd->regid != -1)
		g_ril_unregister(gcd->ril, gcd->regid);

	g_ril_unref(gcd->ril);
	g_free(gcd);
}

static struct ofono_gprs_context_driver driver = {
	.name			= RILMODEM,
	.probe			= ril_gprs_context_probe,
	.remove			= ril_gprs_context_remove,
	.activate_primary	= ril_gprs_context_activate_primary,
	.deactivate_primary	= ril_gprs_context_deactivate_primary,
	.detach_shutdown	= ril_gprs_context_detach_shutdown,
};

void ril_gprs_context_init(void)
{
	ofono_gprs_context_driver_register(&driver);
}

void ril_gprs_context_exit(void)
{
	ofono_gprs_context_driver_unregister(&driver);
}
