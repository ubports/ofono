/*
 *  oFono - Open Source Telephony
 *
 *  Copyright (C) 2018-2021 Jolla Ltd.
 *  Copyright (C) 2019 Open Mobile Platform LLC.
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

#include <ofono/conf.h>
#include <ofono/log.h>
#include "ofono.h"

#include <gutil_strv.h>

#include <stdio.h>
#include <sys/stat.h>
#include <sys/types.h>

#define TMP_DIR_TEMPLATE "test-conf-XXXXXX"

static gboolean test_keyfile_empty(GKeyFile *k)
{
	gsize n = 0;
	char **groups = g_key_file_get_groups(k, &n);

	g_strfreev(groups);
	return !n;
}

static void test_merge_ignore(const char *filename, const char *contents,
	const char *dirname, const char *filename1, const char *contents1)
{
	char *dir = g_dir_make_tmp(TMP_DIR_TEMPLATE, NULL);
	char *file = g_strconcat(dir, "/", filename, NULL);
	char *subdir = g_strconcat(dir, "/", dirname, NULL);
	char *file1 = g_strconcat(subdir, "/", filename1, NULL);
	GKeyFile *k = g_key_file_new();
	char *data;

	g_assert(!mkdir(subdir, 0700));
	g_assert(g_file_set_contents(file, contents, -1, NULL));
	g_assert(g_file_set_contents(file1, contents1, -1, NULL));
	DBG("reading %s", file);
	ofono_conf_merge_files(k, file);
	data = g_key_file_to_data(k, NULL, NULL);
	DBG("\n%s", data);
	g_assert(!g_strcmp0(data, contents));
	g_free(data);
	g_key_file_unref(k);

	remove(file);
	remove(file1);
	remove(subdir);
	remove(dir);

	g_free(file);
	g_free(file1);
	g_free(dir);
	g_free(subdir);
}

static void test_merge1(const char *conf, const char *conf1, const char *out)
{
	char *dir = g_dir_make_tmp(TMP_DIR_TEMPLATE, NULL);
	char *file = g_strconcat(dir, "/foo.conf", NULL);
	char *subdir = g_strconcat(dir, "/foo.d", NULL);
	char *file1 = g_strconcat(subdir, "/bar.conf", NULL);
	GKeyFile *k = g_key_file_new();
	char *data;

	g_assert(!mkdir(subdir, 0700));
	g_assert(g_file_set_contents(file, conf, -1, NULL));
	g_assert(g_file_set_contents(file1, conf1, -1, NULL));

	DBG("reading %s", file);
	g_key_file_set_list_separator(k, ',');
	ofono_conf_merge_files(k, file);
	data = g_key_file_to_data(k, NULL, NULL);
	DBG("\n%s", data);
	g_assert(!g_strcmp0(data, out));
	g_free(data);
	g_key_file_unref(k);

	remove(file);
	remove(file1);
	remove(subdir);
	remove(dir);

	g_free(file);
	g_free(file1);
	g_free(dir);
	g_free(subdir);
}

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

/* ==== merge_basic ==== */

static void test_merge_basic(void)
{
	GKeyFile *k = g_key_file_new();
	char *nonexistent = g_dir_make_tmp(TMP_DIR_TEMPLATE, NULL);

	ofono_conf_merge_files(NULL, NULL);

	remove(nonexistent);
	ofono_conf_merge_files(k, nonexistent);
	g_assert(test_keyfile_empty(k));

	ofono_conf_merge_files(k, NULL);
	g_assert(test_keyfile_empty(k));

	ofono_conf_merge_files(k, "");
	g_assert(test_keyfile_empty(k));

	g_key_file_unref(k);
	g_free(nonexistent);
}

/* ==== merge_simple ==== */

