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
#include "ril_network.h"
#include "ril_data.h"
#include "ril_util.h"
#include "ril_log.h"

#include "common.h"

/*
 * This module is the ofono_gprs_driver implementation for rilmodem.
 *
 * Notes:
 *
 * 1. ofono_gprs_suspend/resume() are not used by this module, as
 *    the concept of suspended GPRS is not exposed by RILD.
 *
 * 2. ofono_gprs_bearer_notify() is never called as RILD does not
 *    expose an unsolicited event equivalent to +CPSB ( see 27.007
 *    7.29 ), and the tech values returned by REQUEST_DATA/VOICE
 *    _REGISTRATION requests do not match the values defined for
 *    <AcT> in the +CPSB definition.  Note, the values returned by
 *    the *REGISTRATION commands are aligned with those defined by
 *    +CREG ( see 27.003 7.2 ).
 */

struct ril_gprs {
	struct ofono_gprs *gprs;
	struct ril_modem *md;
	struct ril_data *data;
	struct ril_network *network;
	GRilIoChannel *io;
	GRilIoQueue *q;
	gboolean attached;
	int max_cids;
	enum network_registration_status registration_status;
	guint register_id;
	gulong network_event_id;
	gulong data_event_id;
	guint set_attached_id;
};

struct ril_gprs_cbd {
	struct ril_gprs *gd;
	ofono_gprs_cb_t cb;
	gpointer data;
};

#define ril_gprs_cbd_free g_free

static struct ril_gprs *ril_gprs_get_data(struct ofono_gprs *ofono)
{
	return ofono ? ofono_gprs_get_data(ofono) : NULL;
}

static struct ril_gprs_cbd *ril_gprs_cbd_new(struct ril_gprs *gd,
						ofono_gprs_cb_t cb, void *data)
{
	struct ril_gprs_cbd *cbd = g_new0(struct ril_gprs_cbd, 1);

	cbd->gd = gd;
	cbd->cb = cb;
	cbd->data = data;
	return cbd;
}

static enum network_registration_status ril_gprs_fix_registration_status(
		struct ril_gprs *gd, enum network_registration_status status)
{
	if (!ril_data_allowed(gd->data)) {
		return NETWORK_REGISTRATION_STATUS_NOT_REGISTERED;
	} else {
		/* TODO: need a way to make sure that SPDI information has
		 * already been read from the SIM (i.e. sim_spdi_read_cb in
		 * network.c has been called) */
		struct ofono_netreg *netreg = ril_modem_ofono_netreg(gd->md);
		return ril_netreg_check_if_really_roaming(netreg, status);
	}
}

static void ril_gprs_data_update_registration_state(struct ril_gprs *gd)
{
	const enum network_registration_status status =
		ril_gprs_fix_registration_status(gd, gd->network->data.status);

	if (gd->registration_status != status) {
		ofono_info("data reg changed %d -> %d (%s), attached %d",
				gd->registration_status, status,
				registration_status_to_string(status),
				gd->attached);
		gd->registration_status = status;
		ofono_gprs_status_notify(gd->gprs, gd->registration_status);
	}
}

static void ril_gprs_check_data_allowed(struct ril_gprs *gd)
{
	DBG("%s %d %d", ril_modem_get_path(gd->md), ril_data_allowed(gd->data),
								gd->attached);
	if (!ril_data_allowed(gd->data) && gd->attached) {
		gd->attached = FALSE;
		if (gd->gprs) {
			ofono_gprs_detached_notify(gd->gprs);
		}
	}

	ril_gprs_data_update_registration_state(gd);
}

static gboolean ril_gprs_set_attached_cb(gpointer user_data)
{
	struct ofono_error error;
	struct ril_gprs_cbd *cbd = user_data;
	struct ril_gprs *gd = cbd->gd;

	GASSERT(gd->set_attached_id);
	gd->set_attached_id = 0;
	ril_gprs_check_data_allowed(gd);
	cbd->cb(ril_error_ok(&error), cbd->data);
	return FALSE;
}

static void ril_gprs_set_attached(struct ofono_gprs *gprs, int attached,
					ofono_gprs_cb_t cb, void *data)
{
	struct ril_gprs *gd = ril_gprs_get_data(gprs);

	if (ril_data_allowed(gd->data) || !attached) {
		DBG("%s attached: %d", ril_modem_get_path(gd->md), attached);
		if (gd->set_attached_id) {
			g_source_remove(gd->set_attached_id);
		}
		gd->attached = attached;
		gd->set_attached_id = g_idle_add_full(G_PRIORITY_DEFAULT_IDLE,
					ril_gprs_set_attached_cb,
					ril_gprs_cbd_new(gd, cb, data),
					ril_gprs_cbd_free);
	} else {
		struct ofono_error error;
		DBG("%s not allowed to attach", ril_modem_get_path(gd->md));
		cb(ril_error_failure(&error), data);
	}
}

