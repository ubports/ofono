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

#include "ofono.h"

#include "grilunsol.h"

#include "gprs.h"
#include "rilmodem.h"

#define NUM_DEACTIVATION_RETRIES 4
#define TIME_BETWEEN_DEACT_RETRIES_S 2

enum state {
	STATE_IDLE,
	STATE_ENABLING,
	STATE_DISABLING,
	STATE_ACTIVE,
};

struct gprs_context_data {
	GRil *ril;
	struct ofono_modem *modem;
	unsigned vendor;
	gint active_ctx_cid;
	gint active_rild_cid;
	enum state state;
	guint call_list_id;
	char *apn;
	enum ofono_gprs_context_type type;
	int deact_retries;
	guint retry_ev_id;
	struct cb_data *retry_cbd;
	guint reset_ev_id;
};

static void ril_gprs_context_deactivate_primary(struct ofono_gprs_context *gc,
						unsigned int id,
						ofono_gprs_context_cb_t cb,
						void *data);
static void ril_deactivate_data_call_cb(struct ril_msg *message,
					gpointer user_data);

static void set_context_disconnected(struct gprs_context_data *gcd)
{
	DBG("");

	gcd->active_ctx_cid = -1;
	gcd->active_rild_cid = -1;
	gcd->state = STATE_IDLE;
	g_free(gcd->apn);
	gcd->apn = NULL;
}

static void disconnect_context(struct ofono_gprs_context *gc)
{
	ril_gprs_context_deactivate_primary(gc, 0, NULL, NULL);
}

static void ril_gprs_context_call_list_changed(struct ril_msg *message,
						gpointer user_data)
{
	struct ofono_gprs_context *gc = user_data;
	struct gprs_context_data *gcd = ofono_gprs_context_get_data(gc);
	struct ril_data_call *call = NULL;
	struct ril_data_call_list *call_list;
	gboolean active_cid_found = FALSE;
	gboolean disconnect = FALSE;
	GSList *iterator = NULL;

	call_list = g_ril_unsol_parse_data_call_list(gcd->ril, message);
	if (call_list == NULL)
		return;

	DBG("*gc: %p num calls: %d", gc, g_slist_length(call_list->calls));

	for (iterator = call_list->calls; iterator; iterator = iterator->next) {
		call = (struct ril_data_call *) iterator->data;

		if (call->cid == gcd->active_rild_cid) {
			active_cid_found = TRUE;
			DBG("found call - cid: %d", call->cid);

			if (call->active == 0) {
				DBG("call !active; notify disconnect: %d",
					call->cid);
				disconnect = TRUE;
			}

			break;
		}
	}

	if ((disconnect == TRUE || active_cid_found == FALSE)
		&& gcd->state != STATE_IDLE) {
		ofono_info("Clearing active context; disconnect: %d"
			" active_cid_found: %d active_ctx_cid: %d",
			disconnect, active_cid_found, gcd->active_ctx_cid);

		ofono_gprs_context_deactivated(gc, gcd->active_ctx_cid);
		set_context_disconnected(gcd);
	}

	g_ril_unsol_free_data_call_list(call_list);
}

