/*
 *  oFono - Open Source Telephony - RIL-based devices
 *
 *  Copyright (C) 2015 Jolla Ltd.
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License version 2 as
 *  published by the Free Software Foundation.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 */

#include "ril_plugin.h"
#include "ril_util.h"
#include "ril_constants.h"
#include "ril_log.h"

#include "simutil.h"
#include "util.h"
#include "ofono.h"

#define SIM_STATUS_RETRY_SECS (2)

/*
 * TODO:
 * 1. Define constants for hex literals
 * 2. Document P1-P3 usage (+CSRM)
 */

#define EF_STATUS_INVALIDATED 0
#define EF_STATUS_VALID 1

/* Commands defined for TS 27.007 +CRSM */
#define CMD_READ_BINARY   176 /* 0xB0   */
#define CMD_READ_RECORD   178 /* 0xB2   */
#define CMD_GET_RESPONSE  192 /* 0xC0   */
#define CMD_UPDATE_BINARY 214 /* 0xD6   */
#define CMD_UPDATE_RECORD 220 /* 0xDC   */
#define CMD_STATUS        242 /* 0xF2   */
#define CMD_RETRIEVE_DATA 203 /* 0xCB   */
#define CMD_SET_DATA      219 /* 0xDB   */

/* FID/path of SIM/USIM root directory */
#define ROOTMF "3F00"

/* RIL_Request* parameter counts */
#define GET_IMSI_NUM_PARAMS 1
#define ENTER_SIM_PIN_PARAMS 2
#define SET_FACILITY_LOCK_PARAMS 5
#define ENTER_SIM_PUK_PARAMS 3
#define CHANGE_SIM_PIN_PARAMS 3

#define MAX_UICC_APPS 16

struct ril_sim_status {
	guint card_state;
	guint pin_state;
	guint gsm_umts_index;
	guint cdma_index;
	guint ims_index;
	guint num_apps;
};

struct ril_sim_app {
	guint app_type;
	guint app_state;
	guint perso_substate;
	char *aid_str;
	char *app_str;
	guint pin_replaced;
	guint pin1_state;
	guint pin2_state;
};

/*
 * TODO: CDMA/IMS
 *
 * This code currently only grabs the AID/application ID from
 * the gsm_umts application on the SIM card.  This code will
 * need to be modified for CDMA support, and possibly IMS-based
 * applications.  In this case, app_id should be changed to an
 * array or HashTable of app_status structures.
 *
 * The same applies to the app_type.
 */
struct ril_sim {
	GRilIoChannel *io;
	GRilIoQueue *q;
	GRilIoQueue *q2;
	struct ofono_sim *sim;
	guint slot;
	gchar *aid_str;
	int app_type;
	gchar *app_str;
	guint app_index;
	enum ofono_sim_password_type passwd_type;
	int retries[OFONO_SIM_PASSWORD_INVALID];
	enum ofono_sim_password_type passwd_state;
	gboolean initialized;
	gboolean removed;
	guint retry_status_timer_id;
	guint idle_id;
	guint status_req_id;
	gulong event_id;
};

struct ril_sim_cbd {
	struct ril_sim *sd;
	union _ofono_sim_cb {
		ofono_sim_file_info_cb_t file_info;
		ofono_sim_read_cb_t read;
		ofono_sim_imsi_cb_t imsi;
		ofono_sim_passwd_cb_t passwd;
		ofono_sim_lock_unlock_cb_t lock_unlock;
		gpointer ptr;
	} cb;
	gpointer data;
};

#define ril_sim_cbd_free g_free

static void ril_sim_request_status(struct ril_sim *sd);

static inline struct ril_sim *ril_sim_get_data(struct ofono_sim *sim)
{
	return ofono_sim_get_data(sim);
}

static struct ril_sim_cbd *ril_sim_cbd_new(struct ril_sim *sd, void *cb,
								void *data)
{
	struct ril_sim_cbd *cbd = g_new0(struct ril_sim_cbd, 1);

	cbd->sd = sd;
	cbd->cb.ptr = cb;
	cbd->data = data;
	return cbd;
}

int ril_sim_app_type(struct ofono_sim *sim)
{
	struct ril_sim *sd = ril_sim_get_data(sim);
	return sd ? sd->app_type : RIL_APPTYPE_UNKNOWN;
}

static void ril_sim_append_path(struct ril_sim *sd, GRilIoRequest *req,
		const int fileid, const guchar *path, const guint path_len)
{
	guchar db_path[6] = { 0x00 };
	char *hex_path = NULL;
	int len = 0;

	DBG("");

	if (path_len > 0 && path_len < 7) {
		memcpy(db_path, path, path_len);
		len = path_len;
	} else if (sd->app_type == RIL_APPTYPE_USIM) {
		len = sim_ef_db_get_path_3g(fileid, db_path);
	} else if (sd->app_type == RIL_APPTYPE_SIM) {
		len = sim_ef_db_get_path_2g(fileid, db_path);
	} else {
		ofono_error("Unsupported app_type: 0x%x", sd->app_type);
	}

	if (len > 0) {
		hex_path = encode_hex(db_path, len, 0);
		grilio_request_append_utf8(req, hex_path);
		DBG("%s", hex_path);
		g_free(hex_path);
	} else if (fileid == SIM_EF_ICCID_FILEID || fileid == SIM_EFPL_FILEID) {
		/*
		 * Special catch-all for EF_ICCID (unique card ID)
		 * and EF_PL files which exist in the root directory.
		 * As the sim_info_cb function may not have yet
		 * recorded the app_type for the SIM, and the path
		 * for both files is the same for 2g|3g, just hard-code.
		 *
		 * See 'struct ef_db' in:
		 * ../../src/simutil.c for more details.
		 */
		DBG("%s", ROOTMF);
		grilio_request_append_utf8(req, ROOTMF);
	} else {
		/*
		 * The only known case of this is EFPHASE_FILED (0x6FAE).
		 * The ef_db table ( see /src/simutil.c ) entry for
		 * EFPHASE contains a value of 0x0000 for it's
		 * 'parent3g' member.  This causes a NULL path to
		 * be returned.
		 */

		DBG("db_get_path*() returned empty path.");
		grilio_request_append_utf8(req, NULL);
	}
}

