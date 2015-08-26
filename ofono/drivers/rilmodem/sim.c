/*
 *
 *  oFono - Open Source Telephony - RIL Modem Support
 *
 *  Copyright (C) 2008-2011  Intel Corporation. All rights reserved.
 *  Copyright (C) 2013 Canonical, Ltd. All rights reserved.
 *  Copyright (C) 2014 Jolla Ltd.
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

#include <glib.h>

#include <ofono/log.h>
#include <ofono/modem.h>
#include <ofono/sim.h>

#include "ofono.h"

#include "simutil.h"
#include "util.h"

#include "gril.h"
#include "grilrequest.h"
#include "grilutil.h"
#include "parcel.h"
#include "ril_constants.h"
#include "rilmodem.h"

/* Based on ../drivers/atmodem/sim.c.
 *
 * TODO:
 * 1. Defines constants for hex literals
 * 2. Document P1-P3 usage (+CSRM)
 */

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

/* Current SIM */
static struct ofono_sim *current_sim;
/* Current active app */
int current_active_app = RIL_APPTYPE_UNKNOWN;

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
struct sim_data {
	GRil *ril;
	gchar *aid_str;
	guint app_type;
	gchar *app_str;
	guint app_index;
	enum ofono_sim_password_type passwd_type;
	int retries[OFONO_SIM_PASSWORD_INVALID];
	enum ofono_sim_password_type passwd_state;
	guint idle_id;
	gboolean initialized;
	gboolean removed;
};

static void ril_pin_change_state_cb(struct ril_msg *message,
			gpointer user_data);

static void set_path(struct sim_data *sd, struct parcel *rilp,
			const int fileid, const guchar *path,
			const guint path_len)
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
		ofono_error("%s Unsupported app_type: 0%x", __func__,
				sd->app_type);
	}

	if (len > 0) {
		hex_path = encode_hex(db_path, len, 0);
		parcel_w_string(rilp, (char *) hex_path);

		g_ril_append_print_buf(sd->ril,
					"%spath=%s,",
					print_buf,
					hex_path);

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
		parcel_w_string(rilp, (char *) ROOTMF);
	} else {
		/*
		 * The only known case of this is EFPHASE_FILED (0x6FAE).
		 * The ef_db table ( see /src/simutil.c ) entry for
		 * EFPHASE contains a value of 0x0000 for it's
		 * 'parent3g' member.  This causes a NULL path to
		 * be returned.
		 */

		DBG("db_get_path*() returned empty path.");
		parcel_w_string(rilp, NULL);
	}
}

static void ril_file_info_cb(struct ril_msg *message, gpointer user_data)
{
	struct cb_data *cbd = user_data;
	ofono_sim_file_info_cb_t cb = cbd->cb;
	struct sim_data *sd = cbd->user;
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
		ofono_error("%s RIL_CARDSTATE_ABSENT", __func__);
		return;
	}

	if (message->error == RIL_E_SUCCESS) {
		decode_ril_error(&error, "OK");
	} else {
		DBG("Reply failure: %s", ril_error_to_string(message->error));
		decode_ril_error(&error, "FAIL");
		goto error;
	}

	if ((response = (guchar *)
		ril_util_parse_sim_io_rsp(sd->ril,
						message,
						&sw1,
						&sw2,
						&response_len)) == NULL) {
		ofono_error("%s Can't parse SIM IO response", __func__);
		decode_ril_error(&error, "FAIL");
		goto error;
	}

	if ((sw1 != 0x90 && sw1 != 0x91 && sw1 != 0x92 && sw1 != 0x9f) ||
		(sw1 == 0x90 && sw2 != 0x00)) {
		ofono_error("%s invalid values: sw1: %02x sw2: %02x", __func__,
				sw1, sw2);
		memset(&error, 0, sizeof(error));

		/* TODO: fix decode_ril_error to take type & error */

		error.type = OFONO_ERROR_TYPE_SIM;
		error.error = (sw1 << 8) | sw2;

		goto error;
	}

	if (response_len) {
		if (response[0] == 0x62) {
			ok = sim_parse_3g_get_response(
				response, response_len,
				&flen, &rlen, &str, access, NULL);
		} else
			ok = sim_parse_2g_get_response(
				response, response_len,
				&flen, &rlen, &str, access, &file_status);
	}

	if (!ok) {
		ofono_error("%s parse response failed", __func__);
		decode_ril_error(&error, "FAIL");
		goto error;
	}

	cb(&error, flen, str, rlen, access, file_status, cbd->data);
	g_free(response);
	return;

