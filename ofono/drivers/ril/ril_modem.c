/*
 *  oFono - Open Source Telephony - RIL-based devices
 *
 *  Copyright (C) 2015-2021 Jolla Ltd.
 *  Copyright (C) 2019 Open Mobile Platform LLC.
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
#include "ril_network.h"
#include "ril_radio.h"
#include "ril_sim_card.h"
#include "ril_sim_settings.h"
#include "ril_cell_info.h"
#include "ril_vendor.h"
#include "ril_data.h"
#include "ril_util.h"
#include "ril_log.h"

#include <ofono/message-waiting.h>
#include <ofono/cell-info.h>
#include <ofono/sim-auth.h>
#include <ofono/watch.h>

#define ONLINE_TIMEOUT_SECS     (15) /* 20 sec is hardcoded in ofono core */

enum ril_modem_power_state {
	POWERED_OFF,
	POWERED_ON,
	POWERING_OFF
};

enum ril_modem_online_state {
	OFFLINE,
	GOING_ONLINE,
	ONLINE,
	GOING_OFFLINE
};

enum ril_modem_watch_event {
	WATCH_IMSI,
	WATCH_ICCID,
	WATCH_SIM_STATE,
	WATCH_EVENT_COUNT
};

struct ril_modem_online_request {
	const char *name;
	ofono_modem_online_cb_t cb;
	struct ril_modem_data *md;
	void *data;
	guint timeout_id;
};

struct ril_modem_data {
	struct ril_modem modem;
	struct ofono_watch *watch;
	GRilIoQueue *q;
	char *log_prefix;
	char *imeisv;
	char *imei;
	char *ecclist_file;

	gulong watch_event_id[WATCH_EVENT_COUNT];
	char* last_known_iccid;
	char* reset_iccid;

	guint online_check_id;
	enum ril_modem_power_state power_state;
	gulong radio_state_event_id;

	struct ril_modem_online_request set_online;
	struct ril_modem_online_request set_offline;
};

#define RADIO_POWER_TAG(md) (md)

#define DBG_(md,fmt,args...) DBG("%s" fmt, (md)->log_prefix, ##args)

static struct ril_modem_data *ril_modem_data_from_ofono(struct ofono_modem *o)
{
	struct ril_modem_data *md = ofono_modem_get_data(o);
	GASSERT(md->modem.ofono == o);
	return md;
}

struct ofono_sim *ril_modem_ofono_sim(struct ril_modem *m)
{
	return (m && m->ofono) ? ofono_modem_get_sim(m->ofono) : NULL;
}

struct ofono_gprs *ril_modem_ofono_gprs(struct ril_modem *m)
{
	return (m && m->ofono) ? ofono_modem_get_gprs(m->ofono) : NULL;
}

struct ofono_netreg *ril_modem_ofono_netreg(struct ril_modem *m)
{
	return (m && m->ofono) ? ofono_modem_get_netreg(m->ofono) : NULL;
}

static inline struct ofono_radio_settings *ril_modem_radio_settings(
						struct ril_modem *modem)
{
	return (modem && modem->ofono) ?
			ofono_modem_get_radio_settings(modem->ofono) : NULL;
}

void ril_modem_delete(struct ril_modem *md)
{
	if (md && md->ofono) {
		ofono_modem_remove(md->ofono);
	}
}

static void ril_modem_online_request_done(struct ril_modem_online_request *req)
{
	if (req->cb) {
		struct ofono_error error;
		ofono_modem_online_cb_t cb = req->cb;
		void *data = req->data;

		req->cb = NULL;
		req->data = NULL;
		DBG_(req->md, "%s", req->name);
		cb(ril_error_ok(&error), data);
	}
}

static void ril_modem_online_request_ok(struct ril_modem_online_request *req)
{
	if (req->timeout_id) {
		g_source_remove(req->timeout_id);
		req->timeout_id = 0;
	}

	ril_modem_online_request_done(req);
}

static void ril_modem_update_online_state(struct ril_modem_data *md)
{
	switch (md->modem.radio->state) {
	case RADIO_STATE_ON:
		DBG_(md, "online");
		ril_modem_online_request_ok(&md->set_online);
		break;

	case RADIO_STATE_OFF:
	case RADIO_STATE_UNAVAILABLE:
		DBG_(md, "offline");
		ril_modem_online_request_ok(&md->set_offline);
		break;

	default:
		break;
	}

	if (!md->set_offline.timeout_id && !md->set_online.timeout_id &&
					md->power_state == POWERING_OFF) {
		md->power_state = POWERED_OFF;
		if (md->modem.ofono) {
			ofono_modem_set_powered(md->modem.ofono, FALSE);
		}
	}
}

