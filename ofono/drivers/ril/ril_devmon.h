/*
 *  oFono - Open Source Telephony - RIL-based devices
 *
 *  Copyright (C) 2019-2020 Jolla Ltd.
 *  Copyright (C) 2020 Open Mobile Platform LLC.
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

#ifndef RIL_DEVMON_H
#define RIL_DEVMON_H

#include "ril_cell_info.h"

/*
 * Separate instance of ril_devmon is created for each modem.
 * Device monitor is started after RIL has been connected.
 */

struct ril_devmon_io {
	void (*free)(struct ril_devmon_io *devmon_io);
};

struct ril_devmon {
	void (*free)(struct ril_devmon *devmon);
	struct ril_devmon_io *(*start_io)(struct ril_devmon *devmon,
		GRilIoChannel *channel, struct sailfish_cell_info *cell_info);
};

/*
 * Legacy Device Monitor uses RIL_REQUEST_SCREEN_STATE to tell
 * the modem when screen turns on and off.
 */
struct ril_devmon *ril_devmon_ss_new(const struct ril_slot_config *config);

/*
 * This Device Monitor uses RIL_REQUEST_SEND_DEVICE_STATE to let
 * the modem choose the right power saving strategy. It basically
 * mirrors the logic of Android's DeviceStateMonitor class.
 */
struct ril_devmon *ril_devmon_ds_new(const struct ril_slot_config *config);

/*
 * This Device Monitor implementation controls network state updates
 * by sending SET_UNSOLICITED_RESPONSE_FILTER.
 */
struct ril_devmon *ril_devmon_ur_new(const struct ril_slot_config *config);

/*
 * This one selects the type based on the RIL version.
 */
struct ril_devmon *ril_devmon_auto_new(const struct ril_slot_config *config);

/*
 * This one combines several methods. Takes ownership of ril_devmon objects.
 */
struct ril_devmon *ril_devmon_combine(struct ril_devmon *devmon[], guint n);

/* Utilities (NULL tolerant) */
struct ril_devmon_io *ril_devmon_start_io(struct ril_devmon *devmon,
		GRilIoChannel *channel, struct sailfish_cell_info *cell_info);
void ril_devmon_io_free(struct ril_devmon_io *devmon_io);
void ril_devmon_free(struct ril_devmon *devmon);

#endif /* RIL_CONNMAN_H */

/*
 * Local Variables:
 * mode: C
 * c-basic-offset: 8
 * indent-tabs-mode: t
 * End:
 */
