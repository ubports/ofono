/*
 *  oFono - Open Source Telephony
 *
 *  Copyright (C) 2017 Jolla Ltd.
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

#ifndef SAILFISH_WATCH_H
#define SAILFISH_WATCH_H

struct ofono_modem;
struct ofono_sim;
struct ofono_netreg;

#include <glib.h>
#include <glib-object.h>

/* This object watches ofono modem and various related things */
struct sailfish_watch_priv;
struct sailfish_watch {
	GObject object;
	struct sailfish_watch_priv *priv;
	const char *path;
	/* Modem */
	struct ofono_modem *modem;
	gboolean online;
	/* OFONO_ATOM_TYPE_SIM */
	struct ofono_sim *sim;
	const char *iccid;
	const char *imsi;
	const char *spn;
	/* OFONO_ATOM_TYPE_NETREG */
	struct ofono_netreg *netreg;
};

typedef void (*sailfish_watch_cb_t)(struct sailfish_watch *w, void *user_data);

struct sailfish_watch *sailfish_watch_new(const char *path);
struct sailfish_watch *sailfish_watch_ref(struct sailfish_watch *w);
void sailfish_watch_unref(struct sailfish_watch *w);

gulong sailfish_watch_add_modem_changed_handler(struct sailfish_watch *w,
				sailfish_watch_cb_t cb, void *user_data);
gulong sailfish_watch_add_online_changed_handler(struct sailfish_watch *w,
				sailfish_watch_cb_t cb, void *user_data);
gulong sailfish_watch_add_sim_changed_handler(struct sailfish_watch *w,
				sailfish_watch_cb_t cb, void *user_data);
gulong sailfish_watch_add_sim_state_changed_handler(struct sailfish_watch *w,
				sailfish_watch_cb_t cb, void *user_data);
gulong sailfish_watch_add_iccid_changed_handler(struct sailfish_watch *w,
				sailfish_watch_cb_t cb, void *user_data);
gulong sailfish_watch_add_imsi_changed_handler(struct sailfish_watch *w,
				sailfish_watch_cb_t cb, void *user_data);
gulong sailfish_watch_add_spn_changed_handler(struct sailfish_watch *w,
				sailfish_watch_cb_t cb, void *user_data);
gulong sailfish_watch_add_netreg_changed_handler(struct sailfish_watch *w,
				sailfish_watch_cb_t cb, void *user_data);
void sailfish_watch_remove_handler(struct sailfish_watch *w, gulong id);
void sailfish_watch_remove_handlers(struct sailfish_watch *w, gulong *ids,
								int count);

#define sailfish_watch_remove_all_handlers(w,ids) \
	sailfish_watch_remove_handlers(w, ids, G_N_ELEMENTS(ids))

#endif /* SAILFISH_WATCH_H */

/*
 * Local Variables:
 * mode: C
 * c-basic-offset: 8
 * indent-tabs-mode: t
 * End:
 */
