/*
 *
 *  oFono - Open Source Telephony
 *
 *  Copyright (C) 2008-2012  Intel Corporation. All rights reserved.
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

#include <ofono/log.h>
#include <ofono/modem.h>
#include <ofono/sim.h>

#include "qmi.h"
#include "dms.h"
#include "uim.h"

#include "qmimodem.h"
#include "simutil.h"

#define EF_STATUS_INVALIDATED 0
#define EF_STATUS_VALID 1

/* max number of retry of commands that can temporary fail */
#define MAX_RETRY_COUNT 100

enum get_card_status_result {
	GET_CARD_STATUS_RESULT_OK, /* No error */
	GET_CARD_STATUS_RESULT_ERROR, /* Definitive error */
	GET_CARD_STATUS_RESULT_TEMP_ERROR, /* error, a retry could work */
};

/* information from QMI_UIM_GET_CARD_STATUS command */
struct sim_status {
	uint8_t card_state;
	uint8_t app_type;
	uint8_t passwd_state;
	int retries[OFONO_SIM_PASSWORD_INVALID];
};

struct sim_data {
	struct qmi_device *qmi_dev;
	struct qmi_service *dms;
	struct qmi_service *uim;
	uint32_t event_mask;
	uint8_t app_type;
	uint32_t retry_count;
	guint poll_source;
};

static void qmi_query_passwd_state(struct ofono_sim *sim,
				ofono_sim_passwd_cb_t cb, void *user_data);

static int create_fileid_data(uint8_t app_type, int fileid,
					const unsigned char *path,
					unsigned int path_len,
					unsigned char *fileid_data)
{
	unsigned char db_path[6];
	unsigned int len;

	if (path_len > 0) {
		memcpy(db_path, path, path_len);
		len = path_len;
	} else {
		switch (app_type) {
		case 0x01:	/* SIM card */
			len = sim_ef_db_get_path_2g(fileid, db_path);
			break;
		case 0x02:	/* USIM application */
			len = sim_ef_db_get_path_3g(fileid, db_path);
			break;
		default:
			len = 0;
			break;
		}
	}

	/* Minimum length of path is 2 bytes */
	if (len < 2)
		return -1;

	fileid_data[0] = fileid & 0xff;
	fileid_data[1] = (fileid & 0xff00) >> 8;
	fileid_data[2] = len;
	fileid_data[3] = db_path[1];
	fileid_data[4] = db_path[0];
	fileid_data[5] = db_path[3];
	fileid_data[6] = db_path[2];
	fileid_data[7] = db_path[5];
	fileid_data[8] = db_path[4];

	return len + 3;
}

static void get_file_attributes_cb(struct qmi_result *result, void *user_data)
{
        struct cb_data *cbd = user_data;
	ofono_sim_file_info_cb_t cb = cbd->cb;
	struct sim_data *data = ofono_sim_get_data(cbd->user);
	const struct qmi_uim_file_attributes *attr;
	uint16_t len, raw_len;
	int flen, rlen, str;
	unsigned char access[3];
	unsigned char file_status;
	gboolean ok;

	DBG("");

	if (qmi_result_set_error(result, NULL))
		goto error;

	attr = qmi_result_get(result, 0x11, &len);
	if (!attr)
		goto error;

	raw_len = GUINT16_FROM_LE(attr->raw_len);

	switch (data->app_type) {
	case 0x01:	/* SIM card */
		ok = sim_parse_2g_get_response(attr->raw_value, raw_len,
				&flen, &rlen, &str, access, &file_status);
		break;
	case 0x02:	/* USIM application */
		ok = sim_parse_3g_get_response(attr->raw_value, raw_len,
						&flen, &rlen, &str, access,
						NULL);
		file_status = EF_STATUS_VALID;
		break;
	default:
		ok = FALSE;
		break;
	}

	if (ok) {
		CALLBACK_WITH_SUCCESS(cb, flen, str, rlen, access,
						file_status, cbd->data);
		return;
	}

error:
	CALLBACK_WITH_FAILURE(cb, -1, -1, -1, NULL,
					EF_STATUS_INVALIDATED, cbd->data);
}

