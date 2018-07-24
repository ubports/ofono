/*
 *  oFono - Open Source Telephony
 *
 *  Copyright (C) 2018 Jolla Ltd.
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
#include <ofono/ril-transport.h>

#include <string.h>
#include <errno.h>

static void test_null(void)
{
	struct ofono_ril_transport noname;

	memset(&noname, 0, sizeof(noname));
	g_assert(ofono_ril_transport_register(NULL) == -EINVAL);
	g_assert(ofono_ril_transport_register(&noname) == -EINVAL);
	ofono_ril_transport_unregister(NULL);
	ofono_ril_transport_unregister(&noname);
	g_assert(!ofono_ril_transport_connect(NULL, NULL));
}

static void test_register(void)
{
	struct ofono_ril_transport foo;
	struct ofono_ril_transport bar;

	memset(&foo, 0, sizeof(foo));
	memset(&bar, 0, sizeof(bar));

	foo.name = "foo";
	bar.name = "bar";
	g_assert(ofono_ril_transport_register(&foo) == 0);
	g_assert(ofono_ril_transport_register(&bar) == 0);
	g_assert(ofono_ril_transport_register(&bar) == (-EALREADY));
	g_assert(!ofono_ril_transport_connect(foo.name, NULL));
	g_assert(!ofono_ril_transport_connect("test", NULL));
	ofono_ril_transport_unregister(&foo);
	ofono_ril_transport_unregister(&bar);
}

static struct grilio_transport *test_connect_cb(GHashTable *params)
{
	static int dummy;

	return (void*)&dummy;
}

static void test_connect(void)
{
	static const struct ofono_ril_transport test = {
		.name = "test",
		.api_version = OFONO_RIL_TRANSPORT_API_VERSION,
		.connect = test_connect_cb
	};
	
	g_assert(ofono_ril_transport_register(&test) == 0);
	/* The returned pointer points to a static variable, no need to free */
	g_assert(ofono_ril_transport_connect(test.name, NULL));
	ofono_ril_transport_unregister(&test);
}

#define TEST_(name) "/ril-transport/" name

int main(int argc, char *argv[])
{
	g_test_init(&argc, &argv, NULL);

	__ofono_log_init("test-ril_util",
		g_test_verbose() ? "*" : NULL,
		FALSE, FALSE);

	g_test_add_func(TEST_("null"), test_null);
	g_test_add_func(TEST_("register"), test_register);
	g_test_add_func(TEST_("connect"), test_connect);

	return g_test_run();
}

/*
 * Local Variables:
 * mode: C
 * c-basic-offset: 8
 * indent-tabs-mode: t
 * End:
 */
