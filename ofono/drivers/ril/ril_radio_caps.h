/*
 *  oFono - Open Source Telephony - RIL-based devices
 *
 *  Copyright (C) 2017-2020 Jolla Ltd.
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

#ifndef RIL_RADIO_CAPS_H
#define RIL_RADIO_CAPS_H

#include "ril_types.h"

struct ril_data_manager;
struct ril_sim_settings;
struct ril_radio_caps;
struct ril_radio_caps_manager;
struct ril_radio_capability;
struct ril_radio_caps_request;

typedef void (*ril_radio_caps_cb_t)(struct ril_radio_caps *caps, void *arg);
typedef void (*ril_radio_caps_manager_cb_t)(struct ril_radio_caps_manager *mgr,
							void *user_data);

/* ril_radio_capability pointer is NULL if functionality is unsupported */
typedef void (*ril_radio_caps_check_cb_t)
		(const struct ril_radio_capability *cap, void *user_data);

/* The check can be cancelled with grilio_channel_cancel_request */
guint ril_radio_caps_check(GRilIoChannel *io, ril_radio_caps_check_cb_t cb,
							void *user_data);

/* There should be a single ril_radio_caps_manager shared by all all modems */
struct ril_radio_caps_manager *ril_radio_caps_manager_new
					(struct ril_data_manager *dm);
struct ril_radio_caps_manager *ril_radio_caps_manager_ref
					(struct ril_radio_caps_manager *mgr);
void ril_radio_caps_manager_unref(struct ril_radio_caps_manager *mgr);
gulong ril_radio_caps_manager_add_tx_aborted_handler
				(struct ril_radio_caps_manager *mgr,
				ril_radio_caps_manager_cb_t cb, void *arg);
gulong ril_radio_caps_manager_add_tx_done_handler
				(struct ril_radio_caps_manager *mgr,
				ril_radio_caps_manager_cb_t cb, void *arg);
void ril_radio_caps_manager_remove_handler(struct ril_radio_caps_manager *mgr,
						gulong id);
void ril_radio_caps_manager_remove_handlers(struct ril_radio_caps_manager *mgr,
						gulong *ids, int count);
#define ril_radio_caps_manager_remove_all_handlers(mgr, ids) \
	ril_radio_caps_manager_remove_handlers(mgr, ids, G_N_ELEMENTS(ids))

/* And one ril_radio_caps object per modem */

struct ril_radio_caps {
	struct ril_radio_caps_manager *mgr;
	enum ofono_radio_access_mode supported_modes;
};

struct ril_radio_caps *ril_radio_caps_new(struct ril_radio_caps_manager *mgr,
		const char *log_prefix, GRilIoChannel *io,
		struct ofono_watch *watch,
		struct ril_data *data, struct ril_radio *radio,
		struct ril_sim_card *sim, struct ril_sim_settings *settings,
		const struct ril_slot_config *config,
		const struct ril_radio_capability *cap);
struct ril_radio_caps *ril_radio_caps_ref(struct ril_radio_caps *caps);
void ril_radio_caps_unref(struct ril_radio_caps *caps);
void ril_radio_caps_drop(struct ril_radio_caps *caps);
gulong ril_radio_caps_add_supported_modes_handler
				(struct ril_radio_caps *caps,
					ril_radio_caps_cb_t cb, void *arg);
void ril_radio_caps_remove_handler(struct ril_radio_caps *caps, gulong id);

/* Data requests */

struct ril_radio_caps_request *ril_radio_caps_request_new
		(struct ril_radio_caps *caps, enum ofono_radio_access_mode mode,
						enum ril_data_role role);
void ril_radio_caps_request_free(struct ril_radio_caps_request *req);

#endif /* RIL_RADIO_CAPS_H */

/*
 * Local Variables:
 * mode: C
 * c-basic-offset: 8
 * indent-tabs-mode: t
 * End:
 */
