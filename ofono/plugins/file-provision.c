/*
 *
 *  oFono - Open Source Telephony
 *
 *  Copyright (C) 2017  Kerlink SA.
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License version 2 as
 *  published by the Free Software Foundation.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 */

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <errno.h>
#include <stdio.h>
#include <string.h>

#include <glib.h>

#define OFONO_API_SUBJECT_TO_CHANGE
#include <ofono/modem.h>
#include <ofono/gprs-provision.h>
#include <ofono/log.h>
#include <ofono/plugin.h>

/* STORAGEDIR may need to be redefined in unit tests */
#ifndef STORAGEDIR
#  define STORAGEDIR DEFAULT_STORAGEDIR
#endif

#define CONFIG_FILE STORAGEDIR "/provisioning"

static int config_file_provision_get_settings(const char *mcc,
				const char *mnc, const char *spn,
				struct ofono_gprs_provision_data **settings,
				int *count)
{
	int result = 0;
	GKeyFile *key_file = NULL;
	char *setting_group = NULL;
	char *value;

	DBG("Finding settings for MCC %s, MNC %s, SPN '%s'", mcc, mnc, spn);

	*count = 0;
	*settings = NULL;

	key_file = g_key_file_new();

	if (!g_key_file_load_from_file(key_file, CONFIG_FILE, 0, NULL)) {
		result = -ENOENT;
		goto error;
	}

	setting_group = g_try_malloc(strlen("operator:") + strlen(mcc) +
							strlen(mnc) + 2);
	if (setting_group == NULL) {
		result = -ENOMEM;
		goto error;
	}

	sprintf(setting_group, "operator:%s,%s", mcc, mnc);

	value = g_key_file_get_string(key_file, setting_group,
					"internet.AccessPointName", NULL);

	if (value == NULL)
		goto error;

	*settings = g_try_new0(struct ofono_gprs_provision_data, 1);
	if (*settings == NULL) {
		result = -ENOMEM;
		goto error;
	}

	*count = 1;

	(*settings)[0].type = OFONO_GPRS_CONTEXT_TYPE_INTERNET;
	(*settings)[0].apn = value;

	value = g_key_file_get_string(key_file, setting_group,
					"internet.Username", NULL);

	if (value != NULL)
		(*settings)[0].username = value;

	value = g_key_file_get_string(key_file, setting_group,
					"internet.Password", NULL);

	if (value != NULL)
		(*settings)[0].password = value;

	(*settings)[0].auth_method = OFONO_GPRS_AUTH_METHOD_CHAP;
	value = g_key_file_get_string(key_file, setting_group,
					"internet.AuthenticationMethod", NULL);

	if (value != NULL) {
		if (g_strcmp0(value, "chap") == 0)
			(*settings)[0].auth_method =
						OFONO_GPRS_AUTH_METHOD_CHAP;
		else if (g_strcmp0(value, "pap") == 0)
			(*settings)[0].auth_method =
						OFONO_GPRS_AUTH_METHOD_PAP;
		else
			DBG("Unknown auth method: %s", value);

		g_free(value);
	}

	(*settings)[0].proto = OFONO_GPRS_PROTO_IP;
	value = g_key_file_get_string(key_file, setting_group,
					"internet.Protocol", NULL);

	if (value != NULL) {
		DBG("CRO value:%s", value);
		if (g_strcmp0(value, "ip") == 0) {
			DBG("CRO value=ip");
			(*settings)[0].proto = OFONO_GPRS_PROTO_IP;
		} else if (g_strcmp0(value, "ipv6") == 0) {
			DBG("CRO value=ipv6");
			(*settings)[0].proto = OFONO_GPRS_PROTO_IPV6;
		} else if (g_strcmp0(value, "dual") == 0)
			(*settings)[0].proto = OFONO_GPRS_PROTO_IPV4V6;
		else
			DBG("Unknown protocol: %s", value);

		g_free(value);
	}

error:
	if (key_file != NULL)
		g_key_file_free(key_file);

	if (setting_group != NULL)
		g_free(setting_group);

	if (result == 0 && *count > 0)
		DBG("Found. APN:%s, proto:%d, auth_method:%d",
				(*settings)[0].apn, (*settings)[0].proto,
				(*settings)[0].auth_method);
	else
		DBG("Not found. Result:%d", result);

	return result;
}

static struct ofono_gprs_provision_driver config_file_provision_driver = {
	.name		= "GPRS context provisioning",
	.get_settings	= config_file_provision_get_settings,
};

static int config_file_provision_init(void)
{
	return ofono_gprs_provision_driver_register(
						&config_file_provision_driver);
}

static void config_file_provision_exit(void)
{
	ofono_gprs_provision_driver_unregister(
						&config_file_provision_driver);
}

OFONO_PLUGIN_DEFINE(file_provision, "Gprs Provisioning Plugin",
			VERSION, OFONO_PLUGIN_PRIORITY_HIGH,
			config_file_provision_init,
			config_file_provision_exit)
