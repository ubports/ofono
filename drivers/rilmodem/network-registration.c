/*
 *
 *  oFono - Open Source Telephony - RIL Modem Support
 *
 *  Copyright (C) 2008-2011  Intel Corporation. All rights reserved.
 *  Copyright (C) 2010  ST-Ericsson AB.
 *  Copyright (C) 2012-2013  Canonical Ltd.
 *  Copyright (C) 2013  Jolla Ltd.
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

#include <string.h>
#include <stdlib.h>
#include <stdio.h>

#include <glib.h>

#include <ofono/log.h>
#include <ofono/modem.h>
#include <ofono/netreg.h>

#if __GNUC__ > 7
#pragma GCC diagnostic ignored "-Wrestrict"
#endif

#include <gril/gril.h>

#include "common.h"
#include "rilmodem.h"

struct netreg_data {
	GRil *ril;
	char mcc[OFONO_MAX_MCC_LENGTH + 1];
	char mnc[OFONO_MAX_MNC_LENGTH + 1];
	int signal_index; /* If strength is reported via CIND */
	int signal_min; /* min strength reported via CIND */
	int signal_max; /* max strength reported via CIND */
	int signal_invalid; /* invalid strength reported via CIND */
	int tech;
	guint nitz_timeout;
	unsigned int vendor;
};

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