static guchar *ril_sim_parse_io_response(const void *data, guint len,
				int *sw1, int *sw2, int *hex_len)
{
	GRilIoParser rilp;
	char *response = NULL;
	guchar *hex_response = NULL;

	/* Minimum length of SIM_IO_Response is 12:
	 * sw1 (int32)
	 * sw2 (int32)
	 * simResponse (string)
	 */
	if (len < 12) {
		ofono_error("SIM IO reply too small (< 12): %d", len);
		return NULL;
	}

	DBG("length is: %d", len);
	grilio_parser_init(&rilp, data, len);
	grilio_parser_get_int32(&rilp, sw1);
	grilio_parser_get_int32(&rilp, sw2);

	response = grilio_parser_get_utf8(&rilp);
	if (response) {
		long items_written = 0;
		const long response_len = strlen(response);
		DBG("response is set; len is: %ld", response_len);
		hex_response = decode_hex(response, response_len,
			&items_written, -1);
		*hex_len = items_written;
	}

	DBG("sw1=0x%.2X,sw2=0x%.2X,%s", *sw1, *sw2, response);
	g_free(response);
	return hex_response;
}

static void ril_sim_file_info_cb(GRilIoChannel *io, int status,
			const void *data, guint len, void *user_data)
{
	struct ril_sim_cbd *cbd = user_data;
	ofono_sim_file_info_cb_t cb = cbd->cb.file_info;
	struct ril_sim *sd = cbd->sd;
	struct ofono_error error;
	gboolean ok = FALSE;
	int sw1 = 0, sw2 = 0, response_len = 0;
	int flen = 0, rlen = 0, str = 0;
	guchar *response = NULL;
	guchar access[3] = { 0x00, 0x00, 0x00 };
	guchar file_status = EF_STATUS_VALID;

	DBG("");

	/* In case sim card has been removed prior to this callback has been
	 * called we must not call the core call back method as otherwise the
	 * core will crash.
	 */
	if (sd->removed == TRUE) {
		ofono_error("RIL_CARDSTATE_ABSENT");
		return;
	}

	error.error = 0;
	error.type = OFONO_ERROR_TYPE_FAILURE;
	if (status != RIL_E_SUCCESS) {
		DBG("Reply failure: %s", ril_error_to_string(status));
		goto error;
	}

	if ((response = ril_sim_parse_io_response(data, len,
					&sw1, &sw2, &response_len)) == NULL) {
		ofono_error("Can't parse SIM IO response");
		goto error;
	}

	if ((sw1 != 0x90 && sw1 != 0x91 && sw1 != 0x92 && sw1 != 0x9f) ||
		(sw1 == 0x90 && sw2 != 0x00)) {
		ofono_error("Invalid values: sw1: %02x sw2: %02x", sw1, sw2);
		error.type = OFONO_ERROR_TYPE_SIM;
		error.error = (sw1 << 8) | sw2;
		goto error;
	}

	if (response_len) {
		if (response[0] == 0x62) {
			ok = sim_parse_3g_get_response(
				response, response_len,
				&flen, &rlen, &str, access, NULL);
		} else {
			ok = sim_parse_2g_get_response(
				response, response_len,
				&flen, &rlen, &str, access, &file_status);
		}
	}

	if (!ok) {
		ofono_error("%s parse response failed", __func__);
		goto error;
	}

	error.type = OFONO_ERROR_TYPE_NO_ERROR;
	cb(&error, flen, str, rlen, access, file_status, cbd->data);
	g_free(response);
	return;

error:
	cb(&error, -1, -1, -1, NULL, EF_STATUS_INVALIDATED, cbd->data);
	g_free(response);
}

static guint ril_sim_request_io(struct ril_sim *sd, GRilIoQueue *q, int fileid,
		guint cmd, guint p1, guint p2, guint p3, const guchar *path,
		unsigned int path_len, GRilIoChannelResponseFunc cb,
		struct ril_sim_cbd *cbd)
{
	guint id;
	GRilIoRequest *req = grilio_request_sized_new(80);

	DBG("cmd=0x%.2X,efid=0x%.4X,%d,%d,%d,(null),pin2=(null),aid=%s",
		cmd, fileid, p1, p2, p3, sd->aid_str);

	grilio_request_append_int32(req, cmd);
	grilio_request_append_int32(req, fileid);
	ril_sim_append_path(sd, req, fileid, path, path_len);
	grilio_request_append_int32(req, p1);   /* P1 */
	grilio_request_append_int32(req, p2);   /* P2 */
	grilio_request_append_int32(req, p3);   /* P3 */
	grilio_request_append_utf8(req, NULL);  /* data; only for writes */
	grilio_request_append_utf8(req, NULL);  /* pin2; only for writes */
	grilio_request_append_utf8(req, sd->aid_str);

	id = grilio_queue_send_request_full(q, req, RIL_REQUEST_SIM_IO,
					cb, ril_sim_cbd_free, cbd);
	grilio_request_unref(req);
	return id;
}