static void ril_setup_data_call_cb(struct ril_msg *message, gpointer user_data)
{
	struct cb_data *cbd = user_data;
	ofono_gprs_context_cb_t cb = cbd->cb;
	struct ofono_gprs_context *gc = cbd->user;
	struct gprs_context_data *gcd = ofono_gprs_context_get_data(gc);
	struct ril_data_call *call = NULL;
	struct ril_data_call_list *call_list = NULL;

	DBG("*gc: %p", gc);

	if (message->error != RIL_E_SUCCESS) {
		ofono_error("%s: setup data call failed for apn: %s - %s",
				__func__, gcd->apn,
				ril_error_to_string(message->error));

		set_context_disconnected(gcd);
		goto error;
	}

	call_list = g_ril_unsol_parse_data_call_list(gcd->ril, message);
	if (call_list == NULL) {
		/* parsing failed, need to actually disconnect */
		disconnect_context(gc);
		goto error;
	}

	if (g_slist_length(call_list->calls) != 1) {
		ofono_error("%s: setup_data_call reply for apn: %s,"
				" includes %d calls",
				__func__, gcd->apn,
				g_slist_length(call_list->calls));

		disconnect_context(gc);
		goto error;
	}

	call = (struct ril_data_call *) call_list->calls->data;

	/* Check for valid DNS settings, except for MMS contexts */
	if (gcd->type != OFONO_GPRS_CONTEXT_TYPE_MMS
				&& (call->dns_addrs == NULL
				|| g_strv_length(call->dns_addrs) == 0)) {
		ofono_error("%s: no DNS in context of type %d",
				__func__, gcd->type);
		disconnect_context(gc);
		goto error;
	}

	if (call->status != PDP_FAIL_NONE) {
		ofono_error("%s: reply->status for apn: %s, is non-zero: %s",
				__func__, gcd->apn,
				ril_pdp_fail_to_string(call->status));

		set_context_disconnected(gcd);
		goto error;
	}

	gcd->active_rild_cid = call->cid;
	gcd->state = STATE_ACTIVE;

	ofono_gprs_context_set_interface(gc, call->ifname);
	ofono_gprs_context_set_ipv4_netmask(gc,
					ril_util_get_netmask(call->ip_addr));

	ofono_gprs_context_set_ipv4_address(gc, call->ip_addr, TRUE);
	ofono_gprs_context_set_ipv4_gateway(gc, call->gateways[0]);

	ofono_gprs_context_set_ipv4_dns_servers(gc,
					(const char **) call->dns_addrs);

	g_ril_unsol_free_data_call_list(call_list);

	/* activate listener for data call changed events.... */
	gcd->call_list_id =
		g_ril_register(gcd->ril,
				RIL_UNSOL_DATA_CALL_LIST_CHANGED,
				ril_gprs_context_call_list_changed, gc);

	CALLBACK_WITH_SUCCESS(cb, cbd->data);
	return;

error:
	g_ril_unsol_free_data_call_list(call_list);

	CALLBACK_WITH_FAILURE(cb, cbd->data);
}

#define DATA_PROFILE_DEFAULT_STR "0"
#define DATA_PROFILE_TETHERED_STR "1"
#define DATA_PROFILE_IMS_STR "2"
#define DATA_PROFILE_FOTA_STR "3"
#define DATA_PROFILE_CBS_STR "4"
#define DATA_PROFILE_OEM_BASE_STR "1000"
#define DATA_PROFILE_MTK_MMS_STR "1001"

static void ril_gprs_context_activate_primary(struct ofono_gprs_context *gc,
				const struct ofono_gprs_primary_context *ctx,
				ofono_gprs_context_cb_t cb, void *data)
{
	struct gprs_context_data *gcd = ofono_gprs_context_get_data(gc);
	struct ofono_modem *modem = ofono_gprs_context_get_modem(gc);
	struct ofono_atom *gprs_atom =
		__ofono_modem_find_atom(modem, OFONO_ATOM_TYPE_GPRS);
	struct ofono_gprs *gprs = NULL;
	struct ril_gprs_data *gd = NULL;
	struct cb_data *cbd = cb_data_new(cb, data, gc);
	struct parcel rilp;
	char buf[256];
	int num_param = 7;
	int tech;
	const char *profile;
	int auth_type;

	gprs = __ofono_atom_get_data(gprs_atom);
	gd = ofono_gprs_get_data(gprs);

	/*
	 * 0: CDMA 1: GSM/UMTS, 2...
	 * anything 2+ is a RadioTechnology value +2
	 */
	DBG("*gc: %p activating cid: %d; curr_tech: %d", gc, ctx->cid,
		gd->tech);

	parcel_init(&rilp);

	if (g_ril_vendor(gcd->ril) == OFONO_RIL_VENDOR_MTK)
		num_param += 1;

	parcel_w_int32(&rilp, num_param);

	if (gd->tech == RADIO_TECH_UNKNOWN) {
		ofono_error("%s: radio tech for apn: %s UNKNOWN!", __func__,
				gcd->apn);
		tech = 1;
	} else if (gd->tech <= RADIO_TECH_GSM) {
		tech = gd->tech + 2;
	} else

	sprintf(buf, "%d", tech);
	parcel_w_string(&rilp, buf);

	profile = DATA_PROFILE_DEFAULT_STR;

	if (g_ril_vendor(gcd->ril) == OFONO_RIL_VENDOR_MTK &&
			gcd->type == OFONO_GPRS_CONTEXT_TYPE_MMS)
		profile = DATA_PROFILE_MTK_MMS_STR;

	parcel_w_string(&rilp, profile);
	parcel_w_string(&rilp, ctx->apn);
	parcel_w_string(&rilp, ctx->username);
	parcel_w_string(&rilp, ctx->password);

	/*
	 * We do the same as in $AOSP/frameworks/opt/telephony/src/java/com/
	 * android/internal/telephony/dataconnection/DataConnection.java,
	 * onConnect(), and use authentication or not depending on whether
	 * the user field is empty or not.
	 */
	if (ctx->username[0] != '\0')
		auth_type = RIL_AUTH_BOTH;
	else
		auth_type = RIL_AUTH_NONE;

	sprintf(buf, "%d", auth_type);
	parcel_w_string(&rilp, buf);

	parcel_w_string(&rilp, ril_util_gprs_proto_to_ril_string(ctx->proto));

	if (g_ril_vendor(gcd->ril) == OFONO_RIL_VENDOR_MTK) {
		sprintf(buf, "%u", ctx->cid);
		parcel_w_string(&rilp, buf);

		g_ril_append_print_buf(gcd->ril, "(%d,%s,%s,%s,%s,%d,%s,%u)",
				tech, profile, ctx->apn, ctx->username,
				ctx->password, auth_type,
				ril_util_gprs_proto_to_ril_string(ctx->proto),
				ctx->cid);
	} else
		g_ril_append_print_buf(gcd->ril, "(%d,%s,%s,%s,%s,%d,%s)",
				tech, profile, ctx->apn, ctx->username,
				ctx->password, auth_type,
				ril_util_gprs_proto_to_ril_string(ctx->proto));

	if (g_ril_send(gcd->ril, RIL_REQUEST_SETUP_DATA_CALL, &rilp,
				ril_setup_data_call_cb, cbd, g_free) > 0) {
		gcd->apn = g_strdup(ctx->apn);
		gcd->active_ctx_cid = ctx->cid;
		gcd->state = STATE_ENABLING;

		return;
	}

	g_free(cbd);
	CALLBACK_WITH_FAILURE(cb, data);
}

