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
	GRilIoChannel *io;
	GRilIoQueue *q;
	gboolean ofono_attached;
	gboolean ofono_registered;
	int max_cids;
	int last_status;
	int ril_data_tech;
	gulong event_id;
	guint poll_id;
	guint timer_id;
};

struct ril_gprs_cbd {
	struct ril_gprs *gd;
	union _ofono_gprs_cb {
		ofono_gprs_status_cb_t status;
		ofono_gprs_cb_t cb;
		gpointer ptr;
	} cb;
	gpointer data;
};

#define ril_gprs_cbd_free g_free

static void ril_gprs_poll_data_reg_state_cb(GRilIoChannel *io, int ril_status,
				const void *data, guint len, void *user_data);

static inline struct ril_gprs *ril_gprs_get_data(struct ofono_gprs *b)
{
	return ofono_gprs_get_data(b);
}

static struct ril_gprs_cbd *ril_gprs_cbd_new(struct ril_gprs *gd, void *cb,
								void *data)
{
	struct ril_gprs_cbd *cbd = g_new0(struct ril_gprs_cbd, 1);

	cbd->gd = gd;
	cbd->cb.ptr = cb;
	cbd->data = data;
	return cbd;
}

int ril_gprs_ril_data_tech(struct ofono_gprs *gprs)
{
	struct ril_gprs *gd = ril_gprs_get_data(gprs);
	return gd ? gd->ril_data_tech : -1;
}

static void ril_gprs_poll_data_reg_state(struct ril_gprs *gd)
{
	if (!gd->poll_id) {
		DBG("");
		gd->poll_id = grilio_queue_send_request_full(gd->q, NULL,
			RIL_REQUEST_DATA_REGISTRATION_STATE,
			ril_gprs_poll_data_reg_state_cb, NULL, gd);
	}
}

static void ril_gprs_state_changed(GRilIoChannel *io, guint code,
				const void *data, guint len, void *user_data)
{
	struct ril_gprs *gd = user_data;

	DBG("%s", ril_modem_get_path(gd->md));
	ril_gprs_poll_data_reg_state(gd);
}

static gboolean ril_gprs_set_attached_callback(gpointer user_data)
{
	struct ofono_error error;
	struct ril_gprs_cbd *cbd = user_data;

	DBG("%s", ril_modem_get_path(cbd->gd->md));
	cbd->gd->timer_id = 0;
	cbd->cb.cb(ril_error_ok(&error), cbd->data);
	ril_gprs_cbd_free(cbd);

	/* Single shot */
	return FALSE;
}

static void ril_gprs_set_attached(struct ofono_gprs *gprs, int attached,
					ofono_gprs_cb_t cb, void *data)
{
	struct ril_gprs *gd = ril_gprs_get_data(gprs);

	DBG("%s attached: %d", ril_modem_get_path(gd->md), attached);
	/*
	* As RIL offers no actual control over the GPRS 'attached'
	* state, we save the desired state, and use it to override
	* the actual modem's state in the 'attached_status' function.
	* This is similar to the way the core ofono gprs code handles
	* data roaming ( see src/gprs.c gprs_netreg_update().
	*
	* The core gprs code calls driver->set_attached() when a netreg
	* notification is received and any configured roaming conditions
	* are met.
	*/

	gd->ofono_attached = attached;

	/*
	* However we cannot respond immediately, since core sets the
	* value of driver_attached after calling set_attached and that
	* leads to comparison failure in gprs_attached_update in
	* connection drop phase
	*/
	gd->timer_id = g_idle_add(ril_gprs_set_attached_callback,
					ril_gprs_cbd_new(gd, cb, data));
}

static int ril_gprs_parse_data_reg_state(struct ril_gprs *gd,
						const void *data, guint len)
{
	struct ofono_gprs *gprs = gd->gprs;
	struct ril_reg_data reg;

	if (!ril_util_parse_reg(data, len, &reg)) {
		ofono_error("Failure parsing data registration response.");
		gd->ril_data_tech = -1;
		return NETWORK_REGISTRATION_STATUS_UNKNOWN;
	} else {
		const int rawstatus = reg.status;

		if (gd->ril_data_tech != reg.ril_tech) {
			gd->ril_data_tech = reg.ril_tech;
			DBG("ril data tech %d", reg.ril_tech);
		}

		if (!gd->ofono_registered) {
			ofono_gprs_register(gprs);
			gd->ofono_registered = TRUE;
		}

		if (reg.max_calls > gd->max_cids) {
			DBG("Setting max cids to %d", reg.max_calls);
			gd->max_cids = reg.max_calls;
			ofono_gprs_set_cid_range(gprs, 1, reg.max_calls);
		}

		if (reg.status == NETWORK_REGISTRATION_STATUS_ROAMING) {
			reg.status = ril_netreg_check_if_really_roaming(
				ril_modem_ofono_netreg(gd->md), reg.status);
		}

		if (rawstatus != reg.status) {
			ofono_info("data registration modified %d => %d",
						rawstatus, reg.status);
		}

		return reg.status;
	}
}

