/*
 *  oFono - Open Source Telephony - RIL-based devices
 *
 *  Copyright (C) 2015-2017 Jolla Ltd.
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
#include "ril_log.h"

#include "smsutil.h"
#include "util.h"
#include "simutil.h"

#define RIL_SMS_ACK_RETRY_MS    1000
#define RIL_SMS_ACK_RETRY_COUNT 10

#define SIM_EFSMS_FILEID        0x6F3C
#define EFSMS_LENGTH            176

#define TYPE_LOCAL              129
#define TYPE_INTERNATIONAL      145

static unsigned char sim_path[4] = {0x3F, 0x00, 0x7F, 0x10};

enum ril_sms_events {
	SMS_EVENT_NEW_SMS,
	SMS_EVENT_NEW_STATUS_REPORT,
	SMS_EVENT_NEW_SMS_ON_SIM,
	SMS_EVENT_COUNT
};

struct ril_sms {
	GRilIoChannel *io;
	GRilIoQueue *q;
	struct ril_modem *modem;
	struct ofono_sms *sms;
	struct ofono_sim_context *sim_context;
	gulong event_id[SMS_EVENT_COUNT];
	guint timer_id;
};

struct ril_sms_cbd {
	union _ofono_sms_cb {
		ofono_sms_sca_set_cb_t sca_set;
		ofono_sms_sca_query_cb_t sca_query;
		ofono_sms_submit_cb_t submit;
		gpointer ptr;
	} cb;
	gpointer data;
};

struct ril_sms_on_sim_req {
	struct ril_sms *sd;
	int record;
};

#define ril_sms_cbd_free g_free
#define ril_sms_on_sim_req_free g_free

static inline struct ril_sms *ril_sms_get_data(struct ofono_sms *sms)
{
	return ofono_sms_get_data(sms);
}

struct ril_sms_cbd *ril_sms_cbd_new(struct ril_sms *sd, void *cb, void *data)
{
	struct ril_sms_cbd *cbd = g_new0(struct ril_sms_cbd, 1);

	cbd->cb.ptr = cb;
	cbd->data = data;
	return cbd;
}

struct ril_sms_on_sim_req *ril_sms_on_sim_req_new(struct ril_sms *sd, int rec)
{
	struct ril_sms_on_sim_req *cbd = g_new0(struct ril_sms_on_sim_req, 1);

	cbd->sd = sd;
	cbd->record = rec;
	return cbd;
}

static void ril_sms_sca_set_cb(GRilIoChannel *io, int status,
				const void *data, guint len, void *user_data)
{
	struct ofono_error error;
	struct ril_sms_cbd *cbd = user_data;
	ofono_sms_sca_set_cb_t cb = cbd->cb.sca_set;

	if (status == RIL_E_SUCCESS) {
		cb(ril_error_ok(&error), cbd->data);
	} else {
		ofono_error("csca setting failed");
		cb(ril_error_failure(&error), cbd->data);
	}
}

static void ril_sms_sca_set(struct ofono_sms *sms,
			const struct ofono_phone_number *sca,
			ofono_sms_sca_set_cb_t cb, void *data)
{
	struct ril_sms *sd = ril_sms_get_data(sms);
	GRilIoRequest *req = grilio_request_new();
	char number[OFONO_MAX_PHONE_NUMBER_LENGTH + 4];

	if (sca->type == TYPE_LOCAL) {
		snprintf(number, sizeof(number), "\"%s\"", sca->number);
	} else {
		snprintf(number, sizeof(number), "\"+%s\"", sca->number);
	}

	DBG("Setting sca: %s", number);
	grilio_request_append_utf8(req, number);
	grilio_queue_send_request_full(sd->q, req,
			RIL_REQUEST_SET_SMSC_ADDRESS, ril_sms_sca_set_cb,
			ril_sms_cbd_free, ril_sms_cbd_new(sd, cb, data));
	grilio_request_unref(req);
}

static void ril_sms_sca_query_cb(GRilIoChannel *io, int status,
				const void *data, guint len, void *user_data)
{
	struct ril_sms_cbd *cbd = user_data;
	ofono_sms_sca_query_cb_t cb = cbd->cb.sca_query;
	struct ofono_error error;
	GRilIoParser rilp;
	gchar *temp_buf;

	if (status != RIL_E_SUCCESS) {
		ofono_error("csca query failed");
		cb(ril_error_failure(&error), NULL, cbd->data);
		return;
	}

	grilio_parser_init(&rilp, data, len);
	temp_buf = grilio_parser_get_utf8(&rilp);

	if (temp_buf) {
		/* RIL gives address in quotes */
		gchar *number = strtok(temp_buf, "\"");
		struct ofono_phone_number sca;

		strncpy(sca.number, number, OFONO_MAX_PHONE_NUMBER_LENGTH);
		sca.number[OFONO_MAX_PHONE_NUMBER_LENGTH] = '\0';
		if (sca.number[0] == '+') {
			number = number + 1;
			sca.type = TYPE_INTERNATIONAL;
		} else {
			sca.type = TYPE_LOCAL;
		}

		DBG("csca_query_cb: %s, %d", sca.number, sca.type);
		cb(ril_error_ok(&error), &sca, cbd->data);
		g_free(temp_buf);
	} else {
		ofono_error("return value invalid");
		cb(ril_error_failure(&error), NULL, cbd->data);
	}
}

