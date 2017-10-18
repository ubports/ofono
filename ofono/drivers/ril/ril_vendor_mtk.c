/*
 *  oFono - Open Source Telephony - RIL-based devices
 *
 *  Copyright (C) 2016-2020 Jolla Ltd.
 *  Copyright (C) 2019-2020 Open Mobile Platform LLC.
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License version 2 as
 *  published by the Free Software Foundation.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 *  GNU General Public License for more details.
 */

#include "ril_vendor.h"
#include "ril_vendor_impl.h"
#include "ril_network.h"
#include "ril_data.h"
#include "ril_util.h"
#include "ril_log.h"

#include <grilio_channel.h>
#include <grilio_parser.h>
#include <grilio_request.h>
#include <grilio_queue.h>

#include <gutil_misc.h>

#include <ofono/watch.h>
#include <ofono/gprs.h>

#define SET_INITIAL_ATTACH_APN_TIMEOUT (20*1000)

enum ril_mtk_events {
	MTK_EVENT_REGISTRATION_SUSPENDED,
	MTK_EVENT_SET_ATTACH_APN,
	MTK_EVENT_PS_NETWORK_STATE_CHANGED,
	MTK_EVENT_INCOMING_CALL_INDICATION,
	MTK_EVENT_COUNT
};

typedef struct ril_vendor_mtk {
	RilVendor vendor;
	const struct ril_mtk_flavor *flavor;
	GRilIoQueue *q;
	struct ofono_watch *watch;
	guint set_initial_attach_apn_id;
	gboolean initial_attach_apn_ok;
	gulong ril_event_id[MTK_EVENT_COUNT];
	guint slot;
} RilVendorMtk;

typedef struct ril_vendor_mtk_auto {
	RilVendorMtk mtk;
	gulong detect_id;
} RilVendorMtkAuto;

typedef RilVendorClass RilVendorMtkClass;
typedef RilVendorMtkClass RilVendorMtkAutoClass;

#define RIL_VENDOR_TYPE_MTK (ril_vendor_mtk_get_type())
#define RIL_VENDOR_TYPE_MTK_AUTO (ril_vendor_mtk_auto_get_type())

G_DEFINE_TYPE(RilVendorMtk, ril_vendor_mtk, RIL_VENDOR_TYPE)
G_DEFINE_TYPE(RilVendorMtkAuto, ril_vendor_mtk_auto, RIL_VENDOR_TYPE_MTK)

#define RIL_VENDOR_MTK(obj) G_TYPE_CHECK_INSTANCE_CAST((obj), \
	RIL_VENDOR_TYPE_MTK, RilVendorMtk)
#define RIL_VENDOR_MTK_AUTO(obj) G_TYPE_CHECK_INSTANCE_CAST((obj), \
	RIL_VENDOR_TYPE_MTK_AUTO, RilVendorMtkAuto)

/* driver_data point this this: */
struct ril_mtk_flavor {
	const char *name;
	const struct ril_mtk_msg *msg;
	void (*build_attach_apn_req_fn)(GRilIoRequest *req, const char *apn,
				const char *username, const char *password,
				enum ril_auth auth, const char *proto);
	gboolean (*data_call_parse_fn)(struct ril_data_call *call,
				int version, GRilIoParser *rilp);
	gboolean (*signal_strength_fn)(struct ril_vendor_signal_strength *sig,
			GRilIoParser *rilp);
};

/* MTK specific RIL messages (actual codes differ from model to model!) */
struct ril_mtk_msg {
	guint request_resume_registration;
	guint request_set_call_indication;

	/* See ril_vendor_mtk_auto_detect_event */
#define unsol_msgs unsol_ps_network_state_changed
#define MTK_UNSOL_MSGS (4)

	guint unsol_ps_network_state_changed;
	guint unsol_registration_suspended;
	guint unsol_incoming_call_indication;
	guint unsol_set_attach_apn;
};

static const struct ril_mtk_msg msg_mtk1 = {
	.request_resume_registration = 2050,
	.request_set_call_indication = 2065,
	.unsol_ps_network_state_changed = 3012,
	.unsol_registration_suspended = 3021,
	.unsol_incoming_call_indication = 3037,
	.unsol_set_attach_apn = 3065
};

