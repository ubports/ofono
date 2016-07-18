/*
 *  oFono - Open Source Telephony - RIL-based devices
 *
 *  Copyright (C) 2015-2016 Jolla Ltd.
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
#include "ril_sim_card.h"
#include "ril_util.h"
#include "ril_log.h"

#include "simutil.h"
#include "util.h"
#include "ofono.h"

#define SIM_STATE_CHANGE_TIMEOUT_SECS (5)

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

/* P2 coding (modes) for READ RECORD and UPDATE RECORD (see TS 102.221) */
#define MODE_SELECTED (0x00) /* Currently selected EF */
#define MODE_CURRENT  (0x04) /* P1='00' denotes the current record */
#define MODE_ABSOLUTE (0x04) /* The record number is given in P1 */
#define MODE_NEXT     (0x02) /* Next record */
#define MODE_PREVIOUS (0x03) /* Previous record */

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
	GList *pin_cbd_list;
	struct ofono_sim *sim;
	struct ril_sim_card *card;
	enum ofono_sim_password_type ofono_passwd_state;
	int retries[OFONO_SIM_PASSWORD_INVALID];
	gboolean empty_pin_query_allowed;
	gboolean inserted;
	guint idle_id;
	gulong card_status_id;

	const char *log_prefix;
	char *allocated_log_prefix;

	/* query_passwd_state context */
	ofono_sim_passwd_cb_t query_passwd_state_cb;
	void *query_passwd_state_cb_data;
	guint query_passwd_state_timeout_id;
};

struct ril_sim_io_response {
	guint sw1, sw2;
	guchar* data;
	guint data_len;
};

struct ril_sim_cbd {
	struct ril_sim *sd;
	union _ofono_sim_cb {
		ofono_sim_file_info_cb_t file_info;
		ofono_sim_read_cb_t read;
		ofono_sim_write_cb_t write;
		ofono_sim_imsi_cb_t imsi;
		ofono_sim_pin_retries_cb_t retries;
		ofono_query_facility_lock_cb_t query_facility_lock;
		gpointer ptr;
	} cb;
	gpointer data;
};

struct ril_sim_pin_cbd {
	struct ril_sim *sd;
	ofono_sim_lock_unlock_cb_t cb;
	gpointer data;
	struct ril_sim_card *card;
	enum ofono_sim_password_type passwd_type;
	int ril_status;
	guint state_event_count;
	guint timeout_id;
	gulong card_status_id;
};

#define DBG_(sd,fmt,args...) DBG("%s" fmt, (sd)->log_prefix, ##args)

#define ril_sim_cbd_free g_free

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

static void ril_sim_pin_cbd_state_event_count_cb(struct ril_sim_card *sc,
							void *user_data)
{
	struct ril_sim_pin_cbd *cbd = user_data;

	/* Cound the SIM status events received while request is pending
	 * so that ril_sim_pin_change_state_cb can decide whether to wait
	 * for the next event or not */
	cbd->state_event_count++;
}

static struct ril_sim_pin_cbd *ril_sim_pin_cbd_new(struct ril_sim *sd,
			enum ofono_sim_password_type passwd_type,
			gboolean state_change_expected,
			ofono_sim_lock_unlock_cb_t cb, void *data)
{
	struct ril_sim_pin_cbd *cbd = g_new0(struct ril_sim_pin_cbd, 1);

	cbd->sd = sd;
	cbd->cb = cb;
	cbd->data = data;
	cbd->passwd_type = passwd_type;
	cbd->card = ril_sim_card_ref(sd->card);
	if (state_change_expected) {
		cbd->card_status_id =
			ril_sim_card_add_status_received_handler(sd->card,
				ril_sim_pin_cbd_state_event_count_cb, cbd);
	}
	return cbd;
}

static void ril_sim_pin_cbd_free(struct ril_sim_pin_cbd *cbd)
{
	if (cbd->timeout_id) {
		g_source_remove(cbd->timeout_id);
	}

	ril_sim_card_remove_handler(cbd->card, cbd->card_status_id);
	ril_sim_card_unref(cbd->card);
	g_free(cbd);
}

static void ril_sim_pin_cbd_list_free_cb(gpointer data)
{
	ril_sim_pin_cbd_free((struct ril_sim_pin_cbd *)data);
}

static void ril_sim_pin_req_done(gpointer ptr)
{
	struct ril_sim_pin_cbd *cbd = ptr;

	/* Only free if callback isn't waiting for something else to happen */
	if (!cbd->timeout_id) {
		GASSERT(!cbd->card_status_id);
		ril_sim_pin_cbd_free(cbd);
	}
}

static const char *ril_sim_app_id(struct ril_sim *sd)
{
	return sd->card->app ? sd->card->app->aid : NULL;
}

int ril_sim_app_type(struct ofono_sim *sim)
{
	struct ril_sim *sd = ril_sim_get_data(sim);
	return sd ? ril_sim_card_app_type(sd->card) : RIL_APPTYPE_UNKNOWN;
}