static void ril_sms_sca_query(struct ofono_sms *sms,
			ofono_sms_sca_query_cb_t cb, void *data)
{
	struct ril_sms *sd = ril_sms_get_data(sms);

	DBG("Sending csca_query");
	grilio_queue_send_request_full(sd->q, NULL,
			RIL_REQUEST_GET_SMSC_ADDRESS, ril_sms_sca_query_cb,
			ril_sms_cbd_free, ril_sms_cbd_new(sd, cb, data));
}

static void ril_sms_submit_cb(GRilIoChannel *io, int status,
				const void *data, guint len, void *user_data)
{
	struct ril_sms_cbd *cbd = user_data;
	ofono_sms_submit_cb_t cb = cbd->cb.submit;
	struct ofono_error error;
	int mr = 0;

	if (status == RIL_E_SUCCESS) {
		GRilIoParser rilp;
		int err = -1;

		grilio_parser_init(&rilp, data, len);

		/* TP-Message-Reference for GSM/
		 * BearerData MessageId for CDMA
		 */
		grilio_parser_get_int32(&rilp, &mr);
		grilio_parser_skip_string(&rilp);

		/* error: 3GPP 27.005, 3.2.5, -1 if unknown or not applicable */
		grilio_parser_get_int32(&rilp, &err);
		DBG("sms msg ref: %d, error: %d", mr, err);
		ril_error_init_ok(&error);
	} else if (status == RIL_E_GENERIC_FAILURE) {
		ofono_info("not allowed by MO SMS control, do not retry");
		error.type = OFONO_ERROR_TYPE_CMS;
		error.error = 500;
	} else {
		ofono_error("sms sending failed, retry");
		ril_error_init_failure(&error);
	}

	cb(&error, mr, cbd->data);
}

static void ril_sms_submit(struct ofono_sms *sms, const unsigned char *pdu,
			int pdu_len, int tpdu_len, int mms,
			ofono_sms_submit_cb_t cb, void *data)
{
	struct ril_sms *sd = ril_sms_get_data(sms);
	GRilIoRequest *req = grilio_request_new();
	int smsc_len;
	char *tpdu;

	DBG("pdu_len: %d, tpdu_len: %d mms: %d", pdu_len, tpdu_len, mms);

	grilio_request_append_int32(req, 2);     /* Number of strings */

	/* SMSC address:
	 *
	 * smsc_len == 1, then zero-length SMSC was spec'd
	 * RILD expects a NULL string in this case instead
	 * of a zero-length string.
	 */
	smsc_len = pdu_len - tpdu_len;
	if (smsc_len > 1) {
		/* TODO: encode SMSC & write to parcel */
		DBG("SMSC address specified (smsc_len %d); NOT-IMPLEMENTED",
								smsc_len);
	}

	grilio_request_append_utf8(req, NULL); /* default SMSC address */

	/* TPDU:
	 *
	 * 'pdu' is a raw hexadecimal string
	 *  encode_hex() turns it into an ASCII/hex UTF8 buffer
	 *  grilio_request_append_utf8() encodes utf8 -> utf16
	 */
	tpdu = encode_hex(pdu + smsc_len, tpdu_len, 0);
	grilio_request_append_utf8(req, tpdu);

	DBG("%s", tpdu);
	grilio_queue_send_request_full(sd->q, req,
		mms ? RIL_REQUEST_SEND_SMS_EXPECT_MORE : RIL_REQUEST_SEND_SMS,
		ril_sms_submit_cb, ril_sms_cbd_free,
		ril_sms_cbd_new(sd, cb, data));
	grilio_request_unref(req);
	g_free(tpdu);
}

