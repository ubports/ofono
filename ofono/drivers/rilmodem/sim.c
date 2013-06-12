/*
 *
 *  oFono - Open Source Telephony - RIL Modem Support
 *
 *  Copyright (C) 2008-2011  Intel Corporation. All rights reserved.
 *  Copyright (C) 2013 Canonical, Ltd. All rights reserved.
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

static char print_buf[PRINT_BUF_SIZE];

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

static void sim_debug(const gchar *str, gpointer user_data)
{
	const char *prefix = user_data;

	ofono_info("%s%s", prefix, str);
}

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
		DBG("Unsupported app_type: 0%x", sd->app_type);
	}

	if (len > 0) {
		hex_path = encode_hex(db_path, len, 0);
		parcel_w_string(rilp, (char *) hex_path);

		DBG("len > 0");

		/* TODO: make conditional */
		ril_append_print_buf("%spath=%s,",
				print_buf,
				hex_path);
		/* TODO: make conditional */

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

		/* TODO: make conditional */
		ril_append_print_buf("%spath=%s,",
				print_buf,
				ROOTMF);
		/* TODO: make conditional */
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
	struct ofono_error error;
	gboolean ok = FALSE;
	int sw1 = 0, sw2 = 0, response_len = 0;
	int flen = 0, rlen = 0, str = 0;
	guchar *response = NULL;
	guchar access[3] = { 0x00, 0x00, 0x00 };
	guchar file_status = EF_STATUS_VALID;

	DBG("");

	if (message->error == RIL_E_SUCCESS) {
		decode_ril_error(&error, "OK");
	} else {
		DBG("Reply failure: %s", ril_error_to_string(message->error));
		decode_ril_error(&error, "FAIL");
		goto error;
	}

	if ((response = (guchar *)
		ril_util_parse_sim_io_rsp(message,
						&sw1,
						&sw2,
						&response_len)) == NULL) {
		DBG("Can't parse SIM IO response from RILD");
		decode_ril_error(&error, "FAIL");
		goto error;
	}

	if ((sw1 != 0x90 && sw1 != 0x91 && sw1 != 0x92 && sw1 != 0x9f) ||
		(sw1 == 0x90 && sw2 != 0x00)) {
		DBG("Error reply, invalid values: sw1: %02x sw2: %02x", sw1, sw2);
		memset(&error, 0, sizeof(error));

		/* TODO: fix decode_ril_error to take type & error */

		error.type = OFONO_ERROR_TYPE_SIM;
		error.error = (sw1 << 8) | sw2;

		goto error;
	}

	if (response_len) {
		g_ril_util_debug_hexdump(FALSE, response, response_len,
						sim_debug, "sim response: ");

		if (response[0] == 0x62) {
			ok = sim_parse_3g_get_response(response, response_len,
							&flen, &rlen, &str, access, NULL);
		} else
			ok = sim_parse_2g_get_response(response, response_len,
							&flen, &rlen, &str, access, &file_status);
	}

	if (!ok) {
		DBG("parse response failed");
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
				const unsigned char *path, unsigned int path_len,
				ofono_sim_file_info_cb_t cb,
				void *data)
{
	struct sim_data *sd = ofono_sim_get_data(sim);
	struct cb_data *cbd = cb_data_new(cb, data);
	struct parcel rilp;
	int ret;

	parcel_init(&rilp);

	parcel_w_int32(&rilp, CMD_GET_RESPONSE);
	parcel_w_int32(&rilp, fileid);

        /* TODO: make conditional */
	ril_start_request;
	ril_append_print_buf("%scmd=0x%.2X,efid=0x%.4X,",
			print_buf,
			CMD_GET_RESPONSE,
			fileid);
	/* TODO: make conditional */

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
	parcel_w_string(&rilp, sd->app_id); /* AID (Application ID) */

	ret = g_ril_send(sd->ril,
				RIL_REQUEST_SIM_IO,
				rilp.data,
				rilp.size,
				ril_file_info_cb, cbd, g_free);

	/* TODO: make conditional */
	ril_append_print_buf("%s%d,%d,%d,%s,pin2=%s,aid=%s",
			print_buf,
			0,
			0,
			15,
			"",
			"",
			sd->app_id);

	ril_close_request;
	ril_print_request(ret, RIL_REQUEST_SIM_IO);
	/* TODO: make conditional */

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
	struct ofono_error error;
	int sw1 = 0, sw2 = 0, response_len = 0;
	guchar *response = NULL;

	DBG("");

	if (message->error == RIL_E_SUCCESS) {
		decode_ril_error(&error, "OK");
	} else {
		DBG("RILD reply failure: %s", ril_error_to_string(message->error));
		goto error;
	}

	if ((response = (guchar *)
		ril_util_parse_sim_io_rsp(message,
						&sw1,
						&sw2,
						&response_len)) == NULL) {
		DBG("Error parsing IO response");
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
				const unsigned char *path, unsigned int path_len,
				ofono_sim_read_cb_t cb, void *data)
{
	struct sim_data *sd = ofono_sim_get_data(sim);
	struct cb_data *cbd = cb_data_new(cb, data);
	struct parcel rilp;
	int ret;

	DBG("fileid: %s (%x) path: %s", sim_fileid_to_string(fileid),
		fileid, path);

        /* TODO: make conditional */
	ril_start_request;
	ril_append_print_buf("%scmd=0x%.2X,efid=0x%.4X,",
			print_buf,
			CMD_READ_BINARY,
			fileid);
	/* TODO: make conditional */

	parcel_init(&rilp);
	parcel_w_int32(&rilp, CMD_READ_BINARY);
	parcel_w_int32(&rilp, fileid);

	set_path(sd, &rilp, fileid, path, path_len);

	parcel_w_int32(&rilp, (start >> 8));   /* P1 */
	parcel_w_int32(&rilp, (start & 0xff)); /* P2 */
	parcel_w_int32(&rilp, length);         /* P3 */
	parcel_w_string(&rilp, NULL);          /* data; only req'd for writes */
	parcel_w_string(&rilp, NULL);          /* pin2; only req'd for writes */
	parcel_w_string(&rilp, sd->app_id);    /* AID (Application ID) */

	ret = g_ril_send(sd->ril,
				RIL_REQUEST_SIM_IO,
				rilp.data,
				rilp.size,
				ril_file_io_cb, cbd, g_free);

	/* TODO: make conditional */
	ril_append_print_buf("%s%d,%d,%d,%s,pin2=%s,aid=%s",
			print_buf,
			(start >> 8),
			(start & 0xff),
			length,
			"",
			"",
			sd->app_id);

	ril_close_request;
	ril_print_request(ret, RIL_REQUEST_SIM_IO);
	/* TODO: make conditional */

	parcel_free(&rilp);

	if (ret <= 0) {
		g_free(cbd);
		CALLBACK_WITH_FAILURE(cb, NULL, 0, data);
	}
}

static void ril_sim_read_record(struct ofono_sim *sim, int fileid,
				int record, int length,
				const unsigned char *path, unsigned int path_len,
				ofono_sim_read_cb_t cb, void *data)
{
	struct sim_data *sd = ofono_sim_get_data(sim);
	struct cb_data *cbd = cb_data_new(cb, data);
	struct parcel rilp;
	int ret;

        /* TODO: make conditional */
	ril_start_request;
	ril_append_print_buf("%scmd=0x%.2X,efid=0x%.4X,",
			print_buf,
			CMD_GET_RESPONSE,
			fileid);
	/* TODO: make conditional */

	parcel_init(&rilp);
	parcel_w_int32(&rilp, CMD_READ_RECORD);
	parcel_w_int32(&rilp, fileid);

	set_path(sd, &rilp, fileid, path, path_len);

	parcel_w_int32(&rilp, record);      /* P1 */
	parcel_w_int32(&rilp, 4);           /* P2 */
	parcel_w_int32(&rilp, length);      /* P3 */
	parcel_w_string(&rilp, NULL);       /* data; only req'd for writes */
	parcel_w_string(&rilp, NULL);       /* pin2; only req'd for writes */
	parcel_w_string(&rilp, sd->app_id); /* AID (Application ID) */

	ret = g_ril_send(sd->ril,
				RIL_REQUEST_SIM_IO,
				rilp.data,
				rilp.size,
				ril_file_io_cb, cbd, g_free);

	/* TODO: make conditional */
	ril_append_print_buf("%s%d,%d,%d,%s,pin2=%s,aid=%s",
			print_buf,
			record,
			4,
			length,
			"",
			"",
			sd->app_id);
	ril_close_request;
	ril_print_request(ret, RIL_REQUEST_SIM_IO);
	/* TODO: make conditional */

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
	struct ofono_error error;
	struct parcel rilp;
	gchar *imsi;

	if (message->error == RIL_E_SUCCESS) {
		DBG("GET IMSI reply - OK");
		decode_ril_error(&error, "OK");
	} else {
		DBG("Reply failure: %s", ril_error_to_string(message->error));
		decode_ril_error(&error, "FAIL");
		cb(&error, NULL, cbd->data);
		return;
	}

	ril_util_init_parcel(message, &rilp);

        /* 15 is the max length of IMSI
	 * add 4 bytes for string length */
        /* FIXME: g_assert(message->buf_len <= 19); */
	imsi = parcel_r_string(&rilp);

	/* TODO: make conditional */
	ril_append_print_buf("[%04d]< %s",
			message->serial_no,
			ril_request_id_to_string(message->req));

	ril_start_response;

	ril_append_print_buf("%s%s",
			print_buf,
			imsi);

	ril_close_response;
	ril_print_response;
	/* TODO: make conditional */

	cb(&error, imsi, cbd->data);
	g_free(imsi);
}

static void ril_read_imsi(struct ofono_sim *sim, ofono_sim_imsi_cb_t cb,
				void *data)
{
	struct sim_data *sd = ofono_sim_get_data(sim);
	struct cb_data *cbd = cb_data_new(cb, data);
	struct parcel rilp;
	int ret;

	DBG("");

	parcel_init(&rilp);
	parcel_w_int32(&rilp, 1);            /* Number of params */
	parcel_w_string(&rilp, sd->app_id);  /* AID (Application ID) */

	ret = g_ril_send(sd->ril, RIL_REQUEST_GET_IMSI,
				rilp.data, rilp.size, ril_imsi_cb, cbd, g_free);

	/* TODO: make conditional */
	ril_start_request;
	ril_append_print_buf("%s%s", print_buf, sd->app_id);
	ril_close_request;
	ril_print_request(ret, RIL_REQUEST_GET_IMSI);
	/* TODO: make conditional */

	parcel_free(&rilp);

	if (ret <= 0) {
		g_free(cbd);
		CALLBACK_WITH_FAILURE(cb, NULL, data);
	}
}

static void sim_status_cb(struct ril_msg *message, gpointer user_data)
{
	struct ofono_sim *sim = user_data;
	struct sim_data *sd = ofono_sim_get_data(sim);
	struct sim_app app;

	DBG("");

	if (ril_util_parse_sim_status(message, &app, sd)) {
		if (app.app_id)
			sd->app_id = app.app_id;

		if (app.app_type != RIL_APPTYPE_UNKNOWN)
			sd->app_type = app.app_type;

		sd->ready = TRUE;

		ofono_sim_register(sim);
	}

	/* TODO: if no SIM present, handle emergency calling. */
}

static int send_get_sim_status(struct ofono_sim *sim)
{
	struct sim_data *sd = ofono_sim_get_data(sim);
	int ret;

	ret = g_ril_send(sd->ril, RIL_REQUEST_GET_SIM_STATUS,
				NULL, 0, sim_status_cb, sim, NULL);

	/* TODO: make conditional */
	ril_clear_print_buf;
	ril_print_request(ret, RIL_REQUEST_GET_SIM_STATUS);
	/* TODO: make conditional */

	return ret;
}

static void ril_query_passwd_state(struct ofono_sim *sim,
				ofono_sim_passwd_cb_t cb, void *data)
{
	struct sim_data *sd = ofono_sim_get_data(sim);

	DBG("passwd_state %u", sd->passwd_state);

	sd->notify_ready = TRUE;

	switch (sd->passwd_state) {
	case OFONO_SIM_PASSWORD_NONE:
		if (sd->ready)
			CALLBACK_WITH_SUCCESS(cb, sd->passwd_state, data);
		else
			CALLBACK_WITH_FAILURE(cb, -1, data);
		break;
	case OFONO_SIM_PASSWORD_INVALID:
		CALLBACK_WITH_FAILURE(cb, -1, data);
		break;
	default:
		CALLBACK_WITH_SUCCESS(cb, sd->passwd_state, data);
		break;
	}
}

static void sim_pin_status_cb(struct ril_msg *message, gpointer user_data)
{
	struct ofono_sim *sim = user_data;
	struct sim_data *sd = ofono_sim_get_data(sim);

	ril_util_parse_sim_status(message, NULL, sd);

	__ofono_sim_recheck_pin(sim);

}

static void ril_pin_change_state_cb(struct ril_msg *message, gpointer user_data)
{
	struct cb_data *cbd = user_data;
	ofono_sim_lock_unlock_cb_t cb = cbd->cb;

	/* There is no reason to ask SIM status until
	 * unsolicited sim status change indication
	 * Looks like state does not change before that.
	*/
	if (message->error == RIL_E_SUCCESS)
		CALLBACK_WITH_SUCCESS(cb, cbd->data);
	else
		CALLBACK_WITH_FAILURE(cb, cbd->data);

}

static void ril_pin_send(struct ofono_sim *sim, const char *passwd,
				ofono_sim_lock_unlock_cb_t cb, void *data)
{
	struct sim_data *sd = ofono_sim_get_data(sim);
	struct cb_data *cbd = cb_data_new(cb, data);
	struct parcel rilp;
	char buf[0x0A + 1];
	int ret;

	parcel_init(&rilp);
	parcel_w_int32(&rilp, 2);			/* Number of params */

	snprintf(buf, sizeof(buf), "%s", passwd);
	parcel_w_string(&rilp, buf);		/* passwd */

	parcel_w_string(&rilp, sd->app_id);	/* AID (Application ID) */

	ret = g_ril_send(sd->ril, RIL_REQUEST_ENTER_SIM_PIN,
				rilp.data, rilp.size, ril_pin_change_state_cb,
				cbd, g_free);

	parcel_free(&rilp);

	if (ret <= 0) {
		g_free(cbd);
		CALLBACK_WITH_FAILURE(cb, data);
	}
}

static void ril_pin_change_state(struct ofono_sim *sim,
		enum ofono_sim_password_type passwd_type,
		int enable, const char *passwd,
		ofono_sim_lock_unlock_cb_t cb, void *data)
{
	struct sim_data *sd = ofono_sim_get_data(sim);
	struct cb_data *cbd = cb_data_new(cb, data);
	struct parcel rilp;
	char buf[0x0A + 1];
	int ret = 0;

	parcel_init(&rilp);
	parcel_w_int32(&rilp, 5);			/* Number of params */

	switch (passwd_type) {
	case OFONO_SIM_PASSWORD_SIM_PIN:
		parcel_w_string(&rilp, "SC");
		break;
	case OFONO_SIM_PASSWORD_PHSIM_PIN:
		parcel_w_string(&rilp, "PS");
		break;
	case OFONO_SIM_PASSWORD_PHFSIM_PIN:
		parcel_w_string(&rilp, "PF");
		break;
	case OFONO_SIM_PASSWORD_SIM_PIN2:
		parcel_w_string(&rilp, "P2");
		break;
	case OFONO_SIM_PASSWORD_PHNET_PIN:
		parcel_w_string(&rilp, "PN");
		break;
	case OFONO_SIM_PASSWORD_PHNETSUB_PIN:
		parcel_w_string(&rilp, "PU");
		break;
	case OFONO_SIM_PASSWORD_PHSP_PIN:
		parcel_w_string(&rilp, "PP");
		break;
	case OFONO_SIM_PASSWORD_PHCORP_PIN:
		parcel_w_string(&rilp, "PC");
		break;
	default:
		ret = -1;
		goto error;
		break;
	}

	if (enable)
		parcel_w_string(&rilp, "1");
	else
		parcel_w_string(&rilp, "0");

	snprintf(buf, sizeof(buf), "%s", passwd);
	parcel_w_string(&rilp, buf);		/* passwd */

	parcel_w_string(&rilp, "0");		/* class */

	parcel_w_string(&rilp, sd->app_id);	/* AID (Application ID) */

	ret = g_ril_send(sd->ril, RIL_REQUEST_SET_FACILITY_LOCK,
				rilp.data, rilp.size, ril_pin_change_state_cb,
				cbd, g_free);

	parcel_free(&rilp);

	return;

error:
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
	char pukbuf[0x0A + 1];
	char passbuf[0x0A + 1];
	int ret = 0;

	parcel_init(&rilp);

	parcel_w_int32(&rilp, 3);			/* Number of params */

	snprintf(pukbuf, sizeof(pukbuf), "%s", puk);

	parcel_w_string(&rilp, pukbuf);		/* puk */

	snprintf(passbuf, sizeof(passbuf), "%s", passwd);

	parcel_w_string(&rilp, passbuf);	/* passwd */

	parcel_w_string(&rilp, sd->app_id);	/* AID (Application ID) */

	ret = g_ril_send(sd->ril, RIL_REQUEST_ENTER_SIM_PUK,
			rilp.data, rilp.size, ril_pin_change_state_cb,
			cbd, g_free);

	parcel_free(&rilp);

	if (ret <= 0) {
		g_free(cbd);
		CALLBACK_WITH_FAILURE(cb, data);
	}
}

static void ril_change_passwd(struct ofono_sim *sim,
				enum ofono_sim_password_type passwd_type,
				const char *old, const char *new,
				ofono_sim_lock_unlock_cb_t cb, void *data)
{
	struct sim_data *sd = ofono_sim_get_data(sim);
	struct cb_data *cbd = cb_data_new(cb, data);
	struct parcel rilp;
	char oldpass[0x0A + 1];
	char newpass[0x0A + 1];
	int ret = 0;
	int id = RIL_REQUEST_CHANGE_SIM_PIN;

	parcel_init(&rilp);

	parcel_w_int32(&rilp, 3);			/* Number of params */

	snprintf(oldpass, sizeof(oldpass), "%s", old);

	parcel_w_string(&rilp, oldpass);	/* puk */

	snprintf(newpass, sizeof(newpass), "%s", new);

	parcel_w_string(&rilp, newpass);	/* passwd */

	parcel_w_string(&rilp, sd->app_id);	/* AID (Application ID) */

	if (passwd_type == OFONO_SIM_PASSWORD_SIM_PIN2)
		id = RIL_REQUEST_CHANGE_SIM_PIN2;

	ret = g_ril_send(sd->ril, id, rilp.data, rilp.size,
			ril_pin_change_state_cb, cbd, g_free);

	parcel_free(&rilp);

	if (ret <= 0) {
		g_free(cbd);
		CALLBACK_WITH_FAILURE(cb, data);
	}
}

static void ril_sim_notify(struct ril_msg *message, gpointer user_data)
{
	struct ofono_sim *sim = user_data;
	struct sim_data *sd = ofono_sim_get_data(sim);

	g_ril_send(sd->ril, RIL_REQUEST_GET_SIM_STATUS,
		NULL, 0, sim_pin_status_cb, sim, NULL);
}

static gboolean ril_sim_register(gpointer user)
{
	struct ofono_sim *sim = user;
	struct sim_data *sd = ofono_sim_get_data(sim);

	DBG("");

	send_get_sim_status(sim);

	g_ril_register(sd->ril, RIL_UNSOL_RESPONSE_SIM_STATUS_CHANGED,
			ril_sim_notify,	sim);

	return FALSE;
}

static int ril_sim_probe(struct ofono_sim *sim, unsigned int vendor,
				void *data)
{
	GRil *ril = data;
	struct sim_data *sd;

	DBG("");

	sd = g_new0(struct sim_data, 1);
	sd->ril = g_ril_clone(ril);
	sd->app_id = NULL;
	sd->app_type = RIL_APPTYPE_UNKNOWN;
	sd->passwd_state = OFONO_SIM_PASSWORD_INVALID;

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
	g_idle_add(ril_sim_register, sim);

	return 0;
}

static void ril_sim_remove(struct ofono_sim *sim)
{
	struct sim_data *sd = ofono_sim_get_data(sim);

	ofono_sim_set_data(sim, NULL);

	g_ril_unref(sd->ril);
	g_free(sd);
}

static struct ofono_sim_driver driver = {
	.name			= "rilmodem",
	.probe                  = ril_sim_probe,
	.remove                 = ril_sim_remove,
	.read_file_info		= ril_sim_read_info,
	.read_file_transparent	= ril_sim_read_binary,
	.read_file_linear	= ril_sim_read_record,
	.read_file_cyclic	= ril_sim_read_record,
 	.read_imsi		= ril_read_imsi,
	.query_passwd_state	= ril_query_passwd_state,
	.send_passwd		= ril_pin_send,
	.lock				= ril_pin_change_state,
	.reset_passwd		= ril_pin_send_puk,
	.change_passwd		= ril_change_passwd,

/*
 * TODO: Implmenting PIN/PUK support requires defining
 * the following driver methods.
 *
 * In the meanwhile, as long as the SIM card is present,
 * and unlocked, the core SIM code will check for the
 * presence of query_passwd_state, and if null, then the
 * function sim_initialize_after_pin() is called.
 *
 *	.query_pin_retries	= ril_pin_retries_query,
 *	.query_locked		= ril_pin_query_enabled,
 *
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
	ofono_sim_driver_register(&driver);
}

void ril_sim_exit(void)
{
	ofono_sim_driver_unregister(&driver);
}
