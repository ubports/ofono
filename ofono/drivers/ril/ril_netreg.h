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

#ifndef RIL_NETREG_H
#define RIL_NETREG_H

#include "ril_types.h"

#include <ofono/netreg.h>

enum ofono_netreg_status ril_netreg_check_if_really_roaming
		(struct ofono_netreg *reg, enum ofono_netreg_status status);

#endif /* RIL_NETREG_H */

/*
 * Local Variables:
 * mode: C
 * c-basic-offset: 8
 * indent-tabs-mode: t
 * End:
 */
