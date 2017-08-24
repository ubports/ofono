/*
 *  oFono - Open Source Telephony
 *
 *  Copyright (C) 2014-2017 Jolla. All rights reserved.
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

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <glib.h>
#include <gio/gio.h>

#define OFONO_API_SUBJECT_TO_CHANGE
#include "ofono.h"
#include "plugins/mbpi.h"
#include "plugins/provision.h"

#include <string.h>

#define TEST_SUITE "/provision/"

extern struct ofono_plugin_desc __ofono_builtin_provision;

struct provision_test_case {
	const char *name;
	const char *xml;
	const char *mcc;
	const char *mnc;
	const char *spn;
	const struct ofono_gprs_provision_data *settings;
	int count;
};

static GFile *test_write_tmp_file(const char* text, const char *suffix)
{
	char *tmpl = g_strconcat("provisionXXXXXX", suffix, NULL);
	GFileIOStream *io = NULL;
	GFile *file = g_file_new_tmp(tmpl, &io, NULL);
	GOutputStream *out = g_io_stream_get_output_stream(G_IO_STREAM(io));
	gsize len = strlen(text), nout;

	g_assert(file);
	g_assert(g_output_stream_write_all(out, text, len, &nout, NULL, NULL));
	g_output_stream_close(out, NULL, NULL);
	g_object_unref(io);
	g_free(tmpl);

	if (g_test_verbose()) {
		char *path = g_file_get_path(file);

		g_print("Created %s\n", path);
		g_free(path);
	}

	return file;
}

static void test_provision(gconstpointer test_data)
{
	const struct provision_test_case *test = test_data;
	struct ofono_gprs_provision_data *settings = NULL;
	int i, count = 0;
	GFile *file;
	char *path;

	if (test->xml) {
		file = test_write_tmp_file(test->xml, ".xml");
		path = g_file_get_path(file);
	} else {
		/*
		 * Create and delete a temporary file to end up
		 * with the path pointing to a non-existent file.
		 */
		GFileIOStream *io = NULL;
		file = g_file_new_tmp("provisionXXXXXX.xml", &io, NULL);
		path = g_file_get_path(file);
		g_file_delete(file, NULL, NULL);
		g_object_unref(io);
		g_object_unref(file);
		file = NULL;
	}

	mbpi_database = path;
	g_assert(__ofono_builtin_provision.init() == 0);

	if (test->settings) {
		g_assert(__ofono_gprs_provision_get_settings(test->mcc,
				test->mnc, test->spn, &settings, &count));
		g_assert(count == test->count);
		for (i = 0; i < count; i++) {
			const struct ofono_gprs_provision_data *actual =
				settings + i;
			const struct ofono_gprs_provision_data *expected =
				test->settings + i;

			g_assert(actual->type == expected->type);
			g_assert(actual->proto == expected->proto);
			g_assert(!g_strcmp0(actual->provider_name,
						expected->provider_name));
			g_assert(!g_strcmp0(actual->name, expected->name));
			g_assert(actual->provider_primary ==
						expected->provider_primary);
			g_assert(!g_strcmp0(actual->apn, expected->apn));
			g_assert(!g_strcmp0(actual->username,
						expected->username));
			g_assert(!g_strcmp0(actual->password,
						expected->password));
			g_assert(actual->auth_method == expected->auth_method);
			g_assert(!g_strcmp0(actual->message_proxy,
						expected->message_proxy));
			g_assert(!g_strcmp0(actual->message_center,
						expected->message_center));
		}
	} else {
		g_assert(!__ofono_gprs_provision_get_settings(test->mcc,
				test->mnc, test->spn, &settings, &count));
	}

	__ofono_gprs_provision_free_settings(settings, count);
	__ofono_builtin_provision.exit();
	if (file) {
		g_file_delete(file, NULL, NULL);
		g_object_unref(file);
	}
	g_free(path);
}

static void test_no_driver()
{
	struct ofono_gprs_provision_data *settings = NULL;
	int count = 0;

	g_assert(!__ofono_gprs_provision_get_settings("000", "01", NULL,
							&settings, &count));
        g_assert(!settings);
        g_assert(!count);
}

static void test_bad_driver()
{
	static const struct ofono_gprs_provision_driver bad_driver1 = {
		.name = "Bad driver 1",
	};

	static const struct ofono_gprs_provision_driver bad_driver2 = {
		.name = "Bad driver 2",
	};

	struct ofono_gprs_provision_data *settings = NULL;
	int count = 0;

	g_assert(!ofono_gprs_provision_driver_register(&bad_driver1));
	g_assert(!ofono_gprs_provision_driver_register(&bad_driver2));

	g_assert(!__ofono_gprs_provision_get_settings("000", "01", NULL,
							&settings, &count));
        g_assert(!settings);
        g_assert(!count);

	ofono_gprs_provision_driver_unregister(&bad_driver1);
	ofono_gprs_provision_driver_unregister(&bad_driver2);
}

static void test_no_mcc_mnc()
{
	struct ofono_gprs_provision_data *settings = NULL;
	int count = 0;

	g_assert(__ofono_builtin_provision.init() == 0);
	g_assert(!__ofono_gprs_provision_get_settings(NULL, NULL, NULL,
							&settings, &count));
	g_assert(!__ofono_gprs_provision_get_settings("", NULL, NULL,
							&settings, &count));
	g_assert(!__ofono_gprs_provision_get_settings("123", NULL, NULL,
							&settings, &count));
	g_assert(!__ofono_gprs_provision_get_settings("123", "", NULL,
							&settings, &count));
	__ofono_builtin_provision.exit();
}

static char telia_fi_provider_name [] = "Telia FI";
static char telia_fi_name_internet [] = "Telia Internet";
static char telia_fi_name_mms [] = "Telia MMS";
static char telia_fi_apn_internet [] = "internet";
static char telia_fi_apn_mms [] = "mms";
static char telia_fi_message_proxy [] = "195.156.25.33:8080";
static char telia_fi_message_center [] = "http://mms/";