static void ril_sim_internal_read_file_info(struct ril_sim *sd, GRilIoQueue *q,
		int fileid, const unsigned char *path, unsigned int path_len,
		ofono_sim_file_info_cb_t cb, void *data)
{
	if (!sd || !ril_sim_request_io(sd, q, fileid, CMD_GET_RESPONSE,
			0, 0, 15, path, path_len, ril_sim_file_info_cb,
					ril_sim_cbd_new(sd, cb, data))) {
		struct ofono_error error;
		cb(ril_error_failure(&error), -1, -1, -1, NULL,
			EF_STATUS_INVALIDATED, data);
	}
}

static void ril_sim_ofono_read_file_info(struct ofono_sim *sim, int fileid,
		const unsigned char *path, unsigned int len,
		ofono_sim_file_info_cb_t cb, void *data)
{
	struct ril_sim *sd = ril_sim_get_data(sim);

	ril_sim_internal_read_file_info(sd, sd->q, fileid, path, len, cb, data);
}

void ril_sim_read_file_info(struct ofono_sim *sim, int fileid,
		const unsigned char *path, unsigned int path_len,
		ofono_sim_file_info_cb_t cb, void *data)
{
	struct ril_sim *sd = ril_sim_get_data(sim);

	ril_sim_internal_read_file_info(sd, sd->q2, fileid, path, path_len,
								cb, data);
}

static void ril_sim_read_cb(GRilIoChannel *io, int status,
				const void *data, guint len, void *user_data)
{
	struct ril_sim_cbd *cbd = user_data;
	ofono_sim_read_cb_t cb = cbd->cb.read;
	struct ofono_error error;
	int sw1 = 0, sw2 = 0, response_len = 0;
	guchar *response = NULL;

	DBG("");
	if (status != RIL_E_SUCCESS) {
		ofono_error("Error: %s", ril_error_to_string(status));
		goto error;
	}

	if ((response = ril_sim_parse_io_response(data, len,
					&sw1, &sw2, &response_len)) == NULL) {
		ofono_error("Error parsing IO response");
		goto error;
	}

	cb(ril_error_ok(&error), response, response_len, cbd->data);
	g_free(response);
	return;

error:
	cb(ril_error_failure(&error), NULL, 0, cbd->data);
}

static void ril_sim_read(struct ril_sim *sd, GRilIoQueue *q, int fileid,
		guint cmd, guint p1, guint p2, guint p3, const guchar *path,
		unsigned int path_len, ofono_sim_read_cb_t cb, void *data)
{
	if (!sd || !ril_sim_request_io(sd, q, fileid, cmd, p1, p2, p3, path,
		path_len, ril_sim_read_cb, ril_sim_cbd_new(sd, cb, data))) {
		struct ofono_error error;
		cb(ril_error_failure(&error), NULL, 0, data);
	}
}

static inline void ril_sim_internal_read_file_transparent(struct ril_sim *sd,
		GRilIoQueue *q, int fileid, int start, int length,
		const unsigned char *path, unsigned int path_len,
		ofono_sim_read_cb_t cb, void *data)
{
	ril_sim_read(sd, q, fileid, CMD_READ_BINARY, (start >> 8),
			(start & 0xff), length, path, path_len, cb, data);
}

static void ril_sim_ofono_read_file_transparent(struct ofono_sim *sim,
		int fileid, int start, int length, const unsigned char *path,
		unsigned int path_len, ofono_sim_read_cb_t cb, void *data)
{
	struct ril_sim *sd = ril_sim_get_data(sim);

	ril_sim_internal_read_file_transparent(sd, sd->q, fileid, start, length,
						path, path_len, cb, data);
}

void ril_sim_read_file_transparent(struct ofono_sim *sim, int fileid,
		int start, int length, const unsigned char *path,
		unsigned int path_len, ofono_sim_read_cb_t cb, void *data)
{
	struct ril_sim *sd = ril_sim_get_data(sim);

	ril_sim_internal_read_file_transparent(sd, sd->q2, fileid, start,
					length, path, path_len, cb, data);
}

static inline void ril_sim_internal_read_file_linear(struct ril_sim *sd,
		GRilIoQueue *q, int fileid, int record, int length,
		const unsigned char *path, unsigned int path_len,
		ofono_sim_read_cb_t cb, void *data)
{
	ril_sim_read(sd, q, fileid, CMD_READ_RECORD, record, 4, length,
						path, path_len, cb, data);
}

static void ril_sim_ofono_read_file_linear(struct ofono_sim *sim, int fileid,
		int record, int length, const unsigned char *path,
		unsigned int path_len, ofono_sim_read_cb_t cb, void *data)
{
	struct ril_sim *sd = ril_sim_get_data(sim);

	ril_sim_internal_read_file_linear(sd, sd->q, fileid, record, length,
						path, path_len, cb, data);
}

void ril_sim_read_file_linear(struct ofono_sim *sim, int fileid,
		int record, int length, const unsigned char *path,
		unsigned int path_len, ofono_sim_read_cb_t cb, void *data)
{
	struct ril_sim *sd = ril_sim_get_data(sim);

	ril_sim_internal_read_file_linear(sd, sd->q2, fileid, record, length,
						path, path_len, cb, data);
}

void ril_sim_read_file_cyclic(struct ofono_sim *sim, int fileid,
		int rec, int length, const unsigned char *path,
		unsigned int path_len, ofono_sim_read_cb_t cb, void *data)
{
	/* Hmmm... Is this right? */
	ril_sim_read_file_linear(sim, fileid, rec, length, path, path_len,
								cb, data);
}

static void ril_sim_ofono_read_file_cyclic(struct ofono_sim *sim, int fileid,
		int rec, int length, const unsigned char *path,
		unsigned int path_len, ofono_sim_read_cb_t cb, void *data)
{
	/* Hmmm... Is this right? */
	ril_sim_ofono_read_file_linear(sim, fileid, rec, length, path, path_len,
								cb, data);
}

