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
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 */

#ifndef SAILFISH_MANAGER_H
#define SAILFISH_MANAGER_H

struct ofono_modem;

#include <ofono/types.h>
#include <ofono/radio-settings.h>

#include <glib.h>

struct sailfish_manager;
struct sailfish_slot;
struct sailfish_slot_impl;
struct sailfish_slot_driver;
struct sailfish_slot_driver_reg;
struct sailfish_slot_manager;
struct sailfish_slot_manager_impl;
struct sailfish_cell_info;

typedef void (*sailfish_slot_manager_impl_cb_t)
		(struct sailfish_slot_manager_impl *impl, void *user_data);

enum sailfish_slot_flags {
	SAILFISH_SLOT_NO_FLAGS = 0,
	/* Normally we should be able to have two simultaneously active
	 * data contexts - one for mobile data and one for MMS. The flag
	 * below says that for whatever reason it's impossible and mobile
	 * data has to be disconnected before we can send or receive MMS.
	 * On such devices it may not be a good idea to automatically
	 * download MMS because that would kill active mobile data
	 * connections. */
	SAILFISH_SLOT_SINGLE_CONTEXT = 0x01
};

typedef struct sailfish_slot {
	const char *path;
	const char *imei;
	const char *imeisv;
	gboolean sim_present;
	gboolean enabled;
} const *sailfish_slot_ptr;

struct sailfish_manager {
	const char *mms_imsi;
	const char *mms_path;
	const char *default_voice_imsi;
	const char *default_data_imsi;
	const char *default_voice_path;
	const char *default_data_path;
	const sailfish_slot_ptr *slots;
	gboolean ready;
};

enum sailfish_sim_state {
	SAILFISH_SIM_STATE_UNKNOWN,
	SAILFISH_SIM_STATE_ABSENT,
	SAILFISH_SIM_STATE_PRESENT,
	SAILFISH_SIM_STATE_ERROR
};

enum sailfish_data_role {
	SAILFISH_DATA_ROLE_NONE,        /* Data not allowed */
	SAILFISH_DATA_ROLE_MMS,         /* Data is allowed at any speed */
	SAILFISH_DATA_ROLE_INTERNET     /* Data is allowed at full speed */
};

/* Register/unregister the driver */
struct sailfish_slot_driver_reg *sailfish_slot_driver_register
				(const struct sailfish_slot_driver *d);
void sailfish_slot_driver_unregister(struct sailfish_slot_driver_reg *r);

/* For use by the driver implementations */
void sailfish_manager_foreach_slot_manager
		(struct sailfish_slot_driver_reg *r,
			sailfish_slot_manager_impl_cb_t cb, void *user_data);
struct sailfish_slot *sailfish_manager_slot_add
		(struct sailfish_slot_manager *m, struct sailfish_slot_impl *i,
			const char *path, enum ofono_radio_access_mode techs,
			const char *imei, const char *imeisv,
			enum sailfish_sim_state sim_state);
struct sailfish_slot *sailfish_manager_slot_add2
		(struct sailfish_slot_manager *m, struct sailfish_slot_impl *i,
			const char *path, enum ofono_radio_access_mode techs,
			const char *imei, const char *imeisv,
			enum sailfish_sim_state sim_state,
			enum sailfish_slot_flags flags);
void sailfish_manager_imei_obtained(struct sailfish_slot *s, const char *imei);
void sailfish_manager_imeisv_obtained(struct sailfish_slot *s,
						const char *imeisv);
void sailfish_manager_set_sim_state(struct sailfish_slot *s,
						enum sailfish_sim_state state);
void sailfish_slot_manager_started(struct sailfish_slot_manager *m);
void sailfish_manager_slot_error(struct sailfish_slot *s, const char *key,
						const char *message);
void sailfish_manager_error(struct sailfish_slot_manager *m, const char *key,
						const char *message);
void sailfish_manager_set_cell_info(struct sailfish_slot *s,
						struct sailfish_cell_info *ci);

/* Callbacks provided by slot plugins */
struct sailfish_slot_driver {
	const char *name;
	int priority;

	/* Slot manager methods */
	struct sailfish_slot_manager_impl *(*manager_create)
					(struct sailfish_slot_manager *m);
	guint (*manager_start)(struct sailfish_slot_manager_impl *s);
	void (*manager_cancel_start)(struct sailfish_slot_manager_impl *s,
					guint id);
	void (*manager_free)(struct sailfish_slot_manager_impl *s);

	/* Slot methods */
	void (*slot_enabled_changed)(struct sailfish_slot_impl *s);
	void (*slot_set_data_role)(struct sailfish_slot_impl *s,
					enum sailfish_data_role role);
	void (*slot_free)(struct sailfish_slot_impl *s);
};

#endif /* SAILFISH_MANAGER_H */

/*
 * Local Variables:
 * mode: C
 * c-basic-offset: 8
 * indent-tabs-mode: t
 * End:
 */