/* Default Internet settings */
#define DEFAILT_INTERNET_SETTINGS \
	.type = OFONO_GPRS_CONTEXT_TYPE_INTERNET, \
	.proto = OFONO_GPRS_PROTO_IPV4V6, \
	.name = "Internet", \
	.apn = "internet", \
	.auth_method = OFONO_GPRS_AUTH_METHOD_NONE

/* Default MMS settings */
#define DEFAULT_MMS_SETTINGS \
	.type = OFONO_GPRS_CONTEXT_TYPE_MMS, \
	.proto = OFONO_GPRS_PROTO_IP, \
	.name = "MMS", \
	.apn = "mms", \
	.auth_method = OFONO_GPRS_AUTH_METHOD_NONE

static const struct ofono_gprs_provision_data telia_fi_internet_mms_p[] = {
	{
		.type = OFONO_GPRS_CONTEXT_TYPE_INTERNET,
		.proto = OFONO_GPRS_PROTO_IPV4V6,
		.provider_name = telia_fi_provider_name,
		.provider_primary = TRUE,
		.name = telia_fi_name_internet,
		.apn = telia_fi_apn_internet,
		.auth_method = OFONO_GPRS_AUTH_METHOD_NONE
	}, {
		.type = OFONO_GPRS_CONTEXT_TYPE_MMS,
		.proto = OFONO_GPRS_PROTO_IP,
		.provider_name = telia_fi_provider_name,
		.provider_primary = TRUE,
		.name = telia_fi_name_mms,
		.apn = telia_fi_apn_mms,
		.auth_method = OFONO_GPRS_AUTH_METHOD_NONE,
		.message_proxy = telia_fi_message_proxy,
		.message_center = telia_fi_message_center
	}
};

static const struct ofono_gprs_provision_data telia_fi_internet_mms[] = {
	{
		.type = OFONO_GPRS_CONTEXT_TYPE_INTERNET,
		.proto = OFONO_GPRS_PROTO_IPV4V6,
		.provider_name = telia_fi_provider_name,
		.name = telia_fi_name_internet,
		.apn = telia_fi_apn_internet,
		.auth_method = OFONO_GPRS_AUTH_METHOD_NONE
	}, {
		.type = OFONO_GPRS_CONTEXT_TYPE_MMS,
		.proto = OFONO_GPRS_PROTO_IP,
		.provider_name = telia_fi_provider_name,
		.name = telia_fi_name_mms,
		.apn = telia_fi_apn_mms,
		.auth_method = OFONO_GPRS_AUTH_METHOD_NONE,
		.message_proxy = telia_fi_message_proxy,
		.message_center = telia_fi_message_center
	}
};

static const struct ofono_gprs_provision_data telia_fi_internet[] = {
	{
		.type = OFONO_GPRS_CONTEXT_TYPE_INTERNET,
		.proto = OFONO_GPRS_PROTO_IPV4V6,
		.provider_name = telia_fi_provider_name,
		.name = telia_fi_name_internet,
		.apn = telia_fi_apn_internet,
		.auth_method = OFONO_GPRS_AUTH_METHOD_NONE
	},
	{ DEFAULT_MMS_SETTINGS }
};

static const struct ofono_gprs_provision_data telia_fi_mms[] = {
	{ DEFAILT_INTERNET_SETTINGS },
	{
		.type = OFONO_GPRS_CONTEXT_TYPE_MMS,
		.proto = OFONO_GPRS_PROTO_IP,
		.provider_name = telia_fi_provider_name,
		.name = telia_fi_name_mms,
		.apn = telia_fi_apn_mms,
		.auth_method = OFONO_GPRS_AUTH_METHOD_NONE,
		.message_proxy = telia_fi_message_proxy,
		.message_center = telia_fi_message_center
	}
};

static const struct ofono_gprs_provision_data default_settings[] = {
	{ DEFAILT_INTERNET_SETTINGS },
	{ DEFAULT_MMS_SETTINGS }
};

static const struct ofono_gprs_provision_data no_auth_settings[] = {
	{
		.type = OFONO_GPRS_CONTEXT_TYPE_INTERNET,
		.proto = OFONO_GPRS_PROTO_IPV4V6,
		.name = "Internet",
		.apn = "internet",
		.username = "",
		.password = "",
		.auth_method = OFONO_GPRS_AUTH_METHOD_NONE
	}, {
		.type = OFONO_GPRS_CONTEXT_TYPE_MMS,
		.proto = OFONO_GPRS_PROTO_IP,
		.name = "MMS",
		.apn = "mms",
		.auth_method = OFONO_GPRS_AUTH_METHOD_NONE
	}
};

static const struct ofono_gprs_provision_data auth_settings[] = {
	{
		.type = OFONO_GPRS_CONTEXT_TYPE_INTERNET,
		.proto = OFONO_GPRS_PROTO_IPV4V6,
		.name = "Internet",
		.apn = "internet",
		.username = "username",
		.auth_method = OFONO_GPRS_AUTH_METHOD_ANY
	}, {
		.type = OFONO_GPRS_CONTEXT_TYPE_MMS,
		.proto = OFONO_GPRS_PROTO_IP,
		.name = "MMS",
		.apn = "mms",
		.password = "password",
		.auth_method = OFONO_GPRS_AUTH_METHOD_ANY
	}
};

static const struct ofono_gprs_provision_data settings_ip[] = {
	{
		.type = OFONO_GPRS_CONTEXT_TYPE_INTERNET,
		.proto = OFONO_GPRS_PROTO_IP,
		.name = "Internet",
		.apn = "internet",
		.auth_method = OFONO_GPRS_AUTH_METHOD_NONE
	},
	{ DEFAULT_MMS_SETTINGS }
};

