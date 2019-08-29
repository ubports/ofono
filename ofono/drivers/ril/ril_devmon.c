/*
 *  oFono - Open Source Telephony - RIL-based devices
 *
 *  Copyright (C) 2019 Jolla Ltd.
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

#include "ril_devmon.h"

struct ril_devmon_io *ril_devmon_start_io(struct ril_devmon *devmon,
		GRilIoChannel *channel, struct sailfish_cell_info *cell_info)
{
	return devmon ? devmon->start_io(devmon, channel, cell_info) : NULL;
}

void ril_devmon_io_free(struct ril_devmon_io *devmon_io)
{
	if (devmon_io) {
		devmon_io->free(devmon_io);
	}
}

void ril_devmon_free(struct ril_devmon *devmon)
{
	if (devmon) {
		devmon->free(devmon);
	}
}

/*
 * Local Variables:
 * mode: C
 * c-basic-offset: 8
 * indent-tabs-mode: t
 * End:
 */
