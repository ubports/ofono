/*
 *  oFono - Open Source Telephony
 *
 *  Copyright (C) 2018-2021 Jolla Ltd.
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

#ifndef FAKE_CELL_INFO_H
#define FAKE_CELL_INFO_H

#include <ofono/cell-info.h>

struct ofono_cell_info *fake_cell_info_new(void);
void fake_cell_info_add_cell(struct ofono_cell_info *info,
				  const struct ofono_cell* cell);
ofono_bool_t fake_cell_info_remove_cell(struct ofono_cell_info *info,
				  const struct ofono_cell* cell);
void fake_cell_info_remove_all_cells(struct ofono_cell_info *info);
void fake_cell_info_cells_changed(struct ofono_cell_info *info);

#endif /* FAKE_CELL_INFO_H */

/*
 * Local Variables:
 * mode: C
 * c-basic-offset: 8
 * indent-tabs-mode: t
 * End:
 */
