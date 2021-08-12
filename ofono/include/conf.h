/*
 *  oFono - Open Source Telephony
 *
 *  Copyright (C) 2021 Jolla Ltd.
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

#ifndef OFONO_CONF_H
#define OFONO_CONF_H

/* This API exists since mer/1.24+git2 */

#ifdef __cplusplus
extern "C" {
#endif

#include <glib.h>

/* If a value isn't found in the specified group, it's looked up in this one */
#define OFONO_COMMON_SETTINGS_GROUP "Settings"

/* Utilities for parsing config files */
void ofono_conf_merge_files(GKeyFile *conf, const char *file);
char *ofono_conf_get_string(GKeyFile *conf, const char *group,
	const char *key) G_GNUC_WARN_UNUSED_RESULT;
char **ofono_conf_get_strings(GKeyFile *conf, const char *group,
	const char *key, char delimiter) G_GNUC_WARN_UNUSED_RESULT;
gboolean ofono_conf_get_integer(GKeyFile *conf, const char *group,
	const char *key, int *value);
gboolean ofono_conf_get_boolean(GKeyFile *conf, const char *group,
	const char *key, gboolean *value);
gboolean ofono_conf_get_flag(GKeyFile *conf, const char *group,
	const char *key, int flag, int *flags);
gboolean ofono_conf_get_enum(GKeyFile *conf, const char *group,
	const char *key, int *result, const char *name, int value, ...)
	G_GNUC_NULL_TERMINATED;
gboolean ofono_conf_get_mask(GKeyFile *conf, const char *group,
	const char *key, int *result, const char *name, int value, ...)
	G_GNUC_NULL_TERMINATED;

#ifdef __cplusplus
}
#endif

#endif /* OFONO_CONF_H */

/*
 * Local Variables:
 * mode: C
 * c-basic-offset: 8
 * indent-tabs-mode: t
 * End:
 */
