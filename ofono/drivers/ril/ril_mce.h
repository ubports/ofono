/*
 *  oFono - Open Source Telephony - RIL-based devices
 *
 *  Copyright (C) 2015-2016 Jolla Ltd.
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

#ifndef RIL_MCE_H
#define RIL_MCE_H

#include "ril_types.h"

enum ril_mce_display_state {
	RIL_MCE_DISPLAY_OFF,
	RIL_MCE_DISPLAY_DIM,
	RIL_MCE_DISPLAY_ON
};

struct ril_mce_priv;
struct ril_mce {
	GObject object;
	struct ril_mce_priv *priv;
	enum ril_mce_display_state display_state;
};

struct ril_mce *ril_mce_new(void);
struct ril_mce *ril_mce_ref(struct ril_mce *mce);
void ril_mce_unref(struct ril_mce *mce);

typedef void (*ril_mce_cb_t)(struct ril_mce *mce, void *arg);
gulong ril_mce_add_display_state_changed_handler(struct ril_mce *mce,
					ril_mce_cb_t cb, void *arg);
void ril_mce_remove_handler(struct ril_mce *mce, gulong id);

#endif /* RIL_MCE_H */

/*
 * Local Variables:
 * mode: C
 * c-basic-offset: 8
 * indent-tabs-mode: t
 * End:
 */
