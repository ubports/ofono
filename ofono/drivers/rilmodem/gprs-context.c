/*
 *
 *  oFono - Open Source Telephony
 *
 *  Copyright (C) 2008-2011  Intel Corporation. All rights reserved.
 *  Copyright (C) 2013 Canonical Ltd.
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

#include "gril.h"
#include "grilutil.h"

#include "rilmodem.h"

/* REQUEST_DEACTIVATE_DATA_CALL parameter values */
#define DEACTIVATE_DATA_CALL_NUM_PARAMS 2
#define DEACTIVATE_DATA_CALL_NO_REASON "0"

/* REQUEST_SETUP_DATA_CALL parameter values */
#define SETUP_DATA_CALL_PARAMS 7
#define CHAP_PAP_OK "3"
#define DATA_PROFILE_DEFAULT "0"
#define PROTO_IP "IP"
#define PROTO_IPV6 "IPV6"
#define PROTO_IPV4V6 "IPV4V6"

enum state {
	STATE_IDLE,
	STATE_ENABLING,
	STATE_DISABLING,
	STATE_ACTIVE,
};

struct gprs_context_data {
	GRil *ril;
	unsigned int active_ctx_cid;
	unsigned int active_rild_cid;
	char username[OFONO_GPRS_MAX_USERNAME_LENGTH + 1];
	char password[OFONO_GPRS_MAX_PASSWORD_LENGTH + 1];
	enum state state;
};

/* TODO: make conditional */
static char print_buf[PRINT_BUF_SIZE];

static void ril_gprs_context_call_list_changed(struct ril_msg *message,
						gpointer user_data)
{
	struct ofono_gprs_context *gc = user_data;
	struct gprs_context_data *gcd = ofono_gprs_context_get_data(gc);
	struct data_call *call = NULL;
	gboolean active_cid_found = FALSE;
	gboolean disconnect = FALSE;
	GSList *calls = NULL, *iterator = NULL;

	DBG("");

	if (message->req != RIL_UNSOL_DATA_CALL_LIST_CHANGED) {
		ofono_error("ril_gprs_update_calls: invalid message received %d",
				message->req);
		return;
	}

	calls = ril_util_parse_data_call_list(message);

	DBG("number of call in call_list_changed is: %d", g_slist_length(calls));

	for (iterator = calls; iterator; iterator = iterator->next) {
		call = (struct data_call *) iterator->data;

		if (call->cid == gcd->active_rild_cid) {
			DBG("Found current call in call list: %d", call->cid);
			active_cid_found = TRUE;

			if (call->active == 0) {
				DBG("call->status is DISCONNECTED for cid: %d", call->cid);
				disconnect = TRUE;
				ofono_gprs_context_deactivated(gc, gcd->active_ctx_cid);
			}

			break;
		}
	}

	if (disconnect || active_cid_found == FALSE) {
		DBG("Clearing active context");

		gcd->active_ctx_cid = -1;
		gcd->active_rild_cid = -1;
		gcd->state = STATE_IDLE;
	}

	g_slist_foreach(calls, (GFunc) g_free, NULL);
	g_slist_free(calls);
}