static gboolean ril_modem_online_request_timeout(gpointer data)
{
	struct ril_modem_online_request *req = data;

	GASSERT(req->timeout_id);
	req->timeout_id = 0;
	DBG_(req->md, "%s", req->name);
	ril_modem_online_request_done(req);
	ril_modem_update_online_state(req->md);

	return G_SOURCE_REMOVE;
}

static gboolean ril_modem_online_check(gpointer data)
{
	struct ril_modem_data *md = data;

	GASSERT(md->online_check_id);
	md->online_check_id = 0;
	ril_modem_update_online_state(md);
	return FALSE;
}

static void ril_modem_schedule_online_check(struct ril_modem_data *md)
{
	if (!md->online_check_id) {
		md->online_check_id = g_idle_add(ril_modem_online_check, md);
	}
}

static void ril_modem_update_radio_settings(struct ril_modem_data *md)
{
	struct ril_modem *m = &md->modem;
	struct ofono_radio_settings *rs = ril_modem_radio_settings(m);

	if (md->watch->imsi) {
		/* radio-settings.c assumes that IMSI is available */
		if (!rs) {
			DBG_(md, "initializing radio settings interface");
			ofono_radio_settings_create(m->ofono, 0,
						RILMODEM_DRIVER, md);
		}
	} else if (rs) {
		DBG_(md, "removing radio settings interface");
		ofono_radio_settings_remove(rs);
	} else {
		/* ofono core may remove radio settings atom internally */
		DBG_(md, "radio settings interface is already gone");
	}
}

static void ril_modem_radio_state_cb(struct ril_radio *radio, void *data)
{
	struct ril_modem_data *md = data;

	GASSERT(md->modem.radio == radio);
	ril_modem_update_online_state(md);
}

static void ril_modem_imsi_cb(struct ofono_watch *watch, void *data)
{
	struct ril_modem_data *md = data;

	GASSERT(md->watch == watch);
	ril_modem_update_radio_settings(md);
}

static void ril_modem_iccid_cb(struct ofono_watch *watch, void *data)
{
	struct ril_modem_data *md = data;

	GASSERT(md->watch == watch);
	if (watch->iccid) {
		g_free(md->last_known_iccid);
		md->last_known_iccid = g_strdup(watch->iccid);
		DBG_(md, "%s", md->last_known_iccid);
	}
}

static void ril_modem_sim_state_cb(struct ofono_watch *watch, void *data)
{
	struct ril_modem_data *md = data;
	const enum ofono_sim_state state = ofono_sim_get_state(watch->sim);

	GASSERT(md->watch == watch);
	if (state == OFONO_SIM_STATE_RESETTING) {
		g_free(md->reset_iccid);
		md->reset_iccid = md->last_known_iccid;
		md->last_known_iccid = NULL;
		DBG_(md, "%s is resetting", md->reset_iccid);
	}
}

static void ril_modem_pre_sim(struct ofono_modem *modem)
{
	struct ril_modem_data *md = ril_modem_data_from_ofono(modem);

	DBG("%s", ofono_modem_get_path(modem));
	ofono_devinfo_create(modem, 0, RILMODEM_DRIVER, md);
	ofono_sim_create(modem, 0, RILMODEM_DRIVER, md);
	if (md->modem.config.enable_voicecall) {
		ofono_voicecall_create(modem, 0, RILMODEM_DRIVER, md);
	}
	if (!md->radio_state_event_id) {
		md->radio_state_event_id =
			ril_radio_add_state_changed_handler(md->modem.radio,
					ril_modem_radio_state_cb, md);
	}
}

