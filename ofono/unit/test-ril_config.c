/*
 *  oFono - Open Source Telephony
 *
 *  Copyright (C) 2018-2020 Jolla Ltd.
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

/* ==== get_string ==== */

static void test_get_string0_cb(GKeyFile *k)
{
	char *value = ril_config_get_string(k, "g", "k");
	g_assert(!g_strcmp0(value, "v"));
	g_free(value);
	g_assert(!ril_config_get_string(k, RILCONF_SETTINGS_GROUP, "k"));
	g_assert(!ril_config_get_string(k, "foo", "k"));
}

static void test_get_string0(void)
{
	static const char conf [] = "[g]\nk=v\n";
	test_get_value(conf, test_get_string0_cb);
}

static void test_get_string1_cb(GKeyFile *k)
{
	char *value = ril_config_get_string(k, RILCONF_SETTINGS_GROUP, "k");
	g_assert(!g_strcmp0(value, "v"));
	g_free(value);
	value = ril_config_get_string(k, "g", "k");
	g_assert(!g_strcmp0(value, "v"));
	g_free(value);
}

static void test_get_string1(void)
{
	static const char conf [] = "[" RILCONF_SETTINGS_GROUP "]\nk=v\n";

	test_get_value(conf, test_get_string1_cb);
}

static void test_get_string2_cb(GKeyFile *k)
{
	char *value = ril_config_get_string(k, RILCONF_SETTINGS_GROUP, "k");
	g_assert(!g_strcmp0(value, "v1"));
	g_free(value);
	value = ril_config_get_string(k, "g", "k");
	g_assert(!g_strcmp0(value, "v2"));
	g_free(value);
	value = ril_config_get_string(k, "g1", "k");
	g_assert(!g_strcmp0(value, "v1"));
	g_free(value);
}

static void test_get_string2(void)
{
	static const char conf [] =
		"[" RILCONF_SETTINGS_GROUP "]\nk=v1\n\n"
		"[g]\nk=v2\n";

	test_get_value(conf, test_get_string2_cb);
}

/* ==== get_strings ==== */

static void test_get_strings0_cb(GKeyFile *k)
{
	char **values = ril_config_get_strings(k, "g", "k", ',');
	g_assert(values);
	g_assert(gutil_strv_length(values) == 0);
	g_strfreev(values);

	values = ril_config_get_strings(k, RILCONF_SETTINGS_GROUP, "k", ',');
	g_assert(values);
	g_assert(gutil_strv_length(values) == 0);
	g_strfreev(values);
}

static void test_get_strings0(void)
{
	static const char conf [] = "[" RILCONF_SETTINGS_GROUP "]\nk=\n";
	test_get_value(conf, test_get_strings0_cb);
}

static void test_get_strings1_cb(GKeyFile *k)
{
	char **values = ril_config_get_strings(k, "g", "k", ',');
	g_assert(gutil_strv_length(values) == 2);
	g_assert(!g_strcmp0(values[0], "v0"));
	g_assert(!g_strcmp0(values[1], "v1"));
	g_strfreev(values);

	g_assert(!ril_config_get_strings(k, RILCONF_SETTINGS_GROUP, "k", ','));
}

static void test_get_strings1(void)
{
	static const char conf [] = "[g]\nk=v0 , v1\n";

	test_get_value(conf, test_get_strings1_cb);
}

/* ==== get_integer ==== */

static void test_get_integer0_cb(GKeyFile *k)
{
	int val = -1;

	g_assert(!ril_config_get_integer(k, "g1", "k1", NULL));
	g_assert(!ril_config_get_integer(k, "g1", "k1", &val));
	g_assert(val == -1);

	g_assert(ril_config_get_integer(k, "g", "k", NULL));
	g_assert(ril_config_get_integer(k, "g", "k", &val));
	g_assert(val == 1);

	g_assert(ril_config_get_integer(k, RILCONF_SETTINGS_GROUP, "k", &val));
	g_assert(val == 0);
}

static void test_get_integer0(void)
{
	static const char conf [] =
		"[" RILCONF_SETTINGS_GROUP "]\nk=0\n\n"
		"[g]\nk=1\n";

	test_get_value(conf, test_get_integer0_cb);
}

static void test_get_integer1_cb(GKeyFile *k)
{
	int val = -1;

	g_assert(!ril_config_get_integer(k, "g", "k", NULL));
	g_assert(!ril_config_get_integer(k, "g", "k", &val));
	g_assert(val == -1);

	g_assert(!ril_config_get_integer(k, RILCONF_SETTINGS_GROUP, "k", NULL));
	g_assert(!ril_config_get_integer(k, RILCONF_SETTINGS_GROUP, "k", &val));
	g_assert(val == -1);
}