static void test_merge_simple(void)
{
	static const char contents [] = "[foo]\na=1\nb=2\n";
	char *dir = g_dir_make_tmp(TMP_DIR_TEMPLATE, NULL);
	char *file = g_strconcat(dir, "/foo.conf", NULL);
	char *data;
	GKeyFile *k = g_key_file_new();

	g_assert(g_file_set_contents(file, contents, -1, NULL));
	DBG("reading %s", file);
	ofono_conf_merge_files(k, file);
	data = g_key_file_to_data(k, NULL, NULL);
	DBG("\n%s", data);
	g_assert(!g_strcmp0(data, contents));
	g_free(data);
	g_key_file_unref(k);

	remove(file);
	remove(dir);

	g_free(file);
	g_free(dir);
}

/* ==== merge_empty_dir ==== */

static void test_merge_empty_dir(void)
{
	static const char contents [] = "[foo]\na=1\nb=2\n";
	char *dir = g_dir_make_tmp(TMP_DIR_TEMPLATE, NULL);
	char *subdir = g_strconcat(dir, "/foo.d", NULL);
	char *file = g_strconcat(dir, "/foo.conf", NULL);
	GKeyFile *k = g_key_file_new();
	char *data;

	g_assert(!mkdir(subdir, 0700));
	g_assert(g_file_set_contents(file, contents, -1, NULL));
	DBG("reading %s", file);
	ofono_conf_merge_files(k, file);
	data = g_key_file_to_data(k, NULL, NULL);
	DBG("\n%s", data);
	g_assert(!g_strcmp0(data, contents));
	g_free(data);
	g_key_file_unref(k);

	remove(file);
	remove(subdir);
	remove(dir);

	g_free(file);
	g_free(dir);
	g_free(subdir);
}

/* ==== merge_ignore ==== */

static void test_merge_ignore0(void)
{
	static const char contents [] = "[foo]\na=1\nb=2\n";
	char *dir = g_dir_make_tmp(TMP_DIR_TEMPLATE, NULL);
	char *subdir = g_strconcat(dir, "/foo.d", NULL);
	char *subdir2 = g_strconcat(subdir, "/dir.conf", NULL);
	char *file = g_strconcat(dir, "/foo.conf", NULL);
	GKeyFile *k = g_key_file_new();
	char *data;

	/* Two empty subdirectories, one with matching name, one not */
	g_assert(!mkdir(subdir, 0700));
	g_assert(!mkdir(subdir2, 0700));
	g_assert(g_file_set_contents(file, contents, -1, NULL));
	DBG("reading %s", file);
	ofono_conf_merge_files(k, file);
	data = g_key_file_to_data(k, NULL, NULL);
	DBG("\n%s", data);
	g_assert(!g_strcmp0(data, contents));
	g_free(data);
	g_key_file_unref(k);

	remove(file);
	remove(subdir2);
	remove(subdir);
	remove(dir);

	g_free(file);
	g_free(dir);
	g_free(subdir);
	g_free(subdir2);
}

static void test_merge_ignore1(void)
{
	static const char contents [] = "[foo]\na=1\nb=2\n";
	static const char contents1 [] = "[foo]\nb=3\n";

	/* File has no suffix */
	test_merge_ignore("foo.conf", contents, "foo.d", "file", contents1);
}

static void test_merge_ignore2(void)
{
	static const char contents [] = "[foo]\na=1\nb=2\n";
	static const char contents1 [] = "[[[[[[[";

	/* File is not a valid keyfile */
	test_merge_ignore("foo.conf", contents, "foo.d", "a.conf", contents1);
}

/* ==== merge_sort ==== */