static const struct ofono_gprs_provision_data settings_ipv6[] = {
	{
		.type = OFONO_GPRS_CONTEXT_TYPE_INTERNET,
		.proto = OFONO_GPRS_PROTO_IPV6,
		.name = "Internet",
		.apn = "internet",
		.auth_method = OFONO_GPRS_AUTH_METHOD_NONE
	}, {
		.type = OFONO_GPRS_CONTEXT_TYPE_MMS,
		.proto = OFONO_GPRS_PROTO_IPV6,
		.name = "MMS",
		.apn = "mms",
		.auth_method = OFONO_GPRS_AUTH_METHOD_NONE
	}
};

static const struct ofono_gprs_provision_data settings_ipv4v6[] = {
	{ DEFAILT_INTERNET_SETTINGS },
	{
		.type = OFONO_GPRS_CONTEXT_TYPE_MMS,
		.proto = OFONO_GPRS_PROTO_IPV4V6,
		.name = "MMS",
		.apn = "mms",
		.auth_method = OFONO_GPRS_AUTH_METHOD_NONE
	}
};

static char test_provider_name[] = "Test provider";
static char test_message_proxy[] = "192.168.0.1:8888";
static char test_message_center[] = "http://mms/";
static const struct ofono_gprs_provision_data test_username_password[] = {
	{
		.type = OFONO_GPRS_CONTEXT_TYPE_INTERNET,
		.proto = OFONO_GPRS_PROTO_IPV4V6,
		.provider_name = test_provider_name,
		.name = "Test Internet",
		.apn = "test.internet.1",
		.username = "username",
		.auth_method = OFONO_GPRS_AUTH_METHOD_PAP
	}, {
		.type = OFONO_GPRS_CONTEXT_TYPE_MMS,
		.proto = OFONO_GPRS_PROTO_IP,
		.provider_name = test_provider_name,
		.name = "Test MMS",
		.apn = "test.mms",
		.username = "username",
		.password = "password",
		.auth_method = OFONO_GPRS_AUTH_METHOD_CHAP,
		.message_proxy = test_message_proxy,
		.message_center = test_message_center
	}
};

static const char telia_fi_internet_xml[] =
"<serviceproviders format=\"2.0\">\n\
<country code=\"fi\">\n\
  <provider>\n\
    <name>Telia FI</name>\n\
    <gsm>\n\
      <network-id mcc=\"244\" mnc=\"91\"/>\n\
      <apn value=\"internet\">\n\
        <usage type=\"internet\"/>\n\
        <name>Telia Internet</name>\n\
      </apn>\n\
    </gsm>\n\
  </provider>\n\
</country>\n\
</serviceproviders>\n";

