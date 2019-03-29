/*
 *  oFono - Open Source Telephony
 *
 *  Copyright (C) 2019 Jolla Ltd.
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

#include "ofono.h"

#include <dbusaccess_peer.h>
#include <dbusaccess_policy.h>
#include <dbusaccess_system.h>

#include <gutil_idlepool.h>
#include <gutil_log.h>

#include <errno.h>

static GUtilIdlePool* peer_pool;

extern struct ofono_plugin_desc __ofono_builtin_sailfish_access;
extern const char *sailfish_access_config_file;

#define TMP_DIR_TEMPLATE "test-sailfish_access-XXXXXX"

#define ROOT_SENDER ":1.100"
#define PRIVILEGED_SENDER ":1.200"
#define NON_PRIVILEGED_SENDER ":1.300"
#define INVALID_SENDER ":1.400"

#define NEMO_UID (100000)
#define NEMO_GID (100000)
#define PRIVILEGED_GID (996)
#define SAILFISH_RADIO_GID (997)

/*==========================================================================*
 * Stubs
 *==========================================================================*/

DAPeer *da_peer_get(DA_BUS bus, const char *name)
{
	if (name && g_strcmp0(name, INVALID_SENDER)) {
		gsize len = strlen(name);
		DAPeer *peer = g_malloc0(sizeof(DAPeer) + len + 1);
		char *buf = (char*)(peer + 1);
		strcpy(buf, name);
		peer->name = buf;
		gutil_idle_pool_add(peer_pool, peer, g_free);
		if (!strcmp(name, PRIVILEGED_SENDER)) {
			peer->cred.euid = NEMO_UID;
			peer->cred.egid = PRIVILEGED_GID;
		} else if (strcmp(name, ROOT_SENDER)) {
			peer->cred.euid = NEMO_UID;
			peer->cred.egid = NEMO_GID;
		}
		return peer;
	} else {
		return NULL;
	}
}

void da_peer_flush(DA_BUS bus, const char *name)
{
	gutil_idle_pool_drain(peer_pool);
}

/*
 * The build environment doesn't necessarily have these users and groups.
 * And yet, sailfish access plugin depends on those.
 */

int da_system_uid(const char *user)
{
	if (!g_strcmp0(user, "nemo")) {
		return NEMO_UID;
	} else {
		return -1;
	}
}

int da_system_gid(const char *group)
{
	if (!g_strcmp0(group, "sailfish-radio")) {
		return SAILFISH_RADIO_GID;
	} else if (!g_strcmp0(group, "privileged")) {
		return PRIVILEGED_GID;
	} else {
		return -1;
	}
}

/*==========================================================================*
 * Tests
 *==========================================================================*/

static void test_register()
{
	g_assert(__ofono_builtin_sailfish_access.init() == 0);
	g_assert(__ofono_builtin_sailfish_access.init() == -EALREADY);
	__ofono_builtin_sailfish_access.exit();
	__ofono_builtin_sailfish_access.exit();
}

