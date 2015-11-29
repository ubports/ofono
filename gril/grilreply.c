/*
 *
 *  RIL library with GLib integration
 *
 *  Copyright (C) 2008-2011  Intel Corporation. All rights reserved.
 *  Copyright (C) 2013 Jolla Ltd
 *  Contact: Jussi Kangas <jussi.kangas@tieto.com>
 *  Copyright (C) 2012-2014  Canonical Ltd.
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
#include <ofono/call-forwarding.h>
#include <ofono/gprs-context.h>

#include "common.h"
#include "util.h"
#include "grilreply.h"
#include "grilutil.h"

/* Indexes for registration state replies */
#define RST_IX_STATE 0
#define RST_IX_LAC 1
#define RST_IX_CID 2
#define RST_IX_RAT 3
#define RDST_IX_MAXDC 5

#define MTK_MODEM_MAX_CIDS 3

static void set_reg_state(GRil *gril, struct reply_reg_state *reply,
				int i, const char *str)
{
	int val;
	char *endp;
	int base;
	const char *strstate;

	if (str == NULL || *str == '\0')
		goto no_val;

	if (i == RST_IX_LAC || i == RST_IX_CID)
		base = 16;
	else
		base = 10;

	val = (int) strtol(str, &endp, base);
	if (*endp != '\0')
		goto no_val;

	switch (i) {
	case RST_IX_STATE:
		switch (val) {
		case RIL_REG_STATE_NOT_REGISTERED:
		case RIL_REG_STATE_REGISTERED:
		case RIL_REG_STATE_SEARCHING:
		case RIL_REG_STATE_DENIED:
		case RIL_REG_STATE_UNKNOWN:
		case RIL_REG_STATE_ROAMING:
			/* Only valid values for ofono */
			strstate = registration_status_to_string(val);
			break;
		case RIL_REG_STATE_EMERGENCY_NOT_REGISTERED:
		case RIL_REG_STATE_EMERGENCY_SEARCHING:
		case RIL_REG_STATE_EMERGENCY_DENIED:
		case RIL_REG_STATE_EMERGENCY_UNKNOWN:
			/* Map to states valid for ofono core */
			val -= RIL_REG_STATE_EMERGENCY_NOT_REGISTERED;
			strstate = str;
			break;
		default:
			val = NETWORK_REGISTRATION_STATUS_UNKNOWN;
			strstate = str;
		}
		reply->status = val;
		g_ril_append_print_buf(gril, "%s%s", print_buf, strstate);
		break;
	case RST_IX_LAC:
		reply->lac = val;
		g_ril_append_print_buf(gril, "%s0x%x", print_buf, val);
		break;
	case RST_IX_CID:
		reply->ci = val;
		g_ril_append_print_buf(gril, "%s0x%x", print_buf, val);
		break;
	case RST_IX_RAT:
		g_ril_append_print_buf(gril, "%s%s", print_buf,
					ril_radio_tech_to_string(val));

		if (g_ril_vendor(gril) == OFONO_RIL_VENDOR_MTK) {
			switch (val) {
			case MTK_RADIO_TECH_HSDPAP:
			case MTK_RADIO_TECH_HSDPAP_UPA:
			case MTK_RADIO_TECH_HSUPAP:
			case MTK_RADIO_TECH_HSUPAP_DPA:
				val = RADIO_TECH_HSPAP;
				break;
			case MTK_RADIO_TECH_DC_DPA:
				val = RADIO_TECH_HSDPA;
				break;
			case MTK_RADIO_TECH_DC_UPA:
				val = RADIO_TECH_HSUPA;
				break;
			case MTK_RADIO_TECH_DC_HSDPAP:
			case MTK_RADIO_TECH_DC_HSDPAP_UPA:
			case MTK_RADIO_TECH_DC_HSDPAP_DPA:
			case MTK_RADIO_TECH_DC_HSPAP:
				val = RADIO_TECH_HSPAP;
				break;
			}
		}

		reply->tech = val;
		break;
	default:
		goto no_val;
	}

	return;

no_val:
	g_ril_append_print_buf(gril, "%s%s", print_buf, str ? str : "(null)");
}

