/*
 *  oFono - Open Source Telephony
 *
 *  Copyright (C) 2016-2021 Jolla Ltd.
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

#ifndef SLOT_MANAGER_DBUS_H
#define SLOT_MANAGER_DBUS_H

#include <ofono/slot.h>

#include <glib.h>

struct slot_manager_dbus;

enum slot_manager_dbus_block {
	SLOT_MANAGER_DBUS_BLOCK_NONE  = 0,
	SLOT_MANAGER_DBUS_BLOCK_MODEM = 0x01,
	SLOT_MANAGER_DBUS_BLOCK_IMEI  = 0x02,
	SLOT_MANAGER_DBUS_BLOCK_ALL   = 0x03
};

enum slot_manager_dbus_signal {
	SLOT_MANAGER_DBUS_SIGNAL_NONE          = 0,
	SLOT_MANAGER_DBUS_SIGNAL_VOICE_IMSI    = 0x01,
	SLOT_MANAGER_DBUS_SIGNAL_DATA_IMSI     = 0x02,
	SLOT_MANAGER_DBUS_SIGNAL_VOICE_PATH    = 0x04,
	SLOT_MANAGER_DBUS_SIGNAL_DATA_PATH     = 0x08,
	SLOT_MANAGER_DBUS_SIGNAL_ENABLED_SLOTS = 0x10,
	SLOT_MANAGER_DBUS_SIGNAL_MMS_IMSI      = 0x20,
	SLOT_MANAGER_DBUS_SIGNAL_MMS_PATH      = 0x40,
	SLOT_MANAGER_DBUS_SIGNAL_READY         = 0x80
};

enum slot_manager_dbus_slot_signal {
	SLOT_MANAGER_DBUS_SLOT_SIGNAL_NONE      = 0,
	SLOT_MANAGER_DBUS_SLOT_SIGNAL_PRESENT   = 0x01
};

/* Functionality provided by slot_manager to slot_manager_dbus */
struct slot_manager_dbus_cb {
	GHashTable *(*get_errors)(const struct ofono_slot_manager *mgr);
	GHashTable *(*get_slot_errors)(const struct ofono_slot *slot);
	void (*set_enabled_slots)(struct ofono_slot_manager *mgr, char **slots);
	gboolean (*set_mms_imsi)(struct ofono_slot_manager *mgr,
							const char *imsi);
	void (*set_default_voice_imsi)(struct ofono_slot_manager *mgr,
							const char *imsi);
	void (*set_default_data_imsi)(struct ofono_slot_manager *mgr,
							const char *imsi);
};

struct slot_manager_dbus *slot_manager_dbus_new
		(struct ofono_slot_manager *m,
			const struct slot_manager_dbus_cb *cb);
void slot_manager_dbus_free(struct slot_manager_dbus *d);
void slot_manager_dbus_set_block(struct slot_manager_dbus *d,
				enum slot_manager_dbus_block b);
void slot_manager_dbus_signal(struct slot_manager_dbus *d,
				enum slot_manager_dbus_signal mask);
void slot_manager_dbus_signal_sim(struct slot_manager_dbus *d, int index,
				  enum slot_manager_dbus_slot_signal mask);
void slot_manager_dbus_signal_error(struct slot_manager_dbus *d,
				const char *id, const char *message);
void slot_manager_dbus_signal_modem_error(struct slot_manager_dbus *d,
				int index, const char *id, const char *msg);

#endif /* SLOT_MANAGER_DBUS_H */

/*
 * Local Variables:
 * mode: C
 * c-basic-offset: 8
 * indent-tabs-mode: t
 * End:
 */
