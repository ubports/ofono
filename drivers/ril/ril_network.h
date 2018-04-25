/*
 *  oFono - Open Source Telephony - RIL-based devices
 *
 *  Copyright (C) 2015-2018 Jolla Ltd.
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

#ifndef RIL_NETWORK_H
#define RIL_NETWORK_H

#include "ril_types.h"

#include <glib-object.h>

struct ofono_network_operator;

struct ril_registration_state {
	int status;        /* enum network_registration_status */
	int access_tech;   /* enum access_technology or -1 if none */
	int ril_tech;
	int max_calls;
	int lac;
	int ci;
};

struct ril_network {
	GObject object;
	struct ril_network_priv *priv;
	struct ril_registration_state voice;
	struct ril_registration_state data;
	const struct ofono_network_operator *operator;
	enum ofono_radio_access_mode pref_mode;
	enum ofono_radio_access_mode max_pref_mode;
	struct ril_sim_settings *settings;
};

struct ofono_sim;
typedef void (*ril_network_cb_t)(struct ril_network *net, void *arg);

struct ril_network *ril_network_new(const char *path, GRilIoChannel *io,
			const char *log_prefix, struct ril_radio *radio,
			struct ril_sim_card *sim_card,
			struct ril_sim_settings *settings,
			const struct ril_slot_config *ril_slot_config);
struct ril_network *ril_network_ref(struct ril_network *net);
void ril_network_unref(struct ril_network *net);

void ril_network_set_max_pref_mode(struct ril_network *net,
				enum ofono_radio_access_mode max_pref_mode,
				gboolean force_check);
void ril_network_assert_pref_mode(struct ril_network *net, gboolean immediate);
void ril_network_query_registration_state(struct ril_network *net);
gulong ril_network_add_operator_changed_handler(struct ril_network *net,
					ril_network_cb_t cb, void *arg);
gulong ril_network_add_voice_state_changed_handler(struct ril_network *net,
					ril_network_cb_t cb, void *arg);
gulong ril_network_add_data_state_changed_handler(struct ril_network *net,
					ril_network_cb_t cb, void *arg);
gulong ril_network_add_pref_mode_changed_handler(struct ril_network *net,
					ril_network_cb_t cb, void *arg);
gulong ril_network_add_max_pref_mode_changed_handler(struct ril_network *net,
					ril_network_cb_t cb, void *arg);
void ril_network_remove_handler(struct ril_network *net, gulong id);
void ril_network_remove_handlers(struct ril_network *net, gulong *ids, int n);

#define ril_network_remove_all_handlers(net, ids) \
	ril_network_remove_handlers(net, ids, G_N_ELEMENTS(ids))

#endif /* RIL_NETWORK_H */

/*
 * Local Variables:
 * mode: C
 * c-basic-offset: 8
 * indent-tabs-mode: t
 * End:
 */
