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
#include "ril_constants.h"
#include "ril_util.h"
#include "ril_log.h"

#include "ofono.h"

#define MAX_PDP_CONTEXTS        (2)

enum ril_modem_power_state {
	POWERED_OFF,
	POWERING_ON,
	POWERED_ON,
	POWERING_OFF
};

enum ril_modem_online_state {
	OFFLINE,
	GOING_ONLINE,
	ONLINE,
	GOING_OFFLINE
};

enum ril_modem_events {
	MODEM_EVENT_CONNECTED,
	MODEM_EVENT_RADIO_STATE_CHANGED,
	MODEM_EVENT_ERROR,
	MODEM_EVENT_COUNT
};

struct ril_modem_online_request {
	ofono_modem_online_cb_t cb;
	void *data;
	guint id;
};

struct ril_modem {
	GRilIoChannel *io;
	GRilIoQueue *q;
	struct ofono_modem *modem;
	struct ofono_radio_settings *radio_settings;
	struct ril_modem_config config;
	char *default_name;

	enum ril_radio_state radio_state;
	enum ril_modem_power_state power_state;
	gulong event_id[MODEM_EVENT_COUNT];

	ril_modem_cb_t error_cb;
	void *error_cb_data;

	ril_modem_cb_t removed_cb;
	void *removed_cb_data;

	struct ril_modem_online_request set_online;
	struct ril_modem_online_request set_offline;
};

static guint ril_modem_request_power(struct ril_modem *md, gboolean on,
						GRilIoChannelResponseFunc cb);

static inline struct ril_modem *ril_modem_from_ofono(struct ofono_modem *modem)
{
	return ofono_modem_get_data(modem);
}

GRilIoChannel *ril_modem_io(struct ril_modem *md)
{
	return md ? md->io : NULL;
}

const struct ril_modem_config *ril_modem_config(struct ril_modem *md)
{
	return md ? &md->config : NULL;
}

struct ofono_modem *ril_modem_ofono_modem(struct ril_modem *md)
{
	return md ? md->modem : NULL;
}

struct ofono_sim *ril_modem_ofono_sim(struct ril_modem *md)
{
	return (md && md->modem) ?
		__ofono_atom_find(OFONO_ATOM_TYPE_SIM, md->modem) :
		NULL;
}

struct ofono_gprs *ril_modem_ofono_gprs(struct ril_modem *md)
{
	return (md && md->modem) ?
		__ofono_atom_find(OFONO_ATOM_TYPE_GPRS, md->modem) :
		NULL;
}

struct ofono_netreg *ril_modem_ofono_netreg(struct ril_modem *md)
{
	return (md && md->modem) ?
		__ofono_atom_find(OFONO_ATOM_TYPE_NETREG, md->modem) :
		NULL;
}

void ril_modem_delete(struct ril_modem *md)
{
	if (md && md->modem) {
		ofono_modem_remove(md->modem);
	}
}

void ril_modem_set_error_cb(struct ril_modem *md, ril_modem_cb_t cb,
								void *data)
{
	md->error_cb = cb;
	md->error_cb_data = data;
}

void ril_modem_set_removed_cb(struct ril_modem *md, ril_modem_cb_t cb,
								void *data)
{
	md->removed_cb = cb;
	md->removed_cb_data = data;
}

void ril_modem_allow_data(struct ril_modem *md)
{
	if (md && md->modem) {
		GRilIoRequest *req = grilio_request_sized_new(8);

		DBG("%s", ofono_modem_get_path(md->modem));
		grilio_request_append_int32(req, 1);
		grilio_request_append_int32(req, TRUE);
		grilio_queue_send_request(md->q, req, RIL_REQUEST_ALLOW_DATA);
		grilio_request_unref(req);
	}
}

static void ril_modem_signal_error(struct ril_modem *md)
{
	if (md->modem && md->error_cb) {
		ril_modem_cb_t cb = md->error_cb;
		void *data = md->error_cb_data;

		md->error_cb = NULL;
		md->error_cb_data = NULL;
		cb(md, data);
	}
}

static void ril_modem_online_request_ok(GRilIoChannel* io,
		struct ril_modem_online_request *req)
{
	if (req->id) {
		grilio_channel_cancel_request(io, req->id, FALSE);
		req->id = 0;
	}

