/*
 *  oFono - Open Source Telephony - RIL-based devices
 *
 *  Copyright (C) 2015-2021 Jolla Ltd.
 *  Copyright (C) 2019-2020 Open Mobile Platform LLC.
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

#include "ril_config.h"
#include "ril_util.h"
#include "ril_log.h"

#include <gutil_intarray.h>
#include <gutil_ints.h>
#include <gutil_misc.h>

#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>

/* Utilities for parsing ril_subscription.conf */

GUtilInts *ril_config_get_ints(GKeyFile *file, const char *group,
					const char *key)
{
	char *value = ofono_conf_get_string(file, group, key);

	if (value) {
		GUtilIntArray *array = gutil_int_array_new();
		char **values, **ptr;

		/*
		 * Some people are thinking that # is a comment
		 * anywhere on the line, not just at the beginning
		 */
		char *comment = strchr(value, '#');

		if (comment) *comment = 0;
		values = g_strsplit(value, ",", -1);
		ptr = values;

		while (*ptr) {
			int val;

			if (gutil_parse_int(*ptr++, 0, &val)) {
				gutil_int_array_append(array, val);
			}
		}

		g_free(value);
		g_strfreev(values);
		return gutil_int_array_free_to_ints(array);
	}
	return NULL;
}

char *ril_config_ints_to_string(GUtilInts *ints, char separator)
{
	if (ints) {
		guint i, n;
		const int *data = gutil_ints_get_data(ints, &n);
		GString *buf = g_string_new(NULL);

		for (i=0; i<n; i++) {
			if (buf->len > 0) {
				g_string_append_c(buf, separator);
			}
			g_string_append_printf(buf, "%d", data[i]);
		}
		return g_string_free(buf, FALSE);
	}
	return NULL;
}

/*
 * Local Variables:
 * mode: C
 * c-basic-offset: 8
 * indent-tabs-mode: t
 * End:
 */
