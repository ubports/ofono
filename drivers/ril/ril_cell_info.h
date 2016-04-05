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
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 *  GNU General Public License for more details.
 */

#ifndef RIL_CELL_INFO_H
#define RIL_CELL_INFO_H

#include "ril_types.h"

struct ril_cell {
	enum ril_cell_info_type type;
	gboolean registered;
	union {
		struct ril_cell_info_gsm gsm;
		struct ril_cell_info_wcdma wcdma;
		struct ril_cell_info_lte lte;
	} info;
};

struct ril_cell_info_priv;
struct ril_cell_info {
	GObject object;
	struct ril_cell_info_priv *priv;
	GSList *cells;
};

typedef void (*ril_cell_info_cb_t)(struct ril_cell_info *info, void *arg);

gint ril_cell_compare_func(gconstpointer v1, gconstpointer v2);
gint ril_cell_compare_location(const struct ril_cell *c1,
					const struct ril_cell *c2);

struct ril_cell_info *ril_cell_info_new(GRilIoChannel *io,
		const char *log_prefix, struct ril_mce *mce,
		struct ril_radio *radio, struct ril_sim_card *sim_card);
struct ril_cell_info *ril_cell_info_ref(struct ril_cell_info *info);
void ril_cell_info_unref(struct ril_cell_info *info);
struct ril_cell *ril_cell_find_cell(struct ril_cell_info *info,
						const struct ril_cell *cell);
gulong ril_cell_info_add_cells_changed_handler(struct ril_cell_info *info,
					ril_cell_info_cb_t cb, void *arg);
void ril_cell_info_remove_handler(struct ril_cell_info *info, gulong id);

#endif /* RIL_CELL_INFO_H */

/*
 * Local Variables:
 * mode: C
 * c-basic-offset: 8
 * indent-tabs-mode: t
 * End:
 */