static void qmi_read_attributes(struct ofono_sim *sim, int fileid,
				const unsigned char *path,
				unsigned int path_len,
				ofono_sim_file_info_cb_t cb, void *user_data)
{
	struct sim_data *data = ofono_sim_get_data(sim);
	struct cb_data *cbd = cb_data_new(cb, user_data);
	unsigned char aid_data[2] = { 0x00, 0x00 };
	unsigned char fileid_data[9];
	int fileid_len;
	struct qmi_param *param;

	DBG("file id 0x%04x path len %d", fileid, path_len);

	cbd->user = sim;

	fileid_len = create_fileid_data(data->app_type, fileid,
						path, path_len, fileid_data);
	if (fileid_len < 0)
		goto error;

	param = qmi_param_new();
	if (!param)
		goto error;

	qmi_param_append(param, 0x01, sizeof(aid_data), aid_data);
	qmi_param_append(param, 0x02, fileid_len, fileid_data);

	if (qmi_service_send(data->uim, QMI_UIM_GET_FILE_ATTRIBUTES, param,
				get_file_attributes_cb, cbd, g_free) > 0)
		return;

	qmi_param_free(param);

error:
	CALLBACK_WITH_FAILURE(cb, -1, -1, -1, NULL,
					EF_STATUS_INVALIDATED, cbd->data);

	g_free(cbd);
}

static void read_generic_cb(struct qmi_result *result, void *user_data)
{
        struct cb_data *cbd = user_data;
	ofono_sim_read_cb_t cb = cbd->cb;
	const unsigned char *content;
	uint16_t len;

	DBG("");

	if (qmi_result_set_error(result, NULL)) {
		CALLBACK_WITH_FAILURE(cb, NULL, 0, cbd->data);
		return;
	}

	content = qmi_result_get(result, 0x11, &len);
	if (!content) {
		CALLBACK_WITH_FAILURE(cb, NULL, 0, cbd->data);
		return;
	}

	CALLBACK_WITH_SUCCESS(cb, content + 2, len - 2, cbd->data);
}

static void qmi_read_transparent(struct ofono_sim *sim,
				int fileid, int start, int length,
				const unsigned char *path,
				unsigned int path_len,
				ofono_sim_read_cb_t cb, void *user_data)
{
	struct sim_data *data = ofono_sim_get_data(sim);
	struct cb_data *cbd = cb_data_new(cb, user_data);
	unsigned char aid_data[2] = { 0x00, 0x00 };
	unsigned char read_data[4];
	unsigned char fileid_data[9];
	int fileid_len;
	struct qmi_param *param;

	DBG("file id 0x%04x path len %d", fileid, path_len);

	fileid_len = create_fileid_data(data->app_type, fileid,
						path, path_len, fileid_data);
	if (fileid_len < 0)
		goto error;

	read_data[0] = start & 0xff;
	read_data[1] = (start & 0xff00) >> 8;
	read_data[2] = length & 0xff;
	read_data[3] = (length & 0xff00) >> 8;

	param = qmi_param_new();
	if (!param)
		goto error;

	qmi_param_append(param, 0x01, sizeof(aid_data), aid_data);
	qmi_param_append(param, 0x02, fileid_len, fileid_data);
	qmi_param_append(param, 0x03, sizeof(read_data), read_data);

	if (qmi_service_send(data->uim, QMI_UIM_READ_TRANSPARENT, param,
					read_generic_cb, cbd, g_free) > 0)
		return;

	qmi_param_free(param);

error:
	CALLBACK_WITH_FAILURE(cb, NULL, 0, user_data);

	g_free(cbd);
}

static void qmi_read_record(struct ofono_sim *sim,
				int fileid, int record, int length,
				const unsigned char *path,
				unsigned int path_len,
				ofono_sim_read_cb_t cb, void *user_data)
{
	struct sim_data *data = ofono_sim_get_data(sim);
	struct cb_data *cbd = cb_data_new(cb, user_data);
	unsigned char aid_data[2] = { 0x00, 0x00 };
	unsigned char read_data[4];
	unsigned char fileid_data[9];
	int fileid_len;
	struct qmi_param *param;

	DBG("file id 0x%04x path len %d", fileid, path_len);

	fileid_len = create_fileid_data(data->app_type, fileid,
						path, path_len, fileid_data);
	if (fileid_len < 0)
		goto error;

	read_data[0] = record & 0xff;
	read_data[1] = (record & 0xff00) >> 8;
	read_data[2] = length & 0xff;
	read_data[3] = (length & 0xff00) >> 8;

	param = qmi_param_new();
	if (!param)
		goto error;

	qmi_param_append(param, 0x01, sizeof(aid_data), aid_data);
	qmi_param_append(param, 0x02, fileid_len, fileid_data);
	qmi_param_append(param, 0x03, sizeof(read_data), read_data);

	if (qmi_service_send(data->uim, QMI_UIM_READ_RECORD, param,
					read_generic_cb, cbd, g_free) > 0)
		return;

	qmi_param_free(param);

error:
	CALLBACK_WITH_FAILURE(cb, NULL, 0, user_data);

	g_free(cbd);
}