	if (req->cb) {
		struct ofono_error error;
		ofono_modem_online_cb_t cb = req->cb;
		void *data = req->data;

		req->cb = NULL;
		req->data = NULL;
		cb(ril_error_ok(&error), data);
	}
}

static void ril_modem_update_online_state(struct ril_modem *md)
{
	switch (md->radio_state) {
	case RADIO_STATE_ON:
		ril_modem_online_request_ok(md->io, &md->set_online);
		break;

	case RADIO_STATE_OFF:
	case RADIO_STATE_UNAVAILABLE:
		ril_modem_online_request_ok(md->io, &md->set_offline);
		break;

	default:
		break;
	}

	if (!md->set_offline.id && !md->set_online.id &&
					md->power_state == POWERING_OFF) {
		md->power_state = POWERED_OFF;
		ofono_modem_set_powered(md->modem, FALSE);
	}
}

static void ril_modem_online_request_done(struct ril_modem *md,
		struct ril_modem_online_request *req, int ril_status)
{
	GASSERT(req->id);
	GASSERT(req->cb);
	GASSERT(req->data);
	req->id = 0;

	/* If this request has completed successfully, we will
	 * invoke the callback and notify ofono core when we get
	 * RIL_UNSOL_RESPONSE_RADIO_STATE_CHANGED, i.e. the power
	 * state has actually changed */
	if (ril_status != RIL_E_SUCCESS) {
		struct ofono_error error;
		ofono_modem_online_cb_t cb = req->cb;
		void *data = req->data;

		req->cb = NULL;
		req->data = NULL;
		cb(ril_error_failure(&error), data);
	}

	ril_modem_update_online_state(md);
}

static void ril_modem_set_online_cb(GRilIoChannel *io, int status,
				const void *data, guint len, void *user_data)
{
	struct ril_modem *md = user_data;

	DBG("Power on status %s", ril_error_to_string(status));
	ril_modem_online_request_done(md, &md->set_online, status);
}

static void ril_modem_set_offline_cb(GRilIoChannel *io, int status,
				const void *data, guint len, void *user_data)
{
	struct ril_modem *md = user_data;

	DBG("Power on status %s", ril_error_to_string(status));
	ril_modem_online_request_done(md, &md->set_offline, status);
}

static GRilIoRequest *ril_modem_request_radio_power(gboolean on)
{
	GRilIoRequest *req = grilio_request_sized_new(8);

	grilio_request_append_int32(req, 1);
	grilio_request_append_int32(req, on); /* Radio ON=1, OFF=0 */
	return req;
}

static guint ril_modem_request_power(struct ril_modem *md, gboolean on,
						GRilIoChannelResponseFunc cb)
{
	guint id = 0;

	if (md->q) {
		GRilIoRequest *req = ril_modem_request_radio_power(on);

		DBG("[%u] %s", md->config.slot, on ? "ON" : "OFF");
		id = grilio_queue_send_request_full(md->q, req,
				RIL_REQUEST_RADIO_POWER, cb, NULL, md);
		grilio_request_unref(req);
	}

	return id;
}

static void ril_modem_connected(struct ril_modem *md)
{
	ofono_debug("RIL version %u", md->io->ril_version);
	ril_modem_request_power(md, FALSE, NULL);
	if (md->power_state == POWERING_ON) {
		md->power_state = POWERED_ON;
		ofono_modem_set_powered(md->modem, TRUE);
	}
}

static void ril_modem_connected_cb(GRilIoChannel *io, void *user_data)
{
	ril_modem_connected((struct ril_modem *)user_data);
}

static void ril_modem_pre_sim(struct ofono_modem *modem)
{
	struct ril_modem *md = ril_modem_from_ofono(modem);

	DBG("");
	ofono_devinfo_create(modem, 0, RILMODEM_DRIVER, md);
	ofono_sim_create(modem, 0, RILMODEM_DRIVER, md);
	ofono_voicecall_create(modem, 0, RILMODEM_DRIVER, md);
}