static const struct ril_mtk_msg msg_mtk2 = {
	.request_resume_registration = 2065,
	.request_set_call_indication = 2086,
	.unsol_ps_network_state_changed = 3015,
	.unsol_registration_suspended = 3024,
	.unsol_incoming_call_indication = 3042,
	.unsol_set_attach_apn = 3073
};

static const char *ril_vendor_mtk_request_to_string(RilVendor *vendor,
							guint request)
{
	RilVendorMtk *self = RIL_VENDOR_MTK(vendor);
	const struct ril_mtk_msg *msg = self->flavor->msg;

	if (request == msg->request_resume_registration) {
		return "MTK_RESUME_REGISTRATION";
	} else if (request == msg->request_set_call_indication) {
		return "MTK_SET_CALL_INDICATION";
	} else {
		return NULL;
	}
}

static const char *ril_vendor_mtk_unsol_msg_name(const struct ril_mtk_msg *msg,
								guint event)
{
	if (event == msg->unsol_ps_network_state_changed) {
		return "MTK_PS_NETWORK_STATE_CHANGED";
	} else if (event == msg->unsol_registration_suspended) {
		return "MTK_REGISTRATION_SUSPENDED";
	} else if (event == msg->unsol_set_attach_apn) {
		return "MTK_SET_ATTACH_APN";
	} else if (event == msg->unsol_incoming_call_indication) {
		return "MTK_INCOMING_CALL_INDICATION";
	} else {
		return NULL;
	}
}

static const char *ril_vendor_mtk_event_to_string(RilVendor *vendor,
								guint event)
{
	RilVendorMtk *self = RIL_VENDOR_MTK(vendor);

	return ril_vendor_mtk_unsol_msg_name(self->flavor->msg, event);
}

static void ril_vendor_mtk_registration_suspended(GRilIoChannel *io, guint id,
				const void *data, guint len, void *user_data)
{
	RilVendorMtk *self = RIL_VENDOR_MTK(user_data);
	const struct ril_mtk_msg *msg = self->flavor->msg;
	GRilIoParser rilp;
	int session_id;

	GASSERT(id == msg->unsol_registration_suspended);
	grilio_parser_init(&rilp, data, len);
	if (grilio_parser_get_int32(&rilp, NULL) &&
			grilio_parser_get_int32(&rilp, &session_id)) {
		GRilIoRequest *req = grilio_request_new();
		DBG("slot=%u,session_id=%d", self->slot, session_id);
		grilio_request_append_int32(req, 1);
		grilio_request_append_int32(req, session_id);
		grilio_queue_send_request(self->q, req,
					msg->request_resume_registration);
		grilio_request_unref(req);
	}
}

static void ril_vendor_mtk_build_attach_apn_req_1(GRilIoRequest *req,
		const char *apn, const char *username, const char *password,
		enum ril_auth auth, const char *proto)
{
	DBG("\"%s\" %s", apn, proto);
	grilio_request_append_utf8(req, apn);
	grilio_request_append_utf8(req, proto);
	grilio_request_append_utf8(req, proto); /* roamingProtocol */
	grilio_request_append_int32(req, auth);
	grilio_request_append_utf8(req, username);
	grilio_request_append_utf8(req, password);
	grilio_request_append_utf8(req, ""); /* operatorNumeric */
	grilio_request_append_int32(req, FALSE); /* canHandleIms */
	grilio_request_append_int32(req, -1); /* dualApnPlmnList */
}

static void ril_vendor_mtk_build_attach_apn_req_2(GRilIoRequest *req,
		const char *apn, const char *username, const char *password,
		enum ril_auth auth, const char *proto)
{
	DBG("\"%s\" %s", apn, proto);
	grilio_request_append_utf8(req, apn);
	grilio_request_append_utf8(req, proto);
	grilio_request_append_int32(req, auth);
	grilio_request_append_utf8(req, username);
	grilio_request_append_utf8(req, password);
	grilio_request_append_utf8(req, ""); /* operatorNumeric */
	grilio_request_append_int32(req, FALSE); /* canHandleIms */
	grilio_request_append_int32(req, -1); /* dualApnPlmnList */
}

