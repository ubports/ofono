/*
 *  oFono - Open Source Telephony
 *
 *  Copyright (C) 2019-2020 Jolla Ltd.
 *  Copyright (C) 2020 Open Mobile Platform LLC.
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

#include <errno.h>

static enum ofono_dbus_access dontcare_method_access(const char *sender,
	enum ofono_dbus_access_intf intf, int method, const char *arg)
{
	return OFONO_DBUS_ACCESS_DONT_CARE;
}
static enum ofono_dbus_access allow_method_access(const char *sender,
	enum ofono_dbus_access_intf intf, int method, const char *arg)
{
	return OFONO_DBUS_ACCESS_ALLOW;
}
static enum ofono_dbus_access deny_method_access(const char *sender,
	enum ofono_dbus_access_intf intf, int method, const char *arg)
{
	return OFONO_DBUS_ACCESS_DENY;
}

static enum ofono_dbus_access broken_method_access(const char *sender,
	enum ofono_dbus_access_intf intf, int method, const char *arg)
{
	return (enum ofono_dbus_access)(-1);
}

struct ofono_dbus_access_plugin access_inval;
struct ofono_dbus_access_plugin access_dontcare = {
	.name = "DontCare",
	.priority = OFONO_DBUS_ACCESS_PRIORITY_LOW,
	.method_access = dontcare_method_access
};
struct ofono_dbus_access_plugin access_allow = {
	.name = "Allow",
	.priority = OFONO_DBUS_ACCESS_PRIORITY_DEFAULT,
	.method_access = allow_method_access
};
struct ofono_dbus_access_plugin access_deny = {
	.name = "Deny",
	.priority = OFONO_DBUS_ACCESS_PRIORITY_LOW,
	.method_access = deny_method_access
};

struct ofono_dbus_access_plugin access_broken = {
	.name = "Broken",
	.priority = OFONO_DBUS_ACCESS_PRIORITY_LOW,
	.method_access = broken_method_access
};

/*==========================================================================*
 * Tests
 *==========================================================================*/

static void test_intf_name()
{
	int i;

	/* Valid interface ids must have names */
	for (i = 0; i < OFONO_DBUS_ACCESS_INTF_COUNT; i++) {
		g_assert(ofono_dbus_access_intf_name(i));
	}
	/* And the invalid ones must have no names */
	g_assert(!ofono_dbus_access_intf_name(-1));
	g_assert(!ofono_dbus_access_intf_name(i));
	/* An no method names too */
	g_assert(!ofono_dbus_access_method_name(-1, 0));
	g_assert(!ofono_dbus_access_method_name(i, 0));
}

struct test_method_name_data {
	enum ofono_dbus_access_intf intf;
	int n_methods;
};

static const struct test_method_name_data method_name_tests[] = {
	{
		OFONO_DBUS_ACCESS_INTF_MESSAGE,
		OFONO_DBUS_ACCESS_MESSAGE_METHOD_COUNT
	},{
		OFONO_DBUS_ACCESS_INTF_MESSAGEMGR,
		OFONO_DBUS_ACCESS_MESSAGEMGR_METHOD_COUNT
	},{
		OFONO_DBUS_ACCESS_INTF_VOICECALL,
		OFONO_DBUS_ACCESS_VOICECALL_METHOD_COUNT
	},{
		OFONO_DBUS_ACCESS_INTF_VOICECALLMGR,
		OFONO_DBUS_ACCESS_VOICECALLMGR_METHOD_COUNT
	},{
		OFONO_DBUS_ACCESS_INTF_CONNCTX,
		OFONO_DBUS_ACCESS_CONNCTX_METHOD_COUNT
	},{
		OFONO_DBUS_ACCESS_INTF_CONNMGR,
		OFONO_DBUS_ACCESS_CONNMGR_METHOD_COUNT
	},{
		OFONO_DBUS_ACCESS_INTF_SIMMGR,
		OFONO_DBUS_ACCESS_SIMMGR_METHOD_COUNT
	},{
		OFONO_DBUS_ACCESS_INTF_MODEM,
		OFONO_DBUS_ACCESS_MODEM_METHOD_COUNT
	},{
		OFONO_DBUS_ACCESS_INTF_RADIOSETTINGS,
		OFONO_DBUS_ACCESS_RADIOSETTINGS_METHOD_COUNT
	},{
		OFONO_DBUS_ACCESS_INTF_STK,
		OFONO_DBUS_ACCESS_STK_METHOD_COUNT
	},{
		OFONO_DBUS_ACCESS_INTF_OEMRAW,
		OFONO_DBUS_ACCESS_OEMRAW_METHOD_COUNT
	}
};

