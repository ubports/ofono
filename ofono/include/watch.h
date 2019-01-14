/*
 *  oFono - Open Source Telephony
 *
 *  Copyright (C) 2017-2019 Jolla Ltd.
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

#ifndef OFONO_WATCH_H
#define OFONO_WATCH_H

#include <ofono/types.h>

struct ofono_modem;
struct ofono_sim;
struct ofono_netreg;

/* This object watches ofono modem and various other things */
struct ofono_watch {
	const char *path;
	/* Modem */
	struct ofono_modem *modem;
	ofono_bool_t online;
	/* OFONO_ATOM_TYPE_SIM */
	struct ofono_sim *sim;
	const char *iccid;
	const char *imsi;
	const char *spn;
	/* OFONO_ATOM_TYPE_NETREG */
	struct ofono_netreg *netreg;
};

typedef void (*ofono_watch_cb_t)(struct ofono_watch *w, void *user_data);

struct ofono_watch *ofono_watch_new(const char *path);
struct ofono_watch *ofono_watch_ref(struct ofono_watch *w);
void ofono_watch_unref(struct ofono_watch *w);

unsigned long ofono_watch_add_modem_changed_handler(struct ofono_watch *w,
				ofono_watch_cb_t cb, void *user_data);
unsigned long ofono_watch_add_online_changed_handler(struct ofono_watch *w,
				ofono_watch_cb_t cb, void *user_data);
unsigned long ofono_watch_add_sim_changed_handler(struct ofono_watch *w,
				ofono_watch_cb_t cb, void *user_data);
unsigned long ofono_watch_add_sim_state_changed_handler(struct ofono_watch *w,
				ofono_watch_cb_t cb, void *user_data);
unsigned long ofono_watch_add_iccid_changed_handler(struct ofono_watch *w,
				ofono_watch_cb_t cb, void *user_data);
unsigned long ofono_watch_add_imsi_changed_handler(struct ofono_watch *w,
				ofono_watch_cb_t cb, void *user_data);
unsigned long ofono_watch_add_spn_changed_handler(struct ofono_watch *w,
				ofono_watch_cb_t cb, void *user_data);
unsigned long ofono_watch_add_netreg_changed_handler(struct ofono_watch *w,
				ofono_watch_cb_t cb, void *user_data);
void ofono_watch_remove_handler(struct ofono_watch *w, unsigned long id);
void ofono_watch_remove_handlers(struct ofono_watch *w, unsigned long *ids,
							unsigned int count);

#define ofono_watch_remove_all_handlers(w,ids) \
	ofono_watch_remove_handlers(w, ids, sizeof(ids)/sizeof((ids)[0]))

#endif /* OFONO_WATCH_H */

/*
 * Local Variables:
 * mode: C
 * c-basic-offset: 8
 * indent-tabs-mode: t
 * End:
 */