static void ril_modem_post_sim(struct ofono_modem *modem)
{
	struct ril_modem_data *md = ril_modem_data_from_ofono(modem);
	struct ofono_gprs *gprs;

	DBG("%s", ofono_modem_get_path(modem));
	ofono_sms_create(modem, 0, RILMODEM_DRIVER, md);
	gprs = ofono_gprs_create(modem, 0, RILMODEM_DRIVER, md);
	if (gprs) {
		guint i;
		static const enum ofono_gprs_context_type ap_types[] = {
			OFONO_GPRS_CONTEXT_TYPE_INTERNET,
			OFONO_GPRS_CONTEXT_TYPE_MMS,
			OFONO_GPRS_CONTEXT_TYPE_IMS
		};

		/* Create a context for each type */
		for (i = 0; i < G_N_ELEMENTS(ap_types); i++) {
			struct ofono_gprs_context *gc =
				ofono_gprs_context_create(modem, 0,
						RILMODEM_DRIVER, md);
			if (gc == NULL)
				break;

			ofono_gprs_context_set_type(gc, ap_types[i]);
			ofono_gprs_add_context(gprs, gc);
		}
	}

	ofono_phonebook_create(modem, 0, RILMODEM_DRIVER, md);
	ofono_call_forwarding_create(modem, 0, RILMODEM_DRIVER, md);
	ofono_call_barring_create(modem, 0, RILMODEM_DRIVER, md);
	ofono_message_waiting_register(ofono_message_waiting_create(modem));
	if (md->modem.config.enable_stk) {
		if (!md->reset_iccid ||
			g_strcmp0(md->reset_iccid, md->watch->iccid)) {
			/* This SIM was never reset */
			ofono_stk_create(modem, 0, RILMODEM_DRIVER, md);
		} else {
			ofono_warn("Disabling STK after SIM reset");
		}
	}
	if (md->modem.config.enable_cbs) {
		ofono_cbs_create(modem, 0, RILMODEM_DRIVER, md);
	}
	ofono_sim_auth_create(modem);
}

static void ril_modem_post_online(struct ofono_modem *modem)
{
	struct ril_modem_data *md = ril_modem_data_from_ofono(modem);

	DBG("%s", ofono_modem_get_path(modem));
	ofono_call_volume_create(modem, 0, RILMODEM_DRIVER, md);
	ofono_netreg_create(modem, 0, RILMODEM_DRIVER, md);
	ofono_ussd_create(modem, 0, RILMODEM_DRIVER, md);
	ofono_call_settings_create(modem, 0, RILMODEM_DRIVER, md);
	ofono_netmon_create(modem, 0, RILMODEM_DRIVER, md);
}

static void ril_modem_set_online(struct ofono_modem *modem, ofono_bool_t online,
				ofono_modem_online_cb_t cb, void *data)
{
	struct ril_modem_data *md = ril_modem_data_from_ofono(modem);
	struct ril_radio *radio = md->modem.radio;
	struct ril_modem_online_request *req;

	DBG("%s going %sline", ofono_modem_get_path(modem),
						online ? "on" : "off");

	ril_radio_set_online(radio, online);
	if (online) {
		ril_radio_power_on(radio, RADIO_POWER_TAG(md));
		req = &md->set_online;
	} else {
		ril_radio_power_off(radio, RADIO_POWER_TAG(md));
		req = &md->set_offline;
	}

	req->cb = cb;
	req->data = data;
	if (req->timeout_id) {
		g_source_remove(req->timeout_id);
	}
	req->timeout_id = g_timeout_add_seconds(ONLINE_TIMEOUT_SECS,
					ril_modem_online_request_timeout, req);
	ril_modem_schedule_online_check(md);
}

static int ril_modem_enable(struct ofono_modem *modem)
{
	struct ril_modem_data *md = ril_modem_data_from_ofono(modem);

	DBG("%s", ofono_modem_get_path(modem));
	md->power_state = POWERED_ON;
	return 0;
}

static int ril_modem_disable(struct ofono_modem *modem)
{
	struct ril_modem_data *md = ril_modem_data_from_ofono(modem);

	DBG("%s", ofono_modem_get_path(modem));
	if (md->set_online.timeout_id || md->set_offline.timeout_id) {
		md->power_state = POWERING_OFF;
		return -EINPROGRESS;
	} else {
		md->power_state = POWERED_OFF;
		return 0;
	}
}

static int ril_modem_probe(struct ofono_modem *modem)
{
	DBG("%s", ofono_modem_get_path(modem));
	return 0;
}

static void ril_modem_remove(struct ofono_modem *ofono)
{
	struct ril_modem_data *md = ril_modem_data_from_ofono(ofono);
	struct ril_modem *modem = &md->modem;

	DBG("%s", ril_modem_get_path(modem));
	ofono_modem_set_data(ofono, NULL);

	ril_radio_remove_handler(modem->radio, md->radio_state_event_id);
	ril_radio_set_online(modem->radio, FALSE);
	ril_radio_power_off(modem->radio, RADIO_POWER_TAG(md));
	ril_radio_set_online(modem->radio, FALSE);
	ril_radio_unref(modem->radio);
	ril_sim_settings_unref(modem->sim_settings);

	ofono_watch_remove_all_handlers(md->watch, md->watch_event_id);
	ofono_watch_unref(md->watch);

	if (md->online_check_id) {
		g_source_remove(md->online_check_id);
	}

	if (md->set_online.timeout_id) {
		g_source_remove(md->set_online.timeout_id);
	}

	if (md->set_offline.timeout_id) {
		g_source_remove(md->set_offline.timeout_id);
	}

	ril_vendor_unref(modem->vendor);
	ril_network_unref(modem->network);
	ril_sim_card_unref(modem->sim_card);
	ril_data_unref(modem->data);
	ofono_cell_info_unref(modem->cell_info);
	grilio_channel_unref(modem->io);
	grilio_queue_cancel_all(md->q, FALSE);
	grilio_queue_unref(md->q);
	g_free(md->last_known_iccid);
	g_free(md->reset_iccid);
	g_free(md->ecclist_file);
	g_free(md->log_prefix);
	g_free(md->imeisv);
	g_free(md->imei);
	g_free(md);
}