static void write_generic_cb(struct qmi_result *result, void *user_data)
{
	struct cb_data *cbd = user_data;
	ofono_sim_write_cb_t cb = cbd->cb;
	uint16_t len;
	const uint8_t *card_result;
	uint8_t sw1, sw2;

	card_result = qmi_result_get(result, 0x10, &len);
	if (card_result == NULL || len != 2) {
		DBG("card_result: %p, len: %d", card_result, (int) len);
		CALLBACK_WITH_FAILURE(cb, cbd->data);
		return;
	}

	sw1 = card_result[0];
	sw2 = card_result[1];

	DBG("%02x, %02x", sw1, sw2);

	if ((sw1 != 0x90 && sw1 != 0x91 && sw1 != 0x92 && sw1 != 0x9f) ||
			(sw1 == 0x90 && sw2 != 0x00)) {
		struct ofono_error error;

		ofono_error("%s: error sw1 %02x sw2 %02x", __func__, sw1, sw2);

		error.type = OFONO_ERROR_TYPE_SIM;
		error.error = (sw1 << 8) | sw2;

		cb(&error, cbd->data);
		return;
	}

	CALLBACK_WITH_SUCCESS(cb, cbd->data);
}

static void write_generic(struct ofono_sim *sim,
			uint16_t qmi_message, int fileid,
			int start_or_recordnum,
			int length, const unsigned char *value,
			const unsigned char *path, unsigned int path_len,
			ofono_sim_write_cb_t cb, void *user_data)
{
	struct sim_data *data = ofono_sim_get_data(sim);
	struct cb_data *cbd = cb_data_new(cb, user_data);
	unsigned char aid_data[2] = { 0x00, 0x00 };
	unsigned char write_data[4 + length];
	unsigned char fileid_data[9];
	int fileid_len;
	struct qmi_param *param;

	DBG("file id 0x%04x path len %d", fileid, path_len);

	fileid_len = create_fileid_data(data->app_type, fileid,
			path, path_len, fileid_data);

	if (fileid_len < 0)
		goto error;

	write_data[0] = start_or_recordnum & 0xff;
	write_data[1] = (start_or_recordnum & 0xff00) >> 8;
	write_data[2] = length & 0xff;
	write_data[3] = (length & 0xff00) >> 8;
	memcpy(&write_data[4], value, length);

	param = qmi_param_new();
	if (!param)
		goto error;

	qmi_param_append(param, 0x01, sizeof(aid_data), aid_data);
	qmi_param_append(param, 0x02, fileid_len, fileid_data);
	qmi_param_append(param, 0x03, 4 + length, write_data);

	if (qmi_service_send(data->uim, qmi_message, param,
					write_generic_cb, cbd, g_free) > 0)
		return;

	qmi_param_free(param);

error:
	CALLBACK_WITH_FAILURE(cb, user_data);

	g_free(cbd);
}

static void qmi_write_transparent(struct ofono_sim *sim,
				int fileid, int start, int length,
				const unsigned char *value,
				const unsigned char *path,
				unsigned int path_len,
				ofono_sim_write_cb_t cb, void *user_data)
{
	write_generic(sim, QMI_UIM_WRITE_TRANSPARENT, fileid, start,
				length, value, path, path_len, cb, user_data);
}

static void qmi_write_linear(struct ofono_sim *sim,
				int fileid, int record, int length,
				const unsigned char *value,
				const unsigned char *path,
				unsigned int path_len,
				ofono_sim_write_cb_t cb, void *user_data)
{
	write_generic(sim, QMI_UIM_WRITE_RECORD, fileid, record,
				length, value, path, path_len, cb, user_data);
}

static void qmi_write_cyclic(struct ofono_sim *sim,
				int fileid, int length,
				const unsigned char *value,
				const unsigned char *path,
				unsigned int path_len,
				ofono_sim_write_cb_t cb, void *user_data)
{
	write_generic(sim, QMI_UIM_WRITE_RECORD, fileid, 0,
				length, value, path, path_len, cb, user_data);
}

