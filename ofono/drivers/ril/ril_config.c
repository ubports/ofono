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

/**
 * The ril_config_merge_files() function does the following:
 *
 * 1. Loads the specified key file (say, "/etc/foo.conf")
 * 2. Scans the subdirectory named after the file (e.g. "/etc/foo.d/")
 *    for the files with the same suffix as the main file (e.g. "*.conf")
 * 3. Sorts the files from the subdirectory (alphabetically)
 * 4. Merges the contents of the additional files with the main file
 *    according to their sort order.
 *
 * When the entries are merged, keys and groups overwrite the exising
 * ones by default. Keys can be suffixed with special characters to
 * remove or modify the existing entries instead:
 *
 * ':' Sets the (default) value if the key is missing
 * '+' Appends values to the string list
 * '?' Appends only new (non-existent) values to the string list
 * '-' Removes the values from the string list
 *
 * Both keys and groups can be prefixed with '!' to remove the entire key
 * or group.
 *
 * For example if we merge these two files:
 *
 *   /etc/foo.conf:
 *
 *   [foo]
 *   a=1
 *   b=2,3
 *   c=4
 *   d=5
 *   [bar]
 *   e=5
 *
 *   /etc/foo.d/bar.conf:
 *
 *   [foo]
 *   a+=2
 *   b-=2
 *   c=5
 *   !d
 *   [!bar]
 *
 * we end up with this:
 *
 *   [foo]
 *   a=1
 *   b=2,3
 *   c=5
 *
 * Not that the list separator is assumed to be ',' (rather than default ';').
 * The keyfile passed to ril_config_merge_files() should use the same list
 * separator, because the default values are copied from the config files
 * as is.
 */

static gint ril_config_sort_files(gconstpointer a, gconstpointer b)
{
	/* The comparison function for g_ptr_array_sort() doesn't take
	 * the pointers from the array as arguments, it takes pointers
	 * to the pointers in the array. */
	return strcmp(*(char**)a, *(char**)b);
}

static char **ril_config_collect_files(const char *path, const char *suffix)
{
	/* Returns sorted list of regular files in the directory,
	 * optionally having the specified suffix (e.g. ".conf").
	 * Returns NULL if nothing appropriate has been found. */
	char **files = NULL;
	DIR *d = opendir(path);

	if (d) {
		GPtrArray *list = g_ptr_array_new();
		const struct dirent *p;

		while ((p = readdir(d)) != NULL) {
			/* No need to even stat . and .. */
			if (strcmp(p->d_name, ".") &&
				strcmp(p->d_name, "..") && (!suffix ||
					g_str_has_suffix(p->d_name, suffix))) {
				struct stat st;
				char *buf = g_strconcat(path, "/", p->d_name,
									NULL);

				if (!stat(buf, &st) && S_ISREG(st.st_mode)) {
					g_ptr_array_add(list, buf);
				} else {
					g_free(buf);
				}
			}
                }

		if (list->len > 0) {
			g_ptr_array_sort(list, ril_config_sort_files);
			g_ptr_array_add(list, NULL);
			files = (char**)g_ptr_array_free(list, FALSE);
		} else {
			g_ptr_array_free(list, TRUE);
		}

                closedir(d);
        }
	return files;
}

static int ril_config_list_find(char **list, gsize len, const char *value)
{
	guint i;

	for (i = 0; i < len; i++) {
		if (!strcmp(list[i], value)) {
			return i;
		}
	}

	return -1;
}

static void ril_config_list_append(GKeyFile *conf, GKeyFile *k,
				const char *group, const char *key,
				char **values, gsize n, gboolean unique)
{
	/* Note: will steal strings from values */
	if (n > 0) {
		int i;
		gsize len = 0;
		gchar **list = g_key_file_get_string_list(conf, group, key,
							&len, NULL);
		GPtrArray *newlist = g_ptr_array_new_full(0, g_free);

		for (i = 0; i < (int)len; i++) {
			g_ptr_array_add(newlist, list[i]);
		}

		for (i = 0; i < (int)n; i++) {
			char *val = values[i];

			if (!unique || ril_config_list_find((char**)
				newlist->pdata, newlist->len, val) < 0) {
				/* Move the string to the new list */
				g_ptr_array_add(newlist, val);
				memmove(values + i, values + i + 1,
						sizeof(char*) * (n - i));
				i--;
				n--;
			}
		}

		if (newlist->len > len) {
			g_key_file_set_string_list(conf, group, key,
				(const gchar * const *) newlist->pdata,
				newlist->len);
		}

		/* Strings are deallocated by GPtrArray */
		g_ptr_array_free(newlist, TRUE);
		g_free(list);
	}
}

