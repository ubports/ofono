/*
 *  oFono - Open Source Telephony - RIL-based devices
 *
 *  Copyright (C) 2019-2021 Jolla Ltd.
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

#include <ofono/log.h>

#include <grilio_channel.h>

typedef struct ril_devmon_ds {
	struct ril_devmon pub;
	struct ril_devmon *ss;
	struct ril_devmon *ds;
} DevMon;

static inline DevMon *ril_devmon_auto_cast(struct ril_devmon *pub)
{
	return G_CAST(pub, DevMon, pub);
}

static struct ril_devmon_io *ril_devmon_auto_start_io(struct ril_devmon *devmon,
		GRilIoChannel *io, struct ofono_cell_info *cell_info)
{
	DevMon *self = ril_devmon_auto_cast(devmon);

	if (!self->ss) {
		/* We have already chosen SEND_DEVICE_STATE method */
		return ril_devmon_start_io(self->ds, io, cell_info);
	} else if (!self->ds) {
		/* We have already chosen SCREEN_STATE method */
		return ril_devmon_start_io(self->ss, io, cell_info);
	} else if (io->ril_version > 14 /* Covers binder implementation */) {
		/* Choose SEND_DEVICE_STATE method */
		DBG("%s: Will use SEND_DEVICE_STATE method", io->name);
		ril_devmon_free(self->ss);
		self->ss = NULL;
		return ril_devmon_start_io(self->ds, io, cell_info);
	} else {
		/* Choose legacy SCREEN_STATE method */
		DBG("%s: Will use SCREEN_STATE method", io->name);
		ril_devmon_free(self->ds);
		self->ds = NULL;
		return ril_devmon_start_io(self->ss, io, cell_info);
	}
}

static void ril_devmon_auto_free(struct ril_devmon *devmon)
{
	DevMon *self = ril_devmon_auto_cast(devmon);

	ril_devmon_free(self->ss);
	ril_devmon_free(self->ds);
	g_free(self);
}

struct ril_devmon *ril_devmon_auto_new(const struct ril_slot_config *config)
{
	DevMon *self = g_new0(DevMon, 1);

	/*
	 * Allocate both implementations at startup. We need to do that
	 * early so that connections to D-Bus daemon and services are
	 * established before we drop privileges. This isn't much of
	 * an overhead because those implementation don't do much until
	 * we actually start the I/O (at which point we drop one of those).
	 */
	self->pub.free = ril_devmon_auto_free;
	self->pub.start_io = ril_devmon_auto_start_io;
	self->ss = ril_devmon_ss_new(config);
	self->ds = ril_devmon_ds_new(config);
	return &self->pub;
}

/*
 * Local Variables:
 * mode: C
 * c-basic-offset: 8
 * indent-tabs-mode: t
 * End:
 */
