/*
 *  oFono - Open Source Telephony - RIL-based devices
 *
 *  Copyright (C) 2016-2018 Jolla Ltd.
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

#include "ril_plugin.h"
#include "ril_vendor.h"
#include "ril_network.h"
#include "ril_data.h"
#include "ril_log.h"

#include "sailfish_watch.h"

#include <grilio_channel.h>
#include <grilio_parser.h>
#include <grilio_request.h>
#include <grilio_queue.h>

#include <gutil_macros.h>

#include "ofono.h"

#define SET_INITIAL_ATTACH_APN_TIMEOUT (20*1000)

enum ril_mtk_watch_events {
	WATCH_EVENT_IMSI_CHANGED,
	WATCH_EVENT_COUNT
};

enum ril_mtk_network_events {
	NETWORK_EVENT_PREF_MODE_CHANGED,
	NETWORK_EVENT_COUNT
};

enum ril_mtk_events {
	MTK_EVENT_REGISTRATION_SUSPENDED,
	MTK_EVENT_SET_ATTACH_APN,
	MTK_EVENT_PS_NETWORK_STATE_CHANGED,
	MTK_EVENT_COUNT
};

struct ril_vendor_hook_mtk {
	struct ril_vendor_hook hook;
	const struct ril_mtk_msg *msg;
	GRilIoQueue *q;
	GRilIoChannel *io;
	struct ril_network *network;
	struct sailfish_watch *watch;
	guint set_initial_attach_apn_id;
	gboolean initial_attach_apn_ok;
	gulong network_event_id[NETWORK_EVENT_COUNT];
	gulong watch_event_id[WATCH_EVENT_COUNT];
	gulong ril_event_id[MTK_EVENT_COUNT];
	guint slot;
};

/* driver_data point this this: */
struct ril_vendor_mtk_driver_data {
	const char *name;
	const struct ril_mtk_msg *msg;
	const struct ril_vendor_hook_proc *proc;
};

/* MTK specific RIL messages (actual codes differ from model to model!) */
struct ril_mtk_msg {
	gboolean attach_apn_has_roaming_protocol;
	guint request_resume_registration;
	guint unsol_network_info;
	guint unsol_ps_network_state_changed;
	guint unsol_registration_suspended;
	guint unsol_ims_registration_info;
	guint unsol_volte_eps_network_feature_support;
	guint unsol_emergency_bearer_support_notify;
	guint unsol_set_attach_apn;
};

/* Fly FS522 Cirrus 14 */
static const struct ril_mtk_msg mtk_msg_mt6737 = {
	.attach_apn_has_roaming_protocol = TRUE,
	.request_resume_registration = 2050,
	.unsol_network_info = 3001,
	.unsol_ps_network_state_changed = 3012,
	.unsol_registration_suspended = 3021,
	.unsol_ims_registration_info = 3029,
	.unsol_volte_eps_network_feature_support = 3042,
	.unsol_emergency_bearer_support_notify = 3052,
	.unsol_set_attach_apn = 3065
};

/* MT8735 Tablet */
static const struct ril_mtk_msg mtk_msg_mt8735 = {
	.attach_apn_has_roaming_protocol = FALSE,
	.request_resume_registration = 2065,
	.unsol_network_info = 3001,
	.unsol_ps_network_state_changed = 3015,
	.unsol_ims_registration_info = 3033,
	.unsol_volte_eps_network_feature_support = 3048,
	.unsol_emergency_bearer_support_notify = 3059,
	.unsol_registration_suspended = 3024,
	.unsol_set_attach_apn = 3073
};

static inline struct ril_vendor_hook_mtk *ril_vendor_hook_mtk_cast
						(struct ril_vendor_hook *hook)
{
	return G_CAST(hook, struct ril_vendor_hook_mtk, hook);
}

static const char *ril_vendor_mtk_request_to_string
				(struct ril_vendor_hook *hook, guint request)
{
	struct ril_vendor_hook_mtk *self = ril_vendor_hook_mtk_cast(hook);
	const struct ril_mtk_msg *msg = self->msg;

	if (request == msg->request_resume_registration) {
		return "MTK_RESUME_REGISTRATION";
	} else {
		return NULL;
	}
}

