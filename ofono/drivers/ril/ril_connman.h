/*
 *  oFono - Open Source Telephony - RIL-based devices
 *
 *  Copyright (C) 2019 Jolla Ltd.
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

#ifndef RIL_CONNMAN_H
#define RIL_CONNMAN_H

#include <gutil_misc.h>

struct ril_connman {
	gboolean valid;          /* TRUE if other fields are valid */
	gboolean present;        /* ConnMan is present on D-Bus */
	gboolean tethering;      /* At least one technology is tethering */
	gboolean wifi_connected; /* WiFi network is connected */
};

enum ril_connman_property {
	RIL_CONNMAN_PROPERTY_ANY,
	RIL_CONNMAN_PROPERTY_VALID,
	RIL_CONNMAN_PROPERTY_PRESENT,
	RIL_CONNMAN_PROPERTY_TETHERING,
	RIL_CONNMAN_PROPERTY_WIFI_CONNECTED,
	RIL_CONNMAN_PROPERTY_COUNT
};

typedef void (*ril_connman_property_cb_t)(struct ril_connman *connman,
			enum ril_connman_property property, void *arg);

struct ril_connman *ril_connman_new(void);
struct ril_connman *ril_connman_ref(struct ril_connman *connman);
void ril_connman_unref(struct ril_connman *connman);

gulong ril_connman_add_property_changed_handler(struct ril_connman *connman,
	enum ril_connman_property p, ril_connman_property_cb_t cb, void *arg);
void ril_connman_remove_handler(struct ril_connman *connman, gulong id);
void ril_connman_remove_handlers(struct ril_connman *connman, gulong *ids,
								int n);

#define ril_connman_remove_all_handlers(connman, ids) \
	ril_connman_remove_handlers(connman, ids, G_N_ELEMENTS(ids))

#endif /* RIL_CONNMAN_H */

/*
 * Local Variables:
 * mode: C
 * c-basic-offset: 8
 * indent-tabs-mode: t
 * End:
 */