static void ril_sim_append_path(struct ril_sim *sd, GRilIoRequest *req,
		const int fileid, const guchar *path, const guint path_len)
{
	const enum ril_app_type app_type = ril_sim_card_app_type(sd->card);
	guchar db_path[6] = { 0x00 };
	char *hex_path = NULL;
	int len;

	if (path_len > 0 && path_len < 7) {
		memcpy(db_path, path, path_len);
		len = path_len;
	} else if (app_type == RIL_APPTYPE_USIM) {
		len = sim_ef_db_get_path_3g(fileid, db_path);
	} else if (app_type == RIL_APPTYPE_SIM) {
		len = sim_ef_db_get_path_2g(fileid, db_path);
	} else {
		ofono_error("Unsupported app type %d", app_type);
		len = 0;
	}

	if (len > 0) {
		hex_path = encode_hex(db_path, len, 0);
		grilio_request_append_utf8(req, hex_path);
		DBG_(sd, "%s", hex_path);
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
		DBG_(sd, "%s", ROOTMF);
		grilio_request_append_utf8(req, ROOTMF);
	} else {
		/*
		 * The only known case of this is EFPHASE_FILED (0x6FAE).
		 * The ef_db table ( see /src/simutil.c ) entry for
		 * EFPHASE contains a value of 0x0000 for it's
		 * 'parent3g' member.  This causes a NULL path to
		 * be returned.
		 */

		DBG_(sd, "returning empty path.");
		grilio_request_append_utf8(req, NULL);
	}
}

static struct ril_sim_io_response *ril_sim_parse_io_response(const void *data,
								guint len)
{
	struct ril_sim_io_response *res = NULL;
	GRilIoParser rilp;
	int sw1, sw2;

	grilio_parser_init(&rilp, data, len);

	if (grilio_parser_get_int32(&rilp, &sw1) &&
			grilio_parser_get_int32(&rilp, &sw2)) {
		char *hex_data = grilio_parser_get_utf8(&rilp);

		DBG("sw1=0x%02X,sw2=0x%02X,%s", sw1, sw2, hex_data);
		res = g_slice_new0(struct ril_sim_io_response);
		res->sw1 = sw1;
		res->sw2 = sw2;
		if (hex_data) {
			long num_bytes = 0;
			res->data = decode_hex(hex_data, -1, &num_bytes, 0);
			res->data_len = num_bytes;
			g_free(hex_data);
		}
	}

	return res;
}

static gboolean ril_sim_io_response_ok(const struct ril_sim_io_response *res)
{
	if (res) {
		static const struct ril_sim_io_error {
			int sw;
			const char* msg;
		} errmsg [] = {
			/* TS 102.221 */
			{ 0x6a80, "Incorrect parameters in the data field" },
			{ 0x6a81, "Function not supported" },
			{ 0x6a82, "File not found" },
			{ 0x6a83, "Record not found" },
			{ 0x6a84, "Not enough memory space" },
			{ 0x6a86, "Incorrect parameters P1 to P2" },
			{ 0x6a87, "Lc inconsistent with P1 to P2" },
			{ 0x6a88, "Referenced data not found" },
			/* TS 51.011 */
			{ 0x9240, "Memory problem" },
			{ 0x9400, "No EF selected" },
			{ 0x9402, "Out of range (invalid address)" },
			{ 0x9404, "File id/pattern not found" },
			{ 0x9408, "File is inconsistent with the command" }
		};

		int low, high, sw;

		switch (res->sw1) {
		case 0x90:
			/* '90 00' is the normal completion */
			if (res->sw2 != 0x00) {
				break;
			}
			/* fall through */
		case 0x91:
		case 0x9e:
		case 0x9f:
			return TRUE;
		case 0x92:
			if (res->sw2 != 0x40) {
				/* '92 40' is "memory problem" */
				return TRUE;
			}
			break;
		default:
			break;
		}

		/* Find the error message */
		low = 0;
		high = G_N_ELEMENTS(errmsg)-1;
		sw = (res->sw1 << 8) | res->sw2;

		while (low <= high) {
			const int mid = (low + high)/2;
			const int val = errmsg[mid].sw;
			if (val < sw) {
				low = mid + 1;
			} else if (val > sw) {
				high = mid - 1;
			} else {
				/* Message found */
				DBG("error: %s", errmsg[mid].msg);
				return FALSE;
			}
		}

		/* No message */
		DBG("error %02x %02x", res->sw1, res->sw2);
	}
	return FALSE;
}

static void ril_sim_io_response_free(struct ril_sim_io_response *res)
{
	if (res) {
		g_free(res->data);
		g_slice_free(struct ril_sim_io_response, res);
	}
}

static void ril_sim_file_info_cb(GRilIoChannel *io, int status,
			const void *data, guint len, void *user_data)
{
	struct ril_sim_cbd *cbd = user_data;
	ofono_sim_file_info_cb_t cb = cbd->cb.file_info;
	struct ril_sim *sd = cbd->sd;
	struct ril_sim_io_response *res = NULL;
	struct ofono_error error;

	DBG_(sd, "");


	/*
	 * In case sim card has been removed prior to this callback has been
	 * called we must not call the core call back method as otherwise the
	 * core will crash.
	 */
	if (!sd->inserted) {
		ofono_error("No SIM card");
		return;
	}

	ril_error_init_failure(&error);
	res = ril_sim_parse_io_response(data, len);
	if (ril_sim_io_response_ok(res) && status == RIL_E_SUCCESS) {
		gboolean ok = FALSE;
		guchar access[3] = { 0x00, 0x00, 0x00 };
		guchar file_status = EF_STATUS_VALID;
		int flen = 0, rlen = 0, str = 0;

		if (res->data_len) {
			if (res->data[0] == 0x62) {
				ok = sim_parse_3g_get_response(res->data,
					res->data_len, &flen, &rlen, &str,
					access, NULL);
			} else {
				ok = sim_parse_2g_get_response(res->data,
					res->data_len, &flen, &rlen, &str,
					access, &file_status);
			}
		}

		if (ok) {
			/* Success */
			cb(ril_error_ok(&error), flen, str, rlen, access,
				file_status, cbd->data);
			ril_sim_io_response_free(res);
			return;
		} else {
			ofono_error("file info parse error");
		}
	} else if (res) {
		ril_error_init_sim_error(&error, res->sw1, res->sw2);
	}

	cb(&error, -1, -1, -1, NULL, EF_STATUS_INVALIDATED, cbd->data);
	ril_sim_io_response_free(res);
}