static void ril_vendor_mtk_initial_attach_apn_resp(GRilIoChannel *io,
		int ril_status, const void *data, guint len, void *user_data)
{
	RilVendorMtk *self = RIL_VENDOR_MTK(user_data);

	GASSERT(self->set_initial_attach_apn_id);
	self->set_initial_attach_apn_id = 0;
	if (ril_status == RIL_E_SUCCESS) {
		DBG("ok");
		self->initial_attach_apn_ok = TRUE;
	}
}

static void ril_vendor_mtk_initial_attach_apn_check(RilVendorMtk *self)
{

	if (!self->set_initial_attach_apn_id && !self->initial_attach_apn_ok) {
		struct ofono_watch *watch = self->watch;
		const struct ofono_gprs_primary_context *pc =
			ofono_gprs_context_settings_by_type(watch->gprs,
					OFONO_GPRS_CONTEXT_TYPE_INTERNET);

		if (pc) {
			const char *username;
			const char *password;
			enum ril_auth auth;
			GRilIoRequest *req = grilio_request_new();

			if (pc->username[0] || pc->password[0]) {
				username = pc->username;
				password = pc->password;
				auth = ril_auth_method_from_ofono
					(pc->auth_method);
			} else {
				username = "";
				password = "";
				auth = RIL_AUTH_NONE;
			}

			self->flavor->build_attach_apn_req_fn(req,
					pc->apn, username, password, auth,
					ril_protocol_from_ofono(pc->proto));
			grilio_request_set_timeout(req,
					SET_INITIAL_ATTACH_APN_TIMEOUT);
			self->set_initial_attach_apn_id =
				grilio_queue_send_request_full(self->q, req,
					RIL_REQUEST_SET_INITIAL_ATTACH_APN,
					ril_vendor_mtk_initial_attach_apn_resp,
					NULL, self);
			grilio_request_unref(req);
		}
	}
}

static void ril_vendor_mtk_set_attach_apn(GRilIoChannel *io, guint id,
				const void *data, guint len, void *user_data)
{
	ril_vendor_mtk_initial_attach_apn_check(RIL_VENDOR_MTK(user_data));
}

static void ril_vendor_mtk_ps_network_state_changed(GRilIoChannel *io,
		guint id, const void *data, guint len, void *user_data)
{
	ril_network_query_registration_state(RIL_VENDOR(user_data)->network);
}

static void ril_vendor_mtk_incoming_call_indication(GRilIoChannel *io, guint id,
				const void *data, guint len, void *user_data)
{
	RilVendorMtk *self = RIL_VENDOR_MTK(user_data);
	const struct ril_mtk_msg *msg = self->flavor->msg;
	GRilIoRequest* req = NULL;

	GASSERT(id == msg->unsol_incoming_call_indication);

	if (msg->request_set_call_indication) {
		int nparams, cid, seq;
		gchar *call_id = NULL, *seq_no = NULL;
		GRilIoParser rilp;

		grilio_parser_init(&rilp, data, len);

		if (grilio_parser_get_int32(&rilp, &nparams) && nparams >= 5 &&
			(call_id = grilio_parser_get_utf8(&rilp)) != NULL &&
			grilio_parser_skip_string(&rilp) /* number */ &&
			grilio_parser_skip_string(&rilp) /* type */ &&
			grilio_parser_skip_string(&rilp) /* call_mode */ &&
			(seq_no = grilio_parser_get_utf8(&rilp)) != NULL &&
				gutil_parse_int(call_id, 10, &cid) &&
				gutil_parse_int(seq_no, 10, &seq)) {

			DBG("slot=%u,cid=%d,seq=%d", self->slot, cid, seq);
			req = grilio_request_new();
			grilio_request_append_int32(req, 3); /* Param count */
			/* mode - IMS_ALLOW_INCOMING_CALL_INDICATION: */
			grilio_request_append_int32(req, 0);
			grilio_request_append_int32(req, cid);
			grilio_request_append_int32(req, seq);
		} else {
			DBG("failed to parse INCOMING_CALL_INDICATION");
		}

		g_free(call_id);
		g_free(seq_no);
	}

	if (req) {
		grilio_queue_send_request(self->q, req,
					msg->request_set_call_indication);
		grilio_request_unref(req);
	} else {
		/* Let ril_voicecall.c know that something happened */
		grilio_channel_inject_unsol_event(io,
			RIL_UNSOL_RESPONSE_CALL_STATE_CHANGED, NULL, 0);
	}
}