static void test_merge_sort(void)
{
	static const char contents [] = "[foo]\na=1\nb=2\n";
	static const char contents1 [] = "[foo]\nb=3\n";
	static const char contents2 [] = "[foo]\nb=4\n";
	static const char result [] = "[foo]\na=1\nb=4\n";

	/* Test file sort order */
	char *dir = g_dir_make_tmp(TMP_DIR_TEMPLATE, NULL);
	char *file = g_strconcat(dir, "/foo.", NULL);
	char *subdir = g_strconcat(dir, "/foo.d", NULL);
	char *file1 = g_strconcat(subdir, "/1.conf", NULL);
	char *file2 = g_strconcat(subdir, "/2.conf", NULL);
	GKeyFile *k = g_key_file_new();
	char *data;

	g_assert(!mkdir(subdir, 0700));
	g_assert(g_file_set_contents(file, contents, -1, NULL));
	g_assert(g_file_set_contents(file1, contents1, -1, NULL));
	g_assert(g_file_set_contents(file2, contents2, -1, NULL));

	DBG("reading %s", file);
	ofono_conf_merge_files(k, file);
	data = g_key_file_to_data(k, NULL, NULL);
	DBG("\n%s", data);
	g_assert(!g_strcmp0(data, result));
	g_free(data);
	g_key_file_unref(k);

	remove(file);
	remove(file1);
	remove(file2);
	remove(subdir);
	remove(dir);

	g_free(file);
	g_free(file1);
	g_free(file2);
	g_free(dir);
	g_free(subdir);
}

/* ==== merge_remove_group ==== */

static void test_merge_remove_group(void)
{
	static const char contents [] = "[foo]\na=1\n\n[bar]\nb=1\n";
	static const char contents1 [] = "[!bar]\n";
	static const char result [] = "[foo]\na=1\n";

	test_merge1(contents, contents1, result);
}

/* ==== merge_remove_key ==== */

static void test_merge_remove_key(void)
{
	static const char contents [] = "[foo]\na=1\nb=2\n";
	static const char contents1 [] = "[foo]\n!b=\n\n!=\n";
	static const char result [] = "[foo]\na=1\n";

	test_merge1(contents, contents1, result);
}

/* ==== merge_default_value ==== */

static void test_merge_default_value(void)
{
	/* b is assigned the default value, a stays as is */
	static const char contents [] = "[foo]\na=1\n";
	static const char contents1 [] = "[foo]\na:=2\nb:=3\n";
	static const char result [] = "[foo]\na=1\nb=3\n";

	test_merge1(contents, contents1, result);
}

/* ==== merge_list_add ==== */

static void test_merge_list_add0(void)
{
	/* Adding empty list */
	static const char contents [] = "[foo]\na=1\nb=2\n";
	static const char contents1 [] = "[foo]\na+=\n";

	test_merge1(contents, contents1, contents);
}

static void test_merge_list_add1(void)
{
	/* a=1 turns into a=1,2, */
	static const char contents [] = "[foo]\na=1\nb=2\n";
	static const char contents1 [] = "[foo]\na+=2,\n";
	static const char result [] = "[foo]\na=1,2,\nb=2\n";

	test_merge1(contents, contents1, result);
}

static void test_merge_list_add2(void)
{
	/* 2 is already there */
	static const char contents [] = "[foo]\na=1,2,\nb=2\n";
	static const char contents1 [] = "[foo]\na?=2\n";

	test_merge1(contents, contents1, contents);
}

static void test_merge_list_add3(void)
{
	/* 2 is already there, 3 is not */
	static const char contents [] = "[foo]\na=1,2,\n";
	static const char contents1 [] = "[foo]\na?=2,3,\n";
	static const char result [] = "[foo]\na=1,2,3,\n";

	test_merge1(contents, contents1, result);
}

static void test_merge_list_add4(void)
{
	/* b=2,3, is created */
	static const char contents [] = "[foo]\na=1\n";
	static const char contents1 [] = "[foo]\nb?=2,3,\n";
	static const char result [] = "[foo]\na=1\nb=2,3,\n";

	test_merge1(contents, contents1, result);
}

static void test_merge_list_add5(void)
{
	/* Add a new group */
	static const char contents [] = "[foo]\na=1\n";
	static const char contents1 [] = "[bar]\nb=2\n";
	static const char result [] = "[foo]\na=1\n\n[bar]\nb=2\n";

	test_merge1(contents, contents1, result);
}

/* ==== merge_list_remove ==== */

static void test_merge_list_remove0(void)
{
	static const char contents [] = "[foo]\na=1,2,\n";
	static const char contents1 [] = "[foo]\na-=\n";

	test_merge1(contents, contents1, contents);
}