static const char *ril_vendor_mtk_event_to_string(struct ril_vendor_hook *hook,
								guint event)
{
	struct ril_vendor_hook_mtk *self = ril_vendor_hook_mtk_cast(hook);
	const struct ril_mtk_msg *msg = self->msg;

	if (event == msg->unsol_network_info) {
		return "MTK_NETWORK_INFO";
	} else if (event == msg->unsol_ps_network_state_changed) {
		return "MTK_PS_NETWORK_STATE_CHANGED";
	} else if (event == msg->unsol_registration_suspended) {
		return "MTK_REGISTRATION_SUSPENDED";
	} else if (event == msg->unsol_ims_registration_info) {
		return "MTK_IMS_REGISTRATION_INFO";
	} else if (event == msg->unsol_volte_eps_network_feature_support) {
		return "MTK_VOLTE_EPS_NETWORK_FEATURE_SUPPORT";
	} else if (event == msg->unsol_emergency_bearer_support_notify) {
		return "MTK_EMERGENCY_BEARER_SUPPORT_NOTIFY";
	} else if (event == msg->unsol_set_attach_apn) {
		return "MTK_SET_ATTACH_APN";
	} else {
		return NULL;
	}
}

static void ril_vendor_mtk_registration_suspended(GRilIoChannel *io, guint id,
				const void *data, guint len, void *user_data)
{
	struct ril_vendor_hook_mtk *self = user_data;
	const struct ril_mtk_msg *msg = self->msg;
	GRilIoParser rilp;
	int session_id;

	GASSERT(id == msg->unsol_registration_suspended);
	grilio_parser_init(&rilp, data, len);
	if (grilio_parser_get_int32(&rilp, NULL) &&
			grilio_parser_get_int32(&rilp, &session_id)) {
		GRilIoRequest* req = grilio_request_new();
		DBG("slot=%u,session_id=%d", self->slot, session_id);
		grilio_request_append_int32(req, 1);
		grilio_request_append_int32(req, session_id);
		grilio_queue_send_request(self->q, req,
					msg->request_resume_registration);
		grilio_request_unref(req);
	}
}

static GRilIoRequest *ril_vendor_mtk_build_set_attach_apn_req
			(const struct ofono_gprs_primary_context *pc,
						gboolean roamingProtocol)
{
	GRilIoRequest *req = grilio_request_new();
	const char *proto = ril_data_ofono_protocol_to_ril(pc->proto);

	DBG("%s %d", pc->apn, roamingProtocol);
	grilio_request_append_utf8(req, pc->apn);       /* apn */
	grilio_request_append_utf8(req, proto);         /* protocol */
	if (roamingProtocol) {
		grilio_request_append_utf8(req, proto); /* roamingProtocol */
	}

	if (pc->username[0]) {
		int auth;

		switch (pc->auth_method) {
		case OFONO_GPRS_AUTH_METHOD_ANY:
			auth = RIL_AUTH_BOTH;
			break;
		case OFONO_GPRS_AUTH_METHOD_NONE:
			auth = RIL_AUTH_NONE;
			break;
		case OFONO_GPRS_AUTH_METHOD_CHAP:
			auth = RIL_AUTH_CHAP;
			break;
		case OFONO_GPRS_AUTH_METHOD_PAP:
			auth = RIL_AUTH_PAP;
			break;
		default:
			auth = RIL_AUTH_NONE;
			break;
		}

		grilio_request_append_int32(req, auth);
		grilio_request_append_utf8(req, pc->username);
		grilio_request_append_utf8(req, pc->password);
	} else {
		grilio_request_append_int32(req, RIL_AUTH_NONE);
		grilio_request_append_utf8(req, "");
		grilio_request_append_utf8(req, "");
	}

	grilio_request_append_utf8(req, ""); /* operatorNumeric */
	grilio_request_append_int32(req, FALSE); /* canHandleIms */
	grilio_request_append_int32(req, 0); /* Some sort of count */

	return req;
}