static void ril_sim_request_io(struct ril_sim *sd, guint cmd, int fileid,
		guint p1, guint p2, guint p3, const char *hex_data,
		const guchar *path, guint path_len,
		GRilIoChannelResponseFunc cb, struct ril_sim_cbd *cbd)
{
	GRilIoRequest *req = grilio_request_new();

	DBG_(sd, "cmd=0x%.2X,efid=0x%.4X,%d,%d,%d,%s,pin2=(null),aid=%s",
			cmd, fileid, p1, p2, p3, hex_data, ril_sim_app_id(sd));

	grilio_request_append_int32(req, cmd);
	grilio_request_append_int32(req, fileid);
	ril_sim_append_path(sd, req, fileid, path, path_len);
	grilio_request_append_int32(req, p1);       /* P1 */
	grilio_request_append_int32(req, p2);       /* P2 */
	grilio_request_append_int32(req, p3);       /* P3 */
	grilio_request_append_utf8(req, hex_data);  /* data; only for writes */
	grilio_request_append_utf8(req, NULL);      /* pin2; only for writes */
	grilio_request_append_utf8(req, ril_sim_app_id(sd));

	grilio_queue_send_request_full(sd->q, req, RIL_REQUEST_SIM_IO,
					cb, ril_sim_cbd_free, cbd);
	grilio_request_unref(req);
}

static void ril_sim_ofono_read_file_info(struct ofono_sim *sim, int fileid,
		const unsigned char *path, unsigned int len,
		ofono_sim_file_info_cb_t cb, void *data)
{
	struct ril_sim *sd = ril_sim_get_data(sim);
	ril_sim_request_io(sd, CMD_GET_RESPONSE, fileid, 0, 0, 15, NULL,
		path, len, ril_sim_file_info_cb, ril_sim_cbd_new(sd, cb, data));
}

static void ril_sim_read_cb(GRilIoChannel *io, int status,
				const void *data, guint len, void *user_data)
{
	struct ril_sim_cbd *cbd = user_data;
	ofono_sim_read_cb_t cb = cbd->cb.read;
	struct ril_sim_io_response *res;
	struct ofono_error err;

	DBG_(cbd->sd, "");
	res = ril_sim_parse_io_response(data, len);
	if (ril_sim_io_response_ok(res) && status == RIL_E_SUCCESS) {
		cb(ril_error_ok(&err), res->data, res->data_len, cbd->data);
	} else if (res) {
		cb(ril_error_sim(&err, res->sw1, res->sw2), NULL, 0, cbd->data);
	} else {
		cb(ril_error_failure(&err), NULL, 0, cbd->data);
	}
	ril_sim_io_response_free(res);
}

static void ril_sim_read(struct ofono_sim *sim, guint cmd, int fileid,
		guint p1, guint p2, guint p3, const guchar *path,
		guint path_len, ofono_sim_read_cb_t cb, void *data)
{
	struct ril_sim *sd = ril_sim_get_data(sim);
	ril_sim_request_io(sd, cmd, fileid, p1, p2, p3, NULL, path, path_len,
			ril_sim_read_cb, ril_sim_cbd_new(sd, cb, data));
}

static void ril_sim_ofono_read_file_transparent(struct ofono_sim *sim,
		int fileid, int start, int length, const unsigned char *path,
		unsigned int path_len, ofono_sim_read_cb_t cb, void *data)
{
	ril_sim_read(sim, CMD_READ_BINARY, fileid, (start >> 8), (start & 0xff),
					length, path, path_len, cb, data);
}

static void ril_sim_ofono_read_file_linear(struct ofono_sim *sim, int fileid,
		int record, int length, const unsigned char *path,
		unsigned int path_len, ofono_sim_read_cb_t cb, void *data)
{
	ril_sim_read(sim, CMD_READ_RECORD, fileid, record, MODE_ABSOLUTE,
					length, path, path_len, cb, data);
}

static void ril_sim_ofono_read_file_cyclic(struct ofono_sim *sim, int fileid,
		int record, int length, const unsigned char *path,
		unsigned int path_len, ofono_sim_read_cb_t cb, void *data)
{
	ril_sim_read(sim, CMD_READ_RECORD, fileid, record, MODE_ABSOLUTE,
					length, path, path_len, cb, data);
}

static void ril_sim_write_cb(GRilIoChannel *io, int status,
				const void *data, guint len, void *user_data)
{
	struct ril_sim_cbd *cbd = user_data;
	ofono_sim_write_cb_t cb = cbd->cb.write;
	struct ril_sim_io_response *res;
	struct ofono_error err;

	DBG_(cbd->sd, "");
	res = ril_sim_parse_io_response(data, len);
	if (ril_sim_io_response_ok(res) && status == RIL_E_SUCCESS) {
		cb(ril_error_ok(&err), cbd->data);
	} else if (res) {
		cb(ril_error_sim(&err, res->sw1, res->sw2), cbd->data);
	} else {
		cb(ril_error_failure(&err), cbd->data);
	}
	ril_sim_io_response_free(res);
}

static void ril_sim_write(struct ofono_sim *sim, guint cmd, int fileid,
			guint p1, guint p2, guint length, const void *value,
			const guchar *path, guint path_len,
			ofono_sim_write_cb_t cb, void *data)
{
	struct ril_sim *sd = ril_sim_get_data(sim);
	char *hex_data = encode_hex(value, length, 0);
	ril_sim_request_io(sd, cmd, fileid, p1, p2, length, hex_data, path,
		path_len, ril_sim_write_cb, ril_sim_cbd_new(sd, cb, data));
	g_free(hex_data);
}

