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