static void ril_sim_get_imsi_cb(GRilIoChannel *io, int status,
				const void *data, guint len, void *user_data)
{
	struct ril_sim_cbd *cbd = user_data;
	ofono_sim_imsi_cb_t cb = cbd->cb.imsi;
	struct ofono_error error;

	if (status == RIL_E_SUCCESS) {
		gchar *imsi;
		GRilIoParser rilp;
		grilio_parser_init(&rilp, data, len);
		imsi = grilio_parser_get_utf8(&rilp);
		DBG("%s", imsi);
		if (imsi) {
			/* 15 is the max length of IMSI */
			GASSERT(strlen(imsi) == 15);
			cb(ril_error_ok(&error), imsi, cbd->data);
			g_free(imsi);
			return;
		}
	} else {
		ofono_error("Reply failure: %s", ril_error_to_string(status));
	}

	cb(ril_error_failure(&error), NULL, cbd->data);
}

static void ril_sim_read_imsi(struct ofono_sim *sim, ofono_sim_imsi_cb_t cb,
				void *data)
{
	struct ril_sim *sd = ril_sim_get_data(sim);
	GRilIoRequest *req = grilio_request_sized_new(60);

	DBG("%s", sd->aid_str);
	grilio_request_append_int32(req, GET_IMSI_NUM_PARAMS);
	grilio_request_append_utf8(req, sd->aid_str);
	grilio_queue_send_request_full(sd->q, req, RIL_REQUEST_GET_IMSI,
				ril_sim_get_imsi_cb, ril_sim_cbd_free,
				ril_sim_cbd_new(sd, cb, data));
	grilio_request_unref(req);
}

static void ril_sim_configure_app(struct ril_sim *sd,
				struct ril_sim_app **apps, guint index)
{
	const struct ril_sim_app *app = apps[index];

	sd->app_type = app->app_type;

	g_free(sd->aid_str);
	sd->aid_str = g_strdup(app->aid_str);

	g_free(sd->app_str);
	sd->app_str = g_strdup(app->app_str);

	sd->app_index = index;

	DBG("setting aid_str (AID) to: %s", sd->aid_str);
	switch (app->app_state) {
	case RIL_APPSTATE_PIN:
		sd->passwd_state = OFONO_SIM_PASSWORD_SIM_PIN;
		break;
	case RIL_APPSTATE_PUK:
		sd->passwd_state = OFONO_SIM_PASSWORD_SIM_PUK;
		break;
	case RIL_APPSTATE_SUBSCRIPTION_PERSO:
		switch (app->perso_substate) {
		case RIL_PERSOSUBSTATE_SIM_NETWORK:
			sd->passwd_state = OFONO_SIM_PASSWORD_PHNET_PIN;
			break;
		case RIL_PERSOSUBSTATE_SIM_NETWORK_SUBSET:
			sd->passwd_state = OFONO_SIM_PASSWORD_PHNETSUB_PIN;
			break;
		case RIL_PERSOSUBSTATE_SIM_CORPORATE:
			sd->passwd_state = OFONO_SIM_PASSWORD_PHCORP_PIN;
			break;
		case RIL_PERSOSUBSTATE_SIM_SERVICE_PROVIDER:
			sd->passwd_state = OFONO_SIM_PASSWORD_PHSP_PIN;
			break;
		case RIL_PERSOSUBSTATE_SIM_SIM:
			sd->passwd_state = OFONO_SIM_PASSWORD_PHSIM_PIN;
			break;
		case RIL_PERSOSUBSTATE_SIM_NETWORK_PUK:
			sd->passwd_state = OFONO_SIM_PASSWORD_PHNET_PUK;
			break;
		case RIL_PERSOSUBSTATE_SIM_NETWORK_SUBSET_PUK:
			sd->passwd_state = OFONO_SIM_PASSWORD_PHNETSUB_PUK;
			break;
		case RIL_PERSOSUBSTATE_SIM_CORPORATE_PUK:
			sd->passwd_state = OFONO_SIM_PASSWORD_PHCORP_PUK;
			break;
		case RIL_PERSOSUBSTATE_SIM_SERVICE_PROVIDER_PUK:
			sd->passwd_state = OFONO_SIM_PASSWORD_PHSP_PUK;
			break;
		case RIL_PERSOSUBSTATE_SIM_SIM_PUK:
			sd->passwd_state = OFONO_SIM_PASSWORD_PHFSIM_PUK;
			break;
		default:
			sd->passwd_state = OFONO_SIM_PASSWORD_NONE;
			break;
		};
		break;
	case RIL_APPSTATE_READY:
		sd->passwd_state = OFONO_SIM_PASSWORD_NONE;
		break;
	case RIL_APPSTATE_UNKNOWN:
	case RIL_APPSTATE_DETECTED:
	default:
		sd->passwd_state = OFONO_SIM_PASSWORD_INVALID;
		break;
	}
}

static void ril_sim_set_uicc_subscription(struct ril_sim *sd,
				int app_index, int sub_status)
{
	GRilIoRequest *req = grilio_request_sized_new(16);
	const guint sub_id = sd->slot;

	DBG("%d,%d,%d,%d", sd->slot, app_index, sub_id, sub_status);
	grilio_request_append_int32(req, sd->slot);
	grilio_request_append_int32(req, app_index);
	grilio_request_append_int32(req, sub_id);
	grilio_request_append_int32(req, sub_status);
	grilio_queue_send_request(sd->q, req,
					RIL_REQUEST_SET_UICC_SUBSCRIPTION);
	grilio_request_unref(req);
}

static int ril_sim_select_uicc_subscription(struct ril_sim *sim,
				struct ril_sim_app **apps, guint num_apps)
{
	int selected_app = -1;
	guint i;

