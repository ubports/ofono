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

/* Commands defined for TS 27.007 +CRSM */
#define CMD_READ_BINARY   176 /* 0xB0   */
#define CMD_READ_RECORD   178 /* 0xB2   */
#define CMD_UPDATE_BINARY 214 /* 0xD6   */
#define CMD_UPDATE_RECORD 220 /* 0xDC   */
#define CMD_STATUS        242 /* 0xF2   */
#define CMD_RETRIEVE_DATA 203 /* 0xCB   */
#define CMD_SET_DATA      219 /* 0xDB   */

/* FID/path of SIM/USIM root directory */
#define ROOTMF ((char[]) {'\x3F', '\x00'})
#define ROOTMF_SZ sizeof(ROOTMF)

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

/*
 * TODO:
 *
 * A potential future change here is to create a driver
 * abstraction for each request/reply/event method, and a
 * corresponding method to allow new per-message implementations
 * to be registered.  This would allow PES to easily add code
 * to quirk a particular RIL implementation.
 *
 * struct g_ril_messages_driver {
 *	const char *name;
 * };
 *
 */

static gboolean set_path(GRil *ril, guint app_type,
				struct parcel *rilp,
				const int fileid, const guchar *path,
				const guint path_len)
{
	unsigned char db_path[6] = { 0x00 };
	unsigned char *comm_path = db_path;
	char *hex_path = NULL;
	int len = 0;

	if (path_len > 0 && path_len < 7) {
		memcpy(db_path, path, path_len);
		len = path_len;
	} else if (app_type == RIL_APPTYPE_USIM) {
		len = sim_ef_db_get_path_3g(fileid, db_path);
	} else if (app_type == RIL_APPTYPE_SIM) {
		len = sim_ef_db_get_path_2g(fileid, db_path);
	} else {
		ofono_error("Unsupported app_type: 0%x", app_type);
		return FALSE;
	}

	/*
	 * db_path contains the ID of the MF, but MediaTek modems return an
	 * error if we do not remove it. Other devices work the other way
	 * around: they need the MF in the path. In fact MTK behaviour seem to
	 * be the right one: to have the MF in the file is forbidden following
	 * ETSI TS 102 221, section 8.4.2 (we are accessing the card in mode
	 * "select by path from MF", see 3gpp 27.007, +CRSM).
	 */
	if (g_ril_vendor(ril) == OFONO_RIL_VENDOR_MTK && len >= (int) ROOTMF_SZ
			&& memcmp(db_path, ROOTMF, ROOTMF_SZ) == 0) {
		comm_path = db_path + ROOTMF_SZ;
		len -= ROOTMF_SZ;
	}

	if (len > 0) {
		hex_path = encode_hex(comm_path, len, 0);
		parcel_w_string(rilp, hex_path);

		g_ril_append_print_buf(ril,
					"%spath=%s,",
					print_buf,
					hex_path);

		g_free(hex_path);
	} else {
		/*
		 * The only known case of this is EFPHASE_FILED (0x6FAE).
		 * The ef_db table ( see /src/simutil.c ) entry for
		 * EFPHASE contains a value of 0x0000 for it's
		 * 'parent3g' member.  This causes a NULL path to
		 * be returned.
		 * (EF_PHASE does not exist for USIM)
		 */
		parcel_w_string(rilp, NULL);

		g_ril_append_print_buf(ril,
					"%spath=(null),",
					print_buf);
	}

	return TRUE;
}

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

gboolean g_ril_request_sim_read_binary(GRil *gril,
					const struct req_sim_read_binary *req,
					struct parcel *rilp)
{
	g_ril_append_print_buf(gril,
				"(cmd=0x%.2X,efid=0x%.4X,",
				CMD_READ_BINARY,
				req->fileid);

	parcel_init(rilp);
	parcel_w_int32(rilp, CMD_READ_BINARY);
	parcel_w_int32(rilp, req->fileid);

	if (set_path(gril, req->app_type, rilp, req->fileid,
			req->path, req->path_len) == FALSE)
		goto error;

	parcel_w_int32(rilp, (req->start >> 8));   /* P1 */
	parcel_w_int32(rilp, (req->start & 0xff)); /* P2 */
	parcel_w_int32(rilp, req->length);         /* P3 */
	parcel_w_string(rilp, NULL);          /* data; only req'd for writes */
	parcel_w_string(rilp, NULL);          /* pin2; only req'd for writes */
	parcel_w_string(rilp, req->aid_str);

	/* sessionId, specific to latest MTK modems (harmless for older ones) */
	if (g_ril_vendor(gril) == OFONO_RIL_VENDOR_MTK)
		parcel_w_int32(rilp, 0);

	return TRUE;

error:
	return FALSE;
}

gboolean g_ril_request_sim_read_record(GRil *gril,
					const struct req_sim_read_record *req,
					struct parcel *rilp)
{
	parcel_init(rilp);
	parcel_w_int32(rilp, CMD_READ_RECORD);
	parcel_w_int32(rilp, req->fileid);

