/*
 *
 *  oFono - Open Source Telephony
 *
 *  Copyright (C) 2008-2011  Intel Corporation. All rights reserved.
 *  Copyright (C) 2012  Canonical Ltd.
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

#include <glib.h>
#include <gril.h>
#include <string.h>
#include <stdlib.h>

#define OFONO_API_SUBJECT_TO_CHANGE
#include <ofono/log.h>
#include <ofono/types.h>

#include "common.h"
#include "rilutil.h"
#include "simutil.h"
#include "util.h"
#include "ril_constants.h"

struct ril_util_sim_state_query {
	GRil *ril;
	guint cpin_poll_source;
	guint cpin_poll_count;
	guint interval;
	guint num_times;
	ril_util_sim_inserted_cb_t cb;
	void *userdata;
	GDestroyNotify destroy;
};

/* TODO: make conditional */
static char print_buf[PRINT_BUF_SIZE];

static gboolean cpin_check(gpointer userdata);

void decode_ril_error(struct ofono_error *error, const char *final)
{
	if (!strcmp(final, "OK")) {
		error->type = OFONO_ERROR_TYPE_NO_ERROR;
		error->error = 0;
	} else {
		error->type = OFONO_ERROR_TYPE_FAILURE;
		error->error = 0;
	}
}

gint ril_util_call_compare_by_status(gconstpointer a, gconstpointer b)
{
	const struct ofono_call *call = a;
	int status = GPOINTER_TO_INT(b);

	if (status != call->status)
		return 1;

	return 0;
}

gint ril_util_call_compare_by_phone_number(gconstpointer a, gconstpointer b)
{
	const struct ofono_call *call = a;
	const struct ofono_phone_number *pb = b;

	return memcmp(&call->phone_number, pb,
				sizeof(struct ofono_phone_number));
}

gint ril_util_call_compare_by_id(gconstpointer a, gconstpointer b)
{
	const struct ofono_call *call = a;
	unsigned int id = GPOINTER_TO_UINT(b);

	if (id < call->id)
		return -1;

	if (id > call->id)
		return 1;

	return 0;
}

gint ril_util_data_call_compare(gconstpointer a, gconstpointer b)
{
	const struct data_call *ca = a;
	const struct data_call *cb = b;

	if (ca->cid < cb->cid)
		return -1;

	if (ca->cid > cb->cid)
		return 1;

	return 0;
}

gint ril_util_call_compare(gconstpointer a, gconstpointer b)
{
	const struct ofono_call *ca = a;
	const struct ofono_call *cb = b;

	if (ca->id < cb->id)
		return -1;

	if (ca->id > cb->id)
		return 1;

	return 0;
}

static gboolean cpin_check(gpointer userdata)
{
	struct ril_util_sim_state_query *req = userdata;

	req->cpin_poll_source = 0;

	return FALSE;
}

gchar *ril_util_get_netmask(const gchar *address)
{
	char *result;

	if (g_str_has_suffix(address, "/30")) {
		result = PREFIX_30_NETMASK;
	} else if (g_str_has_suffix(address, "/29")) {
		result = PREFIX_29_NETMASK;
	} else if (g_str_has_suffix(address, "/28")) {
		result = PREFIX_28_NETMASK;
	} else if (g_str_has_suffix(address, "/27")) {
		result = PREFIX_27_NETMASK;
	} else if (g_str_has_suffix(address, "/26")) {
		result = PREFIX_26_NETMASK;
	} else if (g_str_has_suffix(address, "/25")) {
		result = PREFIX_25_NETMASK;
	} else if (g_str_has_suffix(address, "/24")) {
		result = PREFIX_24_NETMASK;
	} else {
		/*
		 * This handles the case where the
		 * Samsung RILD returns an address without
		 * a prefix, however it explicitly sets a
		 * /24 netmask ( which isn't returned as
		 * an attribute of the DATA_CALL.
		 *
		 * TODO/OEM: this might need to be quirked
		 * for specific devices.
		 */
		result = PREFIX_24_NETMASK;
	}

	DBG("address: %s netmask: %s", address, result);

	return result;
}

void ril_util_init_parcel(struct ril_msg *message, struct parcel *rilp)
{
	/* Set up Parcel struct for proper parsing */
	rilp->data = message->buf;
	rilp->size = message->buf_len;
	rilp->capacity = message->buf_len;
	rilp->offset = 0;
}

