/*
 *
 *  RIL library with GLib integration
 *
 *  Copyright (C) 2008-2011  Intel Corporation. All rights reserved.
 *  Copyright (C) 2012-2013  Canonical Ltd.
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

#include <stdio.h>
#include <ctype.h>
#include <errno.h>
#include <string.h>

#include <glib.h>

#include <ofono/log.h>
#include <ofono/modem.h>
#include <ofono/gprs-context.h>
#include "util.h"

#include "common.h"
#include "grilunsol.h"

/* Minimum size is two int32s version/number of calls */
#define MIN_DATA_CALL_LIST_SIZE 8

/*
 * Minimum NITZ is: 'yy/mm/dd,hh:mm:ss'
 * TZ '(+/-)tz,dt' are optional
 */
#define MIN_NITZ_SIZE 17

static gint data_call_compare(gconstpointer a, gconstpointer b)
{
	const struct ril_data_call *ca = a;
	const struct ril_data_call *cb = b;

	if (ca->cid < cb->cid)
		return -1;

	if (ca->cid > cb->cid)
		return 1;

	return 0;
}

static void free_data_call(gpointer data, gpointer user_data)
{
	struct ril_data_call *call = data;

	if (call) {
		g_free(call->ifname);
		g_free(call->ip_addr);
		g_free(call->dns_addrs);
		g_free(call->gateways);
		g_free(call);
	}
}

void g_ril_unsol_free_data_call_list(struct ril_data_call_list *call_list)
{
	if (call_list) {
		g_slist_foreach(call_list->calls, (GFunc) free_data_call, NULL);
		g_slist_free(call_list->calls);
		g_free(call_list);
	}
}

static gboolean handle_settings(struct ril_data_call *call, char *type,
				char *ifname, char *raw_ip_addrs,
				char *raw_dns, char *raw_gws)
{
	gboolean result = FALSE;
	int protocol;
	char **dns_addrs = NULL, **gateways = NULL;
	char **ip_addrs = NULL, **split_ip_addr = NULL;

	protocol = ril_protocol_string_to_ofono_protocol(type);
	if (protocol < 0) {
		ofono_error("%s: invalid type(protocol) specified: %s",
				__func__, type);
		goto done;
	}

	if (ifname == NULL || strlen(ifname) == 0) {
		ofono_error("%s: no interface specified: %s",
				__func__, ifname);
		goto done;
	}

	/* Split DNS addresses */
	if (raw_dns)
		dns_addrs = g_strsplit(raw_dns, " ", 3);

	/*
	 * RILD can return multiple addresses; oFono only supports
	 * setting a single IPv4 gateway.
	 */
	if (raw_gws)
		gateways = g_strsplit(raw_gws, " ", 3);

	if (gateways == NULL || g_strv_length(gateways) == 0) {
		ofono_error("%s: no gateways: %s", __func__, raw_gws);
		goto done;
	}

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
	if (raw_ip_addrs)
		ip_addrs = g_strsplit(raw_ip_addrs, " ", 3);

	if (ip_addrs == NULL || g_strv_length(ip_addrs) == 0) {
		ofono_error("%s: no IP address: %s", __func__, raw_ip_addrs);
		goto done;
	}

	DBG("num ip addrs is: %d", g_strv_length(ip_addrs));

	if (g_strv_length(ip_addrs) > 1)
		ofono_warn("%s: more than one IP addr returned: %s", __func__,
				raw_ip_addrs);
	/*
	 * Note - the address may optionally include a prefix size
	 * ( Eg. "/30" ).  As this confuses NetworkManager, we
	 * explicitly strip any prefix after calculating the netmask.
	 */
	split_ip_addr = g_strsplit(ip_addrs[0], "/", 2);

	if (split_ip_addr == NULL || g_strv_length(split_ip_addr) == 0) {
		ofono_error("%s: invalid IP address field returned: %s",
				__func__, ip_addrs[0]);
		goto done;
	}

	call->protocol = protocol;
	call->ifname = g_strdup(ifname);
	call->ip_addr = g_strdup(split_ip_addr[0]);
	call->dns_addrs = g_strdupv(dns_addrs);
	call->gateways = g_strdupv(gateways);

	result = TRUE;

done:
	if (dns_addrs)
		g_strfreev(dns_addrs);

	if (gateways)
		g_strfreev(gateways);

	if (ip_addrs)
		g_strfreev(ip_addrs);

	if (split_ip_addr)
		g_strfreev(split_ip_addr);


	return result;
}

/*
 * This function handles RIL_UNSOL_DATA_CALL_LIST_CHANGED messages,
 * as well as RIL_REQUEST_DATA_CALL_LIST/SETUP_DATA_CALL replies, as
 * all have the same payload.
 */
