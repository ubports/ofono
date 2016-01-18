/*
 *  oFono - Open Source Telephony - RIL-based devices
 *
 *  Copyright (C) 2016 Jolla Ltd.
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

#ifndef RIL_DATA_H
#define RIL_DATA_H

#include "ril_types.h"

struct ril_data_manager;
struct ril_data_manager *ril_data_manager_new(void);
struct ril_data_manager *ril_data_manager_ref(struct ril_data_manager *dm);
void ril_data_manager_unref(struct ril_data_manager *dm);

struct ril_data *ril_data_new(struct ril_data_manager *dm, GRilIoChannel *io);
struct ril_data *ril_data_ref(struct ril_data *data);
void ril_data_unref(struct ril_data *data);
void ril_data_set_name(struct ril_data *data, const char *name);
void ril_data_allow(struct ril_data *data, gboolean allow);
gboolean ril_data_allowed(struct ril_data *data);

typedef void (*ril_data_cb_t)(struct ril_data *data, void *arg);
gulong ril_data_add_allow_changed_handler(struct ril_data *data,
					ril_data_cb_t cb, void *arg);
void ril_data_remove_handler(struct ril_data *data, gulong id);

#endif /* RIL_DATA_H */

/*
 * Local Variables:
 * mode: C
 * c-basic-offset: 8
 * indent-tabs-mode: t
 * End:
 */
