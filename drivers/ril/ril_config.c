/*
 *  oFono - Open Source Telephony - RIL-based devices
 *
 *  Copyright (C) 2015-2017 Jolla Ltd.
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

#include <gutil_intarray.h>
#include <gutil_ints.h>

/* Utilities for parsing ril_subscription.conf */

char* ril_config_get_string(GKeyFile *file, const char *group, const char *key)
{
	char *val = g_key_file_get_string(file, group, key, NULL);

	if (!val && strcmp(group, RILCONF_SETTINGS_GROUP)) {
		/* Check the common section */
		val = g_key_file_get_string(file, RILCONF_SETTINGS_GROUP, key,
									NULL);
	}
	return val;
}

gboolean ril_config_get_integer(GKeyFile *file, const char *group,
					const char *key, int *out_value)
{
	GError *error = NULL;
	int value = g_key_file_get_integer(file, group, key, &error);

	if (!error) {
		if (out_value) {
			*out_value = value;
		}
		return TRUE;
	} else {
		g_error_free(error);
		if (strcmp(group, RILCONF_SETTINGS_GROUP)) {
			/* Check the common section */
			error = NULL;
			value = g_key_file_get_integer(file,
					RILCONF_SETTINGS_GROUP, key, &error);
			if (!error) {
				if (out_value) {
					*out_value = value;
				}
				return TRUE;
			}
			g_error_free(error);
		}
		return FALSE;
	}
}

gboolean ril_config_get_boolean(GKeyFile *file, const char *group,
					const char *key, gboolean *out_value)
{
	GError *error = NULL;
	gboolean value = g_key_file_get_boolean(file, group, key, &error);

	if (!error) {
		if (out_value) {
			*out_value = value;
		}
		return TRUE;
	} else {
		g_error_free(error);
		if (strcmp(group, RILCONF_SETTINGS_GROUP)) {
			/* Check the common section */
			error = NULL;
			value = g_key_file_get_boolean(file,
					RILCONF_SETTINGS_GROUP, key, &error);
			if (!error) {
				if (out_value) {
					*out_value = value;
				}
				return TRUE;
			}
			g_error_free(error);
		}
		return FALSE;
	}
}

gboolean ril_config_get_flag(GKeyFile *file, const char *group,
					const char *key, int flag, int *flags)
{
	gboolean value;

	if (ril_config_get_boolean(file, group, key, &value)) {
		if (value) {
			*flags |= flag;
		} else {
			*flags &= ~flag;
		}
		return TRUE;
	} else {
		return FALSE;
	}
}

GUtilInts *ril_config_get_ints(GKeyFile *file, const char *group,
					const char *key)
{
	char* value = ril_config_get_string(file, group, key);

	if (value) {
		char **values = g_strsplit(value, ",", -1);
		char **ptr = values;
		GUtilIntArray* array = gutil_int_array_new();

		while (*ptr) {
			const char *str = *ptr++;
			char *end = NULL;
			long ival = strtol(str, &end, 0);

			if (str[0] && !end[0]) {
				gutil_int_array_append(array, ival);
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
		GString* buf = g_string_new(NULL);

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
