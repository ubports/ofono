/*
 *  oFono - Open Source Telephony
 *
 *  Copyright (C) 2017-2018 Jolla Ltd.
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

#ifndef SAILFISH_SIM_INFO_H
#define SAILFISH_SIM_INFO_H

#include <ofono/types.h>

#include <glib.h>
#include <glib-object.h>

/*
 * Note that iccid, imsi and spn provided by this class can be cached,
 * i.e. become available before the pin code is entered and before those
 * are known to the ofono core. That's the whole purpose of this thing.
 *
 * If you need to follow imsi known to the ofono core, you can use
 * sailfish_sim_settings for that (or fight with ofono imsi watchers
 * directly).
 */
struct ofono_modem;
struct sailfish_sim_info_priv;
struct sailfish_sim_info {
	GObject object;
	struct sailfish_sim_info_priv *priv;
	const char *path;
	const char *iccid;
	const char *imsi;
	const char *spn;
};

typedef void (*sailfish_sim_info_cb_t)(struct sailfish_sim_info *si,
							void *user_data);

/* SIM info object associated with the particular slot */
struct sailfish_sim_info *sailfish_sim_info_new(const char *path);
struct sailfish_sim_info *sailfish_sim_info_ref(struct sailfish_sim_info *si);
void sailfish_sim_info_unref(struct sailfish_sim_info *si);
gulong sailfish_sim_info_add_iccid_changed_handler(struct sailfish_sim_info *si,
				sailfish_sim_info_cb_t cb, void *user_data);
gulong sailfish_sim_info_add_imsi_changed_handler(struct sailfish_sim_info *si,
				sailfish_sim_info_cb_t cb, void *user_data);
gulong sailfish_sim_info_add_spn_changed_handler(struct sailfish_sim_info *si,
				sailfish_sim_info_cb_t cb, void *user_data);
void sailfish_sim_info_remove_handler(struct sailfish_sim_info *si, gulong id);
void sailfish_sim_info_remove_handlers(struct sailfish_sim_info *si,
						gulong *ids, int count);

#define sailfish_sim_info_remove_all_handlers(si,ids) \
	sailfish_sim_info_remove_handlers(si, ids, G_N_ELEMENTS(ids))

/* And the D-Bus interface for it */
struct sailfish_sim_info_dbus;
struct sailfish_sim_info_dbus *sailfish_sim_info_dbus_new
						(struct sailfish_sim_info *si);
struct sailfish_sim_info_dbus *sailfish_sim_info_dbus_new_path
						(const char *path);
void sailfish_sim_info_dbus_free(struct sailfish_sim_info_dbus *dbus);

#endif /* SAILFISH_SIM_INFO_H */

/*
 * Local Variables:
 * mode: C
 * c-basic-offset: 8
 * indent-tabs-mode: t
 * End:
 */