	for (i = 0; i < num_apps; i++) {
		const int type = apps[i]->app_type;
		if (type == RIL_APPTYPE_USIM || type == RIL_APPTYPE_RUIM) {
			selected_app = i;
			break;
		} else if (type != RIL_APPTYPE_UNKNOWN && selected_app == -1) {
			selected_app = i;
		}
	}

	DBG("Select app %d for subscription.", selected_app);
	if (selected_app != -1) {
		/* Number 1 means activate that app */
		ril_sim_set_uicc_subscription(sim, selected_app, 1);
	}

	return selected_app;
}

static gboolean ril_sim_parse_status_response(const void *data, guint len,
			struct ril_sim_status *status, struct ril_sim_app **apps)
{
	GRilIoParser rilp;
	int i;

	grilio_parser_init(&rilp, data, len);

	/*
	 * FIXME: Need to come up with a common scheme for verifying the
	 * size of RIL message and properly reacting to bad messages.
	 * This could be a runtime assertion, disconnect, drop/ignore
	 * the message, ...
	 *
	 * 20 is the min length of RIL_CardStatus_v6 as the AppState
	 * array can be 0-length.
	 */
	if (len < 20) {
		ofono_error("SIM_STATUS reply too small: %d bytes", len);
		status->card_state = RIL_CARDSTATE_ERROR;
		return FALSE;
	}

	grilio_parser_get_uint32(&rilp, &status->card_state);

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
	grilio_parser_get_uint32(&rilp, &status->pin_state);
	grilio_parser_get_uint32(&rilp, &status->gsm_umts_index);
	grilio_parser_get_uint32(&rilp, &status->cdma_index);
	grilio_parser_get_uint32(&rilp, &status->ims_index);
	grilio_parser_get_uint32(&rilp, &status->num_apps);

	DBG("card_state=%d, universal_pin_state=%d, gsm_umts_index=%d, "
		"cdma_index=%d, ims_index=%d", status->card_state,
		status->pin_state, status->gsm_umts_index,
		status->cdma_index, status->ims_index);

	if (status->card_state != RIL_CARDSTATE_PRESENT) {
		return FALSE;
	}

	DBG("sim num_apps: %d", status->num_apps);
	if (status->num_apps > MAX_UICC_APPS) {
		ofono_error("SIM error; too many apps: %d", status->num_apps);
		status->num_apps = MAX_UICC_APPS;
	}

	for (i = 0; i < status->num_apps; i++) {
		apps[i] = g_try_new0(struct ril_sim_app, 1);
		grilio_parser_get_uint32(&rilp, &apps[i]->app_type);
		grilio_parser_get_uint32(&rilp, &apps[i]->app_state);

		/*
		 * Consider RIL_APPSTATE_ILLEGAL also READY. Even if app state
		 * is  RIL_APPSTATE_ILLEGAL (-1), ICC operations must be
		 * permitted. Network access requests will anyway be rejected
		 * and ME will be in limited service.
		 */
		if (apps[i]->app_state == RIL_APPSTATE_ILLEGAL) {
			DBG("RIL_APPSTATE_ILLEGAL => RIL_APPSTATE_READY");
			apps[i]->app_state = RIL_APPSTATE_READY;
		}

		grilio_parser_get_uint32(&rilp, &apps[i]->perso_substate);

		/* TODO: we need a way to instruct parcel to skip
		 * a string, without allocating memory...
		 */
		apps[i]->aid_str = grilio_parser_get_utf8(&rilp); /* app ID */
		apps[i]->app_str = grilio_parser_get_utf8(&rilp); /* label */

		grilio_parser_get_uint32(&rilp, &apps[i]->pin_replaced);
		grilio_parser_get_uint32(&rilp, &apps[i]->pin1_state);
		grilio_parser_get_uint32(&rilp, &apps[i]->pin2_state);

		DBG("app[%d]: type=%d, state=%d, perso_substate=%d, "
			"aid_ptr=%s, app_label_ptr=%s, pin1_replaced=%d, "
			"pin1=%d, pin2=%d", i, apps[i]->app_type,
			apps[i]->app_state, apps[i]->perso_substate,
			apps[i]->aid_str, apps[i]->app_str,
			apps[i]->pin_replaced, apps[i]->pin1_state,
			apps[i]->pin2_state);
	}

	return TRUE;
}

static void ril_sim_free_apps(struct ril_sim_app **apps, guint num_apps)
{
	guint i;

	for (i = 0; i < num_apps; i++) {
		g_free(apps[i]->aid_str);
		g_free(apps[i]->app_str);
		g_free(apps[i]);
	}
}

static gboolean ril_sim_status_retry(gpointer user_data)
{
	struct ril_sim *sd = user_data;

	DBG("[%u]", sd->slot);
	GASSERT(sd->retry_status_timer_id);
	sd->retry_status_timer_id = 0;
	ril_sim_request_status(sd);
	return FALSE;
}