struct ril_data_call_list *g_ril_unsol_parse_data_call_list(GRil *gril,
						const struct ril_msg *message)
{
	struct ril_data_call *call;
	struct parcel rilp;
	struct ril_data_call_list *reply = NULL;
	unsigned int active, cid, i, num_calls, retry, status;
	char *type = NULL, *ifname = NULL, *raw_addrs = NULL;
	char *raw_dns = NULL, *raw_gws = NULL;

	DBG("");

	/* Can happen for RIL_REQUEST_DATA_CALL_LIST replies */
	if (message->buf_len < MIN_DATA_CALL_LIST_SIZE) {
		if (message->req == RIL_REQUEST_SETUP_DATA_CALL) {
			ofono_error("%s: message too small: %d",
					__func__,
					(int) message->buf_len);
			goto error;
		} else {
			g_ril_append_print_buf(gril, "{");
			goto done;
		}
	}

	reply = g_try_new0(struct ril_data_call_list, 1);
	if (reply == NULL) {
		ofono_error("%s: out of memory", __func__);
		goto error;
	}

	g_ril_init_parcel(message, &rilp);

	/*
	 * ril.h documents the reply to a RIL_REQUEST_DATA_CALL_LIST
	 * as being an array of  RIL_Data_Call_Response_v6 structs,
	 * however in reality, the response also includes a version
	 * to start.
	 */
	reply->version = parcel_r_int32(&rilp);
	num_calls = parcel_r_int32(&rilp);

	g_ril_append_print_buf(gril,
				"{version=%d,num=%d",
				reply->version,
				num_calls);

	for (i = 0; i < num_calls; i++) {
		status = parcel_r_int32(&rilp);
		retry = parcel_r_int32(&rilp);          /* ignore */
		cid = parcel_r_int32(&rilp);
		active = parcel_r_int32(&rilp);
		type = parcel_r_string(&rilp);
		ifname = parcel_r_string(&rilp);
		raw_addrs = parcel_r_string(&rilp);
		raw_dns = parcel_r_string(&rilp);
		raw_gws = parcel_r_string(&rilp);

		/* malformed check */
		if (rilp.malformed) {
			ofono_error("%s: malformed parcel received", __func__);
			goto error;
		}

		g_ril_append_print_buf(gril,
					"%s [status=%d,retry=%d,cid=%d,"
					"active=%d,type=%s,ifname=%s,"
					"address=%s,dns=%s,gateways=%s]",
					print_buf,
					status,
					retry,
					cid,
					active,
					type,
					ifname,
					raw_addrs,
					raw_dns,
					raw_gws);

		call = g_try_new0(struct ril_data_call, 1);
		if (call == NULL) {
			ofono_error("%s: out of memory", __func__);
			goto error;
		}

		call->status = status;
		call->cid = cid;
		call->active = active;

		if (message->req == RIL_REQUEST_SETUP_DATA_CALL &&
			status == PDP_FAIL_NONE &&
			handle_settings(call, type, ifname, raw_addrs,
					raw_dns, raw_gws) == FALSE)
			goto error;

		g_free(type);
		g_free(ifname);
		g_free(raw_addrs);
		g_free(raw_dns);
		g_free(raw_gws);

		reply->calls =
			g_slist_insert_sorted(reply->calls, call,
						data_call_compare);
	}

done:
	g_ril_append_print_buf(gril, "%s}", print_buf);

	if (message->unsolicited)
		g_ril_print_unsol(gril, message);
	else
		g_ril_print_response(gril, message);

	return reply;

error:
	g_free(type);
	g_free(ifname);
	g_free(raw_addrs);
	g_free(raw_dns);
	g_free(raw_gws);
	g_ril_unsol_free_data_call_list(reply);

	return NULL;
}

char *g_ril_unsol_parse_nitz(GRil *gril, const struct ril_msg *message)
{
	struct parcel rilp;
	gchar *nitz = NULL;

	DBG("");

	if (message->buf_len < MIN_NITZ_SIZE) {
		ofono_error("%s: NITZ too small: %d",
				__func__,
				(int) message->buf_len);
		goto error;
	}

	g_ril_init_parcel(message, &rilp);

	nitz = parcel_r_string(&rilp);

	g_ril_append_print_buf(gril, "(%s)", nitz);
	g_ril_print_unsol(gril, message);

error:
	return nitz;
}

int g_ril_unsol_parse_radio_state_changed(GRil *gril,
						const struct ril_msg *message)
{
	struct parcel rilp;
	int radio_state;

	g_ril_init_parcel(message, &rilp);

	radio_state = parcel_r_int32(&rilp);

	if (rilp.malformed) {
		ofono_error("%s: malformed parcel received", __func__);
		radio_state = -1;
	}

	g_ril_append_print_buf(gril, "(state: %s)",
				ril_radio_state_to_string(radio_state));

	g_ril_print_unsol(gril, message);

	return radio_state;
}