static const struct ofono_gprs_primary_context *ril_vendor_mtk_internet_context
					(struct ril_vendor_hook_mtk *self)
{
	struct sailfish_watch *watch = self->watch;

	if (watch->imsi) {
		struct ofono_atom * atom = __ofono_modem_find_atom(watch->modem,
							OFONO_ATOM_TYPE_GPRS);

		if (atom) {
			return __ofono_gprs_context_settings_by_type
				(__ofono_atom_get_data(atom),
					OFONO_GPRS_CONTEXT_TYPE_INTERNET);
		}
	}

	return NULL;
}

static void ril_vendor_mtk_initial_attach_apn_resp(GRilIoChannel *io,
		int ril_status, const void *data, guint len, void *user_data)
{
	struct ril_vendor_hook_mtk *self = user_data;

	GASSERT(self->set_initial_attach_apn_id);
	self->set_initial_attach_apn_id = 0;
	if (ril_status == RIL_E_SUCCESS) {
		DBG("ok");
		self->initial_attach_apn_ok = TRUE;
	}
}

static void ril_vendor_mtk_initial_attach_apn_check
					(struct ril_vendor_hook_mtk *self)
{

	if (!self->set_initial_attach_apn_id && !self->initial_attach_apn_ok) {
		const struct ofono_gprs_primary_context *pc =
			ril_vendor_mtk_internet_context(self);

		if (pc) {
			GRilIoRequest *req =
				ril_vendor_mtk_build_set_attach_apn_req(pc,
				self->msg->attach_apn_has_roaming_protocol);

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

static void ril_vendor_mtk_initial_attach_apn_reset
					(struct ril_vendor_hook_mtk *self)
{
	self->initial_attach_apn_ok = FALSE;
	if (self->set_initial_attach_apn_id) {
		grilio_queue_cancel_request(self->q,
			self->set_initial_attach_apn_id, FALSE);
		self->set_initial_attach_apn_id = 0;
	}
}

static void ril_vendor_mtk_watch_imsi_changed(struct sailfish_watch *watch,
							void *user_data)
{
	struct ril_vendor_hook_mtk *self = user_data;

	if (watch->imsi) {
		ril_vendor_mtk_initial_attach_apn_check(self);
	} else {
		ril_vendor_mtk_initial_attach_apn_reset(self);
	}
}

static void ril_vendor_mtk_network_pref_mode_changed(struct ril_network *net,
							void *user_data)
{
	struct ril_vendor_hook_mtk *self = user_data;

	if (net->pref_mode >= OFONO_RADIO_ACCESS_MODE_LTE) {
		ril_vendor_mtk_initial_attach_apn_check(self);
	} else {
		ril_vendor_mtk_initial_attach_apn_reset(self);
	}
}

static void ril_vendor_mtk_set_attach_apn(GRilIoChannel *io, guint id,
				const void *data, guint len, void *self)
{
	ril_vendor_mtk_initial_attach_apn_check(self);
}

static void ril_vendor_mtk_ps_network_state_changed(GRilIoChannel *io,
		guint id, const void *data, guint len, void *user_data)
{
	struct ril_vendor_hook_mtk *self = user_data;

	ril_network_query_registration_state(self->network);
}

static GRilIoRequest* ril_vendor_mtk_data_call_req
	(struct ril_vendor_hook *hook, int tech, const char *profile,
		const char *apn, const char *username, const char *password,
		enum ril_auth auth, const char *proto)
{
	struct ril_vendor_hook_mtk *self = ril_vendor_hook_mtk_cast(hook);
	GRilIoRequest *req = grilio_request_new();

	grilio_request_append_int32(req, 8); /* Number of parameters */
	grilio_request_append_format(req, "%d", tech);
	grilio_request_append_utf8(req, profile);
	grilio_request_append_utf8(req, apn);
	grilio_request_append_utf8(req, username);
	grilio_request_append_utf8(req, password);
	grilio_request_append_format(req, "%d", auth);
	grilio_request_append_utf8(req, proto);
	grilio_request_append_format(req, "%d", self->slot+1);
	return req;
}

static gboolean ril_vendor_mtk_data_call_parse_v6(struct ril_vendor_hook *hook,
			struct ril_data_call *call, int version,
			GRilIoParser *rilp)
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
		prot = ril_data_protocol_to_ofono(prot_str);
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

static void ril_vendor_mtk_get_defaults(struct ril_vendor_defaults *defaults)
{
	defaults->empty_pin_query = FALSE;
	defaults->legacy_imei_query = TRUE;
}

static struct ril_vendor_hook *ril_vendor_mtk_create_hook_from_data
		(const void *driver_data, GRilIoChannel *io, const char *path,
					const struct ril_slot_config *config,
					struct ril_network *network)
{
	const struct ril_vendor_mtk_driver_data *mtk_driver_data = driver_data;
	const struct ril_mtk_msg *msg = mtk_driver_data->msg;
	struct ril_vendor_hook_mtk *self =
		g_new0(struct ril_vendor_hook_mtk, 1);

	self->msg = msg;
	self->q = grilio_queue_new(io);
	self->io = grilio_channel_ref(io);
	self->watch = sailfish_watch_new(path);
	self->slot = config->slot;
	self->network = ril_network_ref(network);
	self->watch_event_id[WATCH_EVENT_IMSI_CHANGED] =
			sailfish_watch_add_imsi_changed_handler(self->watch,
				ril_vendor_mtk_watch_imsi_changed, self);
	self->network_event_id[NETWORK_EVENT_PREF_MODE_CHANGED] =
			ril_network_add_pref_mode_changed_handler(self->network,
				ril_vendor_mtk_network_pref_mode_changed, self);
	self->ril_event_id[MTK_EVENT_REGISTRATION_SUSPENDED] =
			grilio_channel_add_unsol_event_handler(self->io,
				ril_vendor_mtk_registration_suspended,
				msg->unsol_registration_suspended, self);
	if (msg->unsol_set_attach_apn) {
		self->ril_event_id[MTK_EVENT_SET_ATTACH_APN] =
			grilio_channel_add_unsol_event_handler(self->io,
				ril_vendor_mtk_set_attach_apn,
				msg->unsol_set_attach_apn, self);
	}
	if (msg->unsol_ps_network_state_changed) {
		self->ril_event_id[MTK_EVENT_PS_NETWORK_STATE_CHANGED] =
			grilio_channel_add_unsol_event_handler(self->io,
				ril_vendor_mtk_ps_network_state_changed,
				msg->unsol_ps_network_state_changed, self);
	}
	DBG("%s slot %u", mtk_driver_data->name, self->slot);
	return ril_vendor_hook_init(&self->hook, mtk_driver_data->proc);
}

static void ril_vendor_mtk_free(struct ril_vendor_hook *hook)
{
	struct ril_vendor_hook_mtk *self = ril_vendor_hook_mtk_cast(hook);

	DBG("slot %u", self->slot);
	grilio_queue_cancel_all(self->q, FALSE);
	grilio_channel_remove_all_handlers(self->io, self->ril_event_id);
	grilio_queue_unref(self->q);
	grilio_channel_unref(self->io);
	sailfish_watch_remove_all_handlers(self->watch, self->watch_event_id);
	sailfish_watch_unref(self->watch);
	ril_network_remove_all_handlers(self->network, self->network_event_id);
	ril_network_unref(self->network);
	g_free(self);
}

static const struct ril_vendor_hook_proc ril_vendor_mtk_hook_base_proc = {
	.free               = ril_vendor_mtk_free,
	.request_to_string  = ril_vendor_mtk_request_to_string,
	.event_to_string    = ril_vendor_mtk_event_to_string,
	.data_call_req      = ril_vendor_mtk_data_call_req
};

static const struct ril_vendor_driver ril_vendor_mtk_base = {
	.get_defaults       = ril_vendor_mtk_get_defaults,
	.create_hook        = ril_vendor_mtk_create_hook_from_data
};

static const struct ril_vendor_mtk_driver_data ril_vendor_mtk_mt6737_data = {
	.name               = "MT6737",
	.msg                = &mtk_msg_mt6737,
	.proc               = &ril_vendor_mtk_hook_base_proc
};

static struct ril_vendor_hook_proc ril_vendor_mtk_mt8735_proc = {
	.base               = &ril_vendor_mtk_hook_base_proc,
	.data_call_parse    = ril_vendor_mtk_data_call_parse_v6
};

static const struct ril_vendor_mtk_driver_data ril_vendor_mtk_mt8735_data = {
	.name               = "MT8735",
	.msg                = &mtk_msg_mt8735,
	.proc               = &ril_vendor_mtk_mt8735_proc
};

RIL_VENDOR_DRIVER_DEFINE(ril_vendor_driver_mt6737) {
	.name               = "mt6737t",
	.driver_data        = &ril_vendor_mtk_mt6737_data,
	.base               = &ril_vendor_mtk_base
};

RIL_VENDOR_DRIVER_DEFINE(ril_vendor_driver_mt8735) {
	.name               = "mt8735",
	.driver_data        = &ril_vendor_mtk_mt8735_data,
	.base               = &ril_vendor_mtk_base
};

#define DEFAULT_MTK_DRIVER (&ril_vendor_driver_mt6737)

static const struct ril_vendor_driver *mtk_hw_drivers [] = {
	&ril_vendor_driver_mt6737,
	&ril_vendor_driver_mt8735
};

/* Automatic driver selection based on /proc/cpuinfo */

static GString *ril_vendor_mtk_read_line(GString *buf, FILE *f)
{
	int c = fgetc(f);

	g_string_truncate(buf, 0);
	if (c != EOF) {
		/* Read the line char by char until we hit EOF or EOL */
		while (c != EOF && c != '\r' && c != '\n') {
			g_string_append_c(buf, c);
			c = fgetc(f);
		}
		/* Skip EOL characters */
		while (c != EOF && (c == '\r' || c == '\n')) {
			c = fgetc(f);
		}
		/* Unget the last char (the first char of the next line) */
		if (c != EOF) {
			ungetc(c, f);
		}
		return buf;
	}

	return NULL;
}

static char *ril_vendor_mtk_hardware()
{
	FILE *f = fopen("/proc/cpuinfo", "r");
	char *hardware = NULL;

	if (f) {
		const char prefix[] = "Hardware\t:";
		const gsize prefix_len = sizeof(prefix) - 1;
		GString *buf = g_string_new("");

		/* Find the "Hardware:" line */
		while (ril_vendor_mtk_read_line(buf, f) &&
				strncmp(buf->str, prefix, prefix_len));

		if (buf->len > prefix_len) {
			/* Erase the prefix */
			g_string_erase(buf, 0, prefix_len);

			/* Remove trailing whitespaces */
			while (buf->len > 0 &&
				g_ascii_isspace(buf->str[buf->len - 1])) {
				g_string_truncate(buf, buf->len - 1);
			}

			/* Extract the last word */
			if (buf->len > 0) {
				gsize pos = buf->len;

				while (pos > 0 &&
					!g_ascii_isspace(buf->str[pos - 1])) {
					pos--;
				}

				if (buf->str[pos]) {
					hardware = g_strdup(buf->str + pos);
					DBG("Hardware: %s", hardware);
				}
			}
		}

		g_string_free(buf, TRUE);
		fclose(f);
	}

	return hardware;
}

static const struct ril_vendor_driver *ril_vendor_mtk_detect()
{
	const struct ril_vendor_driver *driver = DEFAULT_MTK_DRIVER;
	char *hw = ril_vendor_mtk_hardware();

	if (hw) {
		guint i;

		for (i = 0; i < G_N_ELEMENTS(mtk_hw_drivers); i++) {
			if (!strcasecmp(mtk_hw_drivers[i]->name, hw)) {
				driver = mtk_hw_drivers[i];
				DBG("Driver: %s", driver->name);
				break;
			}
		}
		g_free(hw);
	}
	return driver;
}

static struct ril_vendor_hook *ril_vendor_mtk_create_hook_auto
	(const void *driver_data, GRilIoChannel *io, const char *path,
		const struct ril_slot_config *cfg, struct ril_network *network)
{
	return ril_vendor_create_hook(ril_vendor_mtk_detect(), io, path, cfg,
								network);
}

RIL_VENDOR_DRIVER_DEFINE(ril_vendor_driver_mtk) {
	.name               = "mtk",
	.get_defaults       = ril_vendor_mtk_get_defaults,
	.create_hook        = ril_vendor_mtk_create_hook_auto
};

/*
 * Local Variables:
 * mode: C
 * c-basic-offset: 8
 * indent-tabs-mode: t
 * End:
 */