struct reply_reg_state *g_ril_reply_parse_voice_reg_state(GRil *gril,
						const struct ril_msg *message)
{
	struct parcel rilp;
	struct parcel_str_array *str_arr;
	struct reply_reg_state *reply = NULL;
	int i;

	g_ril_init_parcel(message, &rilp);

	str_arr = parcel_r_str_array(&rilp);
	if (str_arr == NULL) {
		ofono_error("%s: parse error for %s", __func__,
				ril_request_id_to_string(message->req));
		goto out;
	}

	reply =	g_try_malloc0(sizeof(*reply));
	if (reply == NULL) {
		ofono_error("%s: out of memory", __func__);
		goto out;
	}

	reply->status = -1;
	reply->lac = -1;
	reply->ci = -1;

	g_ril_append_print_buf(gril, "{");

	for (i = 0; i < str_arr->num_str; ++i) {
		char *str = str_arr->str[i];

		if (i > 0)
			g_ril_append_print_buf(gril, "%s,", print_buf);

		switch (i) {
		case RST_IX_STATE: case RST_IX_LAC:
		case RST_IX_CID:   case RST_IX_RAT:
			set_reg_state(gril, reply, i, str);
			break;
		default:
			g_ril_append_print_buf(gril, "%s%s", print_buf,
						str ? str : "(null)");
		}
	}

	g_ril_append_print_buf(gril, "%s}", print_buf);
	g_ril_print_response(gril, message);

	/* As a minimum we require a valid status string */
	if (reply->status == -1) {
		ofono_error("%s: invalid status", __func__);
		g_free(reply);
		reply = NULL;
	}

out:
	parcel_free_str_array(str_arr);

	return reply;
}

static void set_data_reg_state(GRil *gril, struct reply_data_reg_state *reply,
				int i, const char *str)
{
	unsigned val;
	char *endp;

	if (str == NULL || *str == '\0')
		goto no_val;

	val = (unsigned) strtoul(str, &endp, 10);
	if (*endp != '\0')
		goto no_val;

	switch (i) {
	case RDST_IX_MAXDC:
		/*
		 * MTK modem does not return max_cids, string for this index
		 * actually contains the maximum data bearer capability.
		 */
		if (g_ril_vendor(gril) == OFONO_RIL_VENDOR_MTK)
			reply->max_cids = MTK_MODEM_MAX_CIDS;
		else
			reply->max_cids = val;
		g_ril_append_print_buf(gril, "%s%u", print_buf, val);
		break;
	default:
		goto no_val;
	}

	return;

no_val:
	g_ril_append_print_buf(gril, "%s%s", print_buf, str ? str : "(null)");
}

struct reply_data_reg_state *g_ril_reply_parse_data_reg_state(GRil *gril,
						const struct ril_msg *message)
{
	struct parcel rilp;
	struct parcel_str_array *str_arr;
	struct reply_data_reg_state *reply = NULL;
	int i;

	g_ril_init_parcel(message, &rilp);

	str_arr = parcel_r_str_array(&rilp);
	if (str_arr == NULL) {
		ofono_error("%s: parse error for %s", __func__,
				ril_request_id_to_string(message->req));
		goto out;
	}

	reply =	g_try_malloc0(sizeof(*reply));
	if (reply == NULL) {
		ofono_error("%s: out of memory", __func__);
		goto out;
	}

	reply->reg_state.status = -1;
	reply->reg_state.lac = -1;
	reply->reg_state.ci = -1;

	g_ril_append_print_buf(gril, "{");

	for (i = 0; i < str_arr->num_str; ++i) {
		char *str = str_arr->str[i];

		if (i > 0)
			g_ril_append_print_buf(gril, "%s,", print_buf);

		switch (i) {
		case RST_IX_STATE: case RST_IX_LAC:
		case RST_IX_CID:   case RST_IX_RAT:
			set_reg_state(gril, &reply->reg_state, i, str);
			break;
		case RDST_IX_MAXDC:
			set_data_reg_state(gril, reply, i, str);
			break;
		default:
			g_ril_append_print_buf(gril, "%s%s", print_buf,
						str ? str : "(null)");
		}
	}

	g_ril_append_print_buf(gril, "%s}", print_buf);
	g_ril_print_response(gril, message);

	/* As a minimum we require a valid status string */
	if (reply->reg_state.status == -1) {
		ofono_error("%s: invalid status", __func__);
		g_free(reply);
		reply = NULL;
	}

out:
	parcel_free_str_array(str_arr);

	return reply;
}

void g_ril_reply_free_sim_status(struct reply_sim_status *status)
{
	if (status) {
		guint i;

		for (i = 0; i < status->num_apps; i++) {
			if (status->apps[i] != NULL) {
				g_free(status->apps[i]->aid_str);
				g_free(status->apps[i]->app_str);
				g_free(status->apps[i]);
			}
		}

		g_free(status);
	}
}