static int parse_signal_strength(GRil *gril, const struct ril_msg *message,
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

static void ril_registration_status(struct ofono_netreg *netreg,
					ofono_netreg_status_cb_t cb,
					void *data);

static int ril_tech_to_access_tech(int ril_tech)
{
	/*
	 * This code handles the mapping between the RIL_RadioTechnology
	 * and ofono's access technology values ( see <Act> values - 27.007
	 * Section 7.3 ).
	 */

	switch (ril_tech) {
	case RADIO_TECH_UNKNOWN:
		return -1;
	case RADIO_TECH_GSM:
	case RADIO_TECH_GPRS:
		return ACCESS_TECHNOLOGY_GSM;
	case RADIO_TECH_EDGE:
		return ACCESS_TECHNOLOGY_GSM_EGPRS;
	case RADIO_TECH_UMTS:
		return ACCESS_TECHNOLOGY_UTRAN;
	case RADIO_TECH_HSDPA:
		return ACCESS_TECHNOLOGY_UTRAN_HSDPA;
	case RADIO_TECH_HSUPA:
		return ACCESS_TECHNOLOGY_UTRAN_HSUPA;
	case RADIO_TECH_HSPAP:
	case RADIO_TECH_HSPA:
		/* HSPAP is HSPA+; which ofono doesn't define;
		 * so, if differentiating HSPA and HSPA+ is
		 * important, then ofono needs to be patched,
		 * and we probably also need to introduce a
		 * new indicator icon.
		 */

		return ACCESS_TECHNOLOGY_UTRAN_HSDPA_HSUPA;
	case RADIO_TECH_LTE:
		return ACCESS_TECHNOLOGY_EUTRAN;
	default:
		return -1;
	}
}

static void extract_mcc_mnc(const char *str, char *mcc, char *mnc)
{
	/* Three digit country code */
	strncpy(mcc, str, OFONO_MAX_MCC_LENGTH);
	mcc[OFONO_MAX_MCC_LENGTH] = '\0';

	/* Usually a 2 but sometimes 3 digit network code */
	strncpy(mnc, str + OFONO_MAX_MCC_LENGTH, OFONO_MAX_MNC_LENGTH);
	mnc[OFONO_MAX_MNC_LENGTH] = '\0';
}

static void ril_creg_cb(struct ril_msg *message, gpointer user_data)
{
	struct cb_data *cbd = user_data;
	ofono_netreg_status_cb_t cb = cbd->cb;
	struct netreg_data *nd = cbd->user;
	struct parcel rilp;
	char **strv;
	int num_str;
	char *debug_str;
	int status = -1;
	int lac = -1;
	int ci = -1;
	int tech = -1;
	char *end;

	DBG("");

	if (message->error != RIL_E_SUCCESS) {
		ofono_error("%s: failed to pull registration state",
				__func__);
		goto error;
	}

	g_ril_init_parcel(message, &rilp);
	strv = parcel_r_strv(&rilp);
	num_str = g_strv_length(strv);

	if (strv == NULL)
		goto error;

	debug_str = g_strjoinv(",", strv);
	g_ril_append_print_buf(nd->ril, "{%d,%s}", num_str, debug_str);
	g_free(debug_str);
	g_ril_print_response(nd->ril, message);

	status = strtoul(strv[0], &end, 10);
	if (end == strv[0] || *end != '\0')
		goto error_free;

	status = ril_util_registration_state_to_status(status);
	if (status < 0)
		goto error_free;

	if (num_str >= 2) {
		lac = strtoul(strv[1], &end, 16);
		if (end == strv[1] || *end != '\0')
			lac = -1;
	}

	if (num_str >= 3) {
		ci = strtoul(strv[2], &end, 16);
		if (end == strv[2] || *end != '\0')
			ci = -1;
	}

	if (num_str >= 4) {
		tech = strtoul(strv[3], &end, 10);
		if (end == strv[3] || *end != '\0')
			tech = -1;

		if (g_ril_vendor(nd->ril) == OFONO_RIL_VENDOR_MTK) {
			switch (tech) {
			case MTK_RADIO_TECH_HSDPAP:
			case MTK_RADIO_TECH_HSDPAP_UPA:
			case MTK_RADIO_TECH_HSUPAP:
			case MTK_RADIO_TECH_HSUPAP_DPA:
				tech = RADIO_TECH_HSPAP;
				break;
			case MTK_RADIO_TECH_DC_DPA:
				tech = RADIO_TECH_HSDPA;
				break;
			case MTK_RADIO_TECH_DC_UPA:
				tech = RADIO_TECH_HSUPA;
				break;
			case MTK_RADIO_TECH_DC_HSDPAP:
			case MTK_RADIO_TECH_DC_HSDPAP_UPA:
			case MTK_RADIO_TECH_DC_HSDPAP_DPA:
			case MTK_RADIO_TECH_DC_HSPAP:
				tech = RADIO_TECH_HSPAP;
				break;
			}
		}
	}

	g_strfreev(strv);
	nd->tech = tech;

	CALLBACK_WITH_SUCCESS(cb, status, lac, ci,
				ril_tech_to_access_tech(tech),
				cbd->data);
	return;

error_free:
	g_strfreev(strv);
error:
	CALLBACK_WITH_FAILURE(cb, -1, -1, -1, -1, cbd->data);
}

static void ril_creg_notify(struct ofono_error *error, int status, int lac,
					int ci, int tech, gpointer user_data)
{
	struct ofono_netreg *netreg = user_data;

	if (error->type != OFONO_ERROR_TYPE_NO_ERROR) {
		DBG("Error during status notification");
		return;
	}

	ofono_netreg_status_notify(netreg, status, lac, ci, tech);
}

static void ril_network_state_change(struct ril_msg *message,
							gpointer user_data)
{
	struct ofono_netreg *netreg = user_data;
	struct netreg_data *nd = ofono_netreg_get_data(netreg);

	g_ril_print_unsol_no_args(nd->ril, message);

	ril_registration_status(netreg, NULL, NULL);
}

static void ril_registration_status(struct ofono_netreg *netreg,
					ofono_netreg_status_cb_t cb,
					void *data)
{
	struct netreg_data *nd = ofono_netreg_get_data(netreg);
	struct cb_data *cbd;

	/*
	 * If no cb specified, setup internal callback to
	 * handle unsolicited VOICE_NET_STATE_CHANGE events.
	 */
	if (cb == NULL)
		cbd = cb_data_new(ril_creg_notify, netreg, nd);
	else
		cbd = cb_data_new(cb, data, nd);

	if (g_ril_send(nd->ril, RIL_REQUEST_VOICE_REGISTRATION_STATE, NULL,
			ril_creg_cb, cbd, g_free) == 0) {
		g_free(cbd);
		CALLBACK_WITH_FAILURE(cb, -1, -1, -1, -1, data);
	}
}

static void set_oper_name(const char *lalpha, const char *salpha,
				struct ofono_network_operator *op)
{
	/* Try to use long by default */
	if (lalpha)
		strncpy(op->name, lalpha, OFONO_MAX_OPERATOR_NAME_LENGTH);
	else if (salpha)
		strncpy(op->name, salpha, OFONO_MAX_OPERATOR_NAME_LENGTH);
}

static void ril_cops_cb(struct ril_msg *message, gpointer user_data)
{
	struct cb_data *cbd = user_data;
	ofono_netreg_operator_cb_t cb = cbd->cb;
	struct netreg_data *nd = cbd->user;
	struct ofono_network_operator op;
	struct parcel rilp;
	int num_params;
	char *lalpha;
	char *salpha;
	char *numeric;

	DBG("");

	if (message->error != RIL_E_SUCCESS)
		goto error;

	/*
	 * Minimum message length is 16:
	 * - array size
	 * - 3 NULL strings
	 */
	if (message->buf_len < 16) {
		ofono_error("%s: invalid OPERATOR reply: "
				"size too small (< 16): %d ",
				__func__,
				(int) message->buf_len);
		goto error;
	}

	g_ril_init_parcel(message, &rilp);

	num_params = parcel_r_int32(&rilp);
	if (num_params != 3) {
		ofono_error("%s: invalid OPERATOR reply: "
				"number of params is %d; should be 3.",
				__func__,
				num_params);
		goto error;
	}

	lalpha = parcel_r_string(&rilp);
	salpha = parcel_r_string(&rilp);
	numeric = parcel_r_string(&rilp);

	g_ril_append_print_buf(nd->ril,
				"(lalpha=%s, salpha=%s, numeric=%s)",
				lalpha, salpha, numeric);

	g_ril_print_response(nd->ril, message);

	if ((lalpha == NULL && salpha == NULL) || numeric == NULL) {
		g_free(lalpha);
		g_free(salpha);
		g_free(numeric);
		goto error;
	}

	set_oper_name(lalpha, salpha, &op);
	extract_mcc_mnc(numeric, op.mcc, op.mnc);
	op.status = OPERATOR_STATUS_CURRENT;
	op.tech = ril_tech_to_access_tech(nd->tech);

	g_free(lalpha);
	g_free(salpha);
	g_free(numeric);

	CALLBACK_WITH_SUCCESS(cb, &op, cbd->data);
	return;

error:
	CALLBACK_WITH_FAILURE(cb, NULL, cbd->data);
}

static void ril_current_operator(struct ofono_netreg *netreg,
				ofono_netreg_operator_cb_t cb, void *data)
{
	struct netreg_data *nd = ofono_netreg_get_data(netreg);
	struct cb_data *cbd = cb_data_new(cb, data, nd);

	if (g_ril_send(nd->ril, RIL_REQUEST_OPERATOR, NULL,
			ril_cops_cb, cbd, g_free) == 0) {
		g_free(cbd);
		CALLBACK_WITH_FAILURE(cb, NULL, data);
	}
}

static void ril_cops_list_cb(struct ril_msg *message, gpointer user_data)
{
	struct cb_data *cbd = user_data;
	ofono_netreg_operator_list_cb_t cb = cbd->cb;
	struct netreg_data *nd = cbd->user;
	struct ofono_network_operator *ops;
	struct parcel rilp;
	int num_ops;
	unsigned int i = 0;
	unsigned int num_strings;
	int strings_per_opt = 4;

	DBG("");

	if (message->error != RIL_E_SUCCESS)
		goto error;

	/*
	 * Minimum message length is 4:
	 * - array size
	 */
	if (message->buf_len < 4) {
		ofono_error("%s: invalid QUERY_AVAIL_NETWORKS reply: "
				"size too small (< 4): %d ",
				__func__,
				(int) message->buf_len);
		goto error;
	}

	g_ril_init_parcel(message, &rilp);
	g_ril_append_print_buf(nd->ril, "{");

	if (g_ril_vendor(nd->ril) == OFONO_RIL_VENDOR_MTK)
		strings_per_opt = 5;

	/* Number of operators at the list */
	num_strings = (unsigned int) parcel_r_int32(&rilp);
	if (num_strings % strings_per_opt) {
		ofono_error("%s: invalid QUERY_AVAIL_NETWORKS reply: "
				"num_strings (%d) MOD %d != 0",
				__func__,
				num_strings, strings_per_opt);
		goto error;
	}

	num_ops = num_strings / strings_per_opt;
	DBG("noperators = %d", num_ops);
	ops = g_new0(struct ofono_network_operator, num_ops);

	for (i = 0; num_ops; num_ops--) {
		char *lalpha;
		char *salpha;
		char *numeric;
		char *status;
		int tech = -1;

		lalpha = parcel_r_string(&rilp);
		salpha = parcel_r_string(&rilp);
		numeric = parcel_r_string(&rilp);
		status = parcel_r_string(&rilp);

		/*
		 * MTK: additional string with technology: 2G/3G are the only
		 * valid values currently.
		 */
		if (g_ril_vendor(nd->ril) == OFONO_RIL_VENDOR_MTK) {
			char *t = parcel_r_string(&rilp);

			if (strcmp(t, "3G") == 0)
				tech = ACCESS_TECHNOLOGY_UTRAN;
			else
				tech = ACCESS_TECHNOLOGY_GSM;

			g_free(t);
		}

		if (lalpha == NULL && salpha == NULL)
			goto next;

		if (numeric == NULL)
			goto next;

		if (status == NULL)
			goto next;

		set_oper_name(lalpha, salpha, &ops[i]);
		extract_mcc_mnc(numeric, ops[i].mcc, ops[i].mnc);
		ops[i].tech = tech;

		/* Set the proper status  */
		if (!strcmp(status, "unknown"))
			ops[i].status = OPERATOR_STATUS_UNKNOWN;
		else if (!strcmp(status, "available"))
			ops[i].status = OPERATOR_STATUS_AVAILABLE;
		else if (!strcmp(status, "current"))
			ops[i].status = OPERATOR_STATUS_CURRENT;
		else if (!strcmp(status, "forbidden"))
			ops[i].status = OPERATOR_STATUS_FORBIDDEN;

		i++;
next:
		g_ril_append_print_buf(nd->ril, "%s [lalpha=%s, salpha=%s, "
				" numeric=%s status=%s]",
				print_buf,
				lalpha, salpha, numeric, status);
		g_free(lalpha);
		g_free(salpha);
		g_free(numeric);
		g_free(status);
	}

	g_ril_append_print_buf(nd->ril, "%s}", print_buf);
	g_ril_print_response(nd->ril, message);

	CALLBACK_WITH_SUCCESS(cb, i, ops, cbd->data);
	g_free(ops);
	return;

error:
	CALLBACK_WITH_FAILURE(cb, 0, NULL, cbd->data);
}

static void ril_list_operators(struct ofono_netreg *netreg,
				ofono_netreg_operator_list_cb_t cb, void *data)
{
	struct netreg_data *nd = ofono_netreg_get_data(netreg);
	struct cb_data *cbd = cb_data_new(cb, data, nd);

	if (g_ril_send(nd->ril, RIL_REQUEST_QUERY_AVAILABLE_NETWORKS, NULL,
			ril_cops_list_cb, cbd, g_free) == 0) {
		g_free(cbd);
		CALLBACK_WITH_FAILURE(cb, 0, NULL, data);
	}
}

static void ril_register_cb(struct ril_msg *message, gpointer user_data)
{
	struct cb_data *cbd = user_data;
	ofono_netreg_register_cb_t cb = cbd->cb;
	struct netreg_data *nd = cbd->user;
	struct ofono_error error;

	if (message->error == RIL_E_SUCCESS) {
		decode_ril_error(&error, "OK");

		g_ril_print_response_no_args(nd->ril, message);

	} else {
		decode_ril_error(&error, "FAIL");
	}

	cb(&error, cbd->data);
}

static void ril_register_auto(struct ofono_netreg *netreg,
				ofono_netreg_register_cb_t cb, void *data)
{
	struct netreg_data *nd = ofono_netreg_get_data(netreg);
	struct cb_data *cbd = cb_data_new(cb, data, nd);

	if (g_ril_send(nd->ril, RIL_REQUEST_SET_NETWORK_SELECTION_AUTOMATIC,
			NULL, ril_register_cb, cbd, g_free) == 0) {
		g_free(cbd);
		CALLBACK_WITH_FAILURE(cb, data);
	}
}

static void ril_register_manual(struct ofono_netreg *netreg,
				const char *mcc, const char *mnc,
				ofono_netreg_register_cb_t cb, void *data)
{
	struct netreg_data *nd = ofono_netreg_get_data(netreg);
	struct cb_data *cbd = cb_data_new(cb, data, nd);
	char buf[OFONO_MAX_MCC_LENGTH + OFONO_MAX_MNC_LENGTH + 1];
	struct parcel rilp;

	DBG("");

	/* RIL expects a char * specifying MCCMNC of network to select */
	snprintf(buf, sizeof(buf), "%s%s", mcc, mnc);

	parcel_init(&rilp);
	parcel_w_string(&rilp, buf);

	g_ril_append_print_buf(nd->ril, "(%s)", buf);

	/* In case of error free cbd and return the cb with failure */
	if (g_ril_send(nd->ril, RIL_REQUEST_SET_NETWORK_SELECTION_MANUAL, &rilp,
			ril_register_cb, cbd, g_free) > 0)
		return;

	g_free(cbd);
	CALLBACK_WITH_FAILURE(cb, data);
}

static void ril_strength_notify(struct ril_msg *message, gpointer user_data)
{
	struct ofono_netreg *netreg = user_data;
	struct netreg_data *nd = ofono_netreg_get_data(netreg);
	int strength = parse_signal_strength(nd->ril, message, nd->tech);

	ofono_netreg_strength_notify(netreg, strength);
}

static void ril_strength_cb(struct ril_msg *message, gpointer user_data)
{
	struct cb_data *cbd = user_data;
	ofono_netreg_strength_cb_t cb = cbd->cb;
	struct netreg_data *nd = cbd->user;
	struct ofono_error error;
	int strength;

	if (message->error == RIL_E_SUCCESS) {
		decode_ril_error(&error, "OK");
	} else {
		ofono_error("Failed to retrive the signal strength");
		goto error;
	}

	/* parse_signal_strength() handles both reply & unsolicited */
	strength = parse_signal_strength(nd->ril, message, nd->tech);
	cb(&error, strength, cbd->data);

	return;

error:
	CALLBACK_WITH_FAILURE(cb, -1, cbd->data);
}

static void ril_signal_strength(struct ofono_netreg *netreg,
				ofono_netreg_strength_cb_t cb, void *data)
{
	struct netreg_data *nd = ofono_netreg_get_data(netreg);
	struct cb_data *cbd = cb_data_new(cb, data, nd);

	if (g_ril_send(nd->ril, RIL_REQUEST_SIGNAL_STRENGTH, NULL,
			ril_strength_cb, cbd, g_free) == 0) {
		ofono_error("Send RIL_REQUEST_SIGNAL_STRENGTH failed.");

		g_free(cbd);
		CALLBACK_WITH_FAILURE(cb, -1, data);
	}
}

static void ril_nitz_notify(struct ril_msg *message, gpointer user_data)
{
	struct ofono_netreg *netreg = user_data;
	struct netreg_data *nd = ofono_netreg_get_data(netreg);
	struct parcel rilp;
	int year, mon, mday, hour, min, sec, dst, tzi, n_match;
	char tzs, tz[4];
	gchar *nitz;
	struct ofono_network_time time;

	DBG("");

	/* Minimum NITZ is: 'yy/mm/dd,hh:mm:ss' TZ '(+/-)tz,dt' are optional */
	if (message->buf_len < 17)
		return;

	g_ril_init_parcel(message, &rilp);

	nitz = parcel_r_string(&rilp);

	g_ril_append_print_buf(nd->ril, "(%s)", nitz);
	g_ril_print_unsol(nd->ril, message);

	if (nitz == NULL)
		goto error;

	n_match = sscanf(nitz, "%u/%u/%u,%u:%u:%u%c%u,%u", &year, &mon,
				&mday, &hour, &min, &sec, &tzs, &tzi, &dst);
	if (n_match != 9)
		goto error;

	sprintf(tz, "%c%d", tzs, tzi);

	time.utcoff = atoi(tz) * 15 * 60;
	time.dst = dst;
	time.sec = sec;
	time.min = min;
	time.hour = hour;
	time.mday = mday;
	time.mon = mon;
	time.year = 2000 + year;

	ofono_netreg_time_notify(netreg, &time);

error:
	g_free(nitz);
}

static gboolean ril_delayed_register(gpointer user_data)
{
	struct ofono_netreg *netreg = user_data;
	struct netreg_data *nd = ofono_netreg_get_data(netreg);
	ofono_netreg_register(netreg);

	/* Register for network state changes */
	g_ril_register(nd->ril, RIL_UNSOL_RESPONSE_VOICE_NETWORK_STATE_CHANGED,
			ril_network_state_change, netreg);

	/* Register for network time update reports */
	g_ril_register(nd->ril, RIL_UNSOL_NITZ_TIME_RECEIVED,
			ril_nitz_notify, netreg);

	/* Register for signal strength changes */
	g_ril_register(nd->ril, RIL_UNSOL_SIGNAL_STRENGTH,
			ril_strength_notify, netreg);

	/* This makes the timeout a single-shot */
	return FALSE;
}

static int ril_netreg_probe(struct ofono_netreg *netreg, unsigned int vendor,
				void *data)
{
	GRil *ril = data;
	struct netreg_data *nd;

	nd = g_new0(struct netreg_data, 1);

	nd->ril = g_ril_clone(ril);
	nd->vendor = vendor;
	nd->tech = RADIO_TECH_UNKNOWN;

	ofono_netreg_set_data(netreg, nd);

	/*
	 * ofono_netreg_register() needs to be called after
	 * the driver has been set in ofono_netreg_create(),
	 * which calls this function.  Most other drivers make
	 * some kind of capabilities query to the modem, and then
	 * call register in the callback; we use the idle loop here.
	 */
	g_idle_add(ril_delayed_register, netreg);

	return 0;
}

static void ril_netreg_remove(struct ofono_netreg *netreg)
{
	struct netreg_data *nd = ofono_netreg_get_data(netreg);

	if (nd->nitz_timeout)
		g_source_remove(nd->nitz_timeout);

	ofono_netreg_set_data(netreg, NULL);

	g_ril_unref(nd->ril);
	g_free(nd);
}

static const struct ofono_netreg_driver driver = {
	.name				= RILMODEM,
	.probe				= ril_netreg_probe,
	.remove				= ril_netreg_remove,
	.registration_status		= ril_registration_status,
	.current_operator		= ril_current_operator,
	.list_operators			= ril_list_operators,
	.register_auto			= ril_register_auto,
	.register_manual		= ril_register_manual,
	.strength			= ril_signal_strength,
};

void ril_netreg_init(void)
{
	ofono_netreg_driver_register(&driver);
}

void ril_netreg_exit(void)
{
	ofono_netreg_driver_unregister(&driver);
}