static const struct provision_test_case test_cases[] = {
	{
		.name = TEST_SUITE "no_file",
		.mcc = "123",
		.mnc = "34",
		.settings = default_settings,
		.count = G_N_ELEMENTS(default_settings)
	},{
		.name = TEST_SUITE "primary_both",
		/* Both providers primaries, the first one is taken */
		.xml =
"<serviceproviders format=\"2.0\">\n\
<country code=\"fi\">\n\
  <provider primary=\"true\">\n\
    <name>Telia FI</name>\n\
    <gsm>\n\
      <network-id mcc=\"244\" mnc=\"91\"/>\n\
      <apn value=\"internet\">\n\
        <usage type=\"internet\"/>\n\
        <name>Telia Internet</name>\n\
      </apn>\n\
      <apn value=\"mms\">\n\
        <usage type=\"mms\"/>\n\
        <name>Telia MMS</name>\n\
        <mmsc>http://mms/</mmsc>\n\
        <mmsproxy>195.156.25.33:8080</mmsproxy>\n\
      </apn>\n\
    </gsm>\n\
  </provider>\n\
  <provider primary=\"true\">\n\
    <name>Other provider</name>\n\
    <gsm>\n\
      <network-id mcc=\"244\" mnc=\"91\"/>\n\
      <apn value=\"other.internet\">\n\
        <usage type=\"internet\"/>\n\
        <name>Other Internet</name>\n\
      </apn>\n\
      <apn value=\"other.mms\">\n\
        <usage type=\"mms\"/>\n\
        <name>Other MMS</name>\n\
        <mmsc>http://mms</mmsc>\n\
        <mmsproxy>192.168.0.1</mmsproxy>\n\
      </apn>\n\
    </gsm>\n\
  </provider>\n\
</country>\n\
</serviceproviders>\n",
		.mcc = "244",
		.mnc = "91",
		.spn = "Doesn't match",
		.settings = telia_fi_internet_mms_p,
		.count = G_N_ELEMENTS(telia_fi_internet_mms_p)
	},{
		.name = TEST_SUITE "primary_match1",
		/* The first provider is primary, the second one is not: */
		.xml =
"<serviceproviders format=\"2.0\">\n\
<country code=\"fi\">\n\
  <provider primary=\"true\">\n\
    <name>Telia FI</name>\n\
    <gsm>\n\
      <network-id mcc=\"244\" mnc=\"91\"/>\n\
      <apn value=\"internet\">\n\
        <usage type=\"internet\"/>\n\
        <name>Telia Internet</name>\n\
      </apn>\n\
      <apn value=\"mms\">\n\
        <usage type=\"mms\"/>\n\
        <name>Telia MMS</name>\n\
        <mmsc>http://mms/</mmsc>\n\
        <mmsproxy>195.156.25.33:8080</mmsproxy>\n\
      </apn>\n\
    </gsm>\n\
  </provider>\n\
  <provider>\n\
    <name>Other provider</name>\n\
    <gsm>\n\
      <network-id mcc=\"244\" mnc=\"91\"/>\n\
      <apn value=\"other.internet\">\n\
        <usage type=\"internet\"/>\n\
        <name>Other Internet</name>\n\
      </apn>\n\
      <apn value=\"other.mms\">\n\
        <usage type=\"mms\"/>\n\
        <name>Other MMS</name>\n\
        <mmsc>http://mms</mmsc>\n\
        <mmsproxy>192.168.0.1</mmsproxy>\n\
      </apn>\n\
    </gsm>\n\
  </provider>\n\
</country>\n\
</serviceproviders>\n",
		.mcc = "244",
		.mnc = "91",
		.settings = telia_fi_internet_mms_p,
		.count = G_N_ELEMENTS(telia_fi_internet_mms_p)
	},{
		.name = TEST_SUITE "primary_match2",
		/* The second provider is primary, the first one is not */
		.xml =
"<serviceproviders format=\"2.0\">\n\
<country code=\"fi\">\n\
  <provider>\n\
    <name>Other provider</name>\n\
    <gsm>\n\
      <network-id mcc=\"244\" mnc=\"91\"/>\n\
      <apn value=\"other.internet\">\n\
        <usage type=\"internet\"/>\n\
        <name>Other Internet</name>\n\
      </apn>\n\
      <apn value=\"other.mms\">\n\
        <usage type=\"mms\"/>\n\
        <name>Other MMS</name>\n\
        <mmsc>http://mms</mmsc>\n\
        <mmsproxy>192.168.0.1</mmsproxy>\n\
      </apn>\n\
    </gsm>\n\
  </provider>\n\
  <provider primary=\"true\">\n\
    <name>Telia FI</name>\n\
    <gsm>\n\
      <network-id mcc=\"244\" mnc=\"91\"/>\n\
      <apn value=\"internet\">\n\
        <usage type=\"internet\"/>\n\
        <name>Telia Internet</name>\n\
      </apn>\n\
      <apn value=\"mms\">\n\
        <usage type=\"mms\"/>\n\
        <name>Telia MMS</name>\n\
        <mmsc>http://mms/</mmsc>\n\
        <mmsproxy>195.156.25.33:8080</mmsproxy>\n\
      </apn>\n\
    </gsm>\n\
  </provider>\n\
</country>\n\
</serviceproviders>\n",
		.mcc = "244",
		.mnc = "91",
		.settings = telia_fi_internet_mms_p,
		.count = G_N_ELEMENTS(telia_fi_internet_mms_p)
	},{
		.name = TEST_SUITE "spn_match1",
		/* The first provider matches, the second one doesn't */
		.xml =
"<serviceproviders format=\"2.0\">\n\
<country code=\"fi\">\n\
  <provider>\n\
    <name>Telia FI</name>\n\
    <gsm>\n\
      <network-id mcc=\"244\" mnc=\"91\"/>\n\
      <apn value=\"internet\">\n\
        <usage type=\"internet\"/>\n\
        <name>Telia Internet</name>\n\
      </apn>\n\
      <apn value=\"mms\">\n\
        <usage type=\"mms\"/>\n\
        <name>Telia MMS</name>\n\
        <mmsc>http://mms/</mmsc>\n\
        <mmsproxy>195.156.25.33:8080</mmsproxy>\n\
      </apn>\n\
    </gsm>\n\
  </provider>\n\
  <provider>\n\
    <name>Other provider</name>\n\
    <gsm>\n\
      <network-id mcc=\"244\" mnc=\"91\"/>\n\
      <apn value=\"other.internet\">\n\
        <usage type=\"internet\"/>\n\
        <name>Other Internet</name>\n\
      </apn>\n\
      <apn value=\"other.mms\">\n\
        <usage type=\"mms\"/>\n\
        <name>Other MMS</name>\n\
        <mmsc>http://mms</mmsc>\n\
        <mmsproxy>192.168.0.1</mmsproxy>\n\
      </apn>\n\
    </gsm>\n\
  </provider>\n\
</country>\n\
</serviceproviders>\n",
		.mcc = "244",
		.mnc = "91",
		.spn = "Telia FI",
		.settings = telia_fi_internet_mms,
		.count = G_N_ELEMENTS(telia_fi_internet_mms)
	},{
		.name = TEST_SUITE "spn_match2",
		/* The first provider doesn't match, the second one does */
		.xml =
"<serviceproviders format=\"2.0\">\n\
<country code=\"fi\">\n\
  <provider>\n\
    <name>Other provider</name>\n\
    <gsm>\n\
      <network-id mcc=\"244\" mnc=\"91\"/>\n\
      <apn value=\"other.internet\">\n\
        <usage type=\"internet\"/>\n\
        <name>Other Internet</name>\n\
      </apn>\n\
      <apn value=\"other.mms\">\n\
        <usage type=\"mms\"/>\n\
        <name>Other MMS</name>\n\
        <mmsc>http://mms</mmsc>\n\
        <mmsproxy>192.168.0.1</mmsproxy>\n\
      </apn>\n\
    </gsm>\n\
  </provider>\n\
  <provider>\n\
    <name>Telia FI</name>\n\
    <gsm>\n\
      <network-id mcc=\"244\" mnc=\"91\"/>\n\
      <apn value=\"internet\">\n\
        <usage type=\"internet\"/>\n\
        <name>Telia Internet</name>\n\
      </apn>\n\
      <apn value=\"mms\">\n\
        <usage type=\"mms\"/>\n\
        <name>Telia MMS</name>\n\
        <mmsc>http://mms/</mmsc>\n\
        <mmsproxy>195.156.25.33:8080</mmsproxy>\n\
      </apn>\n\
    </gsm>\n\
  </provider>\n\
</country>\n\
</serviceproviders>\n",
		.mcc = "244",
		.mnc = "91",
		.spn = "Telia FI",
		.settings = telia_fi_internet_mms,
		.count = G_N_ELEMENTS(telia_fi_internet_mms)
	},{
		.name = TEST_SUITE "spn_match_case",
		/* Case insensitive match */
		.xml =
"<serviceproviders format=\"2.0\">\n\
<country code=\"fi\">\n\
  <provider>\n\
    <name>Telia FI</name>\n\
    <gsm>\n\
      <network-id mcc=\"244\" mnc=\"91\"/>\n\
      <apn value=\"internet\">\n\
        <usage type=\"internet\"/>\n\
        <name>Telia Internet</name>\n\
      </apn>\n\
      <apn value=\"mms\">\n\
        <usage type=\"mms\"/>\n\
        <name>Telia MMS</name>\n\
        <mmsc>http://mms/</mmsc>\n\
        <mmsproxy>195.156.25.33:8080</mmsproxy>\n\
      </apn>\n\
    </gsm>\n\
  </provider>\n\
  <provider>\n\
    <name>Other provider</name>\n\
    <gsm>\n\
      <network-id mcc=\"244\" mnc=\"91\"/>\n\
      <apn value=\"other.internet\">\n\
        <usage type=\"internet\"/>\n\
        <name>Other Internet</name>\n\
      </apn>\n\
      <apn value=\"other.mms\">\n\
        <usage type=\"mms\"/>\n\
        <name>Other MMS</name>\n\
        <mmsc>http://mms</mmsc>\n\
        <mmsproxy>192.168.0.1</mmsproxy>\n\
      </apn>\n\
    </gsm>\n\
  </provider>\n\
</country>\n\
</serviceproviders>\n",
		.mcc = "244",
		.mnc = "91",
		.spn = "telia fi",
		.settings = telia_fi_internet_mms,
		.count = G_N_ELEMENTS(telia_fi_internet_mms)
	},{
		.name = TEST_SUITE "spn_partial_unnamed",
		/* The second provider matches partially, first has no name */
		.xml =
"<serviceproviders format=\"2.0\">\n\
<country code=\"fi\">\n\
  <provider>\n\
    <gsm>\n\
      <network-id mcc=\"244\" mnc=\"91\"/>\n\
      <apn value=\"other.internet\">\n\
        <usage type=\"internet\"/>\n\
        <name>Other Internet</name>\n\
      </apn>\n\
      <apn value=\"other.mms\">\n\
        <usage type=\"mms\"/>\n\
        <name>Other MMS</name>\n\
        <mmsc>http://mms</mmsc>\n\
        <mmsproxy>192.168.0.1</mmsproxy>\n\
      </apn>\n\
    </gsm>\n\
  </provider>\n\
  <provider>\n\
    <name>Telia FI</name>\n\
    <gsm>\n\
      <network-id mcc=\"244\" mnc=\"91\"/>\n\
      <apn value=\"internet\">\n\
        <usage type=\"internet\"/>\n\
        <name>Telia Internet</name>\n\
      </apn>\n\
      <apn value=\"mms\">\n\
        <usage type=\"mms\"/>\n\
        <name>Telia MMS</name>\n\
        <mmsc>http://mms/</mmsc>\n\
        <mmsproxy>195.156.25.33:8080</mmsproxy>\n\
      </apn>\n\
    </gsm>\n\
  </provider>\n\
</country>\n\
</serviceproviders>\n",
		.mcc = "244",
		.mnc = "91",
		.spn = "Telia",
		.settings = telia_fi_internet_mms,
		.count = G_N_ELEMENTS(telia_fi_internet_mms)
	},{
		.name = TEST_SUITE "internet_mms_primary",
		.xml =
"<serviceproviders format=\"2.0\">\n\
<country code=\"fi\">\n\
  <provider primary=\"true\">\n\
    <name>Telia FI</name>\n\
    <gsm>\n\
      <network-id mcc=\"244\" mnc=\"91\"/>\n\
      <apn value=\"internet\">\n\
        <usage type=\"internet\"/>\n\
        <name>Telia Internet</name>\n\
      </apn>\n\
      <apn value=\"mms\">\n\
        <usage type=\"mms\"/>\n\
        <name>Telia MMS</name>\n\
        <mmsc>http://mms/</mmsc>\n\
        <mmsproxy>195.156.25.33:8080</mmsproxy>\n\
      </apn>\n\
    </gsm>\n\
  </provider>\n\
</country>\n\
</serviceproviders>\n",
		.mcc = "244",
		.mnc = "91",
		.spn = "Telia FI",
		.settings = telia_fi_internet_mms_p,
		.count = G_N_ELEMENTS(telia_fi_internet_mms_p)
	},{
		.name = TEST_SUITE "internet_mms",
		.xml =
"<serviceproviders format=\"2.0\">\n\
<country code=\"fi\">\n\
  <provider>\n\
    <name>Telia FI</name>\n\
    <gsm>\n\
      <network-id mcc=\"244\" mnc=\"91\"/>\n\
      <apn value=\"internet\">\n\
        <usage type=\"internet\"/>\n\
        <name>Telia Internet</name>\n\
      </apn>\n\
      <apn value=\"mms\">\n\
        <usage type=\"mms\"/>\n\
        <name>Telia MMS</name>\n\
        <mmsc>http://mms/</mmsc>\n\
        <mmsproxy>195.156.25.33:8080</mmsproxy>\n\
      </apn>\n\
    </gsm>\n\
  </provider>\n\
</country>\n\
</serviceproviders>\n",
		.mcc = "244",
		.mnc = "91",
		.spn = "Telia FI",
		.settings = telia_fi_internet_mms,
		.count = G_N_ELEMENTS(telia_fi_internet_mms)
	},{
		.name = TEST_SUITE "internet",
		.xml = telia_fi_internet_xml,
		.mcc = "244",
		.mnc = "91",
		.settings = telia_fi_internet,
		.count = G_N_ELEMENTS(telia_fi_internet)
	},{
		.name = TEST_SUITE "mms",
		.xml =
"<serviceproviders format=\"2.0\">\n\
<country code=\"fi\">\n\
  <provider>\n\
    <name>Telia FI</name>\n\
    <gsm>\n\
      <network-id mcc=\"244\" mnc=\"91\"/>\n\
      <apn value=\"mms\">\n\
        <usage type=\"mms\"/>\n\
        <name>Telia MMS</name>\n\
        <mmsc>http://mms/</mmsc>\n\
        <mmsproxy>195.156.25.33:8080</mmsproxy>\n\
      </apn>\n\
    </gsm>\n\
  </provider>\n\
</country>\n\
</serviceproviders>\n",
		.mcc = "244",
		.mnc = "91",
		.settings = telia_fi_mms,
		.count = G_N_ELEMENTS(telia_fi_mms)
	},{
		.name = TEST_SUITE "not_found_mcc",
		.xml = telia_fi_internet_xml,
		.mcc = "245", /* Wrong MCC */
		.mnc = "91",
		.settings = default_settings,
		.count = G_N_ELEMENTS(default_settings)
	},{
		.name = TEST_SUITE "not_found_mnc",
		.xml = telia_fi_internet_xml,
		.mcc = "244",
		.mnc = "90", /* Wrong MNC */
		.settings = default_settings,
		.count = G_N_ELEMENTS(default_settings)
	},{
		.name = TEST_SUITE "apn_error",
		.xml = "<serviceproviders format=\"2.0\">\n\
<country code=\"fi\">\n\
  <provider>\n\
    <name>Telia FI</name>\n\
    <gsm>\n\
      <network-id mcc=\"244\" mnc=\"91\"/>\n\
      <apn value=\"mms\">\n\
    </gsm>\n\
  </provider>\n\
</country>\n\
</serviceproviders>\n",
		.mcc = "244",
		.mnc = "91",
		.settings = default_settings,
		.count = G_N_ELEMENTS(default_settings)
	},{
		.name = TEST_SUITE "username_password",
		.xml =
"<serviceproviders format=\"2.0\">\n\
<country code=\"xx\">\n\
  <provider>\n\
    <name>Test provider</name>\n\
    <gsm>\n\
      <network-id mcc=\"123\" mnc=\"45\"/>\n\
      <apn value=\"test.internet.1\">\n\
        <usage type=\"internet\"/>\n\
        <name>Test Internet</name>\n\
        <authentication method=\"pap\"/>\n\
        <username>username</username>\n\
      </apn>\n\
      <apn value=\"test.internet.2\">\n\
        <usage type=\"internet\"/>\n\
        <name>Test Internet</name>\n\
        <authentication method=\"any\"/>\n\
        <password>password</password>\n\
        <garbage/>\n\
      </apn>\n\
      <apn value=\"test.mms\">\n\
        <usage type=\"mms\"/>\n\
        <name>Test MMS</name>\n\
        <authentication method=\"chap\"/>\n\
        <username>username</username>\n\
        <password>password</password>\n\
        <mmsc>http://mms/</mmsc>\n\
        <mmsproxy>192.168.0.1:8888</mmsproxy>\n\
      </apn>\n\
    </gsm>\n\
  </provider>\n\
</country>\n\
</serviceproviders>\n",
		.mcc = "123",
		.mnc = "45",
		.spn = test_provider_name,
		.settings = test_username_password,
		.count = G_N_ELEMENTS(test_username_password)
	},{
		.name = TEST_SUITE "no_auth",
		.xml =
"<serviceproviders format=\"2.0\">\n\
<country code=\"xx\">\n\
  <provider>\n\
    <gsm>\n\
      <network-id mcc=\"123\" mnc=\"45\"/>\n\
      <apn value=\"internet\">\n\
        <usage type=\"internet\"/>\n\
        <name>Internet</name>\n\
        <username></username>\n\
        <password></password>\n\
      </apn>\n\
      <apn value=\"mms\">\n\
        <usage type=\"mms\"/>\n\
        <name>MMS</name>\n\
        <authentication method=\"none\"/>\n\
      </apn>\n\
    </gsm>\n\
  </provider>\n\
</country>\n\
</serviceproviders>\n",
		.mcc = "123",
		.mnc = "45",
		.settings = no_auth_settings,
		.count = G_N_ELEMENTS(no_auth_settings)
	},{
		.name = TEST_SUITE "auth",
		.xml = /* With username and password auth defaults to ANY */
"<serviceproviders format=\"2.0\">\n\
<country code=\"xx\">\n\
  <provider>\n\
    <gsm>\n\
      <network-id mcc=\"123\" mnc=\"45\"/>\n\
      <apn value=\"internet\">\n\
        <usage type=\"internet\"/>\n\
        <name>Internet</name>\n\
        <username>username</username>\n\
      </apn>\n\
      <apn value=\"mms\">\n\
        <usage type=\"mms\"/>\n\
        <name>MMS</name>\n\
        <password>password</password>\n\
      </apn>\n\
    </gsm>\n\
  </provider>\n\
</country>\n\
</serviceproviders>\n",
		.mcc = "123",
		.mnc = "45",
		.settings = auth_settings,
		.count = G_N_ELEMENTS(auth_settings)
	},{
		.name = TEST_SUITE "protocol_data_ip",
		.xml =
"<serviceproviders format=\"2.0\">\n\
<country code=\"xx\">\n\
  <provider>\n\
    <gsm>\n\
      <network-id mcc=\"123\" mnc=\"45\"/>\n\
      <apn value=\"internet\">\n\
        <usage type=\"internet\"/>\n\
        <name>Internet</name>\n\
        <protocol type=\"ip\"/>\n\
      </apn>\n\
    </gsm>\n\
  </provider>\n\
</country>\n\
</serviceproviders>\n",
		.mcc = "123",
		.mnc = "45",
		.settings = settings_ip,
		.count = G_N_ELEMENTS(settings_ip)
	},{
		.name = TEST_SUITE "protocol_ipv6",
		.xml =
"<serviceproviders format=\"2.0\">\n\
<country code=\"xx\">\n\
  <provider>\n\
    <gsm>\n\
      <network-id mcc=\"123\" mnc=\"45\"/>\n\
      <apn value=\"internet\">\n\
        <usage type=\"internet\"/>\n\
        <name>Internet</name>\n\
        <protocol type=\"ipv6\"/>\n\
      </apn>\n\
      <apn value=\"mms\">\n\
        <usage type=\"mms\"/>\n\
        <name>MMS</name>\n\
        <protocol type=\"ipv6\"/>\n\
      </apn>\n\
    </gsm>\n\
  </provider>\n\
</country>\n\
</serviceproviders>\n",
		.mcc = "123",
		.mnc = "45",
		.settings = settings_ipv6,
		.count = G_N_ELEMENTS(settings_ipv6)
	},{
		.name = TEST_SUITE "protocol_ipv4v6",
		.xml =
"<serviceproviders format=\"2.0\">\n\
<country code=\"xx\">\n\
  <provider>\n\
    <gsm>\n\
      <network-id mcc=\"123\" mnc=\"45\"/>\n\
      <apn value=\"internet\">\n\
        <usage type=\"internet\"/>\n\
        <name>Internet</name>\n\
        <protocol type=\"ipv4v6\"/>\n\
      </apn>\n\
      <apn value=\"mms\">\n\
        <usage type=\"mms\"/>\n\
        <name>MMS</name>\n\
        <protocol type=\"ipv4v6\"/>\n\
      </apn>\n\
    </gsm>\n\
  </provider>\n\
</country>\n\
</serviceproviders>\n",
		.mcc = "123",
		.mnc = "45",
		.settings = settings_ipv4v6,
		.count = G_N_ELEMENTS(settings_ipv4v6)
	},{
		.name = TEST_SUITE "invalid_protocol",
		.xml =
"<serviceproviders format=\"2.0\">\n\
<country code=\"xx\">\n\
  <provider>\n\
    <gsm>\n\
      <network-id mcc=\"123\" mnc=\"45\"/>\n\
      <apn value=\"internet\">\n\
        <usage type=\"internet\"/>\n\
        <name>Internet</name>\n\
        <protocol type=\"foo\"/>\n\
      </apn>\n\
    </gsm>\n\
  </provider>\n\
</country>\n\
</serviceproviders>\n",
		.mcc = "123",
		.mnc = "45",
		.settings = default_settings,
		.count = G_N_ELEMENTS(default_settings)
	},{
		.name = TEST_SUITE "missing_protocol_type",
		.xml =
"<serviceproviders format=\"2.0\">\n\
<country code=\"xx\">\n\
  <provider>\n\
    <gsm>\n\
      <network-id mcc=\"123\" mnc=\"45\"/>\n\
      <apn value=\"internet\">\n\
        <usage type=\"internet\"/>\n\
        <name>Internet</name>\n\
        <protocol foo=\"bar\"/>\n\
      </apn>\n\
    </gsm>\n\
  </provider>\n\
</country>\n\
</serviceproviders>\n",
		.mcc = "123",
		.mnc = "45",
		.settings = default_settings,
		.count = G_N_ELEMENTS(default_settings)
	},{
		.name = TEST_SUITE "duplicate_network",
		.xml =
"<serviceproviders format=\"2.0\">\n\
<country code=\"xx\">\n\
  <provider>\n\
    <name>Test provider</name>\n\
    <gsm>\n\
      <network-id mcc=\"123\" mnc=\"45\"/>\n\
      <apn value=\"test.internet.1\">\n\
        <usage type=\"internet\"/>\n\
        <name>Test Internet</name>\n\
        <authentication method=\"pap\"/>\n\
        <username>username</username>\n\
      </apn>\n\
      <apn value=\"test.mms\">\n\
        <usage type=\"mms\"/>\n\
        <name>Test MMS</name>\n\
        <authentication method=\"chap\"/>\n\
        <username>username</username>\n\
        <password>password</password>\n\
        <mmsc>http://mms/</mmsc>\n\
        <mmsproxy>192.168.0.1:8888</mmsproxy>\n\
      </apn>\n\
      <network-id mcc=\"123\" mnc=\"45\"/>\n\
      <apn value=\"test.internet.2\">\n\
        <usage type=\"internet\"/>\n\
        <name>Test Internet</name>\n\
        <authentication method=\"any\"/>\n\
        <password>password</password>\n\
        <garbage/>\n\
      </apn>\n\
      <apn value=\"test.wap\">\n\
        <usage type=\"wap\"/>\n\
      </apn>\n\
      <garbage/>\n\
    </gsm>\n\
  </provider>\n\
</country>\n\
</serviceproviders>\n",
		.mcc = "123",
		.mnc = "45",
		.spn = test_provider_name,
		.settings = test_username_password,
		.count = G_N_ELEMENTS(test_username_password)
	},{
		.name = TEST_SUITE "missing_mcc",
		.xml =
"<serviceproviders format=\"2.0\">\n\
<country code=\"xx\">\n\
  <provider>\n\
    <name>Test provider</name>\n\
    <gsm>\n\
      <network-id mnc=\"34\"/>\n\
      <apn value=\"test.internet\">\n\
        <usage type=\"internet\"/>\n\
        <name>Test Internet</name>\n\
      </apn>\n\
    </gsm>\n\
  </provider>\n\
</country>\n\
</serviceproviders>\n",
		.mcc = "123",
		.mnc = "34",
		.settings = default_settings,
		.count = G_N_ELEMENTS(default_settings)
	},{
		.name = TEST_SUITE "missing_mnc",
		.xml =
"<serviceproviders format=\"2.0\">\n\
<country code=\"xx\">\n\
  <provider>\n\
    <name>Test provider</name>\n\
    <gsm>\n\
      <network-id mcc=\"123\"/>\n\
      <apn value=\"test.internet\">\n\
        <usage type=\"internet\"/>\n\
        <name>Test Internet</name>\n\
      </apn>\n\
    </gsm>\n\
  </provider>\n\
</country>\n\
</serviceproviders>\n",
		.mcc = "123",
		.mnc = "34",
		.settings = default_settings,
		.count = G_N_ELEMENTS(default_settings)
	},{
		.name = TEST_SUITE "missing_auth_method",
		.xml =
"<serviceproviders format=\"2.0\">\n\
<country code=\"xx\">\n\
  <provider>\n\
    <name>Test provider</name>\n\
    <gsm>\n\
      <network-id mcc=\"123\" mnc=\"34\"/>\n\
      <apn value=\"test.internet\">\n\
        <usage type=\"internet\"/>\n\
        <name>Test Internet</name>\n\
        <authentication garbage=\"junk\"/>\n\
      </apn>\n\
    </gsm>\n\
  </provider>\n\
</country>\n\
</serviceproviders>\n",
		.mcc = "123",
		.mnc = "34",
		.settings = default_settings,
		.count = G_N_ELEMENTS(default_settings)
	},{
		.name = TEST_SUITE "invalid_auth_method",
		.xml =
"<serviceproviders format=\"2.0\">\n\
<country code=\"xx\">\n\
  <provider>\n\
    <name>Test provider</name>\n\
    <gsm>\n\
      <network-id mcc=\"123\" mnc=\"34\"/>\n\
      <apn value=\"test.internet\">\n\
        <usage type=\"internet\"/>\n\
        <name>Test Internet</name>\n\
        <authentication method=\"invalid\"/>\n\
      </apn>\n\
    </gsm>\n\
  </provider>\n\
</country>\n\
</serviceproviders>\n",
		.mcc = "123",
		.mnc = "34",
		.settings = default_settings,
		.count = G_N_ELEMENTS(default_settings)
	},{
		.name = TEST_SUITE "missing_usage_type",
		.xml =
"<serviceproviders format=\"2.0\">\n\
<country code=\"xx\">\n\
  <provider>\n\
    <name>Test provider</name>\n\
    <gsm>\n\
      <network-id mcc=\"123\" mnc=\"34\"/>\n\
      <apn value=\"test.internet\">\n\
        <usage garbage=\"junk\"/>\n\
        <name>Test Internet</name>\n\
      </apn>\n\
    </gsm>\n\
  </provider>\n\
</country>\n\
</serviceproviders>\n",
		.mcc = "123",
		.mnc = "34",
		.settings = default_settings,
		.count = G_N_ELEMENTS(default_settings)
	},{
		.name = TEST_SUITE "invalid_usage_type",
		.xml =
"<serviceproviders format=\"2.0\">\n\
<country code=\"xx\">\n\
  <provider>\n\
    <name>Test provider</name>\n\
    <gsm>\n\
      <network-id mcc=\"123\" mnc=\"34\"/>\n\
      <apn value=\"test.internet\">\n\
        <usage type=\"invalid\"/>\n\
        <name>Test Internet</name>\n\
      </apn>\n\
    </gsm>\n\
  </provider>\n\
</country>\n\
</serviceproviders>\n",
		.mcc = "123",
		.mnc = "34",
		.settings = default_settings,
		.count = G_N_ELEMENTS(default_settings)
	},{
		.name = TEST_SUITE "missing_apn_value",
		.xml =
"<serviceproviders format=\"2.0\">\n\
<country code=\"xx\">\n\
  <provider>\n\
    <name>Test provider</name>\n\
    <gsm>\n\
      <network-id mcc=\"123\" mnc=\"34\"/>\n\
      <apn garbage=\"junk\">\n\
        <usage type=\"internet\"/>\n\
        <name>Test Internet</name>\n\
      </apn>\n\
    </gsm>\n\
  </provider>\n\
</country>\n\
</serviceproviders>\n",
		.mcc = "123",
		.mnc = "34",
		.settings = default_settings,
		.count = G_N_ELEMENTS(default_settings)
	},{
		.name = TEST_SUITE "missing_gsm",
		.xml =
"<serviceproviders format=\"2.0\">\n\
<country code=\"xx\">\n\
  <provider>\n\
    <name>Test provider</name>\n\
    <whatever/>\n\
  </provider>\n\
</country>\n\
</serviceproviders>\n",
		.mcc = "123",
		.mnc = "34",
		.settings = default_settings,
		.count = G_N_ELEMENTS(default_settings)
	},{
		.name = TEST_SUITE "invalid_xml",
		.xml =
"<serviceproviders format=\"2.0\">\n\
<country code=\"xx\">\n\
  <provider>\n\
    <gsm>\n\
      <network-id mcc=\"123\" mnc=\"45\"/>\n\
      <apn value=\"internet\">\n\
        <usage type=\"internet\"/>\n\
        <name>Internet</name>\n\
        <authentication method=\"none\"/>\n\
      </apn>\n\
      <apn value=\"mms\">\n\
        <usage type=\"mms\"/>\n\
        <name>MMS</name>\n\
        <authentication method=\"none\"/>\n\
      </apn>\n\
    </gsm>\n\
  </provider>\n\
</country>\n\
</se",
		.mcc = "123",
		.mnc = "34",
		.settings = default_settings,
		.count = G_N_ELEMENTS(default_settings)
	}
};

int main(int argc, char **argv)
{
	guint i;

	g_test_init(&argc, &argv, NULL);
	g_test_add_func(TEST_SUITE "no_driver", test_no_driver);
	g_test_add_func(TEST_SUITE "bad_driver", test_bad_driver);
	g_test_add_func(TEST_SUITE "no_mcc_mnc", test_no_mcc_mnc);
	for (i = 0; i < G_N_ELEMENTS(test_cases); i++) {
		const struct provision_test_case *test = test_cases + i;
		g_test_add_data_func(test->name, test, test_provision);
	}
	return g_test_run();
}
