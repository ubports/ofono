/*
 *  oFono - Open Source Telephony
 *
 *  Copyright (C) 2018-2019 Jolla Ltd.
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

#include <ofono/log.h>
#include "ofono.h"

#include <gutil_strv.h>
#include <gutil_ints.h>

#include <stdio.h>
#include <sys/stat.h>
#include <sys/types.h>

#define TMP_DIR_TEMPLATE "test-config-XXXXXX"

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
	config_merge_files(k, file);
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
	config_merge_files(k, file);
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

/* ==== merge_basic ==== */

static void test_merge_basic(void)
{
	GKeyFile *k = g_key_file_new();
	char *nonexistent = g_dir_make_tmp(TMP_DIR_TEMPLATE, NULL);

	config_merge_files(NULL, NULL);

	remove(nonexistent);
	config_merge_files(k, nonexistent);
	g_assert(test_keyfile_empty(k));

	config_merge_files(k, NULL);
	g_assert(test_keyfile_empty(k));

	config_merge_files(k, "");
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
	config_merge_files(k, file);
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
	config_merge_files(k, file);
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
	config_merge_files(k, file);
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
	config_merge_files(k, file);
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

#define TEST_(name) "/config/" name

int main(int argc, char *argv[])
{
	g_test_init(&argc, &argv, NULL);

	__ofono_log_init("test-config",
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

	return g_test_run();
}

/*
 * Local Variables:
 * mode: C
 * c-basic-offset: 8
 * indent-tabs-mode: t
 * End:
 */