struct ril_util_sim_state_query *ril_util_sim_state_query_new(GRil *ril,
						guint interval, guint num_times,
						ril_util_sim_inserted_cb_t cb,
						void *userdata,
						GDestroyNotify destroy)
{
	struct ril_util_sim_state_query *req;

	req = g_new0(struct ril_util_sim_state_query, 1);

	req->ril = ril;
	req->interval = interval;
	req->num_times = num_times;
	req->cb = cb;
	req->userdata = userdata;
	req->destroy = destroy;

	cpin_check(req);

	return req;
}

void ril_util_sim_state_query_free(struct ril_util_sim_state_query *req)
{
	if (req == NULL)
		return;

	if (req->cpin_poll_source > 0)
		g_source_remove(req->cpin_poll_source);

	if (req->destroy)
		req->destroy(req->userdata);

	g_free(req);
}

GSList *ril_util_parse_clcc(struct ril_msg *message)
{
	struct ofono_call *call;
	struct parcel rilp;
	GSList *l = NULL;
	int num, i;
	gchar *number, *name;

	ril_util_init_parcel(message, &rilp);

	/* Number of RIL_Call structs */
	num = parcel_r_int32(&rilp);
	for (i = 0; i < num; i++) {
		call = g_try_new(struct ofono_call, 1);
		if (call == NULL)
			break;

		ofono_call_init(call);
		call->status = parcel_r_int32(&rilp);
		call->id = parcel_r_int32(&rilp);
		call->phone_number.type = parcel_r_int32(&rilp);
		parcel_r_int32(&rilp); /* isMpty */
		parcel_r_int32(&rilp); /* isMT */
		parcel_r_int32(&rilp); /* als */
		call->type = parcel_r_int32(&rilp); /* isVoice */
		parcel_r_int32(&rilp); /* isVoicePrivacy */
		number = parcel_r_string(&rilp);
		if (number) {
			strncpy(call->phone_number.number, number,
				OFONO_MAX_PHONE_NUMBER_LENGTH);
			g_free(number);
		}
		parcel_r_int32(&rilp); /* numberPresentation */
		name = parcel_r_string(&rilp);
		if (name) {
			strncpy(call->name, name,
				OFONO_MAX_CALLER_NAME_LENGTH);
			g_free(name);
		}
		parcel_r_int32(&rilp); /* namePresentation */
		parcel_r_int32(&rilp); /* uusInfo */

		if (strlen(call->phone_number.number) > 0)
			call->clip_validity = 0;
		else
			call->clip_validity = 2;

		DBG("Adding call - id: %d, status: %d, type: %d, number: %s, name: %s",
				call->id, call->status, call->type,
				call->phone_number.number, call->name);

		l = g_slist_insert_sorted(l, call, ril_util_call_compare);
	}

	return l;
}

GSList *ril_util_parse_data_call_list(struct ril_msg *message)
{
	struct data_call *call;
	struct parcel rilp;
	GSList *l = NULL;
	int num, i, version;
	gchar *number, *name;

	ril_util_init_parcel(message, &rilp);

	/*
	 * ril.h documents the reply to a RIL_REQUEST_DATA_CALL_LIST
	 * as being an array of  RIL_Data_Call_Response_v6 structs,
	 * however in reality, the response also includes a version
	 * to start.
	 */
	version = parcel_r_int32(&rilp);

	/* Number of calls */
	num = parcel_r_int32(&rilp);

	/* TODO: make conditional */
	ril_append_print_buf("[%04d]< %s",
				message->serial_no,
				ril_unsol_request_to_string(message->req));

	ril_start_response;

	ril_append_print_buf("%sversion=%d,num=%d",
			print_buf,
			version,
			num);
	/* TODO: make conditional */

	for (i = 0; i < num; i++) {
		call = g_try_new(struct data_call, 1);
		if (call == NULL)
			break;

		call->status = parcel_r_int32(&rilp);
		call->retry = parcel_r_int32(&rilp);
		call->cid = parcel_r_int32(&rilp);
		call->active = parcel_r_int32(&rilp);

		call->type = parcel_r_string(&rilp);
		call->ifname = parcel_r_string(&rilp);
		call->addresses = parcel_r_string(&rilp);
		call->dnses = parcel_r_string(&rilp);
		call->gateways = parcel_r_string(&rilp);

		/* TODO: make conditional */
		/* TODO: figure out how to line-wrap properly
		 * without introducing spaces in string.
		 */
		ril_append_print_buf("%s [status=%d,retry=%d,cid=%d,active=%d,type=%s,ifname=%s,address=%s,dns=%s,gateways=%s]",
				print_buf,
				call->status,
				call->retry,
				call->cid,
				call->active,
				call->type,
				call->ifname,
				call->addresses,
				call->dnses,
				call->gateways);
		/* TODO: make conditional */

		l = g_slist_insert_sorted(l, call, ril_util_data_call_compare);
	}

	ril_close_response;
	ril_print_response;
	/* TODO: make conditional */

	return l;
}