static GRilIoRequest *ril_vendor_mtk_data_call_req(RilVendor *vendor, int tech,
			enum ril_data_profile profile, const char *apn,
			const char *username, const char *password,
			enum ril_auth auth, const char *proto)
{
	RilVendorMtk *self = RIL_VENDOR_MTK(vendor);
	GRilIoRequest *req = grilio_request_new();

	grilio_request_append_int32(req, 8); /* Number of parameters */
	grilio_request_append_format(req, "%d", tech);
	grilio_request_append_format(req, "%d", profile);
	grilio_request_append_utf8(req, apn);
	grilio_request_append_utf8(req, username);
	grilio_request_append_utf8(req, password);
	grilio_request_append_format(req, "%d", auth);
	grilio_request_append_utf8(req, proto);
	grilio_request_append_format(req, "%d", self->slot+1);
	return req;
}

static GRilIoRequest *ril_vendor_mtk_set_attach_apn_req(RilVendor *vendor,
			const char *apn, const char *user, const char *pass,
			enum ril_auth auth, const char *prot)
{
	RilVendorMtk *self = RIL_VENDOR_MTK(vendor);
	GRilIoRequest *req = grilio_request_new();

	self->flavor->build_attach_apn_req_fn(req, apn, user, pass, auth, prot);
	return req;
}

static gboolean ril_vendor_mtk_data_call_parse_v6(struct ril_data_call *call,
					int version, GRilIoParser *rilp)
{
	if (version < 11) {
		int prot;
		char *prot_str;
		guint32 status = PDP_FAIL_ERROR_UNSPECIFIED;
		guint32 active = RIL_DATA_CALL_INACTIVE;

		/* RIL_Data_Call_Response_v6 with MTK specific additions */
		grilio_parser_get_uint32(rilp, &status);
		grilio_parser_get_int32(rilp, &call->retry_time);
		grilio_parser_get_int32(rilp, &call->cid);
		grilio_parser_get_uint32(rilp, &active);
		grilio_parser_get_int32(rilp, &call->mtu); /* MTK specific */
		prot_str = grilio_parser_get_utf8(rilp);
		prot = ril_protocol_to_ofono(prot_str);
		g_free(prot_str);

		if (prot >= 0) {
			call->ifname = grilio_parser_get_utf8(rilp);
			call->addresses = grilio_parser_split_utf8(rilp, " ");
			call->dnses = grilio_parser_split_utf8(rilp, " ");
			call->gateways = grilio_parser_split_utf8(rilp, " ");
			if (call->ifname && call->addresses) {
				call->prot = prot;
				call->status = status;
				call->active = active;
				return TRUE;
			}
		}
	}
	return FALSE;
}

static gboolean ril_vendor_mtk_data_call_parse(RilVendor *vendor,
			struct ril_data_call *call, int version,
			GRilIoParser *rilp)
{
	const struct ril_mtk_flavor *flavor = RIL_VENDOR_MTK(vendor)->flavor;

	return flavor->data_call_parse_fn ?
			flavor->data_call_parse_fn(call, version, rilp) :
			RIL_VENDOR_CLASS(ril_vendor_mtk_parent_class)->
				data_call_parse(vendor, call, version, rilp);
}

static gboolean ril_vendor_mtk_signal_strength_1
		(struct ril_vendor_signal_strength *signal, GRilIoParser *rilp)
{
	if (grilio_parser_bytes_remaining(rilp) == 64) {
		gint32 rsrp = 0, rssi = 0;

		/* GW_SignalStrength */
		grilio_parser_get_int32(rilp, &signal->gsm);
		grilio_parser_get_int32(rilp, NULL);   /* bitErrorRate */

		/* CDMA_SignalStrength */
		grilio_parser_get_int32(rilp, NULL);   /* dbm */
		grilio_parser_get_int32(rilp, NULL);   /* ecio */

		/* EVDO_SignalStrength */
		grilio_parser_get_int32(rilp, NULL);   /* dbm */
		grilio_parser_get_int32(rilp, NULL);   /* ecio */
		grilio_parser_get_int32(rilp, NULL);   /* signalNoiseRatio */

		/* LTE_SignalStrength */
		grilio_parser_get_int32(rilp, &signal->lte);
		grilio_parser_get_int32(rilp, &rsrp);  /* rsrp */
		grilio_parser_get_int32(rilp, NULL);   /* rsrq */
		grilio_parser_get_int32(rilp, NULL);   /* rssnr */
		grilio_parser_get_int32(rilp, NULL);   /* cqi */

		/* ???? */
		grilio_parser_get_int32(rilp, NULL);
		grilio_parser_get_int32(rilp, &rssi);
		grilio_parser_get_int32(rilp, NULL);
		grilio_parser_get_int32(rilp, NULL);

		signal->qdbm = (rssi > 0 && rssi != INT_MAX) ? (-4 * rssi) :
			(rsrp >= 44 && rsrp <= 140) ? (-4 * rsrp) : 0;
		return TRUE;
	}
	return FALSE;
}