static void ril_sim_status_cb(GRilIoChannel *io, int ril_status,
				const void *data, guint len, void *user_data)
{
	struct ril_sim *sd = user_data;
	struct ril_sim_app *apps[MAX_UICC_APPS];
	struct ril_sim_status status;

	DBG("[%u]", sd->slot);
	sd->status_req_id = 0;

	if (ril_status != RIL_E_SUCCESS) {
		ofono_error("SIM status request failed: %s",
					ril_error_to_string(ril_status));
		if (!sd->retry_status_timer_id) {
			sd->retry_status_timer_id =
				g_timeout_add_seconds(SIM_STATUS_RETRY_SECS,
						ril_sim_status_retry, sd);

		}
	} else if (ril_sim_parse_status_response(data, len, &status, apps) &&
							status.num_apps) {

		int app_index = status.gsm_umts_index;

		if (app_index < 0) {
			app_index = ril_sim_select_uicc_subscription(sd,
							apps, status.num_apps);
		}
		if (app_index >= 0 && app_index < (int)status.num_apps &&
			apps[app_index]->app_type != RIL_APPTYPE_UNKNOWN) {
			ril_sim_configure_app(sd, apps, app_index);
		}

		sd->removed = FALSE;

		if (sd->passwd_state != OFONO_SIM_PASSWORD_INVALID) {
			/*
			 * ril_sim_parse_status_response returns true only when
			 * card status is RIL_CARDSTATE_PRESENT,
			 * ofono_sim_inserted_notify returns if status doesn't
			 * change. So can notify core always in this branch.
			 */
			ofono_sim_inserted_notify(sd->sim, TRUE);

			/* TODO: There doesn't seem to be any other
			 * way to force the core SIM code to
			 * recheck the PIN.
			 * Wouldn't __ofono_sim_refresh be
			 * more appropriate call here??
			 * __ofono_sim_refresh(sim, NULL, TRUE, TRUE);
			 */
			__ofono_sim_recheck_pin(sd->sim);
		}

		ril_sim_free_apps(apps, status.num_apps);
	} else if (status.card_state == RIL_CARDSTATE_ABSENT) {
		guint i;
		ofono_info("RIL_CARDSTATE_ABSENT");

		sd->passwd_state = OFONO_SIM_PASSWORD_INVALID;
		for (i = 0; i < OFONO_SIM_PASSWORD_INVALID; i++) {
			sd->retries[i] = -1;
		}

		sd->removed = TRUE;
		sd->initialized = FALSE;

		ofono_sim_inserted_notify(sd->sim, FALSE);
	}
}

static void ril_sim_request_status(struct ril_sim *sd)
{
	if (!sd->status_req_id) {
		sd->status_req_id = grilio_queue_send_request_full(sd->q,
				NULL, RIL_REQUEST_GET_SIM_STATUS,
					ril_sim_status_cb, NULL, sd);
	}
}

static void ril_sim_status_changed(GRilIoChannel *io, guint code,
				const void *data, guint len, void *user_data)
{
	struct ril_sim *sd = user_data;

	GASSERT(code == RIL_UNSOL_RESPONSE_SIM_STATUS_CHANGED);
	ril_sim_request_status(sd);
}

static void ril_sim_query_pin_retries(struct ofono_sim *sim,
					ofono_sim_pin_retries_cb_t cb,
					void *data)
{
	struct ril_sim *sd = ril_sim_get_data(sim);
	struct ofono_error error;

	cb(ril_error_ok(&error), sd->retries, data);
}

static void ril_sim_query_passwd_state_cb(GRilIoChannel *io, int err,
				const void *data, guint len, void *user_data)
{
	struct ril_sim_cbd *cbd = user_data;
	struct ril_sim *sd = cbd->sd;
	ofono_sim_passwd_cb_t cb = cbd->cb.passwd;
	struct ril_sim_app *apps[MAX_UICC_APPS];
	struct ril_sim_status status;
	const gint state = ofono_sim_get_state(sd->sim);

	if (ril_sim_parse_status_response(data, len, &status, apps) &&
							status.num_apps) {
		const int app_index = status.gsm_umts_index;

		if (app_index >= 0 && app_index < (int)status.num_apps &&
			apps[app_index]->app_type != RIL_APPTYPE_UNKNOWN) {
			ril_sim_configure_app(sd, apps, app_index);
		}

		ril_sim_free_apps(apps, status.num_apps);
	}

	DBG("passwd_state %u", sd->passwd_state);

	/* if pin code required cannot be initialized yet*/
	if (sd->passwd_state == OFONO_SIM_PASSWORD_SIM_PIN) {
		sd->initialized = FALSE;
	}

	/*
	 * To prevent double call to sim_initialize_after_pin from
	 * sim_pin_query_cb we must prevent calling sim_pin_query_cb
	 * when !OFONO_SIM_STATE_READY && OFONO_SIM_PASSWORD_NONE
	 */
	if ((state == OFONO_SIM_STATE_READY) || (sd->initialized == FALSE) ||
				(sd->passwd_state != OFONO_SIM_PASSWORD_NONE)){
		struct ofono_error error;

		if (sd->passwd_state == OFONO_SIM_PASSWORD_NONE) {
			sd->initialized = TRUE;
		}

		if (state == OFONO_SIM_STATE_LOCKED_OUT) {
			sd->initialized = FALSE;
		}

		if (sd->passwd_state == OFONO_SIM_PASSWORD_INVALID) {
			cb(ril_error_failure(&error), -1, cbd->data);
		} else {
			cb(ril_error_ok(&error), sd->passwd_state, cbd->data);
		}
	}
}

static void ril_sim_query_passwd_state(struct ofono_sim *sim,
					ofono_sim_passwd_cb_t cb, void *data)
{
	struct ril_sim *sd = ril_sim_get_data(sim);

	grilio_queue_send_request_full(sd->q, NULL,
		RIL_REQUEST_GET_SIM_STATUS, ril_sim_query_passwd_state_cb,
		ril_sim_cbd_free, ril_sim_cbd_new(sd, cb, data));
}