struct reply_sim_status *g_ril_reply_parse_sim_status(GRil *gril,
						const struct ril_msg *message)
{
	struct parcel rilp;
	unsigned int i;
	struct reply_sim_status *status;

	g_ril_append_print_buf(gril, "[%d,%04d]< %s",
			g_ril_get_slot(gril), message->serial_no,
			ril_request_id_to_string(message->req));

	g_ril_init_parcel(message, &rilp);

	status = g_new0(struct reply_sim_status, 1);

	status->card_state = parcel_r_int32(&rilp);

	/*
	 * NOTE:
	 *
	 * The global pin_status is used for multi-application
	 * UICC cards.  For example, there are SIM cards that
	 * can be used in both GSM and CDMA phones.  Instead
	 * of managed PINs for both applications, a global PIN
	 * is set instead.  It's not clear at this point if
	 * such SIM cards are supported by ofono or RILD.
	 */

	status->pin_state = parcel_r_int32(&rilp);
	status->gsm_umts_index = parcel_r_int32(&rilp);
	status->cdma_index = parcel_r_int32(&rilp);
	status->ims_index = parcel_r_int32(&rilp);
	status->num_apps = parcel_r_int32(&rilp);

	if (rilp.malformed)
		goto error;

	g_ril_append_print_buf(gril,
				"(card_state=%d,universal_pin_state=%d,"
				"gsm_umts_index=%d,cdma_index=%d,"
				"ims_index=%d, ",
				status->card_state,
				status->pin_state,
				status->gsm_umts_index,
				status->cdma_index,
				status->ims_index);

	if (status->card_state != RIL_CARDSTATE_PRESENT)
		goto done;

	if (status->num_apps > MAX_UICC_APPS) {
		ofono_error("SIM error; too many apps: %d", status->num_apps);
		status->num_apps = MAX_UICC_APPS;
	}

	for (i = 0; i < status->num_apps; i++) {
		struct reply_sim_app *app;
		DBG("processing app[%d]", i);
		status->apps[i] = g_try_new0(struct reply_sim_app, 1);
		app = status->apps[i];
		if (app == NULL) {
			ofono_error("Can't allocate app_data");
			goto error;
		}

		app->app_type = parcel_r_int32(&rilp);
		app->app_state = parcel_r_int32(&rilp);
		app->perso_substate = parcel_r_int32(&rilp);

		/*
		 * TODO: we need a way to instruct parcel to skip
		 * a string, without allocating memory...
		 */
		/* application ID (AID) */
		app->aid_str = parcel_r_string(&rilp);
		/* application label */
		app->app_str = parcel_r_string(&rilp);

		app->pin_replaced = parcel_r_int32(&rilp);
		app->pin1_state = parcel_r_int32(&rilp);
		app->pin2_state = parcel_r_int32(&rilp);

		g_ril_append_print_buf(gril,
					"%s[app_type=%d,app_state=%d,"
					"perso_substate=%d,aid_ptr=%s,"
					"app_label_ptr=%s,pin1_replaced=%d,"
					"pin1=%d,pin2=%d],",
					print_buf,
					app->app_type,
					app->app_state,
					app->perso_substate,
					app->aid_str ? app->aid_str : "NULL",
					app->app_str ? app->app_str : "NULL",
					app->pin_replaced,
					app->pin1_state,
					app->pin2_state);
	}

	if (rilp.malformed)
		goto error;

done:
	g_ril_append_print_buf(gril, "%s}", print_buf);
	g_ril_print_response(gril, message);

	return status;

error:
	g_ril_reply_free_sim_status(status);

	return NULL;
}

static gint g_ril_call_compare(gconstpointer a, gconstpointer b)
{
	const struct ofono_call *ca = a;
	const struct ofono_call *cb = b;

	if (ca->id < cb->id)
		return -1;

	if (ca->id > cb->id)
		return 1;

	return 0;
}

GSList *g_ril_reply_parse_get_calls(GRil *gril, const struct ril_msg *message)
{
	struct ofono_call *call;
	struct parcel rilp;
	GSList *l = NULL;
	int num, i;
	gchar *number, *name;

	g_ril_init_parcel(message, &rilp);

	g_ril_append_print_buf(gril, "{");

	/* maguro signals no calls with empty event data */
	if (rilp.size < sizeof(int32_t))
		goto no_calls;

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

		g_ril_append_print_buf(gril,
					"%s [id=%d,status=%d,type=%d,"
					"number=%s,name=%s]",
					print_buf,
					call->id, call->status, call->type,
					call->phone_number.number, call->name);

		l = g_slist_insert_sorted(l, call, g_ril_call_compare);
	}

no_calls:
	g_ril_append_print_buf(gril, "%s}", print_buf);
	g_ril_print_response(gril, message);

	return l;
}

int *g_ril_reply_parse_retries(GRil *gril, const struct ril_msg *message,
				enum ofono_sim_password_type passwd_type)
{
	struct parcel rilp;
	int i, numint;
	int *retries = g_try_malloc0(sizeof(int) * OFONO_SIM_PASSWORD_INVALID);

	if (retries == NULL) {
		ofono_error("%s: out of memory", __func__);
		goto no_data;
	}

	for (i = 0; i < OFONO_SIM_PASSWORD_INVALID; ++i)
		retries[i] = -1;