/*
 * This function makes a similar processing to was is done by validateInput()
 * and getLteLevel() in $AOSP/frameworks/base/telephony/java/android/telephony/
 * SignalStrength.java. The main difference is that we linearly transform the
 * ranges to ofono's one, while AOSP gives number of bars in a non-linear way
 * (bins for each bar have different size). We rely on the indicator to obtain
 * a translation to bars that makes sense for humans.
 */
static int get_lte_strength(int signal, int rsrp, int rssnr)
{
	int s_rsrp = -1, s_rssnr = -1, s_signal = -1;

	/*
	 * The range of signal is specified to be [0, 31] by ril.h, but the code
	 * in SignalStrength.java contradicts this: valid values are (0-63, 99)
	 * as defined in TS 36.331 for E-UTRA rssi.
	 */
	signal = (signal >= 0 && signal <= 63) ? signal : INT_MAX;
	rsrp = (rsrp >= 44 && rsrp <= 140) ? -rsrp : INT_MAX;
	rssnr = (rssnr >= -200 && rssnr <= 300) ? rssnr : INT_MAX;

	/* Linearly transform [-140, -44] to [0, 100] */
	if (rsrp != INT_MAX)
		s_rsrp = (25 * rsrp + 3500) / 24;

	/* Linearly transform [-200, 300] to [0, 100] */
	if (rssnr != INT_MAX)
		s_rssnr = (rssnr + 200) / 5;

	if (s_rsrp != -1 && s_rssnr != -1)
		return s_rsrp < s_rssnr ? s_rsrp : s_rssnr;

	if (s_rssnr != -1)
		return s_rssnr;

	if (s_rsrp != -1)
		return s_rsrp;

	/* Linearly transform [0, 63] to [0, 100] */
	if (signal != INT_MAX)
		s_signal = (100 * signal) / 63;

	return s_signal;
}

/*
 * Comments to get_lte_strength() apply here also, changing getLteLevel() with
 * getGsmLevel(). The atmodem driver does exactly the same transformation with
 * the rssi from AT+CSQ command.
 */
static int get_gsm_strength(int signal)
{
	/* Checking the range contemplates also the case signal=99 (invalid) */
	if (signal >= 0 && signal <= 31)
		return (signal * 100) / 31;
	else
		return -1;
}

int g_ril_unsol_parse_signal_strength(GRil *gril, const struct ril_msg *message,
					int ril_tech)
{
	struct parcel rilp;
	int gw_sigstr, gw_signal, cdma_dbm, evdo_dbm;
	int lte_sigstr = -1, lte_rsrp = -1, lte_rssnr = -1;
	int lte_signal;
	int signal;

	g_ril_init_parcel(message, &rilp);

	/* RIL_SignalStrength_v5 */
	/* GW_SignalStrength */
	gw_sigstr = parcel_r_int32(&rilp);
	gw_signal = get_gsm_strength(gw_sigstr);
	parcel_r_int32(&rilp); /* bitErrorRate */

	/*
	 * CDMA/EVDO values are not processed as CDMA is not supported
	 */

	/* CDMA_SignalStrength */
	cdma_dbm = parcel_r_int32(&rilp);
	parcel_r_int32(&rilp); /* ecio */

	/* EVDO_SignalStrength */
	evdo_dbm = parcel_r_int32(&rilp);
	parcel_r_int32(&rilp); /* ecio */
	parcel_r_int32(&rilp); /* signalNoiseRatio */

	/* Present only for RIL_SignalStrength_v6 or newer */
	if (parcel_data_avail(&rilp) > 0) {
		/* LTE_SignalStrength */
		lte_sigstr = parcel_r_int32(&rilp);
		lte_rsrp = parcel_r_int32(&rilp);
		parcel_r_int32(&rilp); /* rsrq */
		lte_rssnr = parcel_r_int32(&rilp);
		parcel_r_int32(&rilp); /* cqi */
		lte_signal = get_lte_strength(lte_sigstr, lte_rsrp, lte_rssnr);
	} else {
		lte_signal = -1;
	}

	g_ril_append_print_buf(gril,
				"{gw: %d, cdma: %d, evdo: %d, lte: %d %d %d}",
				gw_sigstr, cdma_dbm, evdo_dbm, lte_sigstr,
				lte_rsrp, lte_rssnr);

	if (message->unsolicited)
		g_ril_print_unsol(gril, message);
	else
		g_ril_print_response(gril, message);

	/* Return the first valid one */
	if (gw_signal != -1 && lte_signal != -1)
		if (ril_tech == RADIO_TECH_LTE)
			signal = lte_signal;
		else
			signal = gw_signal;
	else if (gw_signal != -1)
		signal = gw_signal;
	else if (lte_signal != -1)
		signal = lte_signal;
	else
		signal = -1;

	return signal;
}
