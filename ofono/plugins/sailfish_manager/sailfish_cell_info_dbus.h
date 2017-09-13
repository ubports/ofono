/*
 *  oFono - Open Source Telephony - RIL-based devices
 *
 *  Copyright (C) 2016-2017 Jolla Ltd.
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

#ifndef SAILFISH_CELL_INFO_DBUS_H
#define SAILFISH_CELL_INFO_DBUS_H

struct ofono_modem;

struct sailfish_cell_info;
struct sailfish_cell_info_dbus;

struct sailfish_cell_info_dbus *sailfish_cell_info_dbus_new
		(struct ofono_modem *modem, struct sailfish_cell_info *info);
void sailfish_cell_info_dbus_free(struct sailfish_cell_info_dbus *dbus);

#endif /* SAILFISH_CELL_INFO_DBUS_H */

/*
 * Local Variables:
 * mode: C
 * c-basic-offset: 8
 * indent-tabs-mode: t
 * End:
 */