	g_ril_init_parcel(message, &rilp);

	/* maguro/infineon: no data is returned */
	if (parcel_data_avail(&rilp) == 0)
		goto no_data;

	numint = parcel_r_int32(&rilp);

	switch (g_ril_vendor(gril)) {
	case OFONO_RIL_VENDOR_AOSP:
	case OFONO_RIL_VENDOR_QCOM_MSIM:
		/*
		 * The number of retries is valid only when a wrong password has
		 * been introduced in Nexus 4. TODO: check Nexus 5 behaviour.
		 */
		if (message->error == RIL_E_PASSWORD_INCORRECT)
			retries[passwd_type] = parcel_r_int32(&rilp);

		g_ril_append_print_buf(gril, "{%d}", retries[passwd_type]);
		break;
	case OFONO_RIL_VENDOR_MTK:
		/*
		 * Some versions of MTK modem return just the retries for the
		 * password just entered while others return the retries for all
		 * passwords.
		 */
		if (numint == 1) {
			retries[passwd_type] = parcel_r_int32(&rilp);

			g_ril_append_print_buf(gril, "{%d}",
							retries[passwd_type]);
		} else if (numint == 4) {
			retries[OFONO_SIM_PASSWORD_SIM_PIN] =
							parcel_r_int32(&rilp);
			retries[OFONO_SIM_PASSWORD_SIM_PIN2] =
							parcel_r_int32(&rilp);
			retries[OFONO_SIM_PASSWORD_SIM_PUK] =
							parcel_r_int32(&rilp);
			retries[OFONO_SIM_PASSWORD_SIM_PUK2] =
							parcel_r_int32(&rilp);

			g_ril_append_print_buf(gril,
					"{pin %d, pin2 %d, puk %d, puk2 %d}",
					retries[OFONO_SIM_PASSWORD_SIM_PIN],
					retries[OFONO_SIM_PASSWORD_SIM_PIN2],
					retries[OFONO_SIM_PASSWORD_SIM_PUK],
					retries[OFONO_SIM_PASSWORD_SIM_PUK2]);
		} else {
			ofono_error("%s: wrong format", __func__);
			goto no_data;
		}
		break;
	case OFONO_RIL_VENDOR_INFINEON:
		ofono_error("%s: infineon type should not arrive here",
				__func__);
		g_assert(FALSE);
		break;
	}

	if (rilp.malformed) {
		ofono_error("%s: malformed parcel", __func__);
		goto no_data;
	}

	g_ril_print_response(gril, message);

	return retries;

no_data:
	g_free(retries);

	return NULL;
}

void g_ril_reply_free_oem_hook(struct reply_oem_hook *oem_hook)
{
	if (oem_hook) {
		g_free(oem_hook->data);
		g_free(oem_hook);
	}
}

struct reply_oem_hook *g_ril_reply_oem_hook_raw(GRil *gril,
						const struct ril_msg *message)
{
	struct reply_oem_hook *reply = NULL;
	struct parcel rilp;

	reply = g_try_malloc0(sizeof(*reply));
	if (reply == NULL) {
		ofono_error("%s: out of memory", __func__);
		goto end;
	}

	g_ril_init_parcel(message, &rilp);

	reply->data = parcel_r_raw(&rilp, &(reply->length));

	if (rilp.malformed) {
		ofono_error("%s: malformed parcel", __func__);
		g_ril_reply_free_oem_hook(reply);
		reply = NULL;
		goto end;
	}

	g_ril_append_print_buf(gril, "{%d", reply->length);

	if (reply->data != NULL) {
		char *hex_dump;
		hex_dump = encode_hex(reply->data, reply->length, '\0');
		g_ril_append_print_buf(gril, "%s,%s", print_buf, hex_dump);
		g_free(hex_dump);
	}

	g_ril_append_print_buf(gril, "%s}", print_buf);
	g_ril_print_response(gril, message);

end:
	return reply;
}

struct parcel_str_array *g_ril_reply_oem_hook_strings(GRil *gril,
						const struct ril_msg *message)
{
	struct parcel rilp;
	struct parcel_str_array *str_arr;
	int i;

	g_ril_init_parcel(message, &rilp);

	str_arr = parcel_r_str_array(&rilp);
	if (str_arr == NULL) {
		ofono_error("%s: no strings", __func__);
		goto out;
	}

	g_ril_append_print_buf(gril, "{");

	for (i = 0; i < str_arr->num_str; ++i) {
		if (i + 1 == str_arr->num_str)
			g_ril_append_print_buf(gril, "%s%s}", print_buf,
						str_arr->str[i]);
		else
			g_ril_append_print_buf(gril, "%s%s, ", print_buf,
						str_arr->str[i]);
	}

	g_ril_print_response(gril, message);

out:
	return str_arr;
}
