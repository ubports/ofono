/*
 *
 *  RIL library with GLib integration
 *
 *  Copyright (C) 2008-2011  Intel Corporation. All rights reserved.
 *  Copyright (C) 2012-2014  Canonical Ltd.
 *  Copyright (C) 2015 Ratchanan Srirattanamet.
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

#include "grilrequest.h"
#include "simutil.h"
#include "util.h"
#include "common.h"

/* DEACTIVATE_DATA_CALL request parameters */
#define DEACTIVATE_DATA_CALL_NUM_PARAMS 2

/* SETUP_DATA_CALL_PARAMS request parameters */
#define SETUP_DATA_CALL_PARAMS 7
#define DATA_PROFILE_DEFAULT_STR "0"
#define DATA_PROFILE_TETHERED_STR "1"
#define DATA_PROFILE_IMS_STR "2"
#define DATA_PROFILE_FOTA_STR "3"
#define DATA_PROFILE_CBS_STR "4"
#define DATA_PROFILE_OEM_BASE_STR "1000"
#define DATA_PROFILE_MTK_MMS_STR "1001"

/* SETUP_DATA_CALL_PARAMS reply parameters */
#define MIN_DATA_CALL_REPLY_SIZE 36

/* Call ID should not really be a big number */
#define MAX_CID_DIGITS 3

#define OFONO_EINVAL(error) do {		\
	error->type = OFONO_ERROR_TYPE_FAILURE;	\
	error->error = -EINVAL;			\
} while (0)

#define OFONO_NO_ERROR(error) do {			\
	error->type = OFONO_ERROR_TYPE_NO_ERROR;	\
	error->error = 0;				\
} while (0)

gboolean g_ril_request_deactivate_data_call(GRil *gril,
				const struct req_deactivate_data_call *req,
				struct parcel *rilp,
				struct ofono_error *error)
{
	gchar *cid_str = NULL;
	gchar *reason_str = NULL;

	if (req->reason != RIL_DEACTIVATE_DATA_CALL_NO_REASON &&
		req->reason != RIL_DEACTIVATE_DATA_CALL_RADIO_SHUTDOWN) {
		goto error;
	}

	parcel_init(rilp);
	parcel_w_int32(rilp, DEACTIVATE_DATA_CALL_NUM_PARAMS);

	cid_str = g_strdup_printf("%d", req->cid);
	parcel_w_string(rilp, cid_str);

	/*
	 * TODO: airplane-mode; change reason to '1',
	 * which means "radio power off".
	 */
	reason_str = g_strdup_printf("%d", req->reason);
	parcel_w_string(rilp, reason_str);

	g_ril_append_print_buf(gril, "(%s,%s)", cid_str, reason_str);

	g_free(cid_str);
	g_free(reason_str);

	OFONO_NO_ERROR(error);
	return TRUE;

error:
	OFONO_EINVAL(error);
	return FALSE;
}

void g_ril_request_set_net_select_manual(GRil *gril,
					const char *mccmnc,
					struct parcel *rilp)
{
	DBG("");

	parcel_init(rilp);
	parcel_w_string(rilp, mccmnc);

	g_ril_append_print_buf(gril, "(%s)", mccmnc);
}

gboolean g_ril_request_setup_data_call(GRil *gril,
					const struct req_setup_data_call *req,
					struct parcel *rilp,
					struct ofono_error *error)
{
	const gchar *protocol_str;
	gchar *tech_str;
	gchar *auth_str;
	gchar *profile_str;
	int num_param = SETUP_DATA_CALL_PARAMS;

	DBG("");

	if (g_ril_vendor(gril) == OFONO_RIL_VENDOR_MTK)
		num_param = SETUP_DATA_CALL_PARAMS + 1;

	/*
	 * Radio technology to use: 0-CDMA, 1-GSM/UMTS, 2...
	 * values > 2 are (RADIO_TECH + 2)
	 */
	if (req->tech < 1 || req->tech > (RADIO_TECH_GSM + 2)) {
		ofono_error("%s: Invalid tech value: %d",
				__func__,
				req->tech);
		goto error;
	}

	/*
	 * TODO(OEM): This code doesn't currently support
	 * OEM data profiles.  If a use case exist, then
	 * this code will need to be modified.
	 */
	switch (req->data_profile) {
	case RIL_DATA_PROFILE_DEFAULT:
		profile_str = DATA_PROFILE_DEFAULT_STR;
		break;
	case RIL_DATA_PROFILE_TETHERED:
		profile_str = DATA_PROFILE_TETHERED_STR;
		break;
	case RIL_DATA_PROFILE_IMS:
		profile_str = DATA_PROFILE_IMS_STR;
		break;
	case RIL_DATA_PROFILE_FOTA:
		profile_str = DATA_PROFILE_FOTA_STR;
		break;
	case RIL_DATA_PROFILE_CBS:
		profile_str = DATA_PROFILE_CBS_STR;
		break;
	case RIL_DATA_PROFILE_MTK_MMS:
		if (g_ril_vendor(gril) == OFONO_RIL_VENDOR_MTK) {
			profile_str = DATA_PROFILE_MTK_MMS_STR;
			break;
		}
	default:
		ofono_error("%s, invalid data_profile value: %d",
				__func__,
				req->data_profile);
		goto error;
	}