static void test_merge_list_remove1(void)
{
	static const char contents [] = "[foo]\na=1,2,\n";
	static const char contents1 [] = "[foo]\na-=2,\n";
	static const char result [] = "[foo]\na=1,\n";

	test_merge1(contents, contents1, result);
}

static void test_merge_list_remove2(void)
{
	static const char contents [] = "[foo]\na=1,2,\n";
	static const char contents1 [] = "[foo]\na-=3\n";

	test_merge1(contents, contents1, contents);
}

static void test_merge_list_remove3(void)
{
	static const char contents [] = "[foo]\na=1,2,\n";
	static const char contents1 [] = "[foo]\nb-=1\n";

	test_merge1(contents, contents1, contents);
}

/* ==== get_string ==== */

static void test_get_string0_cb(GKeyFile *k)
{
	char *value = ofono_conf_get_string(k, "g", "k");

	g_assert(!g_strcmp0(value, "v"));
	g_free(value);
	g_assert(!ofono_conf_get_string(k, OFONO_COMMON_SETTINGS_GROUP, "k"));
	g_assert(!ofono_conf_get_string(k, "foo", "k"));
}

static void test_get_string0(void)
{
	static const char conf [] = "[g]\nk=v\n";

	test_get_value(conf, test_get_string0_cb);
}

static void test_get_string1_cb(GKeyFile *k)
{
	char *val = ofono_conf_get_string(k,OFONO_COMMON_SETTINGS_GROUP,"k");

	g_assert_cmpstr(val, == ,"v");
	g_free(val);

	val = ofono_conf_get_string(k, "g", "k");
	g_assert_cmpstr(val, == ,"v");
	g_free(val);
}

static void test_get_string1(void)
{
	static const char conf [] = "[" OFONO_COMMON_SETTINGS_GROUP "]\nk=v\n";

	test_get_value(conf, test_get_string1_cb);
}

static void test_get_string2_cb(GKeyFile *k)
{
	char *val = ofono_conf_get_string(k,OFONO_COMMON_SETTINGS_GROUP,"k");

	g_assert_cmpstr(val, == ,"v1");
	g_free(val);

	val = ofono_conf_get_string(k, "g", "k");
	g_assert_cmpstr(val, == ,"v2");
	g_free(val);

	val = ofono_conf_get_string(k, "g1", "k");
	g_assert_cmpstr(val, == ,"v1");
	g_free(val);
}

static void test_get_string2(void)
{
	static const char conf [] =
		"[" OFONO_COMMON_SETTINGS_GROUP "]\nk=v1\n\n"
		"[g]\nk=v2\n";

	test_get_value(conf, test_get_string2_cb);
}

/* ==== get_strings ==== */

static void test_get_strings0_cb(GKeyFile *k)
{
	char **values = ofono_conf_get_strings(k, "g", "k", ',');

	g_assert(values);
	g_assert_cmpuint(gutil_strv_length(values), == ,0);
	g_strfreev(values);

	values = ofono_conf_get_strings(k, OFONO_COMMON_SETTINGS_GROUP,
		"k", ',');
	g_assert(values);
	g_assert_cmpuint(gutil_strv_length(values), == ,0);
	g_strfreev(values);
}

static void test_get_strings0(void)
{
	static const char conf [] = "[" OFONO_COMMON_SETTINGS_GROUP "]\nk=\n";
	test_get_value(conf, test_get_strings0_cb);
}

static void test_get_strings1_cb(GKeyFile *k)
{
	char **values = ofono_conf_get_strings(k, "g", "k", ',');

	g_assert_cmpuint(gutil_strv_length(values), == ,2);
	g_assert_cmpstr(values[0], == ,"v0");
	g_assert_cmpstr(values[1], == ,"v1");
	g_strfreev(values);

	g_assert(!ofono_conf_get_strings(k, OFONO_COMMON_SETTINGS_GROUP,
		"k", ','));
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

	g_assert(!ofono_conf_get_integer(k, "g1", "k1", NULL));
	g_assert(!ofono_conf_get_integer(k, "g1", "k1", &val));
	g_assert_cmpint(val, == ,-1);

	g_assert(ofono_conf_get_integer(k, "g", "k", NULL));
	g_assert(ofono_conf_get_integer(k, "g", "k", &val));
	g_assert_cmpint(val, == ,1);

	g_assert(ofono_conf_get_integer(k, OFONO_COMMON_SETTINGS_GROUP,
		"k", &val));
	g_assert_cmpint(val, == ,0);
}