static void ril_sim_write_file_transparent(struct ofono_sim *sim, int fileid,
			int start, int length, const unsigned char *value,
			const unsigned char *path, unsigned int path_len,
			ofono_sim_write_cb_t cb, void *data)
{
	ril_sim_write(sim, CMD_UPDATE_BINARY, fileid,
				(start >> 8), (start & 0xff), length, value,
				path, path_len, cb, data);
}

static void ril_sim_write_file_linear(struct ofono_sim *sim, int fileid,
			int record, int length, const unsigned char *value,
			const unsigned char *path, unsigned int path_len,
			ofono_sim_write_cb_t cb, void *data)
{
	ril_sim_write(sim, CMD_UPDATE_RECORD, fileid,
				record, MODE_ABSOLUTE, length, value,
				path, path_len, cb, data);
}

static void ril_sim_write_file_cyclic(struct ofono_sim *sim, int fileid,
			int length, const unsigned char *value,
			const unsigned char *path, unsigned int path_len,
			ofono_sim_write_cb_t cb, void *data)
{
	ril_sim_write(sim, CMD_UPDATE_RECORD, fileid,
				0, MODE_PREVIOUS, length, value,
				path, path_len, cb, data);
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
		DBG_(cbd->sd, "%s", imsi);
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

	DBG_(sd, "%s", ril_sim_app_id(sd));
	grilio_request_append_int32(req, GET_IMSI_NUM_PARAMS);
	grilio_request_append_utf8(req, ril_sim_app_id(sd));

	/*
	 * If we fail the .read_imsi call, ofono gets into "Unable to
	 * read IMSI, emergency calls only" state. Retry the request
	 * on failure.
	 */
	grilio_request_set_retry(req, RIL_RETRY_MS, -1);
	grilio_queue_send_request_full(sd->q, req, RIL_REQUEST_GET_IMSI,
				ril_sim_get_imsi_cb, ril_sim_cbd_free,
				ril_sim_cbd_new(sd, cb, data));
	grilio_request_unref(req);
}

static enum ofono_sim_password_type ril_sim_passwd_state(struct ril_sim *sd)
{
	const struct ril_sim_card_app *app = sd->card->app;
	if (app) {
		switch (app->app_state) {
		case RIL_APPSTATE_PIN:
			return OFONO_SIM_PASSWORD_SIM_PIN;
		case RIL_APPSTATE_PUK:
			return OFONO_SIM_PASSWORD_SIM_PUK;
		case RIL_APPSTATE_READY:
			return OFONO_SIM_PASSWORD_NONE;
		case RIL_APPSTATE_SUBSCRIPTION_PERSO:
			switch (app->perso_substate) {
			case RIL_PERSOSUBSTATE_READY:
				return OFONO_SIM_PASSWORD_NONE;
			case RIL_PERSOSUBSTATE_SIM_NETWORK:
				return OFONO_SIM_PASSWORD_PHNET_PIN;
			case RIL_PERSOSUBSTATE_SIM_NETWORK_SUBSET:
				return OFONO_SIM_PASSWORD_PHNETSUB_PIN;
			case RIL_PERSOSUBSTATE_SIM_CORPORATE:
				return OFONO_SIM_PASSWORD_PHCORP_PIN;
			case RIL_PERSOSUBSTATE_SIM_SERVICE_PROVIDER:
				return OFONO_SIM_PASSWORD_PHSP_PIN;
			case RIL_PERSOSUBSTATE_SIM_SIM:
				return OFONO_SIM_PASSWORD_PHSIM_PIN;
			case RIL_PERSOSUBSTATE_SIM_NETWORK_PUK:
				return OFONO_SIM_PASSWORD_PHNET_PUK;
			case RIL_PERSOSUBSTATE_SIM_NETWORK_SUBSET_PUK:
				return OFONO_SIM_PASSWORD_PHNETSUB_PUK;
			case RIL_PERSOSUBSTATE_SIM_CORPORATE_PUK:
				return OFONO_SIM_PASSWORD_PHCORP_PUK;
			case RIL_PERSOSUBSTATE_SIM_SERVICE_PROVIDER_PUK:
				return OFONO_SIM_PASSWORD_PHSP_PUK;
			case RIL_PERSOSUBSTATE_SIM_SIM_PUK:
				return OFONO_SIM_PASSWORD_PHFSIM_PUK;
			default:
				break;
			}
		default:
			break;
		}
	}
	return OFONO_SIM_PASSWORD_INVALID;
}

static gboolean ril_sim_app_in_transient_state(struct ril_sim *sd)
{
	const struct ril_sim_card_app *app = sd->card->app;
	if (app) {
		switch (app->app_state) {
		case RIL_APPSTATE_DETECTED:
			return TRUE;
		case RIL_APPSTATE_SUBSCRIPTION_PERSO:
			switch (app->perso_substate) {
			case RIL_PERSOSUBSTATE_UNKNOWN:
			case RIL_PERSOSUBSTATE_IN_PROGRESS:
				return TRUE;
			default:
				break;
			}
		default:
			break;
		}
	}
	return FALSE;
}

static void ril_sim_finish_passwd_state_query(struct ril_sim *sd,
					enum ofono_sim_password_type state)
{
	if (sd->query_passwd_state_timeout_id) {
		g_source_remove(sd->query_passwd_state_timeout_id);
		sd->query_passwd_state_timeout_id = 0;
	}