	if (req->apn == NULL)
		goto error;

	if (req->auth_type > RIL_AUTH_BOTH) {
		ofono_error("%s: Invalid auth type: %d",
				__func__,
				req->auth_type);
		goto error;
	}

	protocol_str = ril_ofono_protocol_to_ril_string(req->protocol);
	if (protocol_str == NULL) {
		ofono_error("%s: Invalid protocol: %d",
				__func__,
				req->protocol);
		goto error;
	}

	parcel_init(rilp);

	parcel_w_int32(rilp, num_param);

	tech_str = g_strdup_printf("%d", req->tech);
	parcel_w_string(rilp, tech_str);
	parcel_w_string(rilp, profile_str);
	parcel_w_string(rilp, req->apn);
	parcel_w_string(rilp, req->username);
	parcel_w_string(rilp, req->password);

	auth_str = g_strdup_printf("%d", req->auth_type);
	parcel_w_string(rilp, auth_str);
	parcel_w_string(rilp, protocol_str);

	g_ril_append_print_buf(gril,
				"(%s,%s,%s,%s,%s,%s,%s",
				tech_str,
				profile_str,
				req->apn,
				req->username,
				req->password,
				auth_str,
				protocol_str);

	if (g_ril_vendor(gril) == OFONO_RIL_VENDOR_MTK) {
		/* MTK request_cid parameter */
		char cid_str[MAX_CID_DIGITS + 1];

		snprintf(cid_str, sizeof(cid_str), "%u", req->req_cid);
		parcel_w_string(rilp, cid_str);
		g_ril_append_print_buf(gril, "%s,%s", print_buf, cid_str);
	}

	g_ril_append_print_buf(gril, "%s)", print_buf);

	g_free(tech_str);
	g_free(auth_str);

	OFONO_NO_ERROR(error);
	return TRUE;

error:
	OFONO_EINVAL(error);
	return FALSE;
}

void g_ril_request_oem_hook_raw(GRil *gril, const void *payload, size_t length,
					struct parcel *rilp)
{
	char *hex_dump = NULL;

	parcel_init(rilp);
	parcel_w_raw(rilp, payload, length);

	if (payload != NULL)
		hex_dump = encode_hex(payload, length, '\0');

	g_ril_append_print_buf(gril, "(%s)", hex_dump ? hex_dump : "(null)");
	g_free(hex_dump);
}

void g_ril_request_oem_hook_strings(GRil *gril, const char **strs, int num_str,
							struct parcel *rilp)
{
	int i;

	parcel_init(rilp);
	parcel_w_int32(rilp, num_str);

	g_ril_append_print_buf(gril, "(");

	for (i = 0; i < num_str; ++i) {
		parcel_w_string(rilp, strs[i]);

		if (i == num_str - 1)
			g_ril_append_print_buf(gril, "%s%s)",
							print_buf, strs[i]);
		else
			g_ril_append_print_buf(gril, "%s%s, ",
							print_buf, strs[i]);
	}
}

void g_ril_request_set_initial_attach_apn(GRil *gril, const char *apn,
						int proto,
						const char *user,
						const char *passwd,
						const char *mccmnc,
						struct parcel *rilp)
{
	const char *proto_str;
	const int auth_type = RIL_AUTH_ANY;

	parcel_init(rilp);

	parcel_w_string(rilp, apn);

	proto_str = ril_ofono_protocol_to_ril_string(proto);
	parcel_w_string(rilp, proto_str);

	parcel_w_int32(rilp, auth_type);
	parcel_w_string(rilp, user);
	parcel_w_string(rilp, passwd);

	g_ril_append_print_buf(gril, "(%s,%s,%s,%s,%s", apn, proto_str,
				ril_authtype_to_string(auth_type),
				user, passwd);

	if (g_ril_vendor(gril) == OFONO_RIL_VENDOR_MTK) {
		parcel_w_string(rilp, mccmnc);
		g_ril_append_print_buf(gril, "%s,%s)", print_buf, mccmnc);
	} else {
		g_ril_append_print_buf(gril, "%s)", print_buf);
	}
}

void g_ril_request_set_uicc_subscription(GRil *gril, int slot_id,
					int app_index,
					int sub_id,
					int sub_status,
					struct parcel *rilp)
{
	parcel_init(rilp);

	parcel_w_int32(rilp, slot_id);
	parcel_w_int32(rilp, app_index);
	parcel_w_int32(rilp, sub_id);
	parcel_w_int32(rilp, sub_status);

	g_ril_append_print_buf(gril, "(%d, %d, %d, %d(%s))",
				slot_id,
				app_index,
				sub_id,
				sub_status,
				sub_status ? "ACTIVATE" : "DEACTIVATE");
}