struct ril_modem *ril_modem_create(GRilIoChannel *io, const char *log_prefix,
		const char *path, const char *imei, const char *imeisv,
		const char *ecclist_file, const struct ril_slot_config *config,
		struct ril_radio *radio, struct ril_network *network,
		struct ril_sim_card *card, struct ril_data *data,
		struct ril_sim_settings *settings, struct ril_vendor *vendor,
		struct ofono_cell_info *cell_info)
{
	/* Skip the slash from the path, it looks like "/ril_0" */
	struct ofono_modem *ofono = ofono_modem_create(path + 1,
							RILMODEM_DRIVER);
	if (ofono) {
		int err;
		struct ril_modem_data *md = g_new0(struct ril_modem_data, 1);
		struct ril_modem *modem = &md->modem;

		/*
		 * ril_plugin.c must wait until IMEI becomes known before
		 * creating the modem
		 */
		GASSERT(imei);

		/* Copy config */
		modem->config = *config;
		modem->imei = md->imei = g_strdup(imei);
		modem->imeisv = md->imeisv = g_strdup(imeisv);
		modem->log_prefix = log_prefix;     /* No need to strdup */
		modem->ecclist_file = ecclist_file; /* No need to strdup */
		md->log_prefix = (log_prefix && log_prefix[0]) ?
			g_strconcat(log_prefix, " ", NULL) : g_strdup("");

		modem->ofono = ofono;
		modem->vendor = ril_vendor_ref(vendor);
		modem->radio = ril_radio_ref(radio);
		modem->network = ril_network_ref(network);
		modem->sim_card = ril_sim_card_ref(card);
		modem->sim_settings = ril_sim_settings_ref(settings);
		modem->cell_info = ofono_cell_info_ref(cell_info);
		modem->data = ril_data_ref(data);
		modem->io = grilio_channel_ref(io);
		md->q = grilio_queue_new(io);
		md->watch = ofono_watch_new(path);
		md->last_known_iccid = g_strdup(md->watch->iccid);

		md->watch_event_id[WATCH_IMSI] =
			ofono_watch_add_imsi_changed_handler(md->watch,
						ril_modem_imsi_cb, md);
		md->watch_event_id[WATCH_ICCID] =
			ofono_watch_add_iccid_changed_handler(md->watch,
						ril_modem_iccid_cb, md);
		md->watch_event_id[WATCH_SIM_STATE] =
			ofono_watch_add_sim_state_changed_handler(md->watch,
						ril_modem_sim_state_cb, md);

		md->set_online.name = "online";
		md->set_online.md = md;
		md->set_offline.name = "offline";
		md->set_offline.md = md;
		ofono_modem_set_data(ofono, md);
		err = ofono_modem_register(ofono);
		if (!err) {
			GASSERT(io->connected);
			if (config->radio_power_cycle) {
				ril_radio_power_cycle(modem->radio);
			}

			/*
			 * ofono_modem_reset sets Powered to TRUE without
			 * issuing PropertyChange signal.
			 */
			ofono_modem_set_powered(modem->ofono, FALSE);
			ofono_modem_set_powered(modem->ofono, TRUE);
			md->power_state = POWERED_ON;

			/*
			 * With some RIL implementations, querying available
			 * band modes causes some magic Android properties to
			 * appear.
			 */
			if (config->query_available_band_mode) {
				grilio_queue_send_request(md->q, NULL,
					RIL_REQUEST_QUERY_AVAILABLE_BAND_MODE);
			}

			ril_modem_update_radio_settings(md);
			return modem;
		} else {
			ofono_error("Error %d registering %s",
				    err, RILMODEM_DRIVER);

			/*
			 * If ofono_modem_register() failed, then
			 * ofono_modem_remove() won't invoke
			 * ril_modem_remove() callback.
			 */
			ril_modem_remove(ofono);
		}

		ofono_modem_remove(ofono);
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
