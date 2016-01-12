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

#ifndef RIL_NETWORK_H
#define RIL_NETWORK_H

#include "ril_types.h"

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
};

typedef void (*ril_network_cb_t)(struct ril_network *net, void *arg);

struct ril_network *ril_network_new(GRilIoChannel *io, struct ril_radio *radio);
struct ril_network *ril_network_ref(struct ril_network *net);
void ril_network_unref(struct ril_network *net);

gulong ril_network_add_operator_changed_handler(struct ril_network *net,
					ril_network_cb_t cb, void *arg);
gulong ril_network_add_voice_state_changed_handler(struct ril_network *net,
					ril_network_cb_t cb, void *arg);
gulong ril_network_add_data_state_changed_handler(struct ril_network *net,
					ril_network_cb_t cb, void *arg);
void ril_network_remove_handler(struct ril_network *net, gulong id);

#endif /* RIL_NETWORK */

/*
 * Local Variables:
 * mode: C
 * c-basic-offset: 8
 * indent-tabs-mode: t
 * End:
 */