static gboolean reset_modem(gpointer data)
{
	/* TODO call mtk_reset_modem when driver is upstreamed */
	return FALSE;
}

static gboolean retry_deactivate(gpointer user_data)
{
	struct cb_data *cbd = user_data;
	ofono_gprs_context_cb_t cb = cbd->cb;
	struct ofono_gprs_context *gc = cbd->user;
	struct gprs_context_data *gcd = ofono_gprs_context_get_data(gc);
	struct parcel rilp;

	gcd->retry_ev_id = 0;

	/* We might have received a call list update while waiting */
	if (gcd->state == STATE_IDLE) {
		if (cb)
			CALLBACK_WITH_SUCCESS(cb, cbd->data);

		g_free(cbd);

		return FALSE;
	}

	ril_util_build_deactivate_data_call(gcd->ril, &rilp,
					gcd->active_rild_cid,
					RIL_DEACTIVATE_DATA_CALL_NO_REASON);

	if (g_ril_send(gcd->ril, RIL_REQUEST_DEACTIVATE_DATA_CALL, &rilp,
			ril_deactivate_data_call_cb, cbd, g_free) == 0) {
		ofono_error("%s: send DEACTIVATE_DATA_CALL failed for apn: %s",
				__func__, gcd->apn);
		if (cb)
			CALLBACK_WITH_FAILURE(cb, cbd->data);

		g_free(cbd);
	}

	return FALSE;
}

static void ril_deactivate_data_call_cb(struct ril_msg *message,
					gpointer user_data)
{
	struct cb_data *cbd = user_data;
	ofono_gprs_context_cb_t cb = cbd->cb;
	struct ofono_gprs_context *gc = cbd->user;
	struct gprs_context_data *gcd = ofono_gprs_context_get_data(gc);
	gint active_ctx_cid;

	DBG("*gc: %p", gc);

	if (message->error == RIL_E_SUCCESS) {
		g_ril_print_response_no_args(gcd->ril, message);

		active_ctx_cid = gcd->active_ctx_cid;
		set_context_disconnected(gcd);

		/*
		 * If the deactivate was a result of a data network detach or of
		 * an error in data call establishment, there won't be call
		 * back, so _deactivated() needs to be called directly.
		 */
		if (cb)
			CALLBACK_WITH_SUCCESS(cb, cbd->data);
		else
			ofono_gprs_context_deactivated(gc, active_ctx_cid);

	} else {
		ofono_error("%s: reply failure for apn: %s - %s",
				__func__, gcd->apn,
				ril_error_to_string(message->error));

		/*
		 * It has been detected that some modems fail the deactivation
		 * temporarily. We do retries to handle that case.
		 */
		if (--(gcd->deact_retries) > 0) {
			gcd->retry_cbd = cb_data_new(cb, cbd->data, gc);
			gcd->retry_ev_id =
				g_timeout_add_seconds(
					TIME_BETWEEN_DEACT_RETRIES_S,
					retry_deactivate, gcd->retry_cbd);
		} else {
			ofono_error("%s: retry limit hit", __func__);

			if (cb)
				CALLBACK_WITH_FAILURE(cb, cbd->data);

			/*
			 * Reset modem if MTK. TODO Failures deactivating a
			 * context have not been reported for other modems, but
			 * it would be good to have a generic method to force an
			 * internal reset nonetheless.
			 */
			if (gcd->vendor == OFONO_RIL_VENDOR_MTK)
				gcd->reset_ev_id = g_idle_add(reset_modem, gcd);
		}
	}
}