static void ril_gprs_registration_status_cb(GRilIoChannel *io, int ril_status,
				const void *data, guint len, void *user_data)
{
	struct ril_gprs_cbd *cbd = user_data;
	ofono_gprs_status_cb_t cb = cbd->cb.status;
	struct ril_gprs *gd = cbd->gd;
	struct ofono_gprs *gprs = gd->gprs;
	struct ofono_error error;
	int status = -1;

	DBG("%s", ril_modem_get_path(gd->md));
	if (gd && ril_status == RIL_E_SUCCESS) {
		ril_error_init_ok(&error);
	} else {
		ofono_error("ril_gprs_data_reg_cb: reply failure: %s",
				ril_error_to_string(ril_status));
		ril_error_init_failure(&error);
		goto cb_out;
	}

	status = ril_gprs_parse_data_reg_state(gd, data, len);
	if (status == NETWORK_REGISTRATION_STATUS_UNKNOWN) {
		ril_error_init_failure(&error);
		goto cb_out;
	}

	/* Let's minimize logging */
	if (status != gd->last_status) {
		ofono_info("data reg changes %d (%d), attached %d",
				status, gd->last_status, gd->ofono_attached);
	}

	/* Must be attached if registered or roaming */
	if (gd->last_status != NETWORK_REGISTRATION_STATUS_REGISTERED &&
		gd->last_status != NETWORK_REGISTRATION_STATUS_ROAMING) {
		if (status == NETWORK_REGISTRATION_STATUS_REGISTERED) {
			gd->ofono_attached = TRUE;
		} else if ((status == NETWORK_REGISTRATION_STATUS_ROAMING) &&
				ofono_gprs_get_roaming_allowed(gd->gprs)) {
			gd->ofono_attached = TRUE;
		}
	}

	if (!ofono_modem_get_online(ofono_gprs_get_modem(gprs)))
		gd->ofono_attached = FALSE;

	/* if unsolicitated and no state change let's not notify core */
	if ((status == gd->last_status) && gd->ofono_attached) {
		goto cb_out;
	}

	if (!gd->ofono_attached) {
		if (!cb) {
			if (status == NETWORK_REGISTRATION_STATUS_ROAMING) {
				if (!ofono_gprs_get_roaming_allowed(gd->gprs)) {
					ofono_gprs_detached_notify(gprs);
				}

				/*
				 * This prevents core ending
				 * into eternal loop with driver
				 */
				ril_error_init_failure(&error);
			}

			ofono_gprs_status_notify(gprs, status);

		} else {
			/*
			 * This prevents core ending
			 * into eternal loop with driver
			 */
			ril_error_init_failure(&error);
		}

		gd->last_status = status;
		goto exit;
	}

	if (!cb) {
		ofono_gprs_status_notify(gprs, status);
	}

	gd->last_status = status;

exit:
	DBG("data reg status %d, last status %d, attached %d",
		status, gd->last_status, gd->ofono_attached);
cb_out:
	if (cb) {
		cb(&error, status, cbd->data);
	}
}

static void ril_gprs_poll_data_reg_state_cb(GRilIoChannel *io, int ril_status,
				const void *data, guint len, void *user_data)
{
	struct ril_gprs *gd = user_data;
	int status;

	DBG("%s", ril_modem_get_path(gd->md));
	GASSERT(gd->poll_id);
	gd->poll_id = 0;

	if (ril_status != RIL_E_SUCCESS) {
		ofono_error("ril_gprs_data_probe_reg_cb: reply failure: %s",
				ril_error_to_string(ril_status));
		status = NETWORK_REGISTRATION_STATUS_UNKNOWN;
	} else {
		status = ril_gprs_parse_data_reg_state(gd, data, len);
		ofono_info("data reg status probed %d", status);
	}

	if (status != gd->last_status) {
		ofono_info("data reg changes %d (%d), attached %d",
				status, gd->last_status, gd->ofono_attached);
		gd->last_status = status;
		if (gd->ofono_attached) {
			ofono_gprs_status_notify(gd->gprs, status);
		}
	}
}

static void ril_gprs_registration_status(struct ofono_gprs *gprs,
				ofono_gprs_status_cb_t cb, void *data)
{
	struct ril_gprs *gd = ril_gprs_get_data(gprs);

	DBG("");
	if (gd) {
		grilio_queue_send_request_full(gd->q, NULL,
			RIL_REQUEST_DATA_REGISTRATION_STATE,
			ril_gprs_registration_status_cb, ril_gprs_cbd_free,
			ril_gprs_cbd_new(gd, cb, data));
	}
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
	gd->last_status = -1;
	gd->ril_data_tech = -1;
	gd->gprs = gprs;

	ofono_gprs_set_data(gprs, gd);
	ril_gprs_poll_data_reg_state(gd);
	gd->event_id = grilio_channel_add_unsol_event_handler(gd->io,
			ril_gprs_state_changed,
			RIL_UNSOL_RESPONSE_VOICE_NETWORK_STATE_CHANGED, gd);
	return 0;
}

static void ril_gprs_remove(struct ofono_gprs *gprs)
{
	struct ril_gprs *gd = ril_gprs_get_data(gprs);

	DBG("%s", ril_modem_get_path(gd->md));
	ofono_gprs_set_data(gprs, NULL);

	if (gd->timer_id > 0) {
		g_source_remove(gd->timer_id);
        }

	grilio_channel_remove_handler(gd->io, gd->event_id);
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