error:
	cb(&error, -1, -1, -1, NULL, EF_STATUS_INVALIDATED, cbd->data);
	g_free(response);
}

static void ril_sim_read_info(struct ofono_sim *sim, int fileid,
				const unsigned char *path,
				unsigned int path_len,
				ofono_sim_file_info_cb_t cb,
				void *data)
{
	struct sim_data *sd = ofono_sim_get_data(sim);
	struct cb_data *cbd = cb_data_new(cb, data);
	struct parcel rilp;
	int request = RIL_REQUEST_SIM_IO;
	guint ret;
	cbd->user = sd;

	parcel_init(&rilp);

	parcel_w_int32(&rilp, CMD_GET_RESPONSE);
	parcel_w_int32(&rilp, fileid);

	g_ril_append_print_buf(sd->ril,
				"(cmd=0x%.2X,efid=0x%.4X,",
				CMD_GET_RESPONSE,
				fileid);

	set_path(sd, &rilp, fileid, path, path_len);

	parcel_w_int32(&rilp, 0);           /* P1 */
	parcel_w_int32(&rilp, 0);           /* P2 */

	/*
	 * TODO: review parameters values used by Android.
	 * The values of P1-P3 in this code were based on
	 * values used by the atmodem driver impl.
	 *
	 * NOTE:
	 * GET_RESPONSE_EF_SIZE_BYTES == 15; !255
	 */
	parcel_w_int32(&rilp, 15);         /* P3 - max length */
	parcel_w_string(&rilp, NULL);       /* data; only req'd for writes */
	parcel_w_string(&rilp, NULL);       /* pin2; only req'd for writes */
	parcel_w_string(&rilp, sd->aid_str); /* AID (Application ID) */

	ret = g_ril_send(sd->ril,
				request,
				rilp.data,
				rilp.size,
				ril_file_info_cb, cbd, g_free);

	g_ril_append_print_buf(sd->ril,
				"%s0,0,15,(null),pin2=(null),aid=%s)",
				print_buf,
				sd->aid_str);
	g_ril_print_request(sd->ril, ret, RIL_REQUEST_SIM_IO);

	parcel_free(&rilp);

	if (ret <= 0) {
		g_free(cbd);
		CALLBACK_WITH_FAILURE(cb, -1, -1, -1, NULL,
				EF_STATUS_INVALIDATED, data);
	}
}

static void ril_file_io_cb(struct ril_msg *message, gpointer user_data)
{
	struct cb_data *cbd = user_data;
	ofono_sim_read_cb_t cb = cbd->cb;
	struct sim_data *sd = cbd->user;
	struct ofono_error error;
	int sw1 = 0, sw2 = 0, response_len = 0;
	guchar *response = NULL;

	DBG("");

	if (message->error == RIL_E_SUCCESS) {
		decode_ril_error(&error, "OK");
	} else {
		ofono_error("%s RILD reply failure: %s", __func__,
				ril_error_to_string(message->error));
		goto error;
	}

	if ((response = (guchar *)
		ril_util_parse_sim_io_rsp(sd->ril,
						message,
						&sw1,
						&sw2,
						&response_len)) == NULL) {
		ofono_error("%s Error parsing IO response", __func__);
		goto error;
	}

	cb(&error, response, response_len, cbd->data);
	g_free(response);
	return;

error:
	decode_ril_error(&error, "FAIL");
	cb(&error, NULL, 0, cbd->data);
}

static void ril_sim_read_binary(struct ofono_sim *sim, int fileid,
				int start, int length,
				const unsigned char *path,
				unsigned int path_len,
				ofono_sim_read_cb_t cb, void *data)
{
	struct sim_data *sd = ofono_sim_get_data(sim);
	struct cb_data *cbd = cb_data_new(cb, data);
	struct parcel rilp;
	int request = RIL_REQUEST_SIM_IO;
	guint ret;
	cbd->user = sd;

	g_ril_append_print_buf(sd->ril,
				"(cmd=0x%.2X,efid=0x%.4X,",
				CMD_READ_BINARY,
				fileid);

	parcel_init(&rilp);
	parcel_w_int32(&rilp, CMD_READ_BINARY);
	parcel_w_int32(&rilp, fileid);

	set_path(sd, &rilp, fileid, path, path_len);

	parcel_w_int32(&rilp, (start >> 8));   /* P1 */
	parcel_w_int32(&rilp, (start & 0xff)); /* P2 */
	parcel_w_int32(&rilp, length);         /* P3 */
	parcel_w_string(&rilp, NULL);          /* data; only req'd for writes */
	parcel_w_string(&rilp, NULL);          /* pin2; only req'd for writes */
	parcel_w_string(&rilp, sd->aid_str);

	ret = g_ril_send(sd->ril,
				request,
				rilp.data,
				rilp.size,
				ril_file_io_cb, cbd, g_free);

	g_ril_append_print_buf(sd->ril,
				"%s%d,%d,%d,(null),pin2=(null),aid=%s)",
				print_buf,
				(start >> 8),
				(start & 0xff),
				length,
				sd->aid_str);
	g_ril_print_request(sd->ril, ret, request);

	parcel_free(&rilp);

	if (ret <= 0) {
		g_free(cbd);
		CALLBACK_WITH_FAILURE(cb, NULL, 0, data);
	}
}