	if (sd->query_passwd_state_cb) {
		ofono_sim_passwd_cb_t cb = sd->query_passwd_state_cb;
		void *data = sd->query_passwd_state_cb_data;
		struct ofono_error error;

		sd->query_passwd_state_cb = NULL;
		sd->query_passwd_state_cb_data = NULL;

		error.error = 0;
		error.type = (state == OFONO_SIM_PASSWORD_INVALID) ?
			OFONO_ERROR_TYPE_FAILURE :
			OFONO_ERROR_TYPE_NO_ERROR;

		sd->ofono_passwd_state = state;
		cb(&error, state, data);
	}
}

static void ril_sim_invalidate_passwd_state(struct ril_sim *sd)
{
	guint i;

	sd->ofono_passwd_state = OFONO_SIM_PASSWORD_INVALID;
	for (i = 0; i < OFONO_SIM_PASSWORD_INVALID; i++) {
		sd->retries[i] = -1;
	}

	ril_sim_finish_passwd_state_query(sd, OFONO_SIM_PASSWORD_INVALID);
}

static void ril_sim_status_cb(struct ril_sim_card *sc, void *user_data)
{
	struct ril_sim *sd = user_data;

	GASSERT(sd->card == sc);
	if (sc->status && sc->status->card_state == RIL_CARDSTATE_PRESENT) {
		if (sc->app) {
			enum ofono_sim_password_type ps;

			if (!sd->inserted) {
				sd->inserted = TRUE;
				ofono_info("SIM card OK");
				ofono_sim_inserted_notify(sd->sim, TRUE);
			}

			ps = ril_sim_passwd_state(sd);
			if (ps != OFONO_SIM_PASSWORD_INVALID) {
				ril_sim_finish_passwd_state_query(sd, ps);
			}
		} else {
			ril_sim_invalidate_passwd_state(sd);
		}
	} else {
		ril_sim_invalidate_passwd_state(sd);
		if (sd->inserted) {
			sd->inserted = FALSE;
			ofono_info("No SIM card");
			ofono_sim_inserted_notify(sd->sim, FALSE);
		}
	}
}

static int ril_sim_parse_retry_count(const void *data, guint len)
{
	int retry_count = -1;
	GRilIoParser rilp;

	grilio_parser_init(&rilp, data, len);
	grilio_parser_get_int32(&rilp, NULL);
	grilio_parser_get_int32(&rilp, &retry_count);
	return retry_count;
}

static GRilIoRequest *ril_sim_enter_sim_req(struct ril_sim *sd, const char *pw)
{
	const char *app_id = ril_sim_app_id(sd);
	if (app_id) {
		GRilIoRequest *req = grilio_request_new();
		grilio_request_append_int32(req, ENTER_SIM_PIN_PARAMS);
		grilio_request_append_utf8(req, pw);
		grilio_request_append_utf8(req, app_id);
		return req;
	}
	return NULL;
}

/*
 * Some RIL implementations allow to query the retry count
 * by sending the empty pin in any state.
 */
static void ril_sim_query_pin2_retries_cb(GRilIoChannel *io, int status,
				const void *data, guint len, void *user_data)
{
	struct ril_sim_cbd *cbd = user_data;
	struct ril_sim *sd = cbd->sd;
	ofono_sim_pin_retries_cb_t cb = cbd->cb.retries;
	struct ofono_error error;

	if (status == RIL_E_SUCCESS) {
		const int retry_count = ril_sim_parse_retry_count(data, len);
		DBG_(sd, "pin2 retry_count=%d", retry_count);
		sd->retries[OFONO_SIM_PASSWORD_SIM_PIN2] = retry_count;
	} else {
		ofono_error("pin2 retry query is not supported");
		sd->empty_pin_query_allowed = FALSE;
	}

	cb(ril_error_ok(&error), sd->retries, cbd->data);
}

static gboolean ril_sim_query_pin2_retry_count(struct ril_sim *sd,
				ofono_sim_pin_retries_cb_t cb, void *data)
{
	if (sd->empty_pin_query_allowed &&
				sd->retries[OFONO_SIM_PASSWORD_SIM_PIN2] < 0) {
		GRilIoRequest *req = ril_sim_enter_sim_req(sd, "");
		if (req) {
			DBG_(sd, "querying pin2 retry count...");
			grilio_queue_send_request_full(sd->q, req,
				RIL_REQUEST_ENTER_SIM_PIN2,
				ril_sim_query_pin2_retries_cb,
				ril_sim_cbd_free,
				ril_sim_cbd_new(sd, cb, data));
			grilio_request_unref(req);
			return TRUE;
		}
	}
	return FALSE;
}

static void ril_sim_query_pin_retries_cb(GRilIoChannel *io, int status,
				const void *data, guint len, void *user_data)
{
	struct ril_sim_cbd *cbd = user_data;
	struct ril_sim *sd = cbd->sd;
	ofono_sim_pin_retries_cb_t cb = cbd->cb.retries;
	struct ofono_error error;

	if (status == RIL_E_SUCCESS) {
		const int retry_count = ril_sim_parse_retry_count(data, len);
		DBG_(sd, "pin retry_count=%d", retry_count);
		sd->retries[OFONO_SIM_PASSWORD_SIM_PIN] = retry_count;
		if (ril_sim_query_pin2_retry_count(sd, cb, cbd->data)) {
			/*
			 * ril_sim_query_pin2_retries_cb will invoke
			 * the completion callback
			 */
			return;
		}
	} else {
		ofono_error("pin retry query is not supported");
		sd->empty_pin_query_allowed = FALSE;
	}

	cb(ril_error_ok(&error), sd->retries, cbd->data);
}