static void get_imsi_cb(struct qmi_result *result, void *user_data)
{
	struct cb_data *cbd = user_data;
	ofono_sim_imsi_cb_t cb = cbd->cb;
	char *str;

	DBG("");

	if (qmi_result_set_error(result, NULL)) {
		CALLBACK_WITH_FAILURE(cb, NULL, cbd->data);
		return;
	}

	str = qmi_result_get_string(result, QMI_DMS_RESULT_IMSI);
	if (!str) {
		CALLBACK_WITH_FAILURE(cb, NULL, cbd->data);
		return;
	}

	CALLBACK_WITH_SUCCESS(cb, str, cbd->data);

	qmi_free(str);
}

static void qmi_read_imsi(struct ofono_sim *sim,
				ofono_sim_imsi_cb_t cb, void *user_data)
{
	struct sim_data *data = ofono_sim_get_data(sim);
	struct cb_data *cbd = cb_data_new(cb, user_data);

	DBG("");

	if (qmi_service_send(data->dms, QMI_DMS_GET_IMSI, NULL,
					get_imsi_cb, cbd, g_free) > 0)
		return;

	CALLBACK_WITH_FAILURE(cb, NULL, cbd->data);

	g_free(cbd);
}

/* Return true if a retry could give another (better) result */
static bool get_card_status(const struct qmi_uim_slot_info *slot,
					const struct qmi_uim_app_info1 *info1,
					const struct qmi_uim_app_info2 *info2,
					struct sim_status *sim_stat)
{
	bool need_retry = false;
	sim_stat->card_state = slot->card_state;
	sim_stat->app_type = info1->app_type;

	switch (info1->app_state) {
	case 0x02:	/* PIN1 or UPIN is required */
		sim_stat->passwd_state = OFONO_SIM_PASSWORD_SIM_PIN;
		break;
	case 0x03:	/* PUK1 or PUK for UPIN is required */
		sim_stat->passwd_state = OFONO_SIM_PASSWORD_SIM_PUK;
		break;
	case 0x00:	/* Unknown */
	case 0x01:	/* Detected */
	case 0x04:	/* Personalization state must be checked. */
	case 0x05:	/* PIN1 blocked */
	case 0x06:	/* Illegal */
		/*
		 * This could be temporary, we should retry and
		 * expect another result
		 */
		sim_stat->passwd_state = OFONO_SIM_PASSWORD_INVALID;
		need_retry = true;
		break;
	case 0x07:	/* Ready */
		sim_stat->passwd_state = OFONO_SIM_PASSWORD_NONE;
		break;
	default:
		DBG("info1->app_state:0x%x: OFONO_SIM_PASSWORD_INVALID",
			info1->app_state);
		sim_stat->passwd_state = OFONO_SIM_PASSWORD_INVALID;
		break;
	}

	sim_stat->retries[OFONO_SIM_PASSWORD_SIM_PIN] = info2->pin1_retries;
	sim_stat->retries[OFONO_SIM_PASSWORD_SIM_PUK] = info2->puk1_retries;

	sim_stat->retries[OFONO_SIM_PASSWORD_SIM_PIN2] = info2->pin2_retries;
	sim_stat->retries[OFONO_SIM_PASSWORD_SIM_PUK2] = info2->puk2_retries;

	return need_retry;
}

static enum get_card_status_result handle_get_card_status_data(
		struct qmi_result *result, struct sim_status *sim_stat)
{
	const void *ptr;
	const struct qmi_uim_card_status *status;
	uint16_t len, offset;
	uint8_t i;
	enum get_card_status_result res = GET_CARD_STATUS_RESULT_ERROR;

	ptr = qmi_result_get(result, QMI_UIM_RESULT_CARD_STATUS, &len);
	if (!ptr)
		goto done;

	status = ptr;
	offset = sizeof(struct qmi_uim_card_status);