static void ril_sim_read_record(struct ofono_sim *sim, int fileid,
				int record, int length,
				const unsigned char *path,
				unsigned int path_len,
				ofono_sim_read_cb_t cb, void *data)
{
	struct sim_data *sd = ofono_sim_get_data(sim);
	struct cb_data *cbd = cb_data_new(cb, data);
	struct parcel rilp;
	int request = RIL_REQUEST_SIM_IO;
	guint ret;
	cbd->user = sd;

	parcel_init(&rilp);
	parcel_w_int32(&rilp, CMD_READ_RECORD);
	parcel_w_int32(&rilp, fileid);

	g_ril_append_print_buf(sd->ril,
				"(cmd=0x%.2X,efid=0x%.4X,",
				CMD_GET_RESPONSE,
				fileid);

	set_path(sd, &rilp, fileid, path, path_len);

	parcel_w_int32(&rilp, record);      /* P1 */
	parcel_w_int32(&rilp, 4);           /* P2 */
	parcel_w_int32(&rilp, length);      /* P3 */
	parcel_w_string(&rilp, NULL);       /* data; only req'd for writes */
	parcel_w_string(&rilp, NULL);       /* pin2; only req'd for writes */
	parcel_w_string(&rilp, sd->aid_str); /* AID (Application ID) */

	ret = g_ril_send(sd->ril,
				request,
				rilp.data,
				rilp.size,
				ril_file_io_cb, cbd, g_free);

	g_ril_append_print_buf(sd->ril,
				"%s%d,%d,%d,(null),pin2=(null),aid=%s)",
				print_buf,
				record,
				4,
				length,
				sd->aid_str);
	g_ril_print_request(sd->ril, ret, request);

	parcel_free(&rilp);

	if (ret <= 0) {
		g_free(cbd);
		CALLBACK_WITH_FAILURE(cb, NULL, 0, data);
	}
}

static void ril_imsi_cb(struct ril_msg *message, gpointer user_data)
{
	struct cb_data *cbd = user_data;
	ofono_sim_imsi_cb_t cb = cbd->cb;
	struct sim_data *sd = cbd->user;
	struct ofono_error error;
	struct parcel rilp;
	gchar *imsi;

	if (message->error == RIL_E_SUCCESS) {
		DBG("GET IMSI reply - OK");
		decode_ril_error(&error, "OK");
	} else {
		ofono_error("%s Reply failure: %s", __func__,
			    ril_error_to_string(message->error));
		decode_ril_error(&error, "FAIL");
		cb(&error, NULL, cbd->data);
		return;
	}

	ril_util_init_parcel(message, &rilp);

	/* 15 is the max length of IMSI
	 * add 4 bytes for string length */
	/* FIXME: g_assert(message->buf_len <= 19); */
	imsi = parcel_r_string(&rilp);

	g_ril_append_print_buf(sd->ril, "{%s}", imsi);
	g_ril_print_response(sd->ril, message);

	cb(&error, imsi, cbd->data);
	g_free(imsi);
}

static void ril_read_imsi(struct ofono_sim *sim, ofono_sim_imsi_cb_t cb,
				void *data)
{
	struct sim_data *sd = ofono_sim_get_data(sim);
	struct cb_data *cbd = cb_data_new(cb, data);
	struct parcel rilp;
	int request = RIL_REQUEST_GET_IMSI;
	guint ret;
	cbd->user = sd;

	parcel_init(&rilp);
	parcel_w_int32(&rilp, GET_IMSI_NUM_PARAMS);
	parcel_w_string(&rilp, sd->aid_str);

	ret = g_ril_send(sd->ril, request,
				rilp.data, rilp.size, ril_imsi_cb, cbd, g_free);

	g_ril_append_print_buf(sd->ril, "(%s)", sd->aid_str);
	g_ril_print_request(sd->ril, ret, request);

	parcel_free(&rilp);

	if (ret <= 0) {
		g_free(cbd);
		CALLBACK_WITH_FAILURE(cb, NULL, data);
	}
}