static void ril_modem_post_sim(struct ofono_modem *modem)
{
	struct ril_modem *md = ril_modem_from_ofono(modem);
	struct ofono_gprs *gprs;
	struct ofono_gprs_context *gc;
	int i;

	DBG("");
	ofono_sms_create(modem, 0, RILMODEM_DRIVER, md);
	gprs = ofono_gprs_create(modem, 0, RILMODEM_DRIVER, md);
	if (gprs) {
		for (i = 0; i < MAX_PDP_CONTEXTS; i++) {
			gc = ofono_gprs_context_create(modem, 0,
						RILMODEM_DRIVER, md);
			if (gc == NULL)
				break;

			ofono_gprs_add_context(gprs, gc);
		}
	}

	ofono_phonebook_create(modem, 0, RILMODEM_DRIVER, md);
	ofono_call_forwarding_create(modem, 0, RILMODEM_DRIVER, md);
	ofono_call_barring_create(modem, 0, RILMODEM_DRIVER, md);
	ofono_stk_create(modem, 0, RILMODEM_DRIVER, md);
	ofono_message_waiting_register(ofono_message_waiting_create(modem));
}

static void ril_modem_post_online(struct ofono_modem *modem)
{
	struct ril_modem *md = ril_modem_from_ofono(modem);

	DBG("");
	ofono_call_volume_create(modem, 0, RILMODEM_DRIVER, md);
	ofono_netreg_create(modem, 0, RILMODEM_DRIVER, md);
	ofono_ussd_create(modem, 0, RILMODEM_DRIVER, md);
	ofono_call_settings_create(modem, 0, RILMODEM_DRIVER, md);
	ofono_oem_raw_create(modem, 0, RILMODEM_DRIVER, md);
}

static void ril_modem_set_online(struct ofono_modem *modem, ofono_bool_t online,
				ofono_modem_online_cb_t cb, void *data)
{
	struct ril_modem *md = ril_modem_from_ofono(modem);
	struct ril_modem_online_request *req;

	DBG("%s going %sline", ofono_modem_get_path(modem),
						online ? "on" : "off");

	GASSERT(md->power_state == POWERED_ON);
	if (online) {
		req = &md->set_online;
		GASSERT(!req->id);
		req->id = ril_modem_request_power(md, TRUE,
						ril_modem_set_online_cb);
	} else {
		req = &md->set_offline;
		GASSERT(!req->id);
		req->id = ril_modem_request_power(md, FALSE,
						ril_modem_set_offline_cb);
	}

	if (req->id) {
		req->cb = cb;
		req->data = data;
	} else {
		struct ofono_error error;
		cb(ril_error_failure(&error), data);
	}
}

static int ril_modem_enable(struct ofono_modem *modem)
{
	struct ril_modem *md = ril_modem_from_ofono(modem);

	DBG("%s", ofono_modem_get_path(modem));
	if (md->io->connected) {
		md->power_state = POWERED_ON;
		return 0;
	} else {
		DBG("Waiting for RIL_UNSOL_RIL_CONNECTED");
		md->power_state = POWERING_ON;
		return -EINPROGRESS;
	}
}

static int ril_modem_disable(struct ofono_modem *modem)
{
	struct ril_modem *md = ril_modem_from_ofono(modem);

	DBG("%s", ofono_modem_get_path(modem));
	if (md->set_online.id || md->set_offline.id) {
		md->power_state = POWERING_OFF;
		return -EINPROGRESS;
	} else {
		md->power_state = POWERED_OFF;
		return 0;
	}
}

static void ril_modem_error(GRilIoChannel *io, const GError *error,
							void *user_data)
{
	struct ril_modem *md = user_data;

	ofono_error("%s", error->message);
	grilio_channel_remove_handler(io, md->event_id[MODEM_EVENT_ERROR]);
	md->event_id[MODEM_EVENT_ERROR] = 0;
	ril_modem_signal_error(md);
}

static int ril_modem_probe(struct ofono_modem *modem)
{
	DBG("%s", ofono_modem_get_path(modem));
	return 0;
}