static void test_default()
{
	const char *default_config_file = sailfish_access_config_file;

	sailfish_access_config_file = "/no such file";
	g_assert(__ofono_builtin_sailfish_access.init() == 0);

	/* root and privileged are allowed to Dial by default */
	g_assert(__ofono_dbus_access_method_allowed(ROOT_SENDER,
				OFONO_DBUS_ACCESS_INTF_VOICECALLMGR,
				OFONO_DBUS_ACCESS_VOICECALLMGR_DIAL, NULL));
	g_assert(__ofono_dbus_access_method_allowed(PRIVILEGED_SENDER,
				OFONO_DBUS_ACCESS_INTF_VOICECALLMGR,
				OFONO_DBUS_ACCESS_VOICECALLMGR_DIAL, NULL));

	/* Non-privileged and unknown users are not */
	g_assert(!__ofono_dbus_access_method_allowed(NON_PRIVILEGED_SENDER,
				OFONO_DBUS_ACCESS_INTF_VOICECALLMGR,
				OFONO_DBUS_ACCESS_VOICECALLMGR_DIAL, NULL));
	g_assert(!__ofono_dbus_access_method_allowed(INVALID_SENDER,
				OFONO_DBUS_ACCESS_INTF_VOICECALLMGR,
				OFONO_DBUS_ACCESS_VOICECALLMGR_DIAL, NULL));

	/* Unknown interfaces/methods are allowed */
	g_assert(__ofono_dbus_access_method_allowed(NON_PRIVILEGED_SENDER,
				OFONO_DBUS_ACCESS_INTF_COUNT, 0, NULL));
	g_assert(__ofono_dbus_access_method_allowed(NON_PRIVILEGED_SENDER,
				OFONO_DBUS_ACCESS_INTF_MESSAGE, -1, NULL));
	g_assert(__ofono_dbus_access_method_allowed(NON_PRIVILEGED_SENDER,
				OFONO_DBUS_ACCESS_INTF_MESSAGE,
				OFONO_DBUS_ACCESS_MESSAGE_METHOD_COUNT, NULL));

	__ofono_builtin_sailfish_access.exit();

	/* Restore the defaults */
	sailfish_access_config_file = default_config_file;
}

struct test_config_data {
	gboolean allowed;
	const char *sender;
	enum ofono_dbus_access_intf intf;
	int method;
	const char *config;
};

static const struct test_config_data config_tests [] = {
	{
		TRUE, NON_PRIVILEGED_SENDER,
		OFONO_DBUS_ACCESS_INTF_VOICECALL,
		OFONO_DBUS_ACCESS_VOICECALL_HANGUP,
		"[org.ofono.VoiceCall]\n"
		"Hangup = " DA_POLICY_VERSION "; * = allow \n"
	},{
		FALSE, NON_PRIVILEGED_SENDER,
		OFONO_DBUS_ACCESS_INTF_VOICECALL,
		OFONO_DBUS_ACCESS_VOICECALL_HANGUP,
		"[org.ofono.VoiceCall]\n"
		"Hangup = " DA_POLICY_VERSION "; * = allow \n"
		"=========" /* Invalid key file */
	},{
		FALSE, NON_PRIVILEGED_SENDER,
		OFONO_DBUS_ACCESS_INTF_VOICECALLMGR,
		OFONO_DBUS_ACCESS_VOICECALLMGR_DIAL,
		"[Common]\n"
		"DefaultAccess = " DA_POLICY_VERSION "; * = allow \n"
		"[org.ofono.VoiceCallManager]\n"
		"Dial = " DA_POLICY_VERSION "; * = deny\n"
			"group(privileged) = allow\n"
	},{
		TRUE, NON_PRIVILEGED_SENDER,
		OFONO_DBUS_ACCESS_INTF_VOICECALLMGR,
		OFONO_DBUS_ACCESS_VOICECALLMGR_TRANSFER,
		"[Common]\n"
		"DefaultAccess = " DA_POLICY_VERSION "; * = allow \n"
		"[org.ofono.VoiceCallManager]\n"
		"Dial = " DA_POLICY_VERSION "; * = deny; "
			"group(privileged) = allow \n"
	},{
		TRUE, PRIVILEGED_SENDER,
		OFONO_DBUS_ACCESS_INTF_VOICECALLMGR,
		OFONO_DBUS_ACCESS_VOICECALLMGR_DIAL,
		"[Common]\n"
		"DefaultAccess = " DA_POLICY_VERSION "; * = allow \n"
		"[org.ofono.VoiceCallManager]\n"
		"Dial = " DA_POLICY_VERSION "; * = deny; "
			"group(privileged) = allow \n"
	},{
		TRUE, NON_PRIVILEGED_SENDER,
		OFONO_DBUS_ACCESS_INTF_VOICECALLMGR,
		OFONO_DBUS_ACCESS_VOICECALLMGR_DIAL,
		"[Common]\n"
		"DefaultAccess = " DA_POLICY_VERSION "; * = allow \n"
		"[org.ofono.VoiceCallManager]\n"
		"* = invalid"
	},{
		FALSE, NON_PRIVILEGED_SENDER,
		OFONO_DBUS_ACCESS_INTF_VOICECALLMGR,
		OFONO_DBUS_ACCESS_VOICECALLMGR_DIAL,
		"[Common]\n"
		"DefaultAccess = " DA_POLICY_VERSION "; * = allow \n"
		"[org.ofono.VoiceCallManager]\n"
		"* = " DA_POLICY_VERSION "; * = deny \n" /* <= Applied */
	},{
		TRUE, NON_PRIVILEGED_SENDER,
		OFONO_DBUS_ACCESS_INTF_VOICECALL,
		OFONO_DBUS_ACCESS_VOICECALL_HANGUP,
		"[Common]\n" /* DefaultAccess gets applied */
		"DefaultAccess = " DA_POLICY_VERSION "; * = allow \n"
		"[org.ofono.VoiceCallManager]\n"
		"* = " DA_POLICY_VERSION "; * = deny \n"
	},{
		TRUE, NON_PRIVILEGED_SENDER,
		OFONO_DBUS_ACCESS_INTF_VOICECALLMGR,
		OFONO_DBUS_ACCESS_VOICECALLMGR_DIAL,
		"[org.ofono.VoiceCallManager]\n"
		"* = " DA_POLICY_VERSION "; * = allow \n" /* <= Applied */
		"Dial = invalid \n"
	},{
		FALSE, PRIVILEGED_SENDER,
		OFONO_DBUS_ACCESS_INTF_VOICECALLMGR,
		OFONO_DBUS_ACCESS_VOICECALLMGR_DIAL,
		"[org.ofono.VoiceCallManager]\n"
		"* = " DA_POLICY_VERSION "; * = allow \n"
		"Dial = " DA_POLICY_VERSION "; * = deny \n"  /* <= Applied */
	}
};