static gboolean ril_sim_query_pin_retry_count(struct ril_sim *sd,
				ofono_sim_pin_retries_cb_t cb, void *data)
{
	if (sd->empty_pin_query_allowed &&
				sd->retries[OFONO_SIM_PASSWORD_SIM_PIN] < 0) {
		GRilIoRequest *req = ril_sim_enter_sim_req(sd, "");
		if (req) {
			DBG_(sd, "querying pin retry count...");
			grilio_queue_send_request_full(sd->q, req,
				RIL_REQUEST_ENTER_SIM_PIN,
				ril_sim_query_pin_retries_cb,
				ril_sim_cbd_free,
				ril_sim_cbd_new(sd, cb, data));
			grilio_request_unref(req);
			return TRUE;
		}
	}
	return FALSE;
}

static void ril_sim_query_pin_retries(struct ofono_sim *sim,
				ofono_sim_pin_retries_cb_t cb, void *data)
{
	struct ofono_error error;
	struct ril_sim *sd = ril_sim_get_data(sim);

	DBG_(sd, "");
	if (ril_sim_query_pin_retry_count(sd, cb, data) ||
			ril_sim_query_pin2_retry_count(sd, cb, data)) {
		/* Wait for completion of PIN and then PIN2 query */
		return;
	}

	cb(ril_error_ok(&error), sd->retries, data);
}

static gboolean ril_sim_query_passwd_state_timeout_cb(gpointer user_data)
{
	struct ril_sim *sd = user_data;

	GASSERT(sd->query_passwd_state_cb);
	sd->query_passwd_state_timeout_id = 0;
	ril_sim_finish_passwd_state_query(sd, OFONO_SIM_PASSWORD_INVALID);

	return G_SOURCE_REMOVE;
}

static void ril_sim_query_passwd_state(struct ofono_sim *sim,
					ofono_sim_passwd_cb_t cb, void *data)
{
	struct ril_sim *sd = ril_sim_get_data(sim);
	enum ofono_sim_password_type passwd_state = ril_sim_passwd_state(sd);
	struct ofono_error error;

	if (sd->query_passwd_state_timeout_id) {
		g_source_remove(sd->query_passwd_state_timeout_id);
		sd->query_passwd_state_timeout_id = 0;
	}

	if (passwd_state != OFONO_SIM_PASSWORD_INVALID) {
		DBG_(sd, "%d", passwd_state);
		sd->query_passwd_state_cb = NULL;
		sd->query_passwd_state_cb_data = NULL;
		sd->ofono_passwd_state = passwd_state;
		cb(ril_error_ok(&error), passwd_state, data);
	} else {
		/* Wait for the state to change */
		DBG_(sd, "waiting for the SIM state to change");
		sd->query_passwd_state_cb = cb;
		sd->query_passwd_state_cb_data = data;
		sd->query_passwd_state_timeout_id =
			g_timeout_add_seconds(SIM_STATE_CHANGE_TIMEOUT_SECS,
				ril_sim_query_passwd_state_timeout_cb, sd);
	}
}

static gboolean ril_sim_pin_change_state_timeout_cb(gpointer user_data)
{
	struct ril_sim_pin_cbd *cbd = user_data;
	struct ril_sim *sd = cbd->sd;
	struct ofono_error error;

	DBG_(sd, "oops...");
	cbd->timeout_id = 0;
	sd->pin_cbd_list = g_list_remove(sd->pin_cbd_list, cbd);
	cbd->cb(ril_error_failure(&error), cbd->data);
	ril_sim_pin_cbd_free(cbd);

	return G_SOURCE_REMOVE;
}

static void ril_sim_pin_change_state_status_cb(struct ril_sim_card *sc,
							void *user_data)
{
	struct ril_sim_pin_cbd *cbd = user_data;
	struct ril_sim *sd = cbd->sd;

	if (!ril_sim_app_in_transient_state(sd)) {
		struct ofono_error error;
		enum ofono_sim_password_type ps = ril_sim_passwd_state(sd);

		if (ps == OFONO_SIM_PASSWORD_INVALID ||
				cbd->ril_status != RIL_E_SUCCESS) {
			DBG_(sd, "failure");
			cbd->cb(ril_error_failure(&error), cbd->data);
		} else {
			DBG_(sd, "success, passwd_state=%d", ps);
			cbd->cb(ril_error_ok(&error), cbd->data);
		}

		sd->pin_cbd_list = g_list_remove(sd->pin_cbd_list, cbd);
		ril_sim_pin_cbd_free(cbd);
	} else {
		DBG_(sd, "will keep waiting");
	}
}