static void ril_modem_remove(struct ofono_modem *modem)
{
	struct ril_modem *md = ril_modem_from_ofono(modem);
	int i;

	DBG("%s", ofono_modem_get_path(modem));
	GASSERT(md->modem);

	if (md->radio_state > RADIO_STATE_UNAVAILABLE) {
		GRilIoRequest *req = ril_modem_request_radio_power(FALSE);
		grilio_channel_send_request(md->io, req,
						RIL_REQUEST_RADIO_POWER);
		grilio_request_unref(req);
	}

	if (md->removed_cb) {
		ril_modem_cb_t cb = md->removed_cb;
		void *data = md->removed_cb_data;

		md->removed_cb = NULL;
		md->removed_cb_data = NULL;
		cb(md, data);
	}

	ofono_modem_set_data(modem, NULL);

	for (i=0; i<G_N_ELEMENTS(md->event_id); i++) {
		grilio_channel_remove_handler(md->io, md->event_id[i]);
	}

	grilio_channel_unref(md->io);
	grilio_queue_cancel_all(md->q, FALSE);
	grilio_queue_unref(md->q);
	g_free(md->default_name);
	g_free(md);
}

static void ril_modem_radio_state_changed(GRilIoChannel *io, guint ril_event,
				const void *data, guint len, void *user_data)
{
	struct ril_modem *md = user_data;
	GRilIoParser rilp;
	int radio_state;

	GASSERT(ril_event == RIL_UNSOL_RESPONSE_RADIO_STATE_CHANGED);
	grilio_parser_init(&rilp, data, len);
	if (grilio_parser_get_int32(&rilp, &radio_state) &&
						grilio_parser_at_end(&rilp)) {
		DBG("%s %s", ofono_modem_get_path(md->modem),
				ril_radio_state_to_string(radio_state));
		md->radio_state = radio_state;
		if (radio_state == RADIO_STATE_ON && !md->radio_settings) {
			DBG("Initializing radio settings interface");
			md->radio_settings =
				ofono_radio_settings_create(md->modem, 0,
							RILMODEM_DRIVER, md);
		}

		ril_modem_update_online_state(md);
	} else {
		ofono_error("Error parsing RADIO_STATE_CHANGED");
	}
}

struct ril_modem *ril_modem_create(GRilIoChannel *io, const char *dev,
					const struct ril_modem_config *config)
{
	struct ofono_modem *modem = ofono_modem_create(dev, RILMODEM_DRIVER);

	if (modem) {
		int err;
		struct ril_modem *md = g_new0(struct ril_modem, 1);

		/* Copy config */
		md->config = *config;
		if (config->default_name && config->default_name[0]) {
			md->default_name = g_strdup(config->default_name);
		} else {
			md->default_name = g_strdup_printf("SIM%u",
							config->slot + 1);
		}
		md->config.default_name = md->default_name;

		md->modem = modem;
		md->io = grilio_channel_ref(io);
		md->q = grilio_queue_new(io);
		ofono_modem_set_data(modem, md);
		err = ofono_modem_register(modem);
		if (!err) {
			md->event_id[MODEM_EVENT_ERROR] =
				grilio_channel_add_error_handler(io,
						ril_modem_error, md);
			md->event_id[MODEM_EVENT_RADIO_STATE_CHANGED] =
				grilio_channel_add_unsol_event_handler(io,
					ril_modem_radio_state_changed,
					RIL_UNSOL_RESPONSE_RADIO_STATE_CHANGED,
					md);
			if (io->connected) {
				ril_modem_connected(md);
			} else {
				DBG("[%u] waiting for RIL_UNSOL_RIL_CONNECTED",
							config->slot);
				md->event_id[MODEM_EVENT_CONNECTED] =
					grilio_channel_add_connected_handler(
						io, ril_modem_connected_cb, md);
			}

			/*
			 * ofono_modem_reset sets Powered to TRUE without
			 * issuing PropertyChange signal.
			 */
			ofono_modem_set_powered(md->modem, FALSE);
			ofono_modem_set_powered(md->modem, TRUE);
			return md;
		} else {
			ofono_error("Error %d registering %s",
				    err, RILMODEM_DRIVER);
		}

		ofono_modem_remove(modem);
	}

	return NULL;
}

const struct ofono_modem_driver ril_modem_driver = {
	.name           = RILMODEM_DRIVER,
	.probe          = ril_modem_probe,
	.remove         = ril_modem_remove,
	.enable         = ril_modem_enable,
	.disable        = ril_modem_disable,
	.pre_sim        = ril_modem_pre_sim,
	.post_sim       = ril_modem_post_sim,
	.post_online    = ril_modem_post_online,
	.set_online     = ril_modem_set_online
};

/*
 * Local Variables:
 * mode: C
 * c-basic-offset: 8
 * indent-tabs-mode: t
 * End:
 */