	for (i = 0; i < status->num_slot; i++) {
		const struct qmi_uim_slot_info *slot;
		uint8_t n;

		slot = ptr + offset;
		offset += sizeof(struct qmi_uim_slot_info);

		for (n = 0; n < slot->num_app; n++) {
			const struct qmi_uim_app_info1 *info1;
			const struct qmi_uim_app_info2 *info2;
			uint16_t index;

			info1 = ptr + offset;
			offset += sizeof(struct qmi_uim_app_info1);
			offset += info1->aid_len;

			info2 = ptr + offset;
			offset += sizeof(struct qmi_uim_app_info2);

			index = GUINT16_FROM_LE(status->index_gw_pri);

			if ((index & 0xff) == n && (index >> 8) == i) {
				if (get_card_status(slot, info1, info2,
								sim_stat))
					res = GET_CARD_STATUS_RESULT_TEMP_ERROR;
				else
					res = GET_CARD_STATUS_RESULT_OK;
			}
		}
	}

done:
	return res;
}

static enum get_card_status_result handle_get_card_status_result(
		struct qmi_result *result, struct sim_status *sim_stat)
{
	if (qmi_result_set_error(result, NULL))
		return GET_CARD_STATUS_RESULT_ERROR;

	return handle_get_card_status_data(result, sim_stat);
}

static gboolean query_passwd_state_retry(gpointer userdata)
{
	struct cb_data *cbd = userdata;
	ofono_sim_passwd_cb_t cb = cbd->cb;
	struct ofono_sim *sim = cbd->user;
	struct sim_data *data = ofono_sim_get_data(sim);

	data->poll_source = 0;

	qmi_query_passwd_state(sim, cb, cbd->data);

	return FALSE;
}

static void query_passwd_state_cb(struct qmi_result *result,
					void *user_data)
{
	struct cb_data *cbd = user_data;
	ofono_sim_passwd_cb_t cb = cbd->cb;
	struct ofono_sim *sim = cbd->user;
	struct sim_data *data = ofono_sim_get_data(sim);
	struct sim_status sim_stat;
	enum get_card_status_result res;
	struct cb_data *retry_cbd;
	unsigned int i;

	for (i = 0; i < OFONO_SIM_PASSWORD_INVALID; i++)
		sim_stat.retries[i] = -1;

	res = handle_get_card_status_result(result, &sim_stat);
	switch (res) {
	case GET_CARD_STATUS_RESULT_OK:
		DBG("passwd state %d", sim_stat.passwd_state);
		data->retry_count = 0;
		if (sim_stat.passwd_state == OFONO_SIM_PASSWORD_INVALID) {
			CALLBACK_WITH_FAILURE(cb, -1, cbd->data);
			ofono_sim_inserted_notify(sim, FALSE);
		} else
			CALLBACK_WITH_SUCCESS(cb, sim_stat.passwd_state,
								cbd->data);
		break;
	case GET_CARD_STATUS_RESULT_TEMP_ERROR:
		data->retry_count++;
		if (data->retry_count > MAX_RETRY_COUNT) {
			DBG("Failed after %d attempts. Card state:%d",
							data->retry_count,
							sim_stat.card_state);
			data->retry_count = 0;
			CALLBACK_WITH_FAILURE(cb, -1, cbd->data);
			ofono_sim_inserted_notify(sim, FALSE);
		} else {
			DBG("Retry command");
			retry_cbd = cb_data_new(cb, cbd->data);
			retry_cbd->user = sim;
			data->poll_source = g_timeout_add(20,
						query_passwd_state_retry,
						retry_cbd);
		}
		break;
	case GET_CARD_STATUS_RESULT_ERROR:
		DBG("Command failed");
		data->retry_count = 0;
		CALLBACK_WITH_FAILURE(cb, -1, cbd->data);
		ofono_sim_inserted_notify(sim, FALSE);
		break;
	}
}

static void qmi_query_passwd_state(struct ofono_sim *sim,
				ofono_sim_passwd_cb_t cb, void *user_data)
{
	struct sim_data *data = ofono_sim_get_data(sim);
	struct cb_data *cbd = cb_data_new(cb, user_data);

	DBG("");

	cbd->user = sim;

	if (qmi_service_send(data->uim, QMI_UIM_GET_CARD_STATUS, NULL,
					query_passwd_state_cb, cbd, g_free) > 0)
		return;

	CALLBACK_WITH_FAILURE(cb, -1, cbd->data);

	g_free(cbd);
}