static void ril_ack_delivery_cb(GRilIoChannel *io, int status,
				const void *data, guint len, void *user_data)
{
	if (status != RIL_E_SUCCESS) {
		ofono_error("SMS acknowledgement failed: "
				"Further SMS reception is not guaranteed");
	}
}

static void ril_ack_delivery(struct ril_sms *sd, gboolean error)
{
	GRilIoRequest *req = grilio_request_sized_new(12);
	const int code = (error ? 0 : 0xff);

	DBG("(%d,%d)", error, code);
	grilio_request_append_int32(req, 2);     /* Array size*/
	grilio_request_append_int32(req, error); /* Success (1)/Failure (0) */
	grilio_request_append_int32(req, code);  /* error code */

	/* ACK the incoming NEW_SMS */
	grilio_request_set_retry(req, RIL_SMS_ACK_RETRY_MS,
						RIL_SMS_ACK_RETRY_COUNT);
	grilio_queue_send_request_full(sd->q, req,
		RIL_REQUEST_SMS_ACKNOWLEDGE, ril_ack_delivery_cb, NULL, NULL);
	grilio_request_unref(req);
}

static void ril_sms_notify(GRilIoChannel *io, guint ril_event,
				const void *data, guint len, void *user_data)
{
	struct ril_sms *sd = user_data;
	GRilIoParser rilp;
	char *ril_pdu;
	int ril_pdu_len;
	unsigned int smsc_len;
	long ril_buf_len;
	guchar *ril_data;

	ril_pdu = NULL;
	ril_data = NULL;

	DBG("event: %d; data_len: %d", ril_event, len);

	grilio_parser_init(&rilp, data, len);
	ril_pdu = grilio_parser_get_utf8(&rilp);
	if (ril_pdu == NULL)
		goto error;

	ril_pdu_len = strlen(ril_pdu);

	DBG("ril_pdu_len is %d", ril_pdu_len);
	ril_data = decode_hex(ril_pdu, ril_pdu_len, &ril_buf_len, -1);
	if (ril_data == NULL)
		goto error;

	/* The first octect in the pdu contains the SMSC address length
	 * which is the X following octects it reads. We add 1 octet to
	 * the read length to take into account this read octet in order
	 * to calculate the proper tpdu length.
	 */
	smsc_len = ril_data[0] + 1;
	ofono_info("sms received, smsc_len is %d", smsc_len);
	DBG("(%s)", ril_pdu);

	if (ril_event == RIL_UNSOL_RESPONSE_NEW_SMS) {
		/* Last parameter is 'tpdu_len' ( substract SMSC length ) */
		ofono_sms_deliver_notify(sd->sms, ril_data, ril_buf_len,
						ril_buf_len - smsc_len);
	} else {
		GASSERT(ril_event == RIL_UNSOL_RESPONSE_NEW_SMS_STATUS_REPORT);
		ofono_sms_status_notify(sd->sms, ril_data, ril_buf_len,
						ril_buf_len - smsc_len);
	}

	g_free(ril_pdu);
	g_free(ril_data);
	ril_ack_delivery(sd, TRUE);
	return;

error:
	g_free(ril_pdu);
	g_free(ril_data);
	ril_ack_delivery(sd, FALSE);
	ofono_error("Unable to parse NEW_SMS notification");
}

static void ril_new_sms_on_sim_cb(GRilIoChannel *io, int status,
				const void *data, guint len, void *user_data)
{
	DBG("%d", status);
	if (status == RIL_E_SUCCESS) {
		ofono_info("sms deleted from sim");
	} else {
		ofono_error("deleting sms from sim failed");
	}
}

static void ril_request_delete_sms_om_sim(struct ril_sms *sd, int record)
{
	GRilIoRequest *req = grilio_request_sized_new(8);

	DBG("Deleting record: %d", record);

	grilio_request_append_int32(req, 1);        /* Array length */
	grilio_request_append_int32(req, record);
	grilio_queue_send_request_full(sd->q, req,
			RIL_REQUEST_DELETE_SMS_ON_SIM,
			ril_new_sms_on_sim_cb, NULL, NULL);
	grilio_request_unref(req);
}

static void ril_sms_on_sim_cb(int ok, int total_length, int record,
		const unsigned char *sdata, int length, void *userdata)
{
	struct ril_sms_on_sim_req *cbd = userdata;
	struct ril_sms *sd = cbd->sd;

	/*
	 * It seems when reading EFsms RIL returns the whole record including
	 * the first status byte therefore we ignore that as we are only
	 * interested of the following pdu
	 */
	/* The first octect in the pdu contains the SMSC address length
	 * which is the X following octects it reads. We add 1 octet to
	 * the read length to take into account this read octet in order
	 * to calculate the proper tpdu length.
	 */
	if (ok) {
		unsigned int smsc_len = sdata[1] + 1;
		ofono_sms_deliver_notify(sd->sms, sdata + 1, length - 1,
						length - smsc_len - 1);
		ril_request_delete_sms_om_sim(sd, cbd->record);
	} else {
		ofono_error("cannot read sms from sim");
	}

	ril_sms_on_sim_req_free(cbd);
}