static void ril_sim_pin_change_state_cb(GRilIoChannel *io, int ril_status,
				const void *data, guint len, void *user_data)
{
	struct ril_sim_pin_cbd *cbd = user_data;
	struct ril_sim *sd = cbd->sd;
	const int retry_count = ril_sim_parse_retry_count(data, len);

	DBG_(sd, "result=%d passwd_type=%d retry_count=%d",
			ril_status, cbd->passwd_type, retry_count);

	if (ril_status == RIL_E_SUCCESS && retry_count == 0 &&
			sd->empty_pin_query_allowed &&
			(cbd->passwd_type == OFONO_SIM_PASSWORD_SIM_PIN ||
			cbd->passwd_type == OFONO_SIM_PASSWORD_SIM_PIN2)) {
		/* Will query it */
		sd->retries[cbd->passwd_type] = -1;
	} else {
		sd->retries[cbd->passwd_type] = retry_count;
	}

	cbd->ril_status = ril_status;
	if (cbd->card_status_id && (!cbd->state_event_count ||
					ril_sim_app_in_transient_state(sd))) {

		GASSERT(!g_list_find(sd->pin_cbd_list, cbd));
		GASSERT(!cbd->timeout_id);

		/* Wait for rild to change the state */
		DBG_(sd, "waiting for SIM state change");
		sd->pin_cbd_list = g_list_append(sd->pin_cbd_list, cbd);
		cbd->timeout_id =
			g_timeout_add_seconds(SIM_STATE_CHANGE_TIMEOUT_SECS,
				ril_sim_pin_change_state_timeout_cb, cbd);

		/* We no longer need to maintain state_event_count,
		 * replace the SIM state event handler */
		ril_sim_card_remove_handler(cbd->card, cbd->card_status_id);
		cbd->card_status_id =
			ril_sim_card_add_status_received_handler(sd->card,
				ril_sim_pin_change_state_status_cb, cbd);
	} else {
		struct ofono_error error;

		/* It's either already changed or not expected at all */
		if (ril_status == RIL_E_SUCCESS) {
			cbd->cb(ril_error_ok(&error), cbd->data);
		} else {
			cbd->cb(ril_error_failure(&error), cbd->data);
		}
	}
}

static void ril_sim_pin_send(struct ofono_sim *sim, const char *passwd,
				ofono_sim_lock_unlock_cb_t cb, void *data)
{
	struct ril_sim *sd = ril_sim_get_data(sim);
	GRilIoRequest *req = grilio_request_new();

	grilio_request_append_int32(req, ENTER_SIM_PIN_PARAMS);
	grilio_request_append_utf8(req, passwd);
	grilio_request_append_utf8(req, ril_sim_app_id(sd));

	DBG_(sd, "%s,aid=%s", passwd, ril_sim_app_id(sd));
	grilio_queue_send_request_full(sd->q, req, RIL_REQUEST_ENTER_SIM_PIN,
		ril_sim_pin_change_state_cb, ril_sim_pin_req_done,
		ril_sim_pin_cbd_new(sd, OFONO_SIM_PASSWORD_SIM_PIN,
						TRUE, cb, data));
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
			DBG_(sd, "Not supported, enable=%d", enable);
		}
		break;
	default:
		DBG_(sd, "Not supported, type=%d", passwd_type);
		break;
	}

	if (req) {
		id = grilio_queue_send_request_full(sd->q, req, code,
			ril_sim_pin_change_state_cb, ril_sim_pin_req_done,
			ril_sim_pin_cbd_new(sd, passwd_type, TRUE, cb, data));
		grilio_request_unref(req);
	}

	return id;
}

static const char *ril_sim_facility_code(enum ofono_sim_password_type type)
{
	switch (type) {
	case OFONO_SIM_PASSWORD_SIM_PIN:
		return "SC";
	case OFONO_SIM_PASSWORD_SIM_PIN2:
		return "P2";
	case OFONO_SIM_PASSWORD_PHSIM_PIN:
		return "PS";
	case OFONO_SIM_PASSWORD_PHFSIM_PIN:
		return "PF";
	case OFONO_SIM_PASSWORD_PHNET_PIN:
		return "PN";
	case OFONO_SIM_PASSWORD_PHNETSUB_PIN:
		return "PU";
	case OFONO_SIM_PASSWORD_PHSP_PIN:
		return "PP";
	case OFONO_SIM_PASSWORD_PHCORP_PIN:
		return "PC";
	default:
		return NULL;
	}
};