static void query_pin_retries_cb(struct qmi_result *result, void *user_data)
{
	struct cb_data *cbd = user_data;
	ofono_sim_pin_retries_cb_t cb = cbd->cb;
	struct sim_status sim_stat;
	unsigned int i;

	DBG("");

	for (i = 0; i < OFONO_SIM_PASSWORD_INVALID; i++)
		sim_stat.retries[i] = -1;

	if (handle_get_card_status_result(result, &sim_stat) !=
					GET_CARD_STATUS_RESULT_OK) {
		CALLBACK_WITH_FAILURE(cb, NULL, cbd->data);
		return;
	}

	CALLBACK_WITH_SUCCESS(cb, sim_stat.retries, cbd->data);
}

static void qmi_query_pin_retries(struct ofono_sim *sim,
				ofono_sim_pin_retries_cb_t cb, void *user_data)
{
	struct sim_data *data = ofono_sim_get_data(sim);
	struct cb_data *cbd = cb_data_new(cb, user_data);

	DBG("");

	if (qmi_service_send(data->uim, QMI_UIM_GET_CARD_STATUS, NULL,
					query_pin_retries_cb, cbd, g_free) > 0)
		return;

	CALLBACK_WITH_FAILURE(cb, NULL, cbd->data);

	g_free(cbd);
}

static void pin_send_cb(struct qmi_result *result, void *user_data)
{
	struct cb_data *cbd = user_data;
	ofono_sim_lock_unlock_cb_t cb = cbd->cb;

	DBG("");

	if (qmi_result_set_error(result, NULL)) {
		CALLBACK_WITH_FAILURE(cb, cbd->data);
		return;
	}

	CALLBACK_WITH_SUCCESS(cb, cbd->data);
}

static void qmi_pin_send(struct ofono_sim *sim, const char *passwd,
			ofono_sim_lock_unlock_cb_t cb, void *user_data)
{
	struct sim_data *data = ofono_sim_get_data(sim);
	struct cb_data *cbd = cb_data_new(cb, user_data);
	int passwd_len;
	struct qmi_param *param;
	struct qmi_uim_param_message_info *info_data;
	unsigned char session_info_data[2];

	DBG("");

	if (!passwd)
		goto error;

	passwd_len = strlen(passwd);

	if (passwd_len <= 0 || passwd_len > 0xFF)
		goto error;

	param = qmi_param_new();
	if (!param)
		goto error;

	/* param info */
	info_data = alloca(2 + passwd_len);
	info_data->pin_id = 0x01; /* PIN 1 */
	info_data->length = (uint8_t) passwd_len;
	memcpy(info_data->pin_value, passwd, passwd_len);
	qmi_param_append(param, QMI_UIM_PARAM_MESSAGE_INFO, 2 + passwd_len,
					info_data);
	/* param Session Information */
	session_info_data[0] = 0x6;
	session_info_data[1] = 0x0;
	qmi_param_append(param, QMI_UIM_PARAM_MESSAGE_SESSION_INFO, 2,
					session_info_data);

	if (qmi_service_send(data->uim, QMI_UIM_VERIFY_PIN, param,
					pin_send_cb, cbd, g_free) > 0)
		return;

	qmi_param_free(param);

error:
	CALLBACK_WITH_FAILURE(cb, cbd->data);

	g_free(cbd);
}

static void get_card_status_cb(struct qmi_result *result, void *user_data)
{
	struct ofono_sim *sim = user_data;
	struct sim_data *data = ofono_sim_get_data(sim);
	struct sim_status sim_stat;

	DBG("");

	if (handle_get_card_status_result(result, &sim_stat) !=
					GET_CARD_STATUS_RESULT_OK) {
		data->app_type = 0;	/* Unknown */
		sim_stat.card_state = 0x00;	/* Absent */
	} else {
		data->app_type = sim_stat.app_type;
	}

	ofono_sim_register(sim);

	switch (sim_stat.card_state) {
	case 0x00:	/* Absent */
	case 0x02:	/* Error */
		break;
	case 0x01:	/* Present */
		ofono_sim_inserted_notify(sim, TRUE);
		ofono_sim_initialized_notify(sim);
		break;
	}
}

static void card_status_notify(struct qmi_result *result, void *user_data)
{
	struct ofono_sim *sim = user_data;
	struct sim_data *data = ofono_sim_get_data(sim);
	struct sim_status sim_stat;

	DBG("");

	if (handle_get_card_status_data(result, &sim_stat) !=
					GET_CARD_STATUS_RESULT_OK) {
		data->app_type = 0;	/* Unknown */
		sim_stat.card_state = 0x00;	/* Absent */
	} else {
		data->app_type = sim_stat.app_type;
	}

	switch (sim_stat.card_state) {
	case 0x00:	/* Absent */
	case 0x02:	/* Error */
		ofono_sim_inserted_notify(sim, FALSE);
		break;
	case 0x01:	/* Present */
		ofono_sim_inserted_notify(sim, TRUE);
		break;
	}
}

