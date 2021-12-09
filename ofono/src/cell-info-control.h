/*
 *  oFono - Open Source Telephony - RIL-based devices
 *
 *  Copyright (C) 2021 Jolla Ltd.
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

#ifndef CELL_INFO_CONTROL_H
#define CELL_INFO_CONTROL_H

#include <ofono/types.h>

struct ofono_cell_info;

typedef struct cell_info_control {
	const char* path;
	struct ofono_cell_info *info;
} CellInfoControl;

CellInfoControl *cell_info_control_get(const char* path);
CellInfoControl *cell_info_control_ref(CellInfoControl *ctl);
void cell_info_control_unref(CellInfoControl *ctl);
void cell_info_control_set_cell_info(CellInfoControl *ctl,
				struct ofono_cell_info *ci);
void cell_info_control_drop_all_requests(CellInfoControl *ctl);
void cell_info_control_drop_requests(CellInfoControl *ctl, void *tag);

/* ofono_cell_info gets enabled if there's at least one request to enable it */
void cell_info_control_set_enabled(CellInfoControl *ctl, void *tag,
				ofono_bool_t enabled);

/* the actual update interval will be the smallest of the requested */
void cell_info_control_set_update_interval(CellInfoControl *ctl, void *tag,
				int ms);

#endif /* CELL_INFO_CONTROL_H */

/*
 * Local Variables:
 * mode: C
 * c-basic-offset: 8
 * indent-tabs-mode: t
 * End:
 */