static void test_get_integer0(void)
{
	static const char conf [] =
		"[" OFONO_COMMON_SETTINGS_GROUP "]\nk=0\n\n"
		"[g]\nk=1\n";

	test_get_value(conf, test_get_integer0_cb);
}

static void test_get_integer1_cb(GKeyFile *k)
{
	int val = -1;

	g_assert(!ofono_conf_get_integer(k, "g", "k", NULL));
	g_assert(!ofono_conf_get_integer(k, "g", "k", &val));
	g_assert_cmpint(val, == ,-1);

	g_assert(!ofono_conf_get_integer(k, OFONO_COMMON_SETTINGS_GROUP,
		"k", NULL));
	g_assert(!ofono_conf_get_integer(k, OFONO_COMMON_SETTINGS_GROUP,
		"k", &val));
	g_assert_cmpint(val, == ,-1);
}

static void test_get_integer1(void)
{
	/* Invalid integer values */
	static const char conf [] =
		"[" OFONO_COMMON_SETTINGS_GROUP "]\nk=foo\n\n"
		"[g]\nk=bar\n";

	test_get_value(conf, test_get_integer1_cb);
}

static void test_get_integer2_cb(GKeyFile *k)
{
	int val = -1;

	g_assert(ofono_conf_get_integer(k, "g", "k", NULL));
	g_assert(ofono_conf_get_integer(k, "g", "k", &val));
	g_assert_cmpint(val, == ,1);

	g_assert(ofono_conf_get_integer(k, OFONO_COMMON_SETTINGS_GROUP,
		"k", NULL));
	g_assert(ofono_conf_get_integer(k, OFONO_COMMON_SETTINGS_GROUP,
		"k", &val));
	g_assert_cmpint(val, == ,1);
}

static void test_get_integer2(void)
{
	/* Invalid value in [g] but a valid one in [Settings] */
	static const char conf [] =
		"[" OFONO_COMMON_SETTINGS_GROUP "]\nk=1\n"
		"\n[g]\nk=foo\n";

	test_get_value(conf, test_get_integer2_cb);
}

/* ==== get_boolean ==== */

static void test_get_boolean0_cb(GKeyFile *k)
{
	gboolean val = FALSE;

	g_assert(!ofono_conf_get_boolean(k, "g1", "k1", NULL));
	g_assert(!ofono_conf_get_boolean(k, "g1", "k1", &val));
	g_assert(!val);

	g_assert(ofono_conf_get_boolean(k, "g", "k", NULL));
	g_assert(ofono_conf_get_boolean(k, "g", "k", &val));
	g_assert(val == TRUE);

	g_assert(ofono_conf_get_boolean(k, OFONO_COMMON_SETTINGS_GROUP,
		"k", &val));
	g_assert(val == FALSE);
}

static void test_get_boolean0(void)
{
	static const char conf [] =
		"[" OFONO_COMMON_SETTINGS_GROUP "]\nk=false\n\n"
		"[g]\nk=true\n";

	test_get_value(conf, test_get_boolean0_cb);
}

static void test_get_boolean1_cb(GKeyFile *k)
{
	gboolean val = TRUE;

	g_assert(!ofono_conf_get_boolean(k, "g", "k", NULL));
	g_assert(!ofono_conf_get_boolean(k, "g", "k", &val));
	g_assert(val == TRUE);

	g_assert(!ofono_conf_get_boolean(k, OFONO_COMMON_SETTINGS_GROUP,
		"k", NULL));
	g_assert(!ofono_conf_get_boolean(k, OFONO_COMMON_SETTINGS_GROUP,
		"k", &val));
	g_assert(val == TRUE);
}