	g_ril_append_print_buf(gril,
				"(cmd=0x%.2X,efid=0x%.4X,",
				CMD_READ_RECORD,
				req->fileid);

	if (set_path(gril, req->app_type, rilp, req->fileid,
			req->path, req->path_len) == FALSE)
		goto error;

	parcel_w_int32(rilp, req->record);      /* P1 */
	parcel_w_int32(rilp, 4);           /* P2 */
	parcel_w_int32(rilp, req->length);      /* P3 */
	parcel_w_string(rilp, NULL);       /* data; only req'd for writes */
	parcel_w_string(rilp, NULL);       /* pin2; only req'd for writes */
	parcel_w_string(rilp, req->aid_str); /* AID (Application ID) */

	/* sessionId, specific to latest MTK modems (harmless for older ones) */
	if (g_ril_vendor(gril) == OFONO_RIL_VENDOR_MTK)
		parcel_w_int32(rilp, 0);

	return TRUE;

error:
	return FALSE;
}

gboolean g_ril_request_sim_write_binary(GRil *gril,
					const struct req_sim_write_binary *req,
					struct parcel *rilp)
{
	char *hex_data;
	int p1, p2;

	parcel_init(rilp);
	parcel_w_int32(rilp, CMD_UPDATE_BINARY);
	parcel_w_int32(rilp, req->fileid);

	g_ril_append_print_buf(gril, "(cmd=0x%02X,efid=0x%04X,",
				CMD_UPDATE_BINARY, req->fileid);

	if (set_path(gril, req->app_type, rilp, req->fileid,
			req->path, req->path_len) == FALSE)
		goto error;

	p1 = req->start >> 8;
	p2 = req->start & 0xff;
	hex_data = encode_hex(req->data, req->length, 0);

	parcel_w_int32(rilp, p1);		/* P1 */
	parcel_w_int32(rilp, p2);		/* P2 */
	parcel_w_int32(rilp, req->length);	/* P3 (Lc) */
	parcel_w_string(rilp, hex_data);	/* data */
	parcel_w_string(rilp, NULL);		/* pin2; only for FDN/BDN */
	parcel_w_string(rilp, req->aid_str);	/* AID (Application ID) */

	/* sessionId, specific to latest MTK modems (harmless for older ones) */
	if (g_ril_vendor(gril) == OFONO_RIL_VENDOR_MTK)
		parcel_w_int32(rilp, 0);

	g_ril_append_print_buf(gril,
				"%s%d,%d,%d,%s,pin2=(null),aid=%s)",
				print_buf,
				p1,
				p2,
				req->length,
				hex_data,
				req->aid_str);

	g_free(hex_data);

	return TRUE;

error:
	return FALSE;
}

static int get_sim_record_access_p2(enum req_record_access_mode mode)
{
	switch (mode) {
	case GRIL_REC_ACCESS_MODE_CURRENT:
		return 4;
	case GRIL_REC_ACCESS_MODE_ABSOLUTE:
		return 4;
	case GRIL_REC_ACCESS_MODE_NEXT:
		return 2;
	case GRIL_REC_ACCESS_MODE_PREVIOUS:
		return 3;
	}

	return -1;
}

gboolean g_ril_request_sim_write_record(GRil *gril,
					const struct req_sim_write_record *req,
					struct parcel *rilp)
{
	char *hex_data;
	int p2;

	parcel_init(rilp);
	parcel_w_int32(rilp, CMD_UPDATE_RECORD);
	parcel_w_int32(rilp, req->fileid);

	g_ril_append_print_buf(gril, "(cmd=0x%02X,efid=0x%04X,",
				CMD_UPDATE_RECORD, req->fileid);

	if (set_path(gril, req->app_type, rilp, req->fileid,
			req->path, req->path_len) == FALSE)
		goto error;

	p2 = get_sim_record_access_p2(req->mode);
	hex_data = encode_hex(req->data, req->length, 0);

	parcel_w_int32(rilp, req->record);	/* P1 */
	parcel_w_int32(rilp, p2);		/* P2 (access mode) */
	parcel_w_int32(rilp, req->length);	/* P3 (Lc) */
	parcel_w_string(rilp, hex_data);	/* data */
	parcel_w_string(rilp, NULL);		/* pin2; only for FDN/BDN */
	parcel_w_string(rilp, req->aid_str);	/* AID (Application ID) */

	/* sessionId, specific to latest MTK modems (harmless for older ones) */
	if (g_ril_vendor(gril) == OFONO_RIL_VENDOR_MTK)
		parcel_w_int32(rilp, 0);

	g_ril_append_print_buf(gril,
				"%s%d,%d,%d,%s,pin2=(null),aid=%s)",
				print_buf,
				req->record,
				p2,
				req->length,
				hex_data,
				req->aid_str);

	g_free(hex_data);

	return TRUE;

error:
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