char *ril_util_parse_sim_io_rsp(struct ril_msg *message,
				int *sw1, int *sw2,
				int *hex_len)
{
	struct parcel rilp;
	char *response = NULL;
	char *hex_response = NULL;

	/* Minimum length of SIM_IO_Response is 12:
	 * sw1 (int32)
	 * sw2 (int32)
	 * simResponse (string)
	 */
	if (message->buf_len < 12) {
		DBG("message->buf_len < 12");
		return FALSE;
	}

	DBG("message->buf_len is: %d", message->buf_len);

	ril_util_init_parcel(message, &rilp);

	*sw1 = parcel_r_int32(&rilp);
	*sw2 = parcel_r_int32(&rilp);

	response = parcel_r_string(&rilp);
	if (response) {
		DBG("response is set; len is: %d", strlen(response));
		hex_response = (char *) decode_hex((const char *) response,
							strlen(response),
							(long *) hex_len, -1);
	}

	/* TODO: make conditional */
	ril_append_print_buf("[%04d]< %s",
			message->serial_no,
			ril_request_id_to_string(message->req));
	ril_start_response;
	ril_append_print_buf("%ssw1=0x%.2X,sw2=0x%.2X,%s",
		       print_buf,
			*sw1,
			*sw2,
			response);
	ril_close_response;
	ril_print_response;
	/* TODO: make conditional */

	g_free(response);
	return hex_response;
}

gboolean ril_util_parse_sim_status(struct ril_msg *message,
									struct sim_app *app,
									struct sim_data *sd)
{
	struct parcel rilp;
	gboolean result = FALSE;
	char *aid_str = NULL;
	char *app_str = NULL;
	int i, card_state, num_apps, pin_state, gsm_umts_index, ims_index;
	int app_state, app_type, pin_replaced, pin1_state, pin2_state, perso_substate;

	ril_append_print_buf("[%04d]< %s",
			message->serial_no,
			ril_request_id_to_string(message->req));

	if (app) {
		app->app_type = RIL_APPTYPE_UNKNOWN;
		app->app_id = NULL;
	}

	ril_util_init_parcel(message, &rilp);

	/*
	 * FIXME: Need to come up with a common scheme for verifying the
	 * size of RIL message and properly reacting to bad messages.
	 * This could be a runtime assertion, disconnect, drop/ignore
	 * the message, ...
	 *
	 * Currently if the message is smaller than expected, our parcel
	 * code happily walks off the end of the buffer and segfaults.
	 *
	 * 20 is the min length of RIL_CardStatus_v6 as the AppState
	 * array can be 0-length.
	 */
	if (message->buf_len < 20) {
		ofono_error("Size of SIM_STATUS reply too small: %d bytes",
				message->buf_len);
		goto done;
	}

	card_state = parcel_r_int32(&rilp);
	pin_state = parcel_r_int32(&rilp);
	gsm_umts_index = parcel_r_int32(&rilp);
	parcel_r_int32(&rilp); /* ignore: cdma_subscription_app_index */
	ims_index = parcel_r_int32(&rilp);
	num_apps = parcel_r_int32(&rilp);

	ril_start_response;

	/* TODO:
	 * How do we handle long (>80 chars) ril_append_print_buf strings?
	 * Using line wrapping ( via '\' ) introduces spaces in the output.
	 * Do we just make a style-guide exception for PrintBuf operations?
	 */
	ril_append_print_buf("%s card_state=%d,universal_pin_state=%d,gsm_umts_index=%d,cdma_index=%d,ims_index=%d, ",
		       print_buf,
		       card_state,
		       pin_state,
		       gsm_umts_index,
		       -1,
		       ims_index);

