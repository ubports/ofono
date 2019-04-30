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

#include <ofono/gprs-context.h>

struct ofono_modem;
struct ofono_sim;
struct ofono_netreg;

enum ofono_netreg_status {
	OFONO_NETREG_STATUS_NONE =             -1,
	OFONO_NETREG_STATUS_NOT_REGISTERED =    0,
	OFONO_NETREG_STATUS_REGISTERED =        1,
	OFONO_NETREG_STATUS_SEARCHING =         2,
	OFONO_NETREG_STATUS_DENIED =            3,
	OFONO_NETREG_STATUS_UNKNOWN =           4,
	OFONO_NETREG_STATUS_ROAMING =           5
};

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
	/* Since mer/1.21+git47 */
	enum ofono_netreg_status reg_status;
	const char *reg_mcc;
	const char *reg_mnc;
	const char *reg_name;
	/* OFONO_ATOM_TYPE_GPRS */
	struct ofono_gprs *gprs;
};

typedef void (*ofono_watch_cb_t)(struct ofono_watch *w, void *user_data);
typedef void (*ofono_watch_gprs_settings_cb_t)(struct ofono_watch *watch,
			enum ofono_gprs_context_type type,
			const struct ofono_gprs_primary_context *settings,
			void *user_data);

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

/* Since mer/1.21+git47 */
unsigned long ofono_watch_add_reg_status_changed_handler(struct ofono_watch *w,
				ofono_watch_cb_t cb, void *user_data);
unsigned long ofono_watch_add_reg_mcc_changed_handler(struct ofono_watch *w,
				ofono_watch_cb_t cb, void *user_data);
unsigned long ofono_watch_add_reg_mnc_changed_handler(struct ofono_watch *w,
				ofono_watch_cb_t cb, void *user_data);
unsigned long ofono_watch_add_reg_name_changed_handler(struct ofono_watch *w,
				ofono_watch_cb_t cb, void *user_data);
unsigned long ofono_watch_add_gprs_changed_handler(struct ofono_watch *w,
				ofono_watch_cb_t cb, void *user_data);
unsigned long ofono_watch_add_gprs_settings_changed_handler
		(struct ofono_watch *watch, ofono_watch_gprs_settings_cb_t cb,
							void *user_data);

#endif /* OFONO_WATCH_H */

/*
 * Local Variables:
 * mode: C
 * c-basic-offset: 8
 * indent-tabs-mode: t
 * End:
 */