static void test_method_name(gconstpointer test_data)
{
	const struct test_method_name_data *test = test_data;
	int i;

	/* Valid method ids must have names */
	for (i = 0; i < test->n_methods; i++) {
		g_assert(ofono_dbus_access_method_name(test->intf, i));
	}
	/* And the invalid ones must have no names */
	g_assert(!ofono_dbus_access_method_name(test->intf, -1));
	g_assert(!ofono_dbus_access_method_name(test->intf, i));
}

G_STATIC_ASSERT(G_N_ELEMENTS(method_name_tests)==OFONO_DBUS_ACCESS_INTF_COUNT);

static void test_register()
{
	g_assert(ofono_dbus_access_plugin_register(NULL) == -EINVAL);
	g_assert(ofono_dbus_access_plugin_register(&access_inval) == -EINVAL);
	ofono_dbus_access_plugin_unregister(NULL);

	/* Plugin won't be registered more than once */
	g_assert(!ofono_dbus_access_plugin_register(&access_deny));
	g_assert(ofono_dbus_access_plugin_register(&access_deny) == -EALREADY);

	/* Allow has higher priority */
	g_assert(!ofono_dbus_access_plugin_register(&access_allow));
	g_assert(__ofono_dbus_access_method_allowed(":1.0", 0, 1, NULL));
	ofono_dbus_access_plugin_unregister(&access_deny);
	ofono_dbus_access_plugin_unregister(&access_allow);

	/* Allow has higher priority */
	g_assert(!ofono_dbus_access_plugin_register(&access_allow));
	g_assert(!ofono_dbus_access_plugin_register(&access_deny));
	g_assert(__ofono_dbus_access_method_allowed(":1.0", 0, 1, NULL));
	ofono_dbus_access_plugin_unregister(&access_deny);
	ofono_dbus_access_plugin_unregister(&access_allow);

	/* Deny wins here */
	g_assert(!ofono_dbus_access_plugin_register(&access_dontcare));
	g_assert(!ofono_dbus_access_plugin_register(&access_deny));
	g_assert(!__ofono_dbus_access_method_allowed(":1.0", 0, 1, NULL));
	ofono_dbus_access_plugin_unregister(&access_deny);
	ofono_dbus_access_plugin_unregister(&access_dontcare);

	/* And here too */
	g_assert(!ofono_dbus_access_plugin_register(&access_broken));
	g_assert(!ofono_dbus_access_plugin_register(&access_deny));
	g_assert(!__ofono_dbus_access_method_allowed(":1.0", 0, 1, NULL));
	ofono_dbus_access_plugin_unregister(&access_deny);
	ofono_dbus_access_plugin_unregister(&access_dontcare);

	/* DontCare will allow everything */
	g_assert(!ofono_dbus_access_plugin_register(&access_dontcare));
	g_assert(__ofono_dbus_access_method_allowed(":1.0", 0, 1, NULL));
	ofono_dbus_access_plugin_unregister(&access_dontcare);
}

#define TEST_(test) "/dbus-access/" test

int main(int argc, char *argv[])
{
	int i;

	g_test_init(&argc, &argv, NULL);

	__ofono_log_init("test-dbus-access", g_test_verbose() ? "*" : NULL,
		FALSE, FALSE);

	g_test_add_func(TEST_("intf_name"), test_intf_name);
	for (i = 0; i < G_N_ELEMENTS(method_name_tests); i++) {
		char* name = g_strdup_printf(TEST_("method_name/%d"), i + 1);
		const struct test_method_name_data *test =
			method_name_tests + i;

		g_test_add_data_func(name, test, test_method_name);
		g_free(name);
	}
	g_test_add_func(TEST_("register"), test_register);
	return g_test_run();
}

/*
 * Local Variables:
 * mode: C
 * c-basic-offset: 8
 * indent-tabs-mode: t
 * End:
 */