static void test_get_integer1(void)
{
	/* Invalid integer values */
	static const char conf [] =
		"[" RILCONF_SETTINGS_GROUP "]\nk=foo\n\n"
		"[g]\nk=bar\n";

	test_get_value(conf, test_get_integer1_cb);
}

static void test_get_integer2_cb(GKeyFile *k)
{
	int val = -1;

	g_assert(ril_config_get_integer(k, "g", "k", NULL));
	g_assert(ril_config_get_integer(k, "g", "k", &val));
	g_assert(val == 1);

	g_assert(ril_config_get_integer(k, RILCONF_SETTINGS_GROUP, "k", NULL));
	g_assert(ril_config_get_integer(k, RILCONF_SETTINGS_GROUP, "k", &val));
	g_assert(val == 1);
}

static void test_get_integer2(void)
{
	/* Invalid value in [g] but a valid one in [Settings] */
	static const char conf [] =
		"[" RILCONF_SETTINGS_GROUP "]\nk=1\n"
		"\n[g]\nk=foo\n";

	test_get_value(conf, test_get_integer2_cb);
}

/* ==== get_boolean ==== */

static void test_get_boolean0_cb(GKeyFile *k)
{
	gboolean val = FALSE;

	g_assert(!ril_config_get_boolean(k, "g1", "k1", NULL));
	g_assert(!ril_config_get_boolean(k, "g1", "k1", &val));
	g_assert(!val);

	g_assert(ril_config_get_boolean(k, "g", "k", NULL));
	g_assert(ril_config_get_boolean(k, "g", "k", &val));
	g_assert(val == TRUE);

	g_assert(ril_config_get_boolean(k, RILCONF_SETTINGS_GROUP, "k", &val));
	g_assert(val == FALSE);
}

static void test_get_boolean0(void)
{
	static const char conf [] =
		"[" RILCONF_SETTINGS_GROUP "]\nk=false\n\n"
		"[g]\nk=true\n";

	test_get_value(conf, test_get_boolean0_cb);
}

static void test_get_boolean1_cb(GKeyFile *k)
{
	gboolean val = TRUE;

	g_assert(!ril_config_get_boolean(k, "g", "k", NULL));
	g_assert(!ril_config_get_boolean(k, "g", "k", &val));
	g_assert(val == TRUE);

	g_assert(!ril_config_get_boolean(k, RILCONF_SETTINGS_GROUP, "k", NULL));
	g_assert(!ril_config_get_boolean(k, RILCONF_SETTINGS_GROUP, "k", &val));
	g_assert(val == TRUE);
}

static void test_get_boolean1(void)
{
	/* Invalid boolean values */
	static const char conf [] =
		"[" RILCONF_SETTINGS_GROUP "]\nk=foo\n\n"
		"[g]\nk=bar\n";

	test_get_value(conf, test_get_boolean1_cb);
}

static void test_get_boolean2_cb(GKeyFile *k)
{
	gboolean val = FALSE;

	g_assert(ril_config_get_boolean(k, "g", "k", NULL));
	g_assert(ril_config_get_boolean(k, "g", "k", &val));
	g_assert(val == TRUE);

	g_assert(ril_config_get_boolean(k, RILCONF_SETTINGS_GROUP, "k", NULL));
	g_assert(ril_config_get_boolean(k, RILCONF_SETTINGS_GROUP, "k", &val));
	g_assert(val == TRUE);
}

static void test_get_boolean2(void)
{
	/* Invalid value in [g] but a valid one in [Settings] */
	static const char conf [] =
		"[" RILCONF_SETTINGS_GROUP "]\nk=true\n"
		"\n[g]\nk=foo\n";

	test_get_value(conf, test_get_boolean2_cb);
}

static void test_get_boolean3_cb(GKeyFile *k)
{
	gboolean val = FALSE;

	g_assert(ril_config_get_boolean(k, "g", "k", NULL));
	g_assert(ril_config_get_boolean(k, "g", "k", &val));
	g_assert(val == TRUE);

	g_assert(!ril_config_get_boolean(k, RILCONF_SETTINGS_GROUP, "k", NULL));
	g_assert(!ril_config_get_boolean(k, RILCONF_SETTINGS_GROUP, "k", &val));
	g_assert(val == TRUE);
}

static void test_get_boolean3(void)
{
	/* Valid value in [g] and invalid one in [Settings] */
	static const char conf [] =
		"[" RILCONF_SETTINGS_GROUP "]\nk=foo\n\n"
		"[g]\nk=true\n";

	test_get_value(conf, test_get_boolean3_cb);
}

/* ==== get_flag ==== */

