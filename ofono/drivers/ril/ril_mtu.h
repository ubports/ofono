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

#ifndef RIL_MTU_H
#define RIL_MTU_H

#include "ril_types.h"

struct ril_mtu_watch *ril_mtu_watch_new(int max_mtu);
void ril_mtu_watch_free(struct ril_mtu_watch *mw);
void ril_mtu_watch_set_ifname(struct ril_mtu_watch *mw, const char *ifname);

#endif /* RIL_MTU_H */

/*
 * Local Variables:
 * mode: C
 * c-basic-offset: 8
 * indent-tabs-mode: t
 * End:
 */
