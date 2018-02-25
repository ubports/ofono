/*
 *  oFono - Open Source Telephony - RIL-based devices
 *
 *  Copyright (C) 2016-2018 Jolla Ltd.
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

#ifndef RIL_ECCLIST_H
#define RIL_ECCLIST_H

#include "ril_types.h"

#include <glib-object.h>

struct ril_ecclist_priv;

struct ril_ecclist {
	GObject object;
	struct ril_ecclist_priv *priv;
	char **list;
};

typedef void (*ril_ecclist_cb_t)(struct ril_ecclist *ecc, void *arg);

struct ril_ecclist *ril_ecclist_new(const char *path);
struct ril_ecclist *ril_ecclist_ref(struct ril_ecclist *ecc);
void ril_ecclist_unref(struct ril_ecclist *ecc);
gulong ril_ecclist_add_list_changed_handler(struct ril_ecclist *ecc,
					ril_ecclist_cb_t cb, void *arg);
void ril_ecclist_remove_handler(struct ril_ecclist *ecc, gulong id);

#endif /* RIL_ECCLIST_H */

/*
 * Local Variables:
 * mode: C
 * c-basic-offset: 8
 * indent-tabs-mode: t
 * End:
 */