static void test_get_boolean1(void)
{
	/* Invalid boolean values */
	static const char conf [] =
		"[" OFONO_COMMON_SETTINGS_GROUP "]\nk=foo\n\n"
		"[g]\nk=bar\n";

	test_get_value(conf, test_get_boolean1_cb);
}

static void test_get_boolean2_cb(GKeyFile *k)
{
	gboolean val = FALSE;

	g_assert(ofono_conf_get_boolean(k, "g", "k", NULL));
	g_assert(ofono_conf_get_boolean(k, "g", "k", &val));
	g_assert(val == TRUE);

	g_assert(ofono_conf_get_boolean(k, OFONO_COMMON_SETTINGS_GROUP,
		"k", NULL));
	g_assert(ofono_conf_get_boolean(k, OFONO_COMMON_SETTINGS_GROUP,
		"k", &val));
	g_assert(val == TRUE);
}

static void test_get_boolean2(void)
{
	/* Invalid value in [g] but a valid one in [Settings] */
	static const char conf [] =
		"[" OFONO_COMMON_SETTINGS_GROUP "]\nk=true\n"
		"\n[g]\nk=foo\n";

	test_get_value(conf, test_get_boolean2_cb);
}

static void test_get_boolean3_cb(GKeyFile *k)
{
	gboolean val = FALSE;

	g_assert(ofono_conf_get_boolean(k, "g", "k", NULL));
	g_assert(ofono_conf_get_boolean(k, "g", "k", &val));
	g_assert(val == TRUE);

	g_assert(!ofono_conf_get_boolean(k, OFONO_COMMON_SETTINGS_GROUP,
		"k", NULL));
	g_assert(!ofono_conf_get_boolean(k, OFONO_COMMON_SETTINGS_GROUP,
		"k", &val));
	g_assert(val == TRUE);
}

static void test_get_boolean3(void)
{
	/* Valid value in [g] and invalid one in [Settings] */
	static const char conf [] =
		"[" OFONO_COMMON_SETTINGS_GROUP "]\nk=foo\n\n"
		"[g]\nk=true\n";

	test_get_value(conf, test_get_boolean3_cb);
}

/* ==== get_flag ==== */

static void test_get_flag_cb(GKeyFile *k)
{
	const int f = 0x01;
	int mask = 0;

	g_assert(!ofono_conf_get_flag(k, "g1", "k1", f, &mask));
	g_assert(!mask);

	g_assert(ofono_conf_get_flag(k, "g", "k", f, &mask));
	g_assert(mask & f);

	g_assert(ofono_conf_get_flag(k, OFONO_COMMON_SETTINGS_GROUP,
		"k", f, &mask));
	g_assert(!(mask & f));
}

static void test_get_flag(void)
{
	static const char conf [] =
		"[" OFONO_COMMON_SETTINGS_GROUP "]\nk=false\n\n"
		"[g]\nk=true\n";

	test_get_value(conf, test_get_flag_cb);
}

/* ==== get_enum ==== */

static void test_get_enum_cb(GKeyFile *k)
{
	int val = 0;

	g_assert(!ofono_conf_get_enum(k, "g1", "k1", &val, "foo", 1, NULL));
	g_assert_cmpint(val, == ,0);

	g_assert(!ofono_conf_get_enum(k, "g", "k", NULL, "foo", 1, NULL));
	g_assert(!ofono_conf_get_enum(k, "g", "k", &val, "foo", 1, NULL));
	g_assert_cmpint(val, == ,0);

	g_assert(ofono_conf_get_enum(k,"g","k",NULL,"foo",1,"bar",2,NULL));
	g_assert(ofono_conf_get_enum(k,"g","k",&val,"bar",2,"foo",1,NULL));
	g_assert_cmpint(val, == ,2);

	g_assert(ofono_conf_get_enum(k, "g", "x", NULL,
		"a", 1, "b", 2, "y", 3, NULL));
	g_assert(ofono_conf_get_enum(k, "g", "x", &val,
		"a", 1, "b", 2, "y", 3, NULL));
	g_assert(val == 3);

	g_assert(ofono_conf_get_enum(k, OFONO_COMMON_SETTINGS_GROUP,
		"k", NULL, "foo", 1, NULL));
	g_assert(ofono_conf_get_enum(k, OFONO_COMMON_SETTINGS_GROUP,
		"k", &val, "foo", 1, NULL));
	g_assert_cmpint(val, == ,1);
}

