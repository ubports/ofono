/*
 *  oFono - Open Source Telephony - RIL-based devices
 *
 *  Copyright (C) 2020 Jolla Ltd.
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

#include "ril_devmon.h"

#include <ofono/log.h>

typedef struct ril_devmon_combine {
	struct ril_devmon pub;
	struct ril_devmon **impl;
	guint count;
} DevMon;

typedef struct ril_devmon_combine_io {
	struct ril_devmon_io pub;
	struct ril_devmon_io **impl;
	guint count;
} DevMonIo;

static inline DevMon *ril_devmon_combine_cast(struct ril_devmon *dm)
{
	return G_CAST(dm, DevMon, pub);
}

static inline DevMonIo *ril_devmon_ds_io_cast(struct ril_devmon_io *io)
{
	return G_CAST(io, DevMonIo, pub);
}

static void ril_devmon_combine_io_free(struct ril_devmon_io *io)
{
	guint i;
	DevMonIo *self = ril_devmon_ds_io_cast(io);

	for (i = 0; i < self->count; i++) {
		ril_devmon_io_free(self->impl[i]);
	}
	g_free(self);
}

static struct ril_devmon_io *ril_devmon_combine_start_io(struct ril_devmon *dm,
			GRilIoChannel *chan, struct sailfish_cell_info *ci)
{
	guint i;
	DevMon *self = ril_devmon_combine_cast(dm);
	DevMonIo *io = g_malloc0(sizeof(DevMonIo) +
				sizeof(struct ril_devmon_io *) * self->count);

	io->pub.free = ril_devmon_combine_io_free;
	io->impl = (struct ril_devmon_io**)(io + 1);
	io->count = self->count;
	for (i = 0; i < io->count; i++) {
		io->impl[i] = ril_devmon_start_io(self->impl[i], chan, ci);
	}
	return &io->pub;
}

static void ril_devmon_combine_free(struct ril_devmon *dm)
{
	DevMon *self = ril_devmon_combine_cast(dm);
	guint i;

	for (i = 0; i < self->count; i++) {
		ril_devmon_free(self->impl[i]);
	}
	g_free(self);
}

struct ril_devmon *ril_devmon_combine(struct ril_devmon *dm[], guint n)
{
	guint i;
	DevMon *self = g_malloc0(sizeof(DevMon) +
					sizeof(struct ril_devmon *) * n);

	self->pub.free = ril_devmon_combine_free;
	self->pub.start_io = ril_devmon_combine_start_io;
	self->impl = (struct ril_devmon **)(self + 1);
	self->count = n;
	for (i = 0; i < n; i++) {
		self->impl[i] = dm[i];
	}
	return &self->pub;
}

/*
 * Local Variables:
 * mode: C
 * c-basic-offset: 8
 * indent-tabs-mode: t
 * End:
 */
