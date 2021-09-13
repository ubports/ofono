/*
 *  oFono - Open Source Telephony - RIL-based devices
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

#ifndef RIL_CELL_INFO_H
#define RIL_CELL_INFO_H

#include "ril_types.h"

#include <ofono/cell-info.h>

struct ofono_cell_info *ril_cell_info_new(GRilIoChannel *io,
			const char *log_prefix, struct ril_radio *radio,
			struct ril_sim_card *sim_card);

#endif /* RIL_CELL_INFO_H */

/*
 * Local Variables:
 * mode: C
 * c-basic-offset: 8
 * indent-tabs-mode: t
 * End:
 */