static void test_get_enum(void)
{
	static const char conf [] =
		"[" OFONO_COMMON_SETTINGS_GROUP "]\nk= foo# comment\n\n"
		"[g]\nk= bar \nx=y\n";

	test_get_value(conf, test_get_enum_cb);
}

/* ==== get_mask ==== */

static void test_get_mask_cb(GKeyFile *k)
{
	int v = 0;

	g_assert(!ofono_conf_get_mask(k,"g1","k",NULL,"x",1,"y",2,NULL));
	g_assert(!ofono_conf_get_mask(k,"g1","k",&v,"x",1,"y",2,NULL));
	g_assert_cmpint(v, ==, 0);

	g_assert(ofono_conf_get_mask(k,"g","k",NULL,"x",1,"y",2,NULL));
	g_assert(ofono_conf_get_mask(k,"g","k",&v,"x",1,"y",2,NULL));
	g_assert_cmpint(v, ==, 1);

	g_assert(ofono_conf_get_mask(k,"g","k1",NULL,"x",1,"y",2,NULL));
	g_assert(ofono_conf_get_mask(k,"g","k1",&v,"x",1,"y",2,NULL));
	g_assert_cmpint(v, ==, 3);

	g_assert(!ofono_conf_get_mask(k,"g","k2",NULL,"x",1,"y",2,NULL));
	g_assert(!ofono_conf_get_mask(k,"g","k2",&v,"x",1,"y",2,NULL));
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

#define TEST_(name) "/conf/" name

int main(int argc, char *argv[])
{
	g_test_init(&argc, &argv, NULL);

	__ofono_log_init("test-conf",
		g_test_verbose() ? "*" : NULL,
		FALSE, FALSE);

	g_test_add_func(TEST_("merge_basic"), test_merge_basic);
	g_test_add_func(TEST_("merge_simple"), test_merge_simple);
	g_test_add_func(TEST_("merge_empty_dir"), test_merge_empty_dir);
	g_test_add_func(TEST_("merge_ignore0"), test_merge_ignore0);
	g_test_add_func(TEST_("merge_ignore1"), test_merge_ignore1);
	g_test_add_func(TEST_("merge_ignore2"), test_merge_ignore2);
	g_test_add_func(TEST_("merge_sort"), test_merge_sort);
	g_test_add_func(TEST_("merge_remove_group"), test_merge_remove_group);
	g_test_add_func(TEST_("merge_remove_key"), test_merge_remove_key);
	g_test_add_func(TEST_("merge_default_value"), test_merge_default_value);
	g_test_add_func(TEST_("merge_list_add0"), test_merge_list_add0);
	g_test_add_func(TEST_("merge_list_add1"), test_merge_list_add1);
	g_test_add_func(TEST_("merge_list_add2"), test_merge_list_add2);
	g_test_add_func(TEST_("merge_list_add3"), test_merge_list_add3);
	g_test_add_func(TEST_("merge_list_add4"), test_merge_list_add4);
	g_test_add_func(TEST_("merge_list_add5"), test_merge_list_add5);
	g_test_add_func(TEST_("merge_list_remove0"), test_merge_list_remove0);
	g_test_add_func(TEST_("merge_list_remove1"), test_merge_list_remove1);
	g_test_add_func(TEST_("merge_list_remove2"), test_merge_list_remove2);
	g_test_add_func(TEST_("merge_list_remove3"), test_merge_list_remove3);
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

	return g_test_run();
}

/*
 * Local Variables:
 * mode: C
 * c-basic-offset: 8
 * indent-tabs-mode: t
 * End:
 */
