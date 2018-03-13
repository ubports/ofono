/*
 *  oFono - Open Source Telephony - RIL-based devices
 *
 *  Copyright (C) 2015-2018 Jolla Ltd.
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

#ifndef RIL_CONFIG_H
#define RIL_CONFIG_H

#include "ril_types.h"

/* Utilities for parsing ril_subscription.conf */

#define RILCONF_SETTINGS_GROUP      "Settings"

void ril_config_merge_files(GKeyFile *conf, const char *file);

char *ril_config_get_string(GKeyFile *file, const char *group,
					const char *key);
char **ril_config_get_strings(GKeyFile *file, const char *group,
			      const char *key, char delimiter);
gboolean ril_config_get_integer(GKeyFile *file, const char *group,
					const char *key, int *value);
gboolean ril_config_get_boolean(GKeyFile *file, const char *group,
					const char *key, gboolean *value);
gboolean ril_config_get_flag(GKeyFile *file, const char *group,
					const char *key, int flag, int *flags);
gboolean ril_config_get_enum(GKeyFile *file, const char *group,
					const char *key, int *result,
					const char *name, int value, ...);
GUtilInts *ril_config_get_ints(GKeyFile *file, const char *group,
					const char *key);
char *ril_config_ints_to_string(GUtilInts *ints, char separator);

#endif /* RIL_CONFIG_H */

/*
 * Local Variables:
 * mode: C
 * c-basic-offset: 8
 * indent-tabs-mode: t
 * End:
 */