static void ril_setup_data_call_cb(struct ril_msg *message, gpointer user_data)
{
	struct cb_data *cbd = user_data;
	ofono_gprs_context_cb_t cb = cbd->cb;
	struct ofono_gprs_context *gc = cbd->user;
	struct gprs_context_data *gcd = ofono_gprs_context_get_data(gc);
	struct ofono_error error;
	struct parcel rilp;
	int status, retry_time, cid, active, num, version;
	char *dnses = NULL, *ifname = NULL;
	char *raw_ip_addrs = NULL, *raw_gws = NULL, *type = NULL;
	char **dns_addresses = NULL, **gateways = NULL;
	char **ip_addrs = NULL, **split_ip_addr = NULL;

	/* TODO:
	 * Cleanup duplicate code between this function and
	 * ril_util_parse_data_call_list().
	 */

	/* valid size: 36 (34 if HCRADIO defined) */
	if (message->buf_len < 36) {
		DBG("Parcel is less then minimum DataCallResponseV6 size!");
		decode_ril_error(&error, "FAIL");
		goto error;
	}

	if (message->error != RIL_E_SUCCESS) {
		DBG("Reply failure: %s", ril_error_to_string(message->error));
		decode_ril_error(&error, "FAIL");
		error.error = message->error;
		goto error;
	}

	ril_util_init_parcel(message, &rilp);

	/*
	 * ril.h documents the reply to a RIL_REQUEST_SETUP_DATA_CALL
	 * as being a RIL_Data_Call_Response_v6 struct, however in
	 * reality, the response actually includes the version of the
	 * struct, followed by an array of calls, so the array size
	 * also has to be read after the version.
	 *
	 * TODO: What if there's more than 1 call in the list??
	 */
	version = parcel_r_int32(&rilp);
	num = parcel_r_int32(&rilp);

	status = parcel_r_int32(&rilp);
	retry_time = parcel_r_int32(&rilp);
	cid = parcel_r_int32(&rilp);
	active = parcel_r_int32(&rilp);

	type = parcel_r_string(&rilp);
	ifname = parcel_r_string(&rilp);
	raw_ip_addrs = parcel_r_string(&rilp);
	dnses = parcel_r_string(&rilp);
	raw_gws = parcel_r_string(&rilp);

	/* TODO: make conditional */
	ril_append_print_buf("[%04d]< %s",
			message->serial_no,
			ril_request_id_to_string(message->req));
	ril_start_response;

	ril_append_print_buf("%sversion=%d,num=%d",
			print_buf,
			version,
			num);

	ril_append_print_buf("%s [status=%d,retry=%d,cid=%d,active=%d,type=%s,ifname=%s,address=%s,dns=%s,gateways=%s]",
			print_buf,
			status,
			retry_time,
			cid,
			active,
			type,
			ifname,
			raw_ip_addrs,
			dnses,
			raw_gws);
	ril_close_response;
	ril_print_response;
	/* TODO: make conditional */

	if (status != 0) {
		DBG("Reply failure; status %d", status);
		gcd->state = STATE_IDLE;
		goto error;
	}

	gcd->state = STATE_ACTIVE;
	gcd->active_rild_cid = cid;

	ofono_gprs_context_set_interface(gc, ifname);

	/*
	 * TODO: re-factor the following code into a
	 * ril_util function that can be unit-tested.
	 */

	/* TODO:
	 * RILD can return multiple addresses; oFono only supports
	 * setting a single IPv4 address.  At this time, we only
	 * use the first address.  It's possible that a RIL may
	 * just specify the end-points of the point-to-point
	 * connection, in which case this code will need to
	 * changed to handle such a device.
	 *
	 * For now split into a maximum of three, and only use
	 * the first address for the remaining operations.
	 */
	ip_addrs = g_strsplit(raw_ip_addrs, " ", 3);
	if (ip_addrs[0] == NULL) {
		DBG("No IP address specified: %s", raw_ip_addrs);
		decode_ril_error(&error, "FAIL");
		goto error;
	}

	ofono_gprs_context_set_ipv4_netmask(gc,
			ril_util_get_netmask(ip_addrs[0]));

	/*
	 * Note - the address may optionally include a prefix size
	 * ( Eg. "/30" ).  As this confuses NetworkManager, we
	 * explicitly strip any prefix after calculating the netmask.
	 */
	split_ip_addr = g_strsplit(ip_addrs[0], "/", 2);
	if (split_ip_addr[0] == NULL) {
		DBG("Invalid IP address field returned: %s", raw_ip_addrs);
		decode_ril_error(&error, "FAIL");
		goto error;
	}

	ofono_gprs_context_set_ipv4_address(gc, split_ip_addr[0], TRUE);

	/*
	 * RILD can return multiple addresses; oFono only supports
	 * setting a single IPv4 gateway.
	 */
	gateways = g_strsplit(raw_gws, " ", 3);
	if (gateways[0] == NULL) {
		DBG("Invalid gateways field returned: %s", raw_gws);
		decode_ril_error(&error, "FAIL");
		goto error;
	}

	ofono_gprs_context_set_ipv4_gateway(gc, gateways[0]);

	/* Split DNS addresses */
	dns_addresses = g_strsplit(dnses, " ", 3);
	ofono_gprs_context_set_ipv4_dns_servers(gc,
						(const char **) dns_addresses);

	decode_ril_error(&error, "OK");

error:
	g_strfreev(dns_addresses);
	g_strfreev(ip_addrs);
	g_strfreev(split_ip_addr);
	g_strfreev(gateways);

	g_free(type);
	g_free(ifname);
	g_free(raw_ip_addrs);
	g_free(dnses);
	g_free(raw_gws);

	cb(&error, cbd->data);
}