static void test_get_flag_cb(GKeyFile *k)
{
	const int f = 0x01;
	int mask = 0;

	g_assert(!ril_config_get_flag(k, "g1", "k1", f, &mask));
	g_assert(!mask);

	g_assert(ril_config_get_flag(k, "g", "k", f, &mask));
	g_assert(mask & f);

	g_assert(ril_config_get_flag(k, RILCONF_SETTINGS_GROUP, "k", f, &mask));
	g_assert(!(mask & f));
}

static void test_get_flag(void)
{
	static const char conf [] =
		"[" RILCONF_SETTINGS_GROUP "]\nk=false\n\n"
		"[g]\nk=true\n";

	test_get_value(conf, test_get_flag_cb);
}

/* ==== get_enum ==== */

static void test_get_enum_cb(GKeyFile *k)
{
	int val = 0;

	g_assert(!ril_config_get_enum(k, "g1", "k1", &val, "foo", 1, NULL));
	g_assert(!val);

	g_assert(!ril_config_get_enum(k, "g", "k", NULL, "foo", 1, NULL));
	g_assert(!ril_config_get_enum(k, "g", "k", &val, "foo", 1, NULL));
	g_assert(!val);

	g_assert(ril_config_get_enum(k,"g","k",NULL,"foo",1,"bar",2,NULL));
	g_assert(ril_config_get_enum(k,"g","k",&val,"bar",2,"foo",1,NULL));
	g_assert(val == 2);

	g_assert(ril_config_get_enum(k, "g", "x", NULL,
		"a", 1, "b", 2, "y", 3, NULL));
	g_assert(ril_config_get_enum(k, "g", "x", &val,
		"a", 1, "b", 2, "y", 3, NULL));
	g_assert(val == 3);

	g_assert(ril_config_get_enum(k, RILCONF_SETTINGS_GROUP, "k", NULL,
		"foo", 1, NULL));
	g_assert(ril_config_get_enum(k, RILCONF_SETTINGS_GROUP, "k", &val,
		"foo", 1, NULL));
	g_assert(val == 1);
}

static void test_get_enum(void)
{
	static const char conf [] =
		"[" RILCONF_SETTINGS_GROUP "]\nk= foo# comment\n\n"
		"[g]\nk= bar \nx=y\n";

	test_get_value(conf, test_get_enum_cb);
}

/* ==== get_mask ==== */

static void test_get_mask_cb(GKeyFile *k)
{
	int v = 0;

	g_assert(!ril_config_get_mask(k, "g1", "k", NULL, "x",1, "y",2, NULL));
	g_assert(!ril_config_get_mask(k, "g1", "k", &v, "x",1, "y",2, NULL));
	g_assert_cmpint(v, ==, 0);

	g_assert(ril_config_get_mask(k, "g", "k", NULL, "x",1, "y",2, NULL));
	g_assert(ril_config_get_mask(k, "g", "k", &v, "x",1, "y",2, NULL));
	g_assert_cmpint(v, ==, 1);

	g_assert(ril_config_get_mask(k, "g", "k1", NULL, "x",1, "y",2, NULL));
	g_assert(ril_config_get_mask(k, "g", "k1", &v, "x",1, "y",2, NULL));
	g_assert_cmpint(v, ==, 3);

	g_assert(!ril_config_get_mask(k, "g", "k2", NULL, "x",1, "y",2, NULL));
	g_assert(!ril_config_get_mask(k, "g", "k2", &v, "x",1, "y",2, NULL));
	g_assert_cmpint(v, ==, 0);
}

static void test_get_mask(void)
{
	static const char conf [] = "[g]\n"
		"k = x# comment\n"
		"k1 = x+y\n"
		"k2 = x+z+y\n";

	test_get_value(conf, test_get_mask_cb);
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

	g_test_add_func(TEST_("get_string0"), test_get_string0);
	g_test_add_func(TEST_("get_string1"), test_get_string1);
	g_test_add_func(TEST_("get_string2"), test_get_string2);
	g_test_add_func(TEST_("get_strings0"), test_get_strings0);
	g_test_add_func(TEST_("get_strings1"), test_get_strings1);
	g_test_add_func(TEST_("get_integer0"), test_get_integer0);
	g_test_add_func(TEST_("get_integer1"), test_get_integer1);
	g_test_add_func(TEST_("get_integer2"), test_get_integer2);
	g_test_add_func(TEST_("get_boolean0"), test_get_boolean0);
	g_test_add_func(TEST_("get_boolean1"), test_get_boolean1);
	g_test_add_func(TEST_("get_boolean2"), test_get_boolean2);
	g_test_add_func(TEST_("get_boolean3"), test_get_boolean3);
	g_test_add_func(TEST_("get_flag"), test_get_flag);
	g_test_add_func(TEST_("get_enum"), test_get_enum);
	g_test_add_func(TEST_("get_mask"), test_get_mask);
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