	for (i = 0; i < num_apps; i++) {
		app_type = parcel_r_int32(&rilp);
		app_state = parcel_r_int32(&rilp);
		perso_substate = parcel_r_int32(&rilp);

		/* TODO: we need a way to instruct parcel to skip
		 * a string, without allocating memory...
		 */
		aid_str = parcel_r_string(&rilp); /* application ID (AID) */
		app_str = parcel_r_string(&rilp); /* application label */

		pin_replaced = parcel_r_int32(&rilp);
		pin1_state = parcel_r_int32(&rilp);
		pin2_state = parcel_r_int32(&rilp);

		/* PIN state of active application should take precedence
		* Since qualcomm modem does not seem to give clear
		* active indication we have to rely to app_type which
		* according to traces seems to not zero if app is active.
		*/
		if (app_type != 0 && sd) {
			switch (app_state) {
			case APPSTATE_PIN:
				sd->passwd_state = OFONO_SIM_PASSWORD_SIM_PIN;
				break;
			case APPSTATE_PUK:
				sd->passwd_state = OFONO_SIM_PASSWORD_SIM_PUK;
				break;
			case APPSTATE_SUBSCRIPTION_PERSO:
				/* TODO: Check out how to dig out exact
				* SIM lock.
				*/
				sd->passwd_state = OFONO_SIM_PASSWORD_PHSIM_PIN;
				break;
			case APPSTATE_READY:
				sd->passwd_state = OFONO_SIM_PASSWORD_NONE;
				break;
			case APPSTATE_UNKNOWN:
			case APPSTATE_DETECTED:
			default:
				sd->passwd_state = OFONO_SIM_PASSWORD_INVALID;
				break;
			}
		}

		ril_append_print_buf("%s[app_type=%d,app_state=%d,perso_substate=%d,aid_ptr=%s,app_label_ptr=%s,pin1_replaced=%d,pin1=%d,pin2=%d],",
				print_buf,
				app_type,
				app_state,
				perso_substate,
				aid_str,
				app_str,
				pin_replaced,
				pin1_state,
				pin2_state);

		/* FIXME: CDMA/IMS -- see comment @ top-of-source. */
		if (i == gsm_umts_index && app) {
			if (aid_str) {
				app->app_id = aid_str;
				DBG("setting app_id (AID) to: %s", aid_str);
			}

			app->app_type = app_type;
		} else
			g_free(aid_str);

		g_free(app_str);
	}

	ril_close_response;
	ril_print_response;

	if (card_state == RIL_CARDSTATE_PRESENT)
		result = TRUE;
done:
	return result;
}

gboolean ril_util_parse_reg(struct ril_msg *message, int *status,
				int *lac, int *ci, int *tech, int *max_calls)
{
	struct parcel rilp;
	int tmp;
	gchar *sstatus = NULL, *slac = NULL, *sci = NULL;
	gchar *stech = NULL, *sreason = NULL, *smax = NULL;

	ril_util_init_parcel(message, &rilp);


	/* TODO: make conditional */
	ril_append_print_buf("[%04d]< %s",
			message->serial_no,
			ril_request_id_to_string(message->req));

	ril_start_response;
	/* TODO: make conditional */

	/* FIXME: need minimum message size check FIRST!!! */

	/* Size of response string array
	 *
	 * Should be:
	 *   >= 4 for VOICE_REG reply
	 *   >= 5 for DATA_REG reply
	 */
	if ((tmp = parcel_r_int32(&rilp)) < 4) {
		DBG("Size of response array is too small: %d", tmp);
		goto error;
	}

	sstatus = parcel_r_string(&rilp);
	slac = parcel_r_string(&rilp);
	sci = parcel_r_string(&rilp);
	stech = parcel_r_string(&rilp);

	tmp -= 4;

	/* FIXME: need to review VOICE_REGISTRATION response
	 * as it returns ~15 parameters ( vs. 6 for DATA ).
	 *
	 * The first four parameters are the same for both
	 * responses ( although status includes values for
	 * emergency calls for VOICE response ).
	 *
	 * Parameters 5 & 6 have different meanings for
	 * voice & data response.
	 */
	if (tmp--) {
		sreason = parcel_r_string(&rilp);        /* TODO: different use for CDMA */

		if (tmp--) {
			smax = parcel_r_string(&rilp);           /* TODO: different use for CDMA */

			if (smax && max_calls)
				*max_calls = atoi(smax);
		}
	}

	/* TODO: make conditional */
	ril_append_print_buf("%s%s,%s,%s,%s,%s,%s",
			print_buf,
			sstatus,
			slac,
			sci,
			stech,
			sreason,
			smax);
	ril_close_response;
	ril_print_response;
	/* TODO: make conditional */

	if (status) {
		if (!sstatus) {
			DBG("No sstatus value returned!");
			goto error;
		}

		*status = atoi(sstatus);
	}

	if (lac) {
		if (slac)
			*lac = strtol(slac, NULL, 16);
		else
			*lac = -1;
	}

	if (ci) {
		if (sci)
			*ci = strtol(sci, NULL, 16);
		else
			*ci = -1;
	}


	if (tech) {
		if (stech) {
			switch(atoi(stech)) {
			case RADIO_TECH_UNKNOWN:
				*tech = -1;
				break;
			case RADIO_TECH_GPRS:
				*tech = ACCESS_TECHNOLOGY_GSM;
				break;
			case RADIO_TECH_EDGE:
				*tech = ACCESS_TECHNOLOGY_GSM_EGPRS;
				break;
			case RADIO_TECH_UMTS:
				*tech = ACCESS_TECHNOLOGY_UTRAN;
				break;
			case RADIO_TECH_HSDPA:
				*tech = ACCESS_TECHNOLOGY_UTRAN_HSDPA;
				break;
			case RADIO_TECH_HSUPA:
				*tech = ACCESS_TECHNOLOGY_UTRAN_HSUPA;
				break;
			case RADIO_TECH_HSPA:
				*tech = ACCESS_TECHNOLOGY_UTRAN_HSDPA_HSUPA;
				break;
			default:
				*tech = -1;
			}
		} else
			*tech = -1;
	}

	/* Free our parcel handlers */
	g_free(sstatus);
	g_free(slac);
	g_free(sci);
	g_free(stech);
	g_free(sreason);
	g_free(smax);

	return TRUE;

error:
	return FALSE;
}