static void ril_gprs_context_activate_primary(struct ofono_gprs_context *gc,
						const struct ofono_gprs_primary_context *ctx,
						ofono_gprs_context_cb_t cb, void *data)
{
	struct gprs_context_data *gcd = ofono_gprs_context_get_data(gc);
	struct cb_data *cbd = cb_data_new(cb, data);
	struct parcel rilp;
	gchar *protocol = PROTO_IP;
	gchar tech[3];
	int request = RIL_REQUEST_SETUP_DATA_CALL;
	int ret;

	cbd->user = gc;
	gcd->active_ctx_cid = ctx->cid;
	gcd->state = STATE_ENABLING;

	memcpy(gcd->username, ctx->username, sizeof(ctx->username));
	memcpy(gcd->password, ctx->password, sizeof(ctx->password));

	parcel_init(&rilp);
	parcel_w_int32(&rilp, SETUP_DATA_CALL_PARAMS);

        /* RadioTech: hardcoded to HSPA for now... */
	sprintf((char *) tech, "%d", (int) RADIO_TECH_HSPA);
	DBG("setting tech to: %s", tech);
	parcel_w_string(&rilp, (char *) tech);

        /*
	 * TODO ( OEM/Tethering ): DataProfile:
	 *
	 * Other options are TETHERING (1) or OEM_BASE (1000).
	 */
	parcel_w_string(&rilp, DATA_PROFILE_DEFAULT);

	/* APN */
	parcel_w_string(&rilp, (char *) (ctx->apn));

	if (ctx->username && strlen(ctx->username)) {
		parcel_w_string(&rilp, (char *) (ctx->username));
	} else {
		parcel_w_string(&rilp, NULL);
	}

	if (ctx->password && strlen(ctx->password)) {
		parcel_w_string(&rilp, (char *) (ctx->password));
	} else {
		parcel_w_string(&rilp, NULL);
	}

	/*
	 * TODO: review with operators...
         * Auth type: PAP/CHAP may be performed
	 */
	parcel_w_string(&rilp, CHAP_PAP_OK);

	switch (ctx->proto) {
	case OFONO_GPRS_PROTO_IPV6:
		protocol = PROTO_IPV6;
		break;
	case OFONO_GPRS_PROTO_IPV4V6:
		protocol = PROTO_IPV4V6;
		break;
	case OFONO_GPRS_PROTO_IP:
		break;
	default:
		DBG("Invalid protocol: %d", ctx->proto);
	}

	parcel_w_string(&rilp, protocol);

	ret = g_ril_send(gcd->ril,
				request,
				rilp.data,
				rilp.size,
				ril_setup_data_call_cb, cbd, g_free);

        /* TODO: make conditional */
	ril_start_request;
	ril_append_print_buf("%s %s,%s,%s,%s,%s,%s,%s",
			print_buf,
			tech,
			DATA_PROFILE_DEFAULT,
			ctx->apn,
			ctx->username,
			ctx->password,
			CHAP_PAP_OK,
			protocol);

	ril_close_request;
	ril_print_request(ret, request);
	/* TODO: make conditional */

	parcel_free(&rilp);
	if (ret <= 0) {
		ofono_error("Send RIL_REQUEST_SETUP_DATA_CALL failed.");

		g_free(cbd);
		CALLBACK_WITH_FAILURE(cb, data);
	}
}

