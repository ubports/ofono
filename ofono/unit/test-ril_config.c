/*
 *  oFono - Open Source Telephony
 *
 *  Copyright (C) 2018-2021 Jolla Ltd.
 *  Copyright (C) 2019-2020 Open Mobile Platform LLC.
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

#include "drivers/ril/ril_config.h"

#include <ofono/log.h>
#include "ofono.h"

#include <gutil_strv.h>
#include <gutil_ints.h>

#include <stdio.h>
#include <sys/stat.h>
#include <sys/types.h>

#define TMP_DIR_TEMPLATE "test-ril_config-XXXXXX"

static void test_get_value(const char *conf, void (*test)(GKeyFile *k))
{
	char *dir = g_dir_make_tmp(TMP_DIR_TEMPLATE, NULL);
	char *file = g_strconcat(dir, "/test.conf", NULL);
	GKeyFile *k = g_key_file_new();

	g_assert(g_file_set_contents(file, conf, -1, NULL));
	g_assert(g_key_file_load_from_file(k, file, 0, NULL));

	DBG("%s:\n%s", file, conf);
	test(k);

	remove(file);
	remove(dir);

	g_key_file_unref(k);
	g_free(file);
	g_free(dir);
}

/* ==== get_ints ==== */

static void test_get_ints_cb(GKeyFile *k)
{
	GUtilInts *ints;
	const int* data;
	guint count;

	g_assert(!ril_config_get_ints(k, "g1", "k1"));
	g_assert(!ril_config_get_ints(k, "g", "k2")); /* Empty */

	ints = ril_config_get_ints(k, "g", "k");
	data = gutil_ints_get_data(ints, &count);
	g_assert(count == 2);
	g_assert(data[0] == 0);
	g_assert(data[1] == 1);
	gutil_ints_unref(ints);

	ints = ril_config_get_ints(k, "g", "k1");
	data = gutil_ints_get_data(ints, &count);
	g_assert(count == 3);
	g_assert(data[0] == 2);
	g_assert(data[1] == 3);
	g_assert(data[2] == 4);
	gutil_ints_unref(ints);
}

static void test_get_ints(void)
{
	static const char conf [] =
		"[" RILCONF_SETTINGS_GROUP "]\nk = 0, 1, x\n"
		"[g]\nk1=2,3,4 # comment\nk2=\n";

	test_get_value(conf, test_get_ints_cb);
}

/* ==== ints_to_string ==== */

static void test_ints_to_string(void)
{
	static const int data[] = { 1, 2 };
	GUtilInts* ints = gutil_ints_new_static(data, G_N_ELEMENTS(data));
	char *str = ril_config_ints_to_string(ints, ',');
	g_assert(!g_strcmp0(str, "1,2"));
	g_free(str);
	gutil_ints_unref(ints);
	
	g_assert(!ril_config_ints_to_string(NULL, 0));
}

#define TEST_(name) "/ril_config/" name

int main(int argc, char *argv[])
{
	g_test_init(&argc, &argv, NULL);

	__ofono_log_init("test-ril_config",
				g_test_verbose() ? "*" : NULL,
				FALSE, FALSE);

	g_test_add_func(TEST_("get_ints"), test_get_ints);
	g_test_add_func(TEST_("ints_to_string"), test_ints_to_string);

	return g_test_run();
}

/*
 * Local Variables:
 * mode: C
 * c-basic-offset: 8
 * indent-tabs-mode: t
 * End:
 */