static void ril_gprs_context_deactivate_primary(struct ofono_gprs_context *gc,
					unsigned int id,
					ofono_gprs_context_cb_t cb, void *data)
{
	struct gprs_context_data *gcd = ofono_gprs_context_get_data(gc);
	struct cb_data *cbd = NULL;
	struct parcel rilp;

	DBG("*gc: %p cid: %d active_rild_cid: %d", gc, id,
		gcd->active_rild_cid);

	if (gcd->state == STATE_IDLE || gcd->state == STATE_DISABLING) {
		/* nothing to do */

		if (cb) {
			CALLBACK_WITH_SUCCESS(cb, data);
			g_free(cbd);
		}

		return;
	}

	cbd = cb_data_new(cb, data, gc);

	gcd->state = STATE_DISABLING;
	if (g_ril_unregister(gcd->ril, gcd->call_list_id) == FALSE) {
		ofono_warn("%s: couldn't remove call_list listener"
				" for apn: %s.",
				__func__, gcd->apn);
	}

	gcd->deact_retries = NUM_DEACTIVATION_RETRIES;
	ril_util_build_deactivate_data_call(gcd->ril, &rilp,
					gcd->active_rild_cid,
					RIL_DEACTIVATE_DATA_CALL_NO_REASON);

	if (g_ril_send(gcd->ril, RIL_REQUEST_DEACTIVATE_DATA_CALL, &rilp,
				ril_deactivate_data_call_cb, cbd, g_free) > 0)
		return;

	/* TODO: should we force state to disconnected here? */
	ofono_error("%s: send DEACTIVATE_DATA_CALL failed for apn: %s",
				__func__, gcd->apn);

	if (cb)
			CALLBACK_WITH_FAILURE(cb, data);

	g_free(cbd);
}

static void ril_gprs_context_detach_shutdown(struct ofono_gprs_context *gc,
						unsigned int id)
{
	DBG("*gc: %p cid: %d", gc, id);

	ril_gprs_context_deactivate_primary(gc, 0, NULL, NULL);
}

static int ril_gprs_context_probe(struct ofono_gprs_context *gc,
					unsigned int vendor, void *data)
{
	struct ril_gprs_context_data *ril_data = data;
	struct gprs_context_data *gcd;

	DBG("*gc: %p", gc);

	gcd = g_try_new0(struct gprs_context_data, 1);
	if (gcd == NULL)
		return -ENOMEM;

	gcd->ril = g_ril_clone(ril_data->gril);
	gcd->modem = ril_data->modem;
	gcd->vendor = vendor;
	set_context_disconnected(gcd);
	gcd->call_list_id = -1;
	gcd->type = ril_data->type;

	ofono_gprs_context_set_data(gc, gcd);

	return 0;
}

static void ril_gprs_context_remove(struct ofono_gprs_context *gc)
{
	struct gprs_context_data *gcd = ofono_gprs_context_get_data(gc);

	DBG("*gc: %p", gc);

	if (gcd->state != STATE_IDLE && gcd->state != STATE_DISABLING) {
		struct parcel rilp;

		ril_util_build_deactivate_data_call(gcd->ril, &rilp,
					gcd->active_rild_cid,
					RIL_DEACTIVATE_DATA_CALL_NO_REASON);

		g_ril_send(gcd->ril, RIL_REQUEST_DEACTIVATE_DATA_CALL,
						&rilp, NULL, NULL, NULL);
	}

	if (gcd->retry_ev_id > 0) {
		g_source_remove(gcd->retry_ev_id);
		g_free(gcd->retry_cbd);
	}

	if (gcd->reset_ev_id > 0)
		g_source_remove(gcd->reset_ev_id);

	ofono_gprs_context_set_data(gc, NULL);

	g_ril_unref(gcd->ril);
	g_free(gcd);
}

static struct ofono_gprs_context_driver driver = {
	.name			= RILMODEM,
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