static void test_config(gconstpointer test_data)
{
	const struct test_config_data *test = test_data;
	const char *default_config_file = sailfish_access_config_file;
	char *dir = g_dir_make_tmp(TMP_DIR_TEMPLATE, NULL);
	char *file = g_strconcat(dir, "/test.conf", NULL);

	/* Write temporary config file */
	sailfish_access_config_file = file;
	g_assert(g_file_set_contents(file, test->config, -1, NULL));

	g_assert(__ofono_builtin_sailfish_access.init() == 0);
	g_assert(__ofono_dbus_access_method_allowed(test->sender,
			test->intf, test->method, NULL) == test->allowed);
	__ofono_builtin_sailfish_access.exit();

	/* Restore the defaults */
	sailfish_access_config_file = default_config_file;

	remove(file);
	remove(dir);

	g_free(file);
	g_free(dir);
}

#define TEST_(test) "/sailfish_access/" test

int main(int argc, char *argv[])
{
	int i, ret;

	peer_pool = gutil_idle_pool_new();
	g_test_init(&argc, &argv, NULL);

	gutil_log_timestamp = FALSE;
	gutil_log_default.level = g_test_verbose() ?
		GLOG_LEVEL_VERBOSE : GLOG_LEVEL_NONE;
	__ofono_log_init("test-sailfish_access",
				g_test_verbose() ? "*" : NULL,
				FALSE, FALSE);

	g_test_add_func(TEST_("register"), test_register);
	g_test_add_func(TEST_("default"), test_default);
	for (i = 0; i < G_N_ELEMENTS(config_tests); i++) {
		char* name = g_strdup_printf(TEST_("config/%d"), i + 1);
		const struct test_config_data *test = config_tests + i;

		g_test_add_data_func(name, test, test_config);
		g_free(name);
	}
	ret = g_test_run();
	gutil_idle_pool_unref(peer_pool);
	return ret;
}

/*
 * Local Variables:
 * mode: C
 * c-basic-offset: 8
 * indent-tabs-mode: t
 * End:
 */
