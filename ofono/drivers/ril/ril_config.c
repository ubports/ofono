/*
 *  oFono - Open Source Telephony - RIL-based devices
 *
 *  Copyright (C) 2015-2020 Jolla Ltd.
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

char *ril_config_get_string(GKeyFile *file, const char *group, const char *key)
{
	char *val = g_key_file_get_string(file, group, key, NULL);

	if (!val && strcmp(group, RILCONF_SETTINGS_GROUP)) {
		/* Check the common section */
		val = g_key_file_get_string(file, RILCONF_SETTINGS_GROUP, key,
									NULL);
	}
	return val;
}

char **ril_config_get_strings(GKeyFile *file, const char *group,
					const char *key, char delimiter)
{
	char *str = ril_config_get_string(file, group, key);

	if (str) {
		char **strv, **p;
		char delimiter_str[2];

		delimiter_str[0] = delimiter;
		delimiter_str[1] = 0;
		strv = g_strsplit(str, delimiter_str, -1);

		/* Strip whitespaces */
		for (p = strv; *p; p++) {
			*p = g_strstrip(*p);
		}

		g_free(str);
		return strv;
	}

	return NULL;
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

gboolean ril_config_get_enum(GKeyFile *file, const char *group,
					const char *key, int *result,
					const char *name, int value, ...)
{
	char *str = ril_config_get_string(file, group, key);

	if (str) {
		/*
		 * Some people are thinking that # is a comment
		 * anywhere on the line, not just at the beginning
		 */
		char *comment = strchr(str, '#');

		if (comment) *comment = 0;
		g_strstrip(str);
		if (strcasecmp(str, name)) {
			va_list args;
			va_start(args, value);
			while ((name = va_arg(args, char*)) != NULL) {
				value = va_arg(args, int);
				if (!strcasecmp(str, name)) {
					break;
				}
			}
			va_end(args);
		}

		if (!name) {
			ofono_error("Invalid %s config value (%s)", key, str);
		}

		g_free(str);

		if (name) {
			if (result) {
				*result = value;
			}
			return TRUE;
		}
	}

	return FALSE;
}

gboolean ril_config_get_mask(GKeyFile *file, const char *group,
					const char *key, int *result,
					const char *name, int value, ...)
{
	char *str = ril_config_get_string(file, group, key);
	gboolean ok = FALSE;
	
	if (result) {
		*result = 0;
	}

	if (str) {
		/*
		 * Some people are thinking that # is a comment
		 * anywhere on the line, not just at the beginning
		 */
		char *comment = strchr(str, '#');
		char **values, **ptr;

		if (comment) *comment = 0;
		values = g_strsplit(str, "+", -1);

		for (ok = TRUE, ptr = values; *ptr && ok; ptr++) {
			const char* found_str = NULL;
			const char* s = g_strstrip(*ptr);

			if (!strcasecmp(s, name)) {
				found_str = name;
				if (result) {
					*result |= value;
				}
			} else {
				va_list args;
				const char* known;

				va_start(args, value);
				while ((known = va_arg(args, char*)) != NULL) {
					const int bit = va_arg(args, int);

					if (!strcasecmp(s, known)) {
						found_str = known;
						if (result) {
							*result |= bit;
						}
						break;
					}
				}
				va_end(args);
			}

			if (!found_str) {
				ofono_error("Unknown bit '%s' in %s", s, key);
				ok = FALSE;
			}
		}

		g_strfreev(values);
		g_free(str);
	}

	if (!ok && result) {
		*result = 0;
	}
	return ok;
}

GUtilInts *ril_config_get_ints(GKeyFile *file, const char *group,
					const char *key)
{
	char *value = ril_config_get_string(file, group, key);

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
