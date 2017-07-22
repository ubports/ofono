/*
 *  oFono - Open Source Telephony
 *
 *  Copyright (C) 2016-2017 Jolla Ltd.
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

#ifndef SAILFISH_MANAGER_DBUS_H
#define SAILFISH_MANAGER_DBUS_H

#include <sailfish_manager.h>

struct sailfish_manager_dbus;

enum sailfish_manager_dbus_block {
	SAILFISH_MANAGER_DBUS_BLOCK_NONE  = 0,
	SAILFISH_MANAGER_DBUS_BLOCK_MODEM = 0x01,
	SAILFISH_MANAGER_DBUS_BLOCK_IMEI  = 0x02,
	SAILFISH_MANAGER_DBUS_BLOCK_ALL   = 0x03
};

enum sailfish_manager_dbus_signal {
	SAILFISH_MANAGER_SIGNAL_NONE          = 0,
	SAILFISH_MANAGER_SIGNAL_VOICE_IMSI    = 0x01,
	SAILFISH_MANAGER_SIGNAL_DATA_IMSI     = 0x02,
	SAILFISH_MANAGER_SIGNAL_VOICE_PATH    = 0x04,
	SAILFISH_MANAGER_SIGNAL_DATA_PATH     = 0x08,
	SAILFISH_MANAGER_SIGNAL_ENABLED_SLOTS = 0x10,
	SAILFISH_MANAGER_SIGNAL_MMS_IMSI      = 0x20,
	SAILFISH_MANAGER_SIGNAL_MMS_PATH      = 0x40,
	SAILFISH_MANAGER_SIGNAL_READY         = 0x80
};

/* Functionality provided by sailfish_manager to sailfish_manager_dbus */
struct sailfish_manager_dbus_cb {
	GHashTable *(*get_errors)(struct sailfish_manager *m);
	GHashTable *(*get_slot_errors)(const struct sailfish_slot *s);
	void (*set_enabled_slots)(struct sailfish_manager *m, char **slots);
	gboolean (*set_mms_imsi)(struct sailfish_manager *m, const char *imsi);
	void (*set_default_voice_imsi)(struct sailfish_manager *m,
							const char *imsi);
	void (*set_default_data_imsi)(struct sailfish_manager *m,
							const char *imsi);
};

struct sailfish_manager_dbus *sailfish_manager_dbus_new
		(struct sailfish_manager *m,
			const struct sailfish_manager_dbus_cb *cb);
void sailfish_manager_dbus_free(struct sailfish_manager_dbus *d);
void sailfish_manager_dbus_set_block(struct sailfish_manager_dbus *d,
				enum sailfish_manager_dbus_block b);
void sailfish_manager_dbus_signal(struct sailfish_manager_dbus *d,
				enum sailfish_manager_dbus_signal m);
void sailfish_manager_dbus_signal_sim(struct sailfish_manager_dbus *d,
				int index, gboolean present);
void sailfish_manager_dbus_signal_error(struct sailfish_manager_dbus *d,
				const char *id, const char *message);
void sailfish_manager_dbus_signal_modem_error(struct sailfish_manager_dbus *d,
				int index, const char *id, const char *msg);

#endif /* SAILFISH_MANAGER_DBUS_H */

/*
 * Local Variables:
 * mode: C
 * c-basic-offset: 8
 * indent-tabs-mode: t
 * End:
 */
