/*
 *  oFono - Open Source Telephony
 *
 *  Copyright (C) 2019-2022 Jolla Ltd.
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

#ifndef OFONO_WATCH_PRIVATE_H
#define OFONO_WATCH_PRIVATE_H

#include <ofono/watch.h>

void __ofono_watch_gprs_settings_changed(const char *path,
			enum ofono_gprs_context_type type,
			const struct ofono_gprs_primary_context *settings);

#endif /* OFONO_WATCH_PRIVATE_H */

/*
 * Local Variables:
 * mode: C
 * c-basic-offset: 8
 * indent-tabs-mode: t
 * End:
 */