static void ril_deactivate_data_call_cb(struct ril_msg *message, gpointer user_data)
{
	struct cb_data *cbd = user_data;
	ofono_gprs_context_cb_t cb = cbd->cb;
	struct ofono_gprs_context *gc = cbd->user;
	struct gprs_context_data *gcd = ofono_gprs_context_get_data(gc);
	struct ofono_error error;

	DBG("");

	/* Reply has no data... */
	if (message->error == RIL_E_SUCCESS) {

		/* TODO: make conditional */
		ril_append_print_buf("[%04d]< %s",
				message->serial_no,
				ril_request_id_to_string(message->req));
		ril_print_response;
		/* TODO: make conditional */

		gcd->state = STATE_IDLE;
		CALLBACK_WITH_SUCCESS(cb, cbd->data);

	} else {
		DBG("Reply failure: %s", ril_error_to_string(message->error));

		decode_ril_error(&error, "FAIL");
		error.error = message->error;

		cb(&error, cbd->data);
	}
}

static void ril_gprs_context_deactivate_primary(struct ofono_gprs_context *gc,
						unsigned int id,
						ofono_gprs_context_cb_t cb, void *data)
{
	struct gprs_context_data *gcd = ofono_gprs_context_get_data(gc);
	struct cb_data *cbd = cb_data_new(cb, data);
	struct parcel rilp;
	gchar *cid = NULL;
	int request = RIL_REQUEST_DEACTIVATE_DATA_CALL;
	int ret;

	cbd->user = gc;

	gcd->state = STATE_DISABLING;

	parcel_init(&rilp);
	parcel_w_int32(&rilp, DEACTIVATE_DATA_CALL_NUM_PARAMS);

	cid = g_strdup_printf("%d", gcd->active_rild_cid);
	parcel_w_string(&rilp, cid);

	/*
	 * TODO: airplane-mode; change reason to '1',
	 * which means "radio power off".
	 */
	parcel_w_string(&rilp, DEACTIVATE_DATA_CALL_NO_REASON);

	ret = g_ril_send(gcd->ril,
				request,
				rilp.data,
				rilp.size,
				ril_deactivate_data_call_cb, cbd, g_free);

	/* TODO: make conditional */
	ril_start_request;
	ril_append_print_buf("%s%s,0",
				print_buf,
				cid);

	ril_close_request;
	ril_print_request(ret, request);
	/* TODO: make conditional */

	parcel_free(&rilp);
	g_free(cid);

	if (ret <= 0) {
		ofono_error("Send RIL_REQUEST_DEACTIVATE_DATA_CALL failed.");
		g_free(cbd);
		CALLBACK_WITH_FAILURE(cb, data);
	}
}

static void ril_gprs_context_detach_shutdown(struct ofono_gprs_context *gc,
					unsigned int id)
{
	DBG("");
}

static int ril_gprs_context_probe(struct ofono_gprs_context *gc,
					unsigned int vendor, void *data)
{
	GRil *ril = data;
	struct gprs_context_data *gcd;

	DBG("");

	gcd = g_try_new0(struct gprs_context_data, 1);
	if (gcd == NULL)
		return -ENOMEM;

	gcd->ril = g_ril_clone(ril);
	gcd->active_ctx_cid = -1;
	gcd->active_rild_cid = -1;
	gcd->state = STATE_IDLE;

	ofono_gprs_context_set_data(gc, gcd);

	g_ril_register(gcd->ril, RIL_UNSOL_DATA_CALL_LIST_CHANGED,
			ril_gprs_context_call_list_changed, gc);
	return 0;
}

static void ril_gprs_context_remove(struct ofono_gprs_context *gc)
{
	struct gprs_context_data *gcd = ofono_gprs_context_get_data(gc);

	DBG("");

	if (gcd->state != STATE_IDLE) {
		/* TODO: call detach_shutdown */
	}

	ofono_gprs_context_set_data(gc, NULL);

	g_ril_unref(gcd->ril);
	g_free(gcd);
}

static struct ofono_gprs_context_driver driver = {
	.name			= "rilmodem",
	.probe			= ril_gprs_context_probe,
	.remove			= ril_gprs_context_remove,
	.activate_primary       = ril_gprs_context_activate_primary,
	.deactivate_primary     = ril_gprs_context_deactivate_primary,
	.detach_shutdown        = ril_gprs_context_detach_shutdown,
};

void ril_gprs_context_init(void)
{
	ofono_gprs_context_driver_register(&driver);
}

void ril_gprs_context_exit(void)
{
	ofono_gprs_context_driver_unregister(&driver);
}