static gboolean ril_vendor_mtk_signal_strength_2
		(struct ril_vendor_signal_strength *signal, GRilIoParser *rilp)
{
	if (grilio_parser_bytes_remaining(rilp) == 64) {
		gint32 rsrp = 0, is_gsm = 0, rssi_qdbm = 0;

		/* GW_SignalStrength */
		grilio_parser_get_int32(rilp, &signal->gsm);
		grilio_parser_get_int32(rilp, NULL);   /* bitErrorRate */

		/* CDMA_SignalStrength */
		grilio_parser_get_int32(rilp, NULL);   /* dbm */
		grilio_parser_get_int32(rilp, NULL);   /* ecio */

		/* EVDO_SignalStrength */
		grilio_parser_get_int32(rilp, NULL);   /* dbm */
		grilio_parser_get_int32(rilp, NULL);   /* ecio */
		grilio_parser_get_int32(rilp, NULL);   /* signalNoiseRatio */

		/* LTE_SignalStrength */
		grilio_parser_get_int32(rilp, &signal->lte);
		grilio_parser_get_int32(rilp, &rsrp);  /* rsrp */
		grilio_parser_get_int32(rilp, NULL);   /* rsrq */
		grilio_parser_get_int32(rilp, NULL);   /* rssnr */
		grilio_parser_get_int32(rilp, NULL);   /* cqi */

		/* WCDMA_SignalStrength */
		grilio_parser_get_int32(rilp, &is_gsm);    /* isGsm */
		grilio_parser_get_int32(rilp, &rssi_qdbm); /* rssiQdbm */
		grilio_parser_get_int32(rilp, NULL);       /* rscpQdbm */
		grilio_parser_get_int32(rilp, NULL);       /* Ecn0Qdbm*/

		signal->qdbm = (is_gsm == 1 && rssi_qdbm < 0) ? rssi_qdbm :
			(rsrp >= 44 && rsrp <= 140) ? (-4 * rsrp) : 0;
		return TRUE;
	}
	return FALSE;
}

static gboolean ril_vendor_mtk_signal_strength_parse(RilVendor *vendor,
			struct ril_vendor_signal_strength *signal,
			GRilIoParser *rilp)
{
	const struct ril_mtk_flavor *flavor = RIL_VENDOR_MTK(vendor)->flavor;

	return flavor->signal_strength_fn ?
			flavor->signal_strength_fn(signal, rilp) :
			RIL_VENDOR_CLASS(ril_vendor_mtk_parent_class)->
				signal_strength_parse(vendor, signal, rilp);
}

static void ril_vendor_mtk_get_defaults(struct ril_vendor_defaults *defaults)
{
	/*
	 * With most Qualcomm RIL implementations, querying available band
	 * modes causes some magic Android properties to appear. Otherwise
	 * this request is pretty harmless and useless.
	 *
	 * Most MediaTek RIL implementations don't support this request and
	 * don't even bother to reply which slows things down because we wait
	 * for this request to complete at startup.
	 */
	defaults->query_available_band_mode = FALSE;
	defaults->empty_pin_query = FALSE;
	defaults->legacy_imei_query = TRUE;
	defaults->force_gsm_when_radio_off = FALSE;
	defaults->replace_strange_oper = TRUE;
}