static void ril_sim_pin_change_state_cb(GRilIoChannel *io, int status,
				const void *data, guint len, void *user_data)
{
	struct ril_sim_cbd *cbd = user_data;
	ofono_sim_lock_unlock_cb_t cb = cbd->cb.lock_unlock;
	struct ril_sim *sd = cbd->sd;
	struct ofono_error error;
	GRilIoParser rilp;
	int retry_count = 0;
	int passwd_type = sd->passwd_type;
	int i;

	/* There is no reason to ask SIM status until
	 * unsolicited sim status change indication
	 * Looks like state does not change before that.
	 */

	grilio_parser_init(&rilp, data, len);
	grilio_parser_get_int32(&rilp, NULL);
	grilio_parser_get_int32(&rilp, &retry_count);

	for (i = 0; i < OFONO_SIM_PASSWORD_INVALID; i++) {
		sd->retries[i] = -1;
	}

	sd->retries[passwd_type] = retry_count;

	DBG("result=%d passwd_type=%d retry_count=%d",
		status, passwd_type, retry_count);

	error.error = 0;
	error.type = (status == RIL_E_SUCCESS) ?
		OFONO_ERROR_TYPE_NO_ERROR :
		OFONO_ERROR_TYPE_FAILURE;

	cb(&error, cbd->data);
}

static void ril_sim_pin_send(struct ofono_sim *sim, const char *passwd,
				ofono_sim_lock_unlock_cb_t cb, void *data)
{
	struct ril_sim *sd = ril_sim_get_data(sim);
	GRilIoRequest *req = grilio_request_sized_new(60);

	/* Should passwd_type be stored in cbd? */
	sd->passwd_type = OFONO_SIM_PASSWORD_SIM_PIN;
	grilio_request_append_int32(req, ENTER_SIM_PIN_PARAMS);
	grilio_request_append_utf8(req, passwd);
	grilio_request_append_utf8(req, sd->aid_str);

	DBG("%s,aid=%s", passwd, sd->aid_str);
	grilio_queue_send_request_full(sd->q, req,
			RIL_REQUEST_ENTER_SIM_PIN, ril_sim_pin_change_state_cb,
			ril_sim_cbd_free, ril_sim_cbd_new(sd, cb, data));
	grilio_request_unref(req);
}

static guint ril_perso_change_state(struct ofono_sim *sim,
		enum ofono_sim_password_type passwd_type, int enable,
		const char *passwd, ofono_sim_lock_unlock_cb_t cb, void *data)
{
	struct ril_sim *sd = ril_sim_get_data(sim);
	GRilIoRequest *req = NULL;
	int code = 0;
	guint id = 0;

	switch (passwd_type) {
	case OFONO_SIM_PASSWORD_PHNET_PIN:
		if (!enable) {
			code = RIL_REQUEST_ENTER_NETWORK_DEPERSONALIZATION;
			req = grilio_request_sized_new(12);
			grilio_request_append_int32(req,
					RIL_PERSOSUBSTATE_SIM_NETWORK);
			grilio_request_append_utf8(req, passwd);
		} else {
			DBG("Not supported, enable=%d", enable);
		}
		break;
	default:
		DBG("Not supported, type=%d", passwd_type);
		break;
	}

	if (req) {
		sd->passwd_type = passwd_type;
		id = grilio_queue_send_request_full(sd->q, req, code,
			ril_sim_pin_change_state_cb, ril_sim_cbd_free,
			ril_sim_cbd_new(sd, cb, data));
		grilio_request_unref(req);
	}

	return id;
}

static void ril_sim_pin_change_state(struct ofono_sim *sim,
	enum ofono_sim_password_type passwd_type, int enable,
	const char *passwd, ofono_sim_lock_unlock_cb_t cb, void *data)
{
	struct ril_sim *sd = ril_sim_get_data(sim);
	struct ofono_error error;
	const char *type_str = NULL;
	guint id = 0;

	switch (passwd_type) {
	case OFONO_SIM_PASSWORD_SIM_PIN:
		type_str = "SC";
		break;
	case OFONO_SIM_PASSWORD_PHSIM_PIN:
		type_str = "PS";
		break;
	case OFONO_SIM_PASSWORD_PHFSIM_PIN:
		type_str = "PF";
		break;
	case OFONO_SIM_PASSWORD_SIM_PIN2:
		type_str = "P2";
		break;
	case OFONO_SIM_PASSWORD_PHNET_PIN:
		id = ril_perso_change_state(sim, passwd_type, enable, passwd,
								cb, data);
		break;
	case OFONO_SIM_PASSWORD_PHNETSUB_PIN:
		type_str = "PU";
		break;
	case OFONO_SIM_PASSWORD_PHSP_PIN:
		type_str = "PP";
		break;
	case OFONO_SIM_PASSWORD_PHCORP_PIN:
		type_str = "PC";
		break;
	default:
		break;
	}

	DBG("%d,%s,%d,%s,0,aid=%s", passwd_type, type_str, enable,
							passwd, sd->aid_str);

	if (type_str) {
		GRilIoRequest *req = grilio_request_sized_new(60);
		grilio_request_append_int32(req, SET_FACILITY_LOCK_PARAMS);
		grilio_request_append_utf8(req, type_str);
		grilio_request_append_utf8(req, enable ?
			RIL_FACILITY_LOCK : RIL_FACILITY_UNLOCK);
		grilio_request_append_utf8(req, passwd);
		grilio_request_append_utf8(req, "0");		/* class */
		grilio_request_append_utf8(req, sd->aid_str);

		sd->passwd_type = passwd_type;
		id = grilio_queue_send_request_full(sd->q, req,
			RIL_REQUEST_SET_FACILITY_LOCK,
			ril_sim_pin_change_state_cb, ril_sim_cbd_free,
			ril_sim_cbd_new(sd, cb, data));
		grilio_request_unref(req);
	}

	if (!id) {
		cb(ril_error_failure(&error), data);
	}
}

