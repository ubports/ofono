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

#ifndef RIL_RADIO_H
#define RIL_RADIO_H

#include "ril_types.h"

#include <glib-object.h>

struct ril_radio {
	GObject object;
	struct ril_radio_priv *priv;
	enum ril_radio_state state;
	gboolean online;
};

typedef void (*ril_radio_cb_t)(struct ril_radio *radio, void *arg);

struct ril_radio *ril_radio_new(GRilIoChannel *io);
struct ril_radio *ril_radio_ref(struct ril_radio *radio);
void ril_radio_unref(struct ril_radio *radio);

void ril_radio_power_on(struct ril_radio *radio, gpointer tag);
void ril_radio_power_off(struct ril_radio *radio, gpointer tag);
void ril_radio_power_cycle(struct ril_radio *radio);
void ril_radio_confirm_power_on(struct ril_radio *radio);
void ril_radio_set_online(struct ril_radio *radio, gboolean online);
gulong ril_radio_add_state_changed_handler(struct ril_radio *radio,
					ril_radio_cb_t cb, void *arg);
gulong ril_radio_add_online_changed_handler(struct ril_radio *radio,
					ril_radio_cb_t cb, void *arg);
void ril_radio_remove_handler(struct ril_radio *radio, gulong id);
void ril_radio_remove_handlers(struct ril_radio *radio, gulong *ids, int n);
enum ril_radio_state ril_radio_state_parse(const void *data, guint len);

#define ril_radio_remove_all_handlers(r,ids) \
	ril_radio_remove_handlers(r, ids, G_N_ELEMENTS(ids))

#endif /* RIL_RADIO_H */

/*
 * Local Variables:
 * mode: C
 * c-basic-offset: 8
 * indent-tabs-mode: t
 * End:
 */