static void event_registration_cb(struct qmi_result *result, void *user_data)
{
	struct ofono_sim *sim = user_data;
	struct sim_data *data = ofono_sim_get_data(sim);

	DBG("");

	if (qmi_result_set_error(result, NULL))
		goto error;

	if (!qmi_result_get_uint32(result, QMI_UIM_RESULT_EVENT_MASK,
							&data->event_mask))
		goto error;

	DBG("event mask 0x%04x", data->event_mask);
	if (data->event_mask & 0x0001)
		qmi_service_register(data->uim, QMI_UIM_GET_CARD_STATUS_EVENT,
						card_status_notify, sim, NULL);

	if (qmi_service_send(data->uim, QMI_UIM_GET_CARD_STATUS, NULL,
					get_card_status_cb, sim, NULL) > 0)
		return;

error:
	ofono_sim_remove(sim);
}


static void create_uim_cb(struct qmi_service *service, void *user_data)
{
	struct ofono_sim *sim = user_data;
	struct sim_data *data = ofono_sim_get_data(sim);
	struct qmi_param *param;
	uint32_t mask = 0x0003;

	DBG("");

	if (!service) {
		ofono_error("Failed to request UIM service");
		goto error;
	}

	data->uim = qmi_service_ref(service);

	param = qmi_param_new_uint32(QMI_UIM_PARAM_EVENT_MASK, mask);
	if (!param)
		goto error;

	if (qmi_service_send(data->uim, QMI_UIM_EVENT_REGISTRATION, param,
					event_registration_cb, sim, NULL) > 0)
		return;

error:
	ofono_sim_remove(sim);
}

static void create_dms_cb(struct qmi_service *service, void *user_data)
{
	struct ofono_sim *sim = user_data;
	struct sim_data *data = ofono_sim_get_data(sim);

	DBG("");

	if (!service) {
		ofono_error("Failed to request DMS service");
		ofono_sim_remove(sim);
		return;
	}

	data->dms = qmi_service_ref(service);

	qmi_service_create(data->qmi_dev, QMI_SERVICE_UIM, create_uim_cb, sim,
					NULL);
}

static int qmi_sim_probe(struct ofono_sim *sim,
				unsigned int vendor, void *user_data)
{
	struct qmi_device *device = user_data;
	struct sim_data *data;

	DBG("");

	data = g_new0(struct sim_data, 1);

	data->qmi_dev = device;

	ofono_sim_set_data(sim, data);

	qmi_service_create_shared(device, QMI_SERVICE_DMS,
						create_dms_cb, sim, NULL);

	return 0;
}

static void qmi_sim_remove(struct ofono_sim *sim)
{
	struct sim_data *data = ofono_sim_get_data(sim);

	DBG("");

	ofono_sim_set_data(sim, NULL);

	if (data->poll_source > 0)
		g_source_remove(data->poll_source);

	if (data->uim) {
		qmi_service_unregister_all(data->uim);
		qmi_service_unref(data->uim);
		data->uim = NULL;
	}
	if (data->dms) {
		qmi_service_unregister_all(data->dms);
		qmi_service_unref(data->dms);
	}

	g_free(data);
}

static const struct ofono_sim_driver driver = {
	.name			= "qmimodem",
	.probe			= qmi_sim_probe,
	.remove			= qmi_sim_remove,
	.read_file_info		= qmi_read_attributes,
	.read_file_transparent	= qmi_read_transparent,
	.read_file_linear	= qmi_read_record,
	.read_file_cyclic	= qmi_read_record,
	.write_file_transparent = qmi_write_transparent,
	.write_file_linear	= qmi_write_linear,
	.write_file_cyclic	= qmi_write_cyclic,
	.read_imsi		= qmi_read_imsi,
	.query_passwd_state	= qmi_query_passwd_state,
	.query_pin_retries	= qmi_query_pin_retries,
	.send_passwd		= qmi_pin_send,
};

void qmi_sim_init(void)
{
	ofono_sim_driver_register(&driver);
}

void qmi_sim_exit(void)
{
	ofono_sim_driver_unregister(&driver);
}