static void ril_sim_pin_change_state(struct ofono_sim *sim,
	enum ofono_sim_password_type passwd_type, int enable,
	const char *passwd, ofono_sim_lock_unlock_cb_t cb, void *data)
{
	struct ril_sim *sd = ril_sim_get_data(sim);
	struct ofono_error error;
	const char *type_str = ril_sim_facility_code(passwd_type);
	guint id = 0;

	DBG_(sd, "%d,%s,%d,%s,0,aid=%s", passwd_type, type_str, enable, passwd,
							ril_sim_app_id(sd));

	if (passwd_type == OFONO_SIM_PASSWORD_PHNET_PIN) {
		id = ril_perso_change_state(sim, passwd_type, enable, passwd,
								cb, data);
	} else if (type_str) {
		GRilIoRequest *req = grilio_request_new();
		grilio_request_append_int32(req, SET_FACILITY_LOCK_PARAMS);
		grilio_request_append_utf8(req, type_str);
		grilio_request_append_utf8(req, enable ?
			RIL_FACILITY_LOCK : RIL_FACILITY_UNLOCK);
		grilio_request_append_utf8(req, passwd);
		grilio_request_append_utf8(req, "0");		/* class */
		grilio_request_append_utf8(req, ril_sim_app_id(sd));

		id = grilio_queue_send_request_full(sd->q, req,
			RIL_REQUEST_SET_FACILITY_LOCK,
			ril_sim_pin_change_state_cb, ril_sim_pin_req_done,
			ril_sim_pin_cbd_new(sd, passwd_type, TRUE, cb, data));
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
	grilio_request_append_utf8(req, ril_sim_app_id(sd));

	DBG_(sd, "puk=%s,pin=%s,aid=%s", puk, passwd, ril_sim_app_id(sd));
	grilio_queue_send_request_full(sd->q, req, RIL_REQUEST_ENTER_SIM_PUK,
		ril_sim_pin_change_state_cb, ril_sim_pin_req_done,
		ril_sim_pin_cbd_new(sd, OFONO_SIM_PASSWORD_SIM_PUK,
						TRUE, cb, data));
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
	grilio_request_append_utf8(req, ril_sim_app_id(sd));

	DBG_(sd, "old=%s,new=%s,aid=%s", old_passwd, new_passwd,
							ril_sim_app_id(sd));
	grilio_queue_send_request_full(sd->q, req,
		(passwd_type == OFONO_SIM_PASSWORD_SIM_PIN2) ?
		RIL_REQUEST_CHANGE_SIM_PIN2 : RIL_REQUEST_CHANGE_SIM_PIN,
		ril_sim_pin_change_state_cb, ril_sim_pin_req_done,
		ril_sim_pin_cbd_new(sd, passwd_type, FALSE, cb, data));
	grilio_request_unref(req);
}

static void ril_sim_query_facility_lock_cb(GRilIoChannel *io, int status,
				const void *data, guint len, void *user_data)
{
	struct ofono_error error;
	struct ril_sim_cbd *cbd = user_data;
	ofono_query_facility_lock_cb_t cb = cbd->cb.query_facility_lock;

	if (status == RIL_E_SUCCESS) {
		int locked = 0;
		GRilIoParser rilp;

		grilio_parser_init(&rilp, data, len);
		if (grilio_parser_get_int32(&rilp, NULL) &&
				grilio_parser_get_int32(&rilp, &locked)) {
			DBG_(cbd->sd, "%d", locked);
			cb(ril_error_ok(&error), locked != 0, cbd->data);
			return;
		}
	}

	cb(ril_error_failure(&error), FALSE, cbd->data);
}

static void ril_sim_query_facility_lock(struct ofono_sim *sim,
				enum ofono_sim_password_type type,
				ofono_query_facility_lock_cb_t cb, void *data)
{
	struct ril_sim *sd = ril_sim_get_data(sim);
	GRilIoRequest *req = grilio_request_new();
	const char *type_str = ril_sim_facility_code(type);

	DBG_(sd, "%s", type_str);
	grilio_request_append_int32(req, 4);
	grilio_request_append_utf8(req, type_str);
	grilio_request_append_utf8(req, "");
	grilio_request_append_utf8(req, "0"); /* class */
	grilio_request_append_utf8(req, ril_sim_app_id(sd));
	grilio_queue_send_request_full(sd->q, req,
		RIL_REQUEST_QUERY_FACILITY_LOCK, ril_sim_query_facility_lock_cb,
		ril_sim_cbd_free, ril_sim_cbd_new(sd, cb, data));
	grilio_request_unref(req);
}

static gboolean ril_sim_register(gpointer user)
{
	struct ril_sim *sd = user;

	DBG_(sd, "");
	GASSERT(sd->idle_id);
	sd->idle_id = 0;

	ofono_sim_register(sd->sim);

	/* Register for change notifications */
	sd->card_status_id = ril_sim_card_add_status_changed_handler(sd->card,
						ril_sim_status_cb, sd);

	/* Check the current state */
	ril_sim_status_cb(sd->card, sd);
	return FALSE;
}

static int ril_sim_probe(struct ofono_sim *sim, unsigned int vendor,
				void *data)
{
	struct ril_modem *modem = data;
	struct ril_sim *sd = g_new0(struct ril_sim, 1);

	DBG("%s", modem->log_prefix);
	sd->sim = sim;
	sd->empty_pin_query_allowed = modem->config.empty_pin_query;
	sd->io = grilio_channel_ref(ril_modem_io(modem));
	sd->card = ril_sim_card_ref(modem->sim_card);
	sd->q = grilio_queue_new(sd->io);

	if (modem->log_prefix && modem->log_prefix[0]) {
		sd->log_prefix = sd->allocated_log_prefix =
			g_strconcat(modem->log_prefix, " ", NULL);
	} else {
		sd->log_prefix = "";
	}

	ril_sim_invalidate_passwd_state(sd);
	sd->idle_id = g_idle_add(ril_sim_register, sd);
	ofono_sim_set_data(sim, sd);
	return 0;
}

static void ril_sim_remove(struct ofono_sim *sim)
{
	struct ril_sim *sd = ril_sim_get_data(sim);

	DBG_(sd, "");
	g_list_free_full(sd->pin_cbd_list, ril_sim_pin_cbd_list_free_cb);
	grilio_queue_cancel_all(sd->q, FALSE);
	ofono_sim_set_data(sim, NULL);

	if (sd->idle_id) {
		g_source_remove(sd->idle_id);
	}

	if (sd->query_passwd_state_timeout_id) {
		g_source_remove(sd->query_passwd_state_timeout_id);
	}

	ril_sim_card_remove_handler(sd->card, sd->card_status_id);
	ril_sim_card_unref(sd->card);

	grilio_channel_unref(sd->io);
	grilio_queue_unref(sd->q);
	g_free(sd->allocated_log_prefix);
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
	.write_file_transparent = ril_sim_write_file_transparent,
	.write_file_linear      = ril_sim_write_file_linear,
	.write_file_cyclic      = ril_sim_write_file_cyclic,
	.read_imsi              = ril_sim_read_imsi,
	.query_passwd_state     = ril_sim_query_passwd_state,
	.send_passwd            = ril_sim_pin_send,
	.lock                   = ril_sim_pin_change_state,
	.reset_passwd           = ril_sim_pin_send_puk,
	.change_passwd          = ril_sim_change_passwd,
	.query_pin_retries      = ril_sim_query_pin_retries,
	.query_facility_lock    = ril_sim_query_facility_lock
};

/*
 * Local Variables:
 * mode: C
 * c-basic-offset: 8
 * indent-tabs-mode: t
 * End:
 */