static void ril_vendor_mtk_base_init(RilVendorMtk *self, GRilIoChannel *io,
		const char *path, const struct ril_slot_config *config)
{
	ril_vendor_init_base(&self->vendor, io);
	self->q = grilio_queue_new(io);
	self->watch = ofono_watch_new(path);
	self->slot = config->slot;
}

static void ril_vendor_mtk_set_flavor(RilVendorMtk *self,
				const struct ril_mtk_flavor *flavor)
{
	GRilIoChannel *io = self->vendor.io;
	const struct ril_mtk_msg *msg = flavor->msg;

	grilio_channel_remove_all_handlers(io, self->ril_event_id);
	self->flavor = flavor;
	self->ril_event_id[MTK_EVENT_REGISTRATION_SUSPENDED] =
			grilio_channel_add_unsol_event_handler(io,
				ril_vendor_mtk_registration_suspended,
				msg->unsol_registration_suspended, self);
	if (msg->unsol_set_attach_apn) {
		self->ril_event_id[MTK_EVENT_SET_ATTACH_APN] =
			grilio_channel_add_unsol_event_handler(io,
				ril_vendor_mtk_set_attach_apn,
				msg->unsol_set_attach_apn, self);
	}
	if (msg->unsol_ps_network_state_changed) {
		self->ril_event_id[MTK_EVENT_PS_NETWORK_STATE_CHANGED] =
			grilio_channel_add_unsol_event_handler(io,
				ril_vendor_mtk_ps_network_state_changed,
				msg->unsol_ps_network_state_changed, self);
	}
	if (msg->unsol_incoming_call_indication) {
		self->ril_event_id[MTK_EVENT_INCOMING_CALL_INDICATION] =
			grilio_channel_add_unsol_event_handler(io,
				ril_vendor_mtk_incoming_call_indication,
				msg->unsol_incoming_call_indication, self);
	}
}

static RilVendor *ril_vendor_mtk_create_from_data(const void *driver_data,
					GRilIoChannel *io, const char *path,
					const struct ril_slot_config *config)
{
	const struct ril_mtk_flavor *flavor = driver_data;
	RilVendorMtk *mtk = g_object_new(RIL_VENDOR_TYPE_MTK, NULL);

	ril_vendor_mtk_base_init(mtk, io, path, config);
	ril_vendor_mtk_set_flavor(mtk, flavor);
	DBG("%s slot %u", flavor->name, mtk->slot);
	return &mtk->vendor;
}

static void ril_vendor_mtk_init(RilVendorMtk *self)
{
}

static void ril_vendor_mtk_finalize(GObject* object)
{
	RilVendorMtk *self = RIL_VENDOR_MTK(object);
	RilVendor *vendor = &self->vendor;

	DBG("slot %u", self->slot);
	grilio_queue_cancel_all(self->q, FALSE);
	grilio_queue_unref(self->q);
	ofono_watch_unref(self->watch);
	grilio_channel_remove_all_handlers(vendor->io, self->ril_event_id);
	G_OBJECT_CLASS(ril_vendor_mtk_parent_class)->finalize(object);
}

static void ril_vendor_mtk_class_init(RilVendorMtkClass* klass)
{
	G_OBJECT_CLASS(klass)->finalize = ril_vendor_mtk_finalize;
	klass->request_to_string = ril_vendor_mtk_request_to_string;
	klass->event_to_string = ril_vendor_mtk_event_to_string;
	klass->set_attach_apn_req = ril_vendor_mtk_set_attach_apn_req;
	klass->data_call_req = ril_vendor_mtk_data_call_req;
	klass->data_call_parse = ril_vendor_mtk_data_call_parse;
	klass->signal_strength_parse = ril_vendor_mtk_signal_strength_parse;
}

static const struct ril_mtk_flavor ril_mtk_flavor1 = {
	.name                    = "mtk1",
	.msg                     = &msg_mtk1,
	.build_attach_apn_req_fn = &ril_vendor_mtk_build_attach_apn_req_1,
	.data_call_parse_fn      = NULL,
	.signal_strength_fn      = &ril_vendor_mtk_signal_strength_1
};

static const struct ril_mtk_flavor ril_mtk_flavor2 = {
	.name                    = "mtk2",
	.msg                     = &msg_mtk2,
	.build_attach_apn_req_fn = &ril_vendor_mtk_build_attach_apn_req_2,
	.data_call_parse_fn      = &ril_vendor_mtk_data_call_parse_v6,
	.signal_strength_fn      = &ril_vendor_mtk_signal_strength_2
};