static void configure_active_app(struct sim_data *sd,
					struct sim_app *app,
					guint index)
{
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

static void free_sim_state(struct sim_data *sd)
{
	guint i = 0;

	sd->passwd_state = OFONO_SIM_PASSWORD_INVALID;

	for (i = 0; i < OFONO_SIM_PASSWORD_INVALID; i++)
		sd->retries[i] = -1;

	sd->removed = TRUE;
	sd->initialized = FALSE;
}

static void sim_send_set_uicc_subscription(struct sim_data *sd, int slot_id,
				int app_index, int sub_id, int sub_status)
{
	struct parcel rilp;

	DBG("");

	g_ril_request_set_uicc_subscription(sd->ril, slot_id, app_index,
						sub_id, sub_status, &rilp);

	g_ril_send(sd->ril, RIL_REQUEST_SET_UICC_SUBSCRIPTION, rilp.data,
		rilp.size, NULL, NULL, NULL);
}

static int sim_select_uicc_subscription(struct sim_data *sim,
			struct sim_status *status, struct sim_app **apps)
{
	int slot_id = 0;
	int selected_app = -1;
	unsigned int i;

	for (i = 0; i < status->num_apps; i++) {
		switch (apps[i]->app_type) {
		case RIL_APPTYPE_UNKNOWN:
			continue;
		case RIL_APPTYPE_USIM:
		case RIL_APPTYPE_RUIM:
			if (selected_app != -1) {
				switch (apps[selected_app]->app_type) {
				case RIL_APPTYPE_USIM:
				case RIL_APPTYPE_RUIM:
					break;
				default:
					selected_app = i;
				}
			} else {
				selected_app = i;
			}
			break;
		default:
			if (selected_app == -1)
				selected_app = i;
		}
	}

	DBG("Select app %d for subscription.", selected_app);

	if (selected_app != -1)
		/* Number 1 means activates that app */
		sim_send_set_uicc_subscription(sim, slot_id, selected_app,
						slot_id, 1);

	return selected_app;
}

static void sim_status_cb(struct ril_msg *message, gpointer user_data)
{
	struct ofono_sim *sim = user_data;
	struct sim_data *sd = ofono_sim_get_data(sim);
	struct sim_app *apps[MAX_UICC_APPS];
	struct sim_status status;
	struct parcel rilp;

	DBG("");

	if (ril_util_parse_sim_status(sd->ril, message, &status, apps) &&
		status.num_apps) {

		/* TODO(CDMA): need some kind of logic to
		 * set the correct app_index,
		 */
		int app_index = status.gsm_umts_index;

		if (app_index < 0) {
			app_index = sim_select_uicc_subscription(sd,
								&status, apps);
		}
		if (app_index >= 0 && app_index < (int)status.num_apps &&
			apps[app_index]->app_type != RIL_APPTYPE_UNKNOWN) {
			current_active_app = apps[app_index]->app_type;
			configure_active_app(sd, apps[app_index], app_index);
		}

		sd->removed = FALSE;

		if (sd->passwd_state != OFONO_SIM_PASSWORD_INVALID) {
			/*
			 * ril_util_parse_sim_status returns true only when
			 * card status is RIL_CARDSTATE_PRESENT,
			 * ofono_sim_inserted_notify returns if status doesn't
			 * change. So can notify core always in this branch.
			 */
			ofono_sim_inserted_notify(sim, TRUE);

			/* TODO: There doesn't seem to be any other
			 * way to force the core SIM code to
			 * recheck the PIN.
			 * Wouldn't __ofono_sim_refresh be
			 * more appropriate call here??
			 * __ofono_sim_refresh(sim, NULL, TRUE, TRUE);
			 */
			__ofono_sim_recheck_pin(sim);
		}

		if (current_online_state == RIL_ONLINE_PREF) {

			parcel_init(&rilp);
			parcel_w_int32(&rilp, 1);
			parcel_w_int32(&rilp, 1);

			ofono_info("RIL_REQUEST_RADIO_POWER ON");
			g_ril_send(sd->ril,
					RIL_REQUEST_RADIO_POWER,
					rilp.data,
					rilp.size,
					NULL, NULL, g_free);

			parcel_free(&rilp);

			current_online_state = RIL_ONLINE;
		}

		ril_util_free_sim_apps(apps, status.num_apps);
	} else {
		if (current_online_state == RIL_ONLINE)
			current_online_state = RIL_ONLINE_PREF;

		if (status.card_state == RIL_CARDSTATE_ABSENT) {
			ofono_info("%s: RIL_CARDSTATE_ABSENT", __func__);

			free_sim_state(sd);

			ofono_sim_inserted_notify(sim, FALSE);
		}
	}
}

static int send_get_sim_status(struct ofono_sim *sim)
{
	struct sim_data *sd = ofono_sim_get_data(sim);
	int request = RIL_REQUEST_GET_SIM_STATUS;
	guint ret;

	ret = g_ril_send(sd->ril, request,
				NULL, 0, sim_status_cb, sim, NULL);

	g_ril_print_request_no_args(sd->ril, ret, request);

	return ret;
}

static void ril_sim_status_changed(struct ril_msg *message, gpointer user_data)
{
	struct ofono_sim *sim = (struct ofono_sim *) user_data;
	struct sim_data *sd = ofono_sim_get_data(sim);

	DBG("");

	g_ril_print_unsol_no_args(sd->ril, message);

	send_get_sim_status(sim);
}

static void ril_query_pin_retries(struct ofono_sim *sim,
					ofono_sim_pin_retries_cb_t cb,
					void *data)
{
	struct sim_data *sd = ofono_sim_get_data(sim);
	CALLBACK_WITH_SUCCESS(cb, sd->retries, data);
}

static void ril_query_passwd_state_cb(struct ril_msg *message, gpointer user_data)
{
	struct cb_data *cbd = user_data;
	struct ofono_sim *sim = cbd->user;
	struct sim_data *sd = ofono_sim_get_data(sim);
	ofono_sim_passwd_cb_t cb = cbd->cb;
	void *data = cbd->data;
	struct sim_app *apps[MAX_UICC_APPS];
	struct sim_status status;
	gint state = ofono_sim_get_state(sim);

	if (ril_util_parse_sim_status(sd->ril, message, &status, apps) &&
		status.num_apps) {

		/* TODO(CDMA): need some kind of logic to
		 * set the correct app_index,
		 */
		int app_index = status.gsm_umts_index;

		if (app_index >= 0 && app_index < (int)status.num_apps &&
			apps[app_index]->app_type != RIL_APPTYPE_UNKNOWN) {
			current_active_app = apps[app_index]->app_type;
			configure_active_app(sd, apps[app_index], app_index);
		}

		ril_util_free_sim_apps(apps, status.num_apps);
	}
	DBG("passwd_state %u", sd->passwd_state);

	/* if pin code required cannot be initialized yet*/
	if (sd->passwd_state == OFONO_SIM_PASSWORD_SIM_PIN)
		sd->initialized = FALSE;
	/*
	 * To prevent double call to sim_initialize_after_pin from
	 * sim_pin_query_cb we must prevent calling sim_pin_query_cb
	 * when !OFONO_SIM_STATE_READY && OFONO_SIM_PASSWORD_NONE
	*/
	if ((state == OFONO_SIM_STATE_READY) || (sd->initialized == FALSE) ||
				(sd->passwd_state != OFONO_SIM_PASSWORD_NONE)){

		if (sd->passwd_state == OFONO_SIM_PASSWORD_NONE)
			sd->initialized = TRUE;

		if (state == OFONO_SIM_STATE_LOCKED_OUT)
			sd->initialized = FALSE;

		if (sd->passwd_state == OFONO_SIM_PASSWORD_INVALID)
			CALLBACK_WITH_FAILURE(cb, -1, data);
		else
			CALLBACK_WITH_SUCCESS(cb, sd->passwd_state, data);
	}

}

static void ril_query_passwd_state(struct ofono_sim *sim,
					ofono_sim_passwd_cb_t cb, void *data)
{
	struct sim_data *sd = ofono_sim_get_data(sim);
	struct cb_data *cbd = cb_data_new2(sim, cb, data);
	int request = RIL_REQUEST_GET_SIM_STATUS;
	guint ret;

	ret = g_ril_send(sd->ril, request,
			NULL, 0, ril_query_passwd_state_cb, cbd, g_free);

	g_ril_print_request_no_args(sd->ril, ret, request);

}

static void ril_pin_change_state_cb(struct ril_msg *message, gpointer user_data)
{
	struct cb_data *cbd = user_data;
	ofono_sim_lock_unlock_cb_t cb = cbd->cb;
	struct sim_data *sd = cbd->user;
	struct parcel rilp;
	int retry_count;
	int passwd_type;
	int i;

	/* There is no reason to ask SIM status until
	 * unsolicited sim status change indication
	 * Looks like state does not change before that.
	 */

	passwd_type = sd->passwd_type;
	ril_util_init_parcel(message, &rilp);
	parcel_r_int32(&rilp);
	retry_count = parcel_r_int32(&rilp);

	for (i = 0; i < OFONO_SIM_PASSWORD_INVALID; i++)
		sd->retries[i] = -1;

	sd->retries[passwd_type] = retry_count;

	DBG("result=%d passwd_type=%d retry_count=%d",
		message->error, passwd_type, retry_count);
	if (message->error == RIL_E_SUCCESS) {
		CALLBACK_WITH_SUCCESS(cb, cbd->data);
		g_ril_print_response_no_args(sd->ril, message);

	} else {
		CALLBACK_WITH_FAILURE(cb, cbd->data);
	}

}

static void ril_pin_send(struct ofono_sim *sim, const char *passwd,
				ofono_sim_lock_unlock_cb_t cb, void *data)
{
	struct sim_data *sd = ofono_sim_get_data(sim);
	struct cb_data *cbd = cb_data_new(cb, data);
	struct parcel rilp;
	int request = RIL_REQUEST_ENTER_SIM_PIN;
	int ret;

	sd->passwd_type = OFONO_SIM_PASSWORD_SIM_PIN;
	cbd->user = sd;

	parcel_init(&rilp);

	parcel_w_int32(&rilp, ENTER_SIM_PIN_PARAMS);
	parcel_w_string(&rilp, (char *) passwd);
	parcel_w_string(&rilp, sd->aid_str);

	ret = g_ril_send(sd->ril, request,
				rilp.data, rilp.size, ril_pin_change_state_cb,
				cbd, g_free);

	g_ril_append_print_buf(sd->ril, "(%s,aid=%s)", passwd, sd->aid_str);
	g_ril_print_request(sd->ril, ret, request);

	parcel_free(&rilp);

	if (ret <= 0) {
		g_free(cbd);
		CALLBACK_WITH_FAILURE(cb, data);
	}
}

static int ril_perso_change_state(struct ofono_sim *sim,
				enum ofono_sim_password_type passwd_type,
				int enable, const char *passwd,
				ofono_sim_lock_unlock_cb_t cb, void *data)
{
	struct sim_data *sd = ofono_sim_get_data(sim);
	struct cb_data *cbd = cb_data_new(cb, data);
	struct parcel rilp;
	int request = 0;
	int ret = 0;
	sd->passwd_type = passwd_type;
	cbd->user = sd;

	parcel_init(&rilp);

	switch (passwd_type) {
	case OFONO_SIM_PASSWORD_PHNET_PIN:
		if (enable) {
			DBG("Not supported, enable=%d", enable);
			goto end;
		}
		request = RIL_REQUEST_ENTER_NETWORK_DEPERSONALIZATION;
		parcel_w_int32(&rilp, RIL_PERSOSUBSTATE_SIM_NETWORK);
		parcel_w_string(&rilp, (char *) passwd);
		break;
	default:
		DBG("Not supported, type=%d", passwd_type);
		goto end;
	}

	ret = g_ril_send(sd->ril, request,
				rilp.data, rilp.size, ril_pin_change_state_cb,
				cbd, g_free);

	g_ril_print_request(sd->ril, ret, request);

end:
	parcel_free(&rilp);
	return ret;
}

static void ril_pin_change_state(struct ofono_sim *sim,
				enum ofono_sim_password_type passwd_type,
				int enable, const char *passwd,
				ofono_sim_lock_unlock_cb_t cb, void *data)
{
	DBG("passwd_type=%d", passwd_type);
	struct sim_data *sd = ofono_sim_get_data(sim);
	struct cb_data *cbd = cb_data_new(cb, data);
	struct parcel rilp;
	int request = RIL_REQUEST_SET_FACILITY_LOCK;
	int ret = 0;

	sd->passwd_type = passwd_type;
	cbd->user = sd;

	parcel_init(&rilp);
	parcel_w_int32(&rilp, SET_FACILITY_LOCK_PARAMS);

	/*
	 * TODO: clean up the use of string literals &
	 * the multiple g_ril_append_print_buf() calls
	 * by using a table lookup as does the core sim code
	 */
	switch (passwd_type) {
	case OFONO_SIM_PASSWORD_SIM_PIN:
		g_ril_append_print_buf(sd->ril, "(SC,");
		parcel_w_string(&rilp, "SC");
		break;
	case OFONO_SIM_PASSWORD_PHSIM_PIN:
		g_ril_append_print_buf(sd->ril, "(PS,");
		parcel_w_string(&rilp, "PS");
		break;
	case OFONO_SIM_PASSWORD_PHFSIM_PIN:
		g_ril_append_print_buf(sd->ril, "(PF,");
		parcel_w_string(&rilp, "PF");
		break;
	case OFONO_SIM_PASSWORD_SIM_PIN2:
		g_ril_append_print_buf(sd->ril, "(P2,");
		parcel_w_string(&rilp, "P2");
		break;
	case OFONO_SIM_PASSWORD_PHNET_PIN:
		ret = ril_perso_change_state(sim, passwd_type, enable, passwd,
						cb, data);
		goto end;
	case OFONO_SIM_PASSWORD_PHNETSUB_PIN:
		g_ril_append_print_buf(sd->ril, "(PU,");
		parcel_w_string(&rilp, "PU");
		break;
	case OFONO_SIM_PASSWORD_PHSP_PIN:
		g_ril_append_print_buf(sd->ril, "(PP,");
		parcel_w_string(&rilp, "PP");
		break;
	case OFONO_SIM_PASSWORD_PHCORP_PIN:
		g_ril_append_print_buf(sd->ril, "(PC,");
		parcel_w_string(&rilp, "PC");
		break;
	default:
		goto end;
	}

	if (enable)
		parcel_w_string(&rilp, RIL_FACILITY_LOCK);
	else
		parcel_w_string(&rilp, RIL_FACILITY_UNLOCK);

	parcel_w_string(&rilp, (char *) passwd);

	/* TODO: make this a constant... */
	parcel_w_string(&rilp, "0");		/* class */

	parcel_w_string(&rilp, sd->aid_str);

	ret = g_ril_send(sd->ril, request,
				rilp.data, rilp.size, ril_pin_change_state_cb,
				cbd, g_free);

	g_ril_append_print_buf(sd->ril, "%s,%d,%s,0,aid=%s)",
				print_buf,
				enable,
				passwd,
				sd->aid_str);

	g_ril_print_request(sd->ril, ret, request);

end:
	parcel_free(&rilp);

	if (ret <= 0) {
		g_free(cbd);
		CALLBACK_WITH_FAILURE(cb, data);
	}
}

static void ril_pin_send_puk(struct ofono_sim *sim,
				const char *puk, const char *passwd,
				ofono_sim_lock_unlock_cb_t cb, void *data)
{
	struct sim_data *sd = ofono_sim_get_data(sim);
	struct cb_data *cbd = cb_data_new(cb, data);
	struct parcel rilp;
	int request = RIL_REQUEST_ENTER_SIM_PUK;
	int ret = 0;

	sd->passwd_type = OFONO_SIM_PASSWORD_SIM_PUK;
	cbd->user = sd;

	parcel_init(&rilp);

	parcel_w_int32(&rilp, ENTER_SIM_PUK_PARAMS);
	parcel_w_string(&rilp, (char *) puk);
	parcel_w_string(&rilp, (char *) passwd);
	parcel_w_string(&rilp, sd->aid_str);

	ret = g_ril_send(sd->ril, request,
			rilp.data, rilp.size, ril_pin_change_state_cb,
			cbd, g_free);

	g_ril_append_print_buf(sd->ril, "(puk=%s,pin=%s,aid=%s)",
				puk, passwd,
				sd->aid_str);

	g_ril_print_request(sd->ril, ret, request);

	parcel_free(&rilp);

	if (ret <= 0) {
		g_free(cbd);
		CALLBACK_WITH_FAILURE(cb, data);
	}
}

static void ril_change_passwd(struct ofono_sim *sim,
				enum ofono_sim_password_type passwd_type,
				const char *old_passwd, const char *new_passwd,
				ofono_sim_lock_unlock_cb_t cb, void *data)
{
	struct sim_data *sd = ofono_sim_get_data(sim);
	struct cb_data *cbd = cb_data_new(cb, data);
	struct parcel rilp;
	int request = RIL_REQUEST_CHANGE_SIM_PIN;
	int ret = 0;

	sd->passwd_type = passwd_type;
	cbd->user = sd;

	parcel_init(&rilp);

	parcel_w_int32(&rilp, CHANGE_SIM_PIN_PARAMS);
	parcel_w_string(&rilp, (char *) old_passwd);
	parcel_w_string(&rilp, (char *) new_passwd);
	parcel_w_string(&rilp, sd->aid_str);

	if (passwd_type == OFONO_SIM_PASSWORD_SIM_PIN2)
		request = RIL_REQUEST_CHANGE_SIM_PIN2;

	ret = g_ril_send(sd->ril, request, rilp.data, rilp.size,
			ril_pin_change_state_cb, cbd, g_free);

	g_ril_append_print_buf(sd->ril, "(old=%s,new=%s,aid=%s)",
				old_passwd, new_passwd,
				sd->aid_str);

	g_ril_print_request(sd->ril, ret, request);

	parcel_free(&rilp);

	if (ret <= 0) {
		g_free(cbd);
		CALLBACK_WITH_FAILURE(cb, data);
	}
}

static gboolean ril_sim_register(gpointer user)
{
	struct ofono_sim *sim = user;
	struct sim_data *sd = ofono_sim_get_data(sim);

	DBG("");

	ofono_sim_register(sim);

	send_get_sim_status(sim);

	sd->idle_id = 0;
	g_ril_register(sd->ril,
			RIL_UNSOL_RESPONSE_SIM_STATUS_CHANGED,
			(GRilNotifyFunc) ril_sim_status_changed, sim);
	return FALSE;
}

static int ril_sim_probe(struct ofono_sim *sim, unsigned int vendor,
				void *data)
{
	GRil *ril = data;
	struct sim_data *sd;
	int i;

	DBG("");

	sd = g_new0(struct sim_data, 1);
	sd->ril = g_ril_clone(ril);

	for (i = 0; i < OFONO_SIM_PASSWORD_INVALID; i++)
		sd->retries[i] = -1;

	current_sim = sim;

	ofono_sim_set_data(sim, sd);

	/*
	 * TODO: analyze if capability check is needed
	 * and/or timer should be adjusted.
	 *
	 * ofono_sim_register() needs to be called after the
	 * driver has been set in ofono_sim_create(), which
	 * calls this function.  Most other drivers make some
	 * kind of capabilities query to the modem, and then
	 * call register in the callback; we use an idle event
	 * instead.
	 */
	sd->idle_id = g_idle_add(ril_sim_register, sim);

	return 0;
}

static void ril_sim_remove(struct ofono_sim *sim)
{
	DBG("");
	struct sim_data *sd = ofono_sim_get_data(sim);

	ofono_sim_set_data(sim, NULL);

	if (sd->idle_id > 0) {
		g_source_remove(sd->idle_id);
		sd->idle_id = 0;
	}

	g_free(sd->aid_str);
	g_free(sd->app_str);
	g_ril_unref(sd->ril);
	g_free(sd);
}

static struct ofono_sim_driver driver = {
	.name			= "rilmodem",
	.probe			= ril_sim_probe,
	.remove			= ril_sim_remove,
	.read_file_info		= ril_sim_read_info,
	.read_file_transparent	= ril_sim_read_binary,
	.read_file_linear	= ril_sim_read_record,
	.read_file_cyclic	= ril_sim_read_record,
	.read_imsi		= ril_read_imsi,
	.query_passwd_state	= ril_query_passwd_state,
	.send_passwd		= ril_pin_send,
	.lock			= ril_pin_change_state,
	.reset_passwd		= ril_pin_send_puk,
	.change_passwd		= ril_change_passwd,
	.query_pin_retries	= ril_query_pin_retries,
/*
 * TODO: Implementing SIM write file IO support requires
 * the following functions to be defined.
 *
 *	.write_file_transparent	= ril_sim_update_binary,
 *	.write_file_linear	= ril_sim_update_record,
 *	.write_file_cyclic	= ril_sim_update_cyclic,
 */
};

void ril_sim_init(void)
{
	DBG("");
	current_sim = NULL;
	ofono_sim_driver_register(&driver);
}

void ril_sim_exit(void)
{
	ofono_sim_driver_unregister(&driver);
}

struct ofono_sim_driver *get_sim_driver()
{
	return &driver;
}

struct ofono_sim *get_sim()
{
	return current_sim;
}

gint ril_get_app_type()
{
	return current_active_app;
}