static void ril_sms_on_sim(GRilIoChannel *io, guint ril_event,
			const void *data, guint len, void *user_data)
{
	struct ril_sms *sd = user_data;
	struct ofono_sim *sim = ril_modem_ofono_sim(sd->modem);
	int data_len = 0, rec = 0;
	GRilIoParser rilp;

	ofono_info("new sms on sim");
	grilio_parser_init(&rilp, data, len);
	if (sim &&
		grilio_parser_get_int32(&rilp, &data_len) && data_len > 0 &&
		grilio_parser_get_int32(&rilp, &rec)) {
		DBG("rec %d", rec);
		if (sd->sim_context) {
			ofono_sim_read_record(sd->sim_context,
					SIM_EFSMS_FILEID,
					OFONO_SIM_FILE_STRUCTURE_FIXED,
					rec, EFSMS_LENGTH,
					sim_path, sizeof(sim_path),
					ril_sms_on_sim_cb,
					ril_sms_on_sim_req_new(sd,rec));
		}
	}
}

static gboolean ril_sms_register(gpointer user_data)
{
	struct ril_sms *sd = user_data;

	DBG("");
	GASSERT(sd->timer_id);
	sd->timer_id = 0;
	ofono_sms_register(sd->sms);

	/* Register event handlers */
	sd->event_id[SMS_EVENT_NEW_SMS] =
		grilio_channel_add_unsol_event_handler(sd->io, ril_sms_notify,
			RIL_UNSOL_RESPONSE_NEW_SMS, sd);
	sd->event_id[SMS_EVENT_NEW_STATUS_REPORT] =
		grilio_channel_add_unsol_event_handler(sd->io, ril_sms_notify,
			RIL_UNSOL_RESPONSE_NEW_SMS_STATUS_REPORT, sd);
	sd->event_id[SMS_EVENT_NEW_SMS_ON_SIM] =
		grilio_channel_add_unsol_event_handler(sd->io, ril_sms_on_sim,
			RIL_UNSOL_RESPONSE_NEW_SMS_ON_SIM, sd);

	/* Single-shot */
	return FALSE;
}

static int ril_sms_probe(struct ofono_sms *sms, unsigned int vendor,
								void *data)
{
	struct ril_modem *modem = data;
	struct ofono_sim *sim = ril_modem_ofono_sim(modem);
	struct ril_sms *sd = g_new0(struct ril_sms, 1);

	sd->modem = modem;
	sd->sms = sms;
	sd->io = grilio_channel_ref(ril_modem_io(modem));
	sd->sim_context = ofono_sim_context_create(sim);
	sd->q = grilio_queue_new(sd->io);
	sd->timer_id = g_idle_add(ril_sms_register, sd);
	ofono_sms_set_data(sms, sd);

	GASSERT(sd->sim_context);
	return 0;
}

static void ril_sms_remove(struct ofono_sms *sms)
{
	unsigned int i;
	struct ril_sms *sd = ril_sms_get_data(sms);

	DBG("");
	ofono_sms_set_data(sms, NULL);

	if (sd->sim_context) {
		ofono_sim_context_free(sd->sim_context);
	}

	for (i=0; i<G_N_ELEMENTS(sd->event_id); i++) {
		grilio_channel_remove_handler(sd->io, sd->event_id[i]);

	}

	if (sd->timer_id > 0) {
		g_source_remove(sd->timer_id);
	}

	grilio_channel_unref(sd->io);
	grilio_queue_cancel_all(sd->q, FALSE);
	grilio_queue_unref(sd->q);
	g_free(sd);
}

const struct ofono_sms_driver ril_sms_driver = {
	.name           = RILMODEM_DRIVER,
	.probe          = ril_sms_probe,
	.remove         = ril_sms_remove,
	.sca_query      = ril_sms_sca_query,
	.sca_set        = ril_sms_sca_set,
	.submit         = ril_sms_submit,
	.bearer_query   = NULL,          /* FIXME: needs investigation. */
	.bearer_set     = NULL
};

/*
 * Local Variables:
 * mode: C
 * c-basic-offset: 8
 * indent-tabs-mode: t
 * End:
 */
