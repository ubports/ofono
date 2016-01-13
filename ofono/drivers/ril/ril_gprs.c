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
	struct ril_network *network;
	GRilIoChannel *io;
	GRilIoQueue *q;
	gboolean allow_data;
	gboolean attached;
	int max_cids;
	enum network_registration_status registration_status;
	guint register_id;
	gulong event_id;
	guint set_attached_id;
};

struct ril_gprs_cbd {
	struct ril_gprs *gd;
	ofono_gprs_cb_t cb;
	gpointer data;
};

#define ril_gprs_cbd_free g_free

static inline struct ril_gprs *ril_gprs_get_data(struct ofono_gprs *ofono)
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

static void ril_gprs_send_allow_data_req(struct ril_gprs *gd, gboolean allow)
{
	GRilIoRequest *req = grilio_request_sized_new(8);

	/*
	 * Some RILs never respond to RIL_REQUEST_ALLOW_DATA, so it doesn't
	 * make sense to register the completion callback - without a timeout
	 * it would just leak memory on our side.
	 */
	grilio_request_append_int32(req, 1);
	grilio_request_append_int32(req, allow != FALSE);
	if (allow) {
		grilio_queue_send_request(gd->q, req, RIL_REQUEST_ALLOW_DATA);
	} else {
		/*
		 * Send "off" requests directly to GRilIoChannel so that they
		 * don't get cancelled by ril_gprs_remove()
		 */
		grilio_channel_send_request(gd->io, req, RIL_REQUEST_ALLOW_DATA);
	}
	grilio_request_unref(req);
}

static void ril_gprs_check_data_allowed(struct ril_gprs *gd)
{
	/* Not doing anything while set_attached call is pending */
	if (!gd->set_attached_id) {
		DBG("%d %d", gd->allow_data, gd->attached);
		if (!gd->allow_data && gd->attached) {
			gd->attached = FALSE;
			if (gd->gprs) {
				ofono_gprs_detached_notify(gd->gprs);
			}
		} else if (gd->allow_data && !gd->attached) {
			switch (gd->registration_status) {
			case NETWORK_REGISTRATION_STATUS_REGISTERED:
			case NETWORK_REGISTRATION_STATUS_ROAMING:
				/*
				 * Already registered, ofono core should
				 * call set_attached.
				 */
				ofono_gprs_status_notify(gd->gprs,
						gd->registration_status);
				break;
			default:
				/*
				 * Otherwise wait for the data registration
				 * status to change
				 */
				break;
			}
		}
	}
}

static gboolean ril_gprs_set_attached_cb(gpointer user_data)
{
	struct ofono_error error;
	struct ril_gprs_cbd *cbd = user_data;
	struct ril_gprs *gd = cbd->gd;

	GASSERT(gd->set_attached_id);
	gd->set_attached_id = 0;
	cbd->cb(ril_error_ok(&error), cbd->data);
	ril_gprs_check_data_allowed(gd);
	return FALSE;
}

static void ril_gprs_set_attached(struct ofono_gprs *gprs, int attached,
					ofono_gprs_cb_t cb, void *data)
{
	struct ril_gprs *gd = ril_gprs_get_data(gprs);

	if (gd && (gd->allow_data || !attached)) {
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

void ril_gprs_allow_data(struct ofono_gprs *gprs, gboolean allow)
{
	struct ril_gprs *gd = ril_gprs_get_data(gprs);

	GASSERT(gd);
	if (gd) {
		DBG("%s %s", ril_modem_get_path(gd->md), allow ? "yes" : "no");
		if (gd->allow_data != allow) {
			gd->allow_data = allow;
			ril_gprs_send_allow_data_req(gd, allow);
			ril_gprs_check_data_allowed(gd);
		}
	}
}

static void ril_gprs_data_registration_state_changed(struct ril_network *net,
							void *user_data)
{
	struct ril_gprs *gd = user_data;
	const struct ril_registration_state *data = &net->data;
	enum network_registration_status status;

	GASSERT(gd->network == net);

	if (data->max_calls > gd->max_cids) {
		DBG("Setting max cids to %d", data->max_calls);
		gd->max_cids = data->max_calls;
		ofono_gprs_set_cid_range(gd->gprs, 1, gd->max_cids);
	}

	/* TODO: need a way to make sure that SPDI information has already
	 * been read from the SIM (i.e. sim_spdi_read_cb  in network.c has
	 * been called) */
	status = ril_netreg_check_if_really_roaming(
				ril_modem_ofono_netreg(gd->md), data->status);

	if (gd->registration_status != status) {
		ofono_info("data reg changed %d -> %d (%s), attached %d",
				gd->registration_status, status,
				registration_status_to_string(status),
				gd->attached);
		gd->registration_status = status;
		ofono_gprs_status_notify(gd->gprs, gd->registration_status);
	}
}

static void ril_gprs_registration_status(struct ofono_gprs *gprs,
				ofono_gprs_status_cb_t cb, void *data)
{
	struct ril_gprs *gd = ril_gprs_get_data(gprs);
	struct ofono_error error;

	DBG("%d (%s)", gd->registration_status,
			registration_status_to_string(gd->registration_status));
	cb(ril_error_ok(&error), gd->registration_status, data);
}

static gboolean ril_gprs_register(gpointer user_data)
{
	struct ril_gprs *gd = user_data;

	gd->register_id = 0;
	gd->event_id = ril_network_add_data_state_changed_handler(gd->network,
			ril_gprs_data_registration_state_changed, gd);
	gd->registration_status = ril_netreg_check_if_really_roaming(
		ril_modem_ofono_netreg(gd->md), gd->network->data.status);

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

	if (gd->attached) {
		/* This one won't get cancelled by grilio_queue_cancel_all */
		ril_gprs_send_allow_data_req(gd, FALSE);
	}

	if (gd->set_attached_id) {
		g_source_remove(gd->set_attached_id);
	}

	if (gd->register_id) {
		g_source_remove(gd->register_id);
	}

	ril_network_remove_handler(gd->network, gd->event_id);
	ril_network_unref(gd->network);

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