gint ril_util_parse_sms_response(struct ril_msg *message)
{
	struct parcel rilp;
	int error, mr;
	char *ack_pdu;

	/* Set up Parcel struct for proper parsing */
	ril_util_init_parcel(message, &rilp);

	/* TP-Message-Reference for GSM/
	 * BearerData MessageId for CDMA
	 */
	mr = parcel_r_int32(&rilp);
	ack_pdu = parcel_r_int32(&rilp);
	error = parcel_r_int32(&rilp);

	DBG("SMS_Response mr: %d, ackPDU: %d, error: %d",
		mr, ack_pdu, error);

	return mr;
}

gint ril_util_get_signal(struct ril_msg *message)
{
	struct parcel rilp;
	int gw_signal, cdma_dbm, evdo_dbm, lte_signal;

	/* Set up Parcel struct for proper parsing */
	ril_util_init_parcel(message, &rilp);

	/* RIL_SignalStrength_v6 */
	/* GW_SignalStrength */
	gw_signal = parcel_r_int32(&rilp);
	parcel_r_int32(&rilp); /* bitErrorRate */

	/* CDMA_SignalStrength */
	cdma_dbm = parcel_r_int32(&rilp);
	parcel_r_int32(&rilp); /* ecio */

	/* EVDO_SignalStrength */
	evdo_dbm = parcel_r_int32(&rilp);
	parcel_r_int32(&rilp); /* ecio */
	parcel_r_int32(&rilp); /* signalNoiseRatio */

	/* LTE_SignalStrength */
	lte_signal = parcel_r_int32(&rilp);
	parcel_r_int32(&rilp); /* rsrp */
	parcel_r_int32(&rilp); /* rsrq */
	parcel_r_int32(&rilp); /* rssnr */
	parcel_r_int32(&rilp); /* cqi */

	DBG("RIL SignalStrength - gw: %d, cdma: %d, evdo: %d, lte: %d",
			gw_signal, cdma_dbm, evdo_dbm, lte_signal);

	/* Return the first valid one */
	if ((gw_signal != 99) && (gw_signal != -1))
		return (gw_signal * 100) / 31;
	if ((lte_signal != 99) && (lte_signal != -1))
		return (lte_signal * 100) / 31;

	/* In case of dbm, return the value directly */
	if (cdma_dbm != -1) {
		if (cdma_dbm > 100)
			cdma_dbm = 100;
		return cdma_dbm;
	}
	if (evdo_dbm != -1) {
		if (evdo_dbm > 100)
			evdo_dbm = 100;
		return evdo_dbm;
	}

	return -1;
}