static void ril_gprs_allow_data_changed(struct ril_data *data, void *user_data)
{
	struct ril_gprs *gd = user_data;

	GASSERT(gd->data == data);
	DBG("%s %d", ril_modem_get_path(gd->md), ril_data_allowed(data));
	if (!gd->set_attached_id) {
		ril_gprs_check_data_allowed(gd);
	}
}

static void ril_gprs_data_registration_state_changed(struct ril_network *net,
							void *user_data)
{
	struct ril_gprs *gd = user_data;
	const struct ril_registration_state *data = &net->data;

	GASSERT(gd->network == net);
	if (data->max_calls > gd->max_cids) {
		DBG("Setting max cids to %d", data->max_calls);
		gd->max_cids = data->max_calls;
		ofono_gprs_set_cid_range(gd->gprs, 1, gd->max_cids);
	}

	ril_gprs_data_update_registration_state(gd);
}

static void ril_gprs_registration_status(struct ofono_gprs *gprs,
				ofono_gprs_status_cb_t cb, void *data)
{
	struct ril_gprs *gd = ril_gprs_get_data(gprs);
	struct ofono_error error;
	const enum network_registration_status status = gd->attached ?
		gd->registration_status :
		NETWORK_REGISTRATION_STATUS_NOT_REGISTERED;


	DBG("%d (%s)", status, registration_status_to_string(status));
	cb(ril_error_ok(&error), status, data);
}

static gboolean ril_gprs_register(gpointer user_data)
{
	struct ril_gprs *gd = user_data;

	gd->register_id = 0;
	gd->network_event_id = ril_network_add_data_state_changed_handler(
		gd->network, ril_gprs_data_registration_state_changed, gd);
	gd->data_event_id = ril_data_add_allow_changed_handler(gd->data,
		ril_gprs_allow_data_changed, gd);
	gd->registration_status = ril_gprs_fix_registration_status(gd,
						gd->network->data.status);

	gd->max_cids = gd->network->data.max_calls;
	if (gd->max_cids > 0) {
		DBG("Setting max cids to %d", gd->max_cids);
		ofono_gprs_set_cid_range(gd->gprs, 1, gd->max_cids);
	}

	ofono_gprs_register(gd->gprs);
	return FALSE;
}

static int ril_gprs_probe(struct ofono_gprs *gprs, unsigned int vendor,
								void *data)
{
	struct ril_modem *modem = data;
	struct ril_gprs *gd = g_new0(struct ril_gprs, 1);

	DBG("%s", ril_modem_get_path(modem));
	gd->md = modem;
	gd->io = grilio_channel_ref(ril_modem_io(modem));
        gd->q = grilio_queue_new(gd->io);
	gd->data = ril_data_ref(modem->data);
	gd->network = ril_network_ref(modem->network);
	gd->gprs = gprs;
	ofono_gprs_set_data(gprs, gd);

	/* ofono crashes if we register right away */
	gd->register_id = g_idle_add(ril_gprs_register, gd);
	return 0;
}

static void ril_gprs_remove(struct ofono_gprs *gprs)
{
	struct ril_gprs *gd = ril_gprs_get_data(gprs);

	DBG("%s", ril_modem_get_path(gd->md));
	ofono_gprs_set_data(gprs, NULL);

	if (gd->set_attached_id) {
		g_source_remove(gd->set_attached_id);
	}

	if (gd->register_id) {
		g_source_remove(gd->register_id);
	}

	ril_network_remove_handler(gd->network, gd->network_event_id);
	ril_network_unref(gd->network);

	ril_data_remove_handler(gd->data, gd->data_event_id);
	ril_data_unref(gd->data);

	grilio_channel_unref(gd->io);
	grilio_queue_cancel_all(gd->q, FALSE);
	grilio_queue_unref(gd->q);
	g_free(gd);
}

const struct ofono_gprs_driver ril_gprs_driver = {
	.name                   = RILMODEM_DRIVER,
	.probe                  = ril_gprs_probe,
	.remove                 = ril_gprs_remove,
	.set_attached           = ril_gprs_set_attached,
	.attached_status        = ril_gprs_registration_status,
};

/*
 * Local Variables:
 * mode: C
 * c-basic-offset: 8
 * indent-tabs-mode: t
 * End:
 */
