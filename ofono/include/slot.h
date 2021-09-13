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

#ifndef __OFONO_SLOT_H
#define __OFONO_SLOT_H

/*
 * Slots are built-in non-removable modems. Which may or may not apprear
 * in the list reported by org.ofono.Manager.GetModems D-Bus call.
 *
 * This API exists since mer/1.24+git2
 */

#ifdef __cplusplus
extern "C" {
#endif

struct ofono_modem;

#include <ofono/types.h>
#include <ofono/radio-settings.h>

enum ofono_slot_sim_presence {
	OFONO_SLOT_SIM_UNKNOWN,
	OFONO_SLOT_SIM_ABSENT,
	OFONO_SLOT_SIM_PRESENT
};

/* Should be treated as a bitmask although currently it's not */
enum ofono_slot_data_role {
	OFONO_SLOT_DATA_NONE = 0,
	OFONO_SLOT_DATA_MMS = 0x01,
	OFONO_SLOT_DATA_INTERNET = 0x02
};

enum ofono_slot_property {
	OFONO_SLOT_PROPERTY_ANY,
	OFONO_SLOT_PROPERTY_ENABLED,
	OFONO_SLOT_PROPERTY_SIM_PRESENCE,
	OFONO_SLOT_PROPERTY_DATA_ROLE
#define	OFONO_SLOT_PROPERTY_LAST OFONO_SLOT_PROPERTY_DATA_ROLE
};

enum ofono_slot_manager_property {
	OFONO_SLOT_MANAGER_PROPERTY_ANY,
	OFONO_SLOT_MANAGER_PROPERTY_MMS_IMSI,
	OFONO_SLOT_MANAGER_PROPERTY_MMS_PATH,
	OFONO_SLOT_MANAGER_PROPERTY_DEFAULT_VOICE_IMSI,
	OFONO_SLOT_MANAGER_PROPERTY_DEFAULT_DATA_IMSI,
	OFONO_SLOT_MANAGER_PROPERTY_DEFAULT_VOICE_PATH,
	OFONO_SLOT_MANAGER_PROPERTY_DEFAULT_DATA_PATH,
	OFONO_SLOT_MANAGER_PROPERTY_READY
#define	OFONO_SLOT_MANAGER_PROPERTY_LAST OFONO_SLOT_MANAGER_PROPERTY_READY
};

enum ofono_slot_flags {
	OFONO_SLOT_NO_FLAGS = 0,
	/* Normally we should be able to have two simultaneously active
	 * data contexts - one for mobile data and one for MMS. The flag
	 * below says that for whatever reason it's impossible and mobile
	 * data has to be disconnected before we can send or receive MMS.
	 * On such devices it may not be a good idea to automatically
	 * download MMS because that would kill active mobile data
	 * connections. */
	OFONO_SLOT_FLAG_SINGLE_CONTEXT = 0x01
};

typedef struct ofono_slot {
	const char *path;
	const char *imei;
	const char *imeisv;
	ofono_bool_t enabled;
	enum ofono_slot_sim_presence sim_presence;
	enum ofono_slot_data_role data_role;
} const *ofono_slot_ptr;

struct ofono_slot_manager {
	const char *mms_imsi;
	const char *mms_path;
	const char *default_voice_imsi;
	const char *default_data_imsi;
	const char *default_voice_path;
	const char *default_data_path;
	const ofono_slot_ptr *slots;
	ofono_bool_t ready;
};

#define OFONO_SLOT_API_VERSION (1)

struct ofono_slot_driver {
	const char *name;
	int api_version;        /* OFONO_SLOT_API_VERSION */

	struct ofono_slot_driver_data *(*init)(struct ofono_slot_manager *m);
	unsigned int (*start)(struct ofono_slot_driver_data *d);
	void (*cancel)(struct ofono_slot_driver_data *d, unsigned int id);
	void (*cleanup)(struct ofono_slot_driver_data *d);
};

typedef void (*ofono_slot_property_cb)(struct ofono_slot *slot,
	enum ofono_slot_property property, void* user_data);
typedef void (*ofono_slot_manager_property_cb)(struct ofono_slot_manager *m,
	enum ofono_slot_property property, void* user_data);

struct ofono_slot_driver_data;
struct ofono_slot_driver_reg;
struct ofono_slot_driver_reg *ofono_slot_driver_register
	(const struct ofono_slot_driver *driver);
struct ofono_slot_driver_data *ofono_slot_driver_get_data
	(struct ofono_slot_driver_reg *reg);
void ofono_slot_driver_unregister(struct ofono_slot_driver_reg *reg);
void ofono_slot_driver_started(struct ofono_slot_driver_reg *reg);

struct ofono_slot_manager *ofono_slot_manager_ref(struct ofono_slot_manager *m);
void ofono_slot_manager_unref(struct ofono_slot_manager *m);
void ofono_slot_manager_error(struct ofono_slot_manager *m, const char *key,
	const char *message);
unsigned long ofono_slot_manager_add_property_handler
	(struct ofono_slot_manager *m, enum ofono_slot_manager_property p,
		ofono_slot_manager_property_cb cb, void* data);
void ofono_slot_manager_remove_handler(struct ofono_slot_manager *m,
	unsigned long id);
void ofono_slot_manager_remove_handlers(struct ofono_slot_manager *m,
	unsigned long *ids, unsigned int n);

struct ofono_cell_info;
struct ofono_slot *ofono_slot_add(struct ofono_slot_manager *m,
	const char *path, enum ofono_radio_access_mode techs, const char *imei,
	const char *imeisv, enum ofono_slot_sim_presence sim_presence,
	enum ofono_slot_flags flags);
struct ofono_slot *ofono_slot_ref(struct ofono_slot *s);
void ofono_slot_unref(struct ofono_slot *s);
void ofono_slot_error(struct ofono_slot *s, const char *key, const char *msg);
void ofono_slot_set_cell_info(struct ofono_slot *s, struct ofono_cell_info *ci);
unsigned long ofono_slot_add_property_handler(struct ofono_slot *s,
	enum ofono_slot_property p, ofono_slot_property_cb cb, void* data);
void ofono_slot_remove_handler(struct ofono_slot *s, unsigned long id);
void ofono_slot_remove_handlers(struct ofono_slot *s, unsigned long *ids,
	unsigned int n);
void ofono_slot_set_sim_presence(struct ofono_slot *s,
	enum ofono_slot_sim_presence sim_presence);

#ifdef __cplusplus
}
#endif

#endif /* __OFONO_SLOT_H */