#define DEFAULT_MTK_TYPE (&ril_mtk_flavor1)

static const struct ril_mtk_flavor *mtk_flavors [] = {
	&ril_mtk_flavor1,
	&ril_mtk_flavor2
};

RIL_VENDOR_DRIVER_DEFINE(ril_vendor_driver_mtk1) {
	.name               = "mtk1",
	.driver_data        = &ril_mtk_flavor1,
	.get_defaults       = ril_vendor_mtk_get_defaults,
	.create_vendor      = ril_vendor_mtk_create_from_data
};

RIL_VENDOR_DRIVER_DEFINE(ril_vendor_driver_mtk2) {
	.name               = "mtk2",
	.driver_data        = &ril_mtk_flavor2,
	.get_defaults       = ril_vendor_mtk_get_defaults,
	.create_vendor      = ril_vendor_mtk_create_from_data
};

/* Auto-selection */

static void ril_vendor_mtk_auto_detect_event(GRilIoChannel *io, guint id,
				const void *data, guint len, void *user_data)
{
	RilVendorMtkAuto *self = RIL_VENDOR_MTK_AUTO(user_data);
	guint i;

	for (i = 0; i < G_N_ELEMENTS(mtk_flavors); i++) {
		const struct ril_mtk_flavor *flavor = mtk_flavors[i];
		const struct ril_mtk_msg *msg = flavor->msg;
		const guint *ids = &msg->unsol_msgs;
		guint j;

		for (j = 0; j < MTK_UNSOL_MSGS; j++) {
			if (ids[j] == id) {
				DBG("event %u is %s %s", id, flavor->name,
					ril_vendor_mtk_unsol_msg_name(msg,id));
				ril_vendor_mtk_set_flavor(&self->mtk, flavor);
				/* We are done */
				grilio_channel_remove_handler(io,
							self->detect_id);
				self->detect_id = 0;
				/* And repeat the event to invoke the handler */
				grilio_channel_inject_unsol_event(io, id,
								data, len);
				return;
			}
		}
	}
}

static void ril_vendor_mtk_auto_init(RilVendorMtkAuto *self)
{
}

static void ril_vendor_mtk_auto_finalize(GObject* object)
{
	RilVendorMtkAuto *self = RIL_VENDOR_MTK_AUTO(object);

	DBG("slot %u", self->mtk.slot);
	grilio_channel_remove_handler(self->mtk.vendor.io, self->detect_id);
	G_OBJECT_CLASS(ril_vendor_mtk_auto_parent_class)->finalize(object);
}

static void ril_vendor_mtk_auto_class_init(RilVendorMtkAutoClass* klass)
{
	G_OBJECT_CLASS(klass)->finalize = ril_vendor_mtk_auto_finalize;
}

static RilVendor *ril_vendor_mtk_auto_create_vendor(const void *driver_data,
					GRilIoChannel *io, const char *path,
					const struct ril_slot_config *config)
{
	RilVendorMtkAuto *self = g_object_new(RIL_VENDOR_TYPE_MTK_AUTO, NULL);
	RilVendorMtk *mtk = &self->mtk;

	ril_vendor_mtk_base_init(mtk, io, path, config);
	ril_vendor_mtk_set_flavor(mtk, DEFAULT_MTK_TYPE);
	DBG("%s slot %u", mtk->flavor->name, mtk->slot);

	/*
	 * Subscribe for (all) unsolicited events. Keep on listening until
	 * we receive an MTK specific event that tells us which particular
	 * kind of MTK adaptation we are using.
	 */
	self->detect_id = grilio_channel_add_unsol_event_handler(io,
				ril_vendor_mtk_auto_detect_event, 0, self);
	return &mtk->vendor;
}

RIL_VENDOR_DRIVER_DEFINE(ril_vendor_driver_mtk) {
	.name               = "mtk",
	.get_defaults       = ril_vendor_mtk_get_defaults,
	.create_vendor      = ril_vendor_mtk_auto_create_vendor
};

/*
 * Local Variables:
 * mode: C
 * c-basic-offset: 8
 * indent-tabs-mode: t
 * End:
 */
