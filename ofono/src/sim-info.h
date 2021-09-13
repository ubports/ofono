/*
 *  oFono - Open Source Telephony
 *
 *  Copyright (C) 2017-2021 Jolla Ltd.
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

#ifndef SIM_INFO_H
#define SIM_INFO_H

#include <ofono/types.h>

#include <glib.h>
#include <glib-object.h>

/*
 * Note that iccid, imsi and spn provided by this class can be cached,
 * i.e. become available before the pin code is entered and before those
 * are known to the ofono core. That's the whole purpose of this thing.
 */
struct ofono_modem;
struct sim_info_priv;
struct sim_info {
	GObject object;
	struct sim_info_priv *priv;
	const char *path;
	const char *iccid;
	const char *imsi;
	const char *spn;
};

typedef void (*sim_info_cb_t)(struct sim_info *si, void *user_data);

/* SIM info object associated with the particular slot */
struct sim_info *sim_info_new(const char *path);
struct sim_info *sim_info_ref(struct sim_info *si);
void sim_info_unref(struct sim_info *si);
gulong sim_info_add_iccid_changed_handler(struct sim_info *si,
	sim_info_cb_t cb, void *user_data);
gulong sim_info_add_imsi_changed_handler(struct sim_info *si,
	sim_info_cb_t cb, void *user_data);
gulong sim_info_add_spn_changed_handler(struct sim_info *si,
	sim_info_cb_t cb, void *user_data);
void sim_info_remove_handler(struct sim_info *si, gulong id);
void sim_info_remove_handlers(struct sim_info *si, gulong *ids, int count);

#define sim_info_remove_all_handlers(si,ids) \
	sim_info_remove_handlers(si, ids, G_N_ELEMENTS(ids))

/* And the D-Bus interface for it */
struct sim_info_dbus;
struct sim_info_dbus *sim_info_dbus_new	(struct sim_info *si);
struct sim_info_dbus *sim_info_dbus_new_path(const char *path);
void sim_info_dbus_free(struct sim_info_dbus *dbus);

#endif /* SIM_INFO_H */

/*
 * Local Variables:
 * mode: C
 * c-basic-offset: 8
 * indent-tabs-mode: t
 * End:
 */
