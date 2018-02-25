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

#ifndef RIL_SIM_SETTINGS_H
#define RIL_SIM_SETTINGS_H

#include "ril_types.h"

#include <glib-object.h>

struct ril_sim_settings_priv;

struct ril_sim_settings {
	GObject object;
	struct ril_sim_settings_priv *priv;
	const char *imsi;
	enum ofono_radio_access_mode techs;
	enum ofono_radio_access_mode pref_mode;
};

typedef void (*ril_sim_settings_cb_t)(struct ril_sim_settings *s, void *arg);

struct ril_sim_settings *ril_sim_settings_new(const char *path,
					enum ofono_radio_access_mode techs);
struct ril_sim_settings *ril_sim_settings_ref(struct ril_sim_settings *s);
void ril_sim_settings_unref(struct ril_sim_settings *s);
void ril_sim_settings_set_pref_mode(struct ril_sim_settings *s,
					enum ofono_radio_access_mode mode);
gulong ril_sim_settings_add_imsi_changed_handler(struct ril_sim_settings *s,
					ril_sim_settings_cb_t cb, void *arg);
gulong ril_sim_settings_add_pref_mode_changed_handler(struct ril_sim_settings *s,
					ril_sim_settings_cb_t cb, void *arg);
void ril_sim_settings_remove_handler(struct ril_sim_settings *s, gulong id);
void ril_sim_settings_remove_handlers(struct ril_sim_settings *s, gulong *ids,
								int count);

#endif /* RIL_SIM_SETTINGS_H */

/*
 * Local Variables:
 * mode: C
 * c-basic-offset: 8
 * indent-tabs-mode: t
 * End:
 */