static void ril_sim_pin_send_puk(struct ofono_sim *sim,
				const char *puk, const char *passwd,
				ofono_sim_lock_unlock_cb_t cb, void *data)
{
	struct ril_sim *sd = ril_sim_get_data(sim);
	GRilIoRequest *req = grilio_request_sized_new(60);

	grilio_request_append_int32(req, ENTER_SIM_PUK_PARAMS);
	grilio_request_append_utf8(req, puk);
	grilio_request_append_utf8(req, passwd);
	grilio_request_append_utf8(req, sd->aid_str);

	DBG("puk=%s,pin=%s,aid=%s", puk, passwd, sd->aid_str);
	sd->passwd_type = OFONO_SIM_PASSWORD_SIM_PUK;
	grilio_queue_send_request_full(sd->q, req,
		RIL_REQUEST_ENTER_SIM_PUK, ril_sim_pin_change_state_cb,
		ril_sim_cbd_free, ril_sim_cbd_new(sd, cb, data));
	grilio_request_unref(req);
}

static void ril_sim_change_passwd(struct ofono_sim *sim,
				enum ofono_sim_password_type passwd_type,
				const char *old_passwd, const char *new_passwd,
				ofono_sim_lock_unlock_cb_t cb, void *data)
{
	struct ril_sim *sd = ril_sim_get_data(sim);
	GRilIoRequest *req = grilio_request_sized_new(60);

	grilio_request_append_int32(req, CHANGE_SIM_PIN_PARAMS);
	grilio_request_append_utf8(req, old_passwd);
	grilio_request_append_utf8(req, new_passwd);
	grilio_request_append_utf8(req, sd->aid_str);

	DBG("old=%s,new=%s,aid=%s", old_passwd, new_passwd, sd->aid_str);
	sd->passwd_type = passwd_type;
	grilio_queue_send_request_full(sd->q, req,
		(passwd_type == OFONO_SIM_PASSWORD_SIM_PIN2) ?
		RIL_REQUEST_CHANGE_SIM_PIN2 : RIL_REQUEST_CHANGE_SIM_PIN,
		ril_sim_pin_change_state_cb, ril_sim_cbd_free,
		ril_sim_cbd_new(sd, cb, data));
}

static gboolean ril_sim_register(gpointer user)
{
	struct ril_sim *sd = user;

	DBG("[%u]", sd->slot);
	GASSERT(sd->idle_id);
	sd->idle_id = 0;

	ril_sim_request_status(sd);
	ofono_sim_register(sd->sim);
	sd->event_id = grilio_channel_add_unsol_event_handler(sd->io,
		ril_sim_status_changed, RIL_UNSOL_RESPONSE_SIM_STATUS_CHANGED,
		sd);

	return FALSE;
}

static int ril_sim_probe(struct ofono_sim *sim, unsigned int vendor,
				void *data)
{
	struct ril_modem *modem = data;
	struct ril_sim *sd = g_new0(struct ril_sim, 1);
	int i;

	sd->sim = sim;
	sd->slot = ril_modem_slot(modem);
	sd->io = grilio_channel_ref(ril_modem_io(modem));

	/* NB: One queue is used for the requests generated by the ofono
	 * code, and the second one for the requests initiated internally
	 * by the RIL code.
	 *
	 * The difference is that when SIM card is removed, ofono requests
	 * are cancelled without invoking they completion callbacks (otherwise
	 * ofono would crash) while our completion callbacks have to be
	 * notified in this case (otherwise we would leak memory)
	 */
	sd->q = grilio_queue_new(sd->io);
	sd->q2 = grilio_queue_new(sd->io);

	DBG("[%u]", sd->slot);

	for (i = 0; i < OFONO_SIM_PASSWORD_INVALID; i++) {
		sd->retries[i] = -1;
	}

	sd->idle_id = g_idle_add(ril_sim_register, sd);
	ofono_sim_set_data(sim, sd);
	return 0;
}

static void ril_sim_remove(struct ofono_sim *sim)
{
	struct ril_sim *sd = ril_sim_get_data(sim);

	DBG("[%u]", sd->slot);
	grilio_queue_cancel_all(sd->q, FALSE);
	grilio_queue_cancel_all(sd->q2, TRUE);
	ofono_sim_set_data(sim, NULL);

	if (sd->idle_id) {
		g_source_remove(sd->idle_id);
	}

	if (sd->retry_status_timer_id) {
		g_source_remove(sd->retry_status_timer_id);
	}

	grilio_channel_remove_handler(sd->io, sd->event_id);
	grilio_channel_unref(sd->io);
	grilio_queue_unref(sd->q);
	grilio_queue_unref(sd->q2);
	g_free(sd->aid_str);
	g_free(sd->app_str);
	g_free(sd);
}

const struct ofono_sim_driver ril_sim_driver = {
	.name                   = RILMODEM_DRIVER,
	.probe                  = ril_sim_probe,
	.remove                 = ril_sim_remove,
	.read_file_info         = ril_sim_ofono_read_file_info,
	.read_file_transparent  = ril_sim_ofono_read_file_transparent,
	.read_file_linear       = ril_sim_ofono_read_file_linear,
	.read_file_cyclic       = ril_sim_ofono_read_file_cyclic,
	.read_imsi              = ril_sim_read_imsi,
	.query_passwd_state     = ril_sim_query_passwd_state,
	.send_passwd            = ril_sim_pin_send,
	.lock                   = ril_sim_pin_change_state,
	.reset_passwd           = ril_sim_pin_send_puk,
	.change_passwd          = ril_sim_change_passwd,
	.query_pin_retries      = ril_sim_query_pin_retries
/*
 * TODO: Implementing SIM write file IO support requires
 * the following functions to be defined.
 *
 *	.write_file_transparent	= ril_sim_update_binary,
 *	.write_file_linear	= ril_sim_update_record,
 *	.write_file_cyclic	= ril_sim_update_cyclic,
 */
};

/*
 * Local Variables:
 * mode: C
 * c-basic-offset: 8
 * indent-tabs-mode: t
 * End:
 */