static void ril_config_list_remove(GKeyFile *conf, GKeyFile *k,
		const char *group, const char *key, char **values, gsize n)
{
	if (n > 0) {
		gsize len = 0;
		gchar **list = g_key_file_get_string_list(conf, group, key,
								&len, NULL);

		if (len > 0) {
			gsize i;
			const gsize oldlen = len;

			for (i = 0; i < n; i++) {
				int pos;

				/* Remove all matching values */
				while ((pos = ril_config_list_find(list, len,
							values[i])) >= 0) {
					g_free(list[pos]);
					memmove(list + pos, list + pos + 1,
						sizeof(char*) * (len - pos));
					len--;
				}
			}

			if (len < oldlen) {
				g_key_file_set_string_list(conf, group, key,
					(const gchar * const *) list, len);
			}
		}

		g_strfreev(list);
	}
}

static void ril_config_merge_group(GKeyFile *conf, GKeyFile *k,
							const char *group)
{
	gsize i, n = 0;
	char **keys = g_key_file_get_keys(k, group, &n, NULL);

	for (i=0; i<n; i++) {
		char *key = keys[i];

		if (key[0] == '!') {
			if (key[1]) {
				g_key_file_remove_key(conf, group, key+1, NULL);
			}
		} else {
			const gsize len = strlen(key);
			const char last = (len > 0) ? key[len-1] : 0;

			if (last == '+' || last == '?') {
				gsize count = 0;
				gchar **values = g_key_file_get_string_list(k,
						group, key, &count, NULL);

				key[len-1] = 0;
				ril_config_list_append(conf, k, group, key,
						values, count, last == '?');
				g_strfreev(values);
			} else if (last == '-') {
				gsize count = 0;
				gchar **values = g_key_file_get_string_list(k,
						group, key, &count, NULL);

				key[len-1] = 0;
				ril_config_list_remove(conf, k, group, key,
							values, count);
				g_strfreev(values);
			} else {
				/* Overwrite the value (it must exist in k) */
				gchar *value = g_key_file_get_value(k, group,
								key, NULL);

				if (last == ':') {
					/* Default value */
					key[len-1] = 0;
					if (!g_key_file_has_key(conf,
							group, key, NULL)) {
						g_key_file_set_value(conf,
							group, key, value);
					}
				} else {
					g_key_file_set_value(conf, group, key,
									value);
				}
				g_free(value);
			}
		}
	}

	g_strfreev(keys);
}

static void ril_config_merge_keyfile(GKeyFile *conf, GKeyFile *k)
{
	gsize i, n = 0;
	char **groups = g_key_file_get_groups(k, &n);

	for (i=0; i<n; i++) {
		const char *group = groups[i];

		if (group[0] == '!') {
			g_key_file_remove_group(conf, group + 1, NULL);
		} else {
			ril_config_merge_group(conf, k, group);
		}
	}

	g_strfreev(groups);
}

static void ril_config_merge_file(GKeyFile *conf, const char *file)
{
	GKeyFile *k = g_key_file_new();

	g_key_file_set_list_separator(k, ',');

	if (g_key_file_load_from_file(k, file, 0, NULL)) {
		ril_config_merge_keyfile(conf, k);
	}

	g_key_file_unref(k);
}

void ril_config_merge_files(GKeyFile *conf, const char *file)
{
	if (conf && file && file[0]) {
		char *dot = strrchr(file, '.');
		const char *suffix;
		char *dir;
		char **files;

		if (!dot) {
			dir = g_strconcat(file, ".d", NULL);
			suffix = NULL;
		} else if (!dot[1]) {
			dir = g_strconcat(file, "d", NULL);
			suffix = NULL;
		} else {
			/* 2 bytes for ".d" and 1 for NULL terminator */
			dir = g_malloc(dot - file + 3);
			strncpy(dir, file, dot - file);
			strcpy(dir + (dot - file), ".d");
			suffix = dot + 1;
		}

		files = ril_config_collect_files(dir, suffix);
		g_free(dir);

		/* Load the main config */
		if (g_file_test(file, G_FILE_TEST_EXISTS)) {
			DBG("Loading %s", file);
			ril_config_merge_file(conf, file);
		}

		if (files) {
			char **ptr;

			for (ptr = files; *ptr; ptr++) {
				DBG("Merging %s", *ptr);
				ril_config_merge_file(conf, *ptr);
			}

			g_strfreev(files);
		}
	}
}

/*
 * Local Variables:
 * mode: C
 * c-basic-offset: 8
 * indent-tabs-mode: t
 * End:
 */
