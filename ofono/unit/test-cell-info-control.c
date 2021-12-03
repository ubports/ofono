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
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 *  GNU General Public License for more details.
 */

#include "ofono.h"

#include "cell-info.h"
#include "cell-info-control.h"

#include "fake_cell_info.h"

#include <gutil_log.h>
#include <gutil_macros.h>

#include <limits.h>

#define TEST_(name) "/cell_info_control/" name

/* ==== null ==== */

static void test_null(void)
{
	g_assert(!cell_info_control_get(NULL));
	g_assert(!cell_info_control_ref(NULL));
	cell_info_control_unref(NULL);
	cell_info_control_set_cell_info(NULL, NULL);
	cell_info_control_drop_all_requests(NULL);
	cell_info_control_drop_requests(NULL, NULL);
	cell_info_control_set_enabled(NULL, NULL, FALSE);
	cell_info_control_set_update_interval(NULL, NULL, FALSE);
}

/* ==== basic ==== */

static void test_basic(void)
{
	const char* path = "/test";
	CellInfoControl *ctl = cell_info_control_get(path);
	struct ofono_cell_info *info = fake_cell_info_new();
	void* tag1 = &ctl;
	void* tag2 = &info;

	/* Second cell_info_control_get returns the same object */
	g_assert_cmpstr(ctl->path, == ,path);
	g_assert(cell_info_control_get(path) == ctl);
	cell_info_control_unref(ctl);

	g_assert(ctl);
	g_assert(ctl == cell_info_control_ref(ctl));
	cell_info_control_unref(ctl);

	cell_info_control_set_cell_info(ctl, info);

	/* NULL tag is ignored */
	cell_info_control_set_enabled(ctl, NULL, TRUE);
	cell_info_control_set_update_interval(ctl, NULL, 0);
	g_assert(!fake_cell_info_is_enabled(info));
	g_assert_cmpint(fake_cell_info_update_interval(info), == ,INT_MAX);

	/* Update all attributes at once when cell_into is set */
	cell_info_control_set_cell_info(ctl, NULL);
	cell_info_control_set_enabled(ctl, tag1, TRUE);
	cell_info_control_set_update_interval(ctl, tag2, 10);
	cell_info_control_set_cell_info(ctl, info);
	g_assert(fake_cell_info_is_enabled(info));
	g_assert_cmpint(fake_cell_info_update_interval(info), == ,10);

	/* And then drop all requests at once */
	cell_info_control_drop_all_requests(ctl);
	g_assert(!fake_cell_info_is_enabled(info));
	g_assert_cmpint(fake_cell_info_update_interval(info), == ,INT_MAX);

	cell_info_control_set_cell_info(ctl, NULL);
	cell_info_control_unref(ctl);
	ofono_cell_info_unref(info);
}

/* ==== enabled ==== */

static void test_enabled(void)
{
	CellInfoControl *ctl = cell_info_control_get("/test");
	struct ofono_cell_info *info = fake_cell_info_new();
	void* tag1 = &ctl;
	void* tag2 = &info;
	void* wrong_tag = &tag1;

	cell_info_control_set_cell_info(ctl, info);

	g_assert(!fake_cell_info_is_enabled(info));
	cell_info_control_set_enabled(ctl, tag1, TRUE);
	g_assert(fake_cell_info_is_enabled(info));
	cell_info_control_set_enabled(ctl, tag2, TRUE);
	g_assert(fake_cell_info_is_enabled(info));
	cell_info_control_set_enabled(ctl, tag1, FALSE);
	g_assert(fake_cell_info_is_enabled(info));
	cell_info_control_set_enabled(ctl, tag2, FALSE);
	g_assert(!fake_cell_info_is_enabled(info));
	cell_info_control_set_enabled(ctl, tag2, FALSE);
	g_assert(!fake_cell_info_is_enabled(info));

	/* Do it again and then drop the request */
	cell_info_control_set_enabled(ctl, tag1, TRUE);
	cell_info_control_set_enabled(ctl, tag2, TRUE);
	g_assert(fake_cell_info_is_enabled(info));
	cell_info_control_drop_requests(ctl, tag1);
	g_assert(fake_cell_info_is_enabled(info)); /* tag2 is still there */
	cell_info_control_drop_requests(ctl, NULL); /* Ignored */
	cell_info_control_drop_requests(ctl, tag1); /* Isn't there */
	cell_info_control_drop_requests(ctl, wrong_tag); /* Wasn't there */
	g_assert(fake_cell_info_is_enabled(info));
	cell_info_control_drop_requests(ctl, tag2);
	g_assert(!fake_cell_info_is_enabled(info));

	/* These have no effect as all requests are already dropped */
	cell_info_control_drop_requests(ctl, tag1);
	g_assert(!fake_cell_info_is_enabled(info));
	cell_info_control_drop_requests(ctl, tag2);
	g_assert(!fake_cell_info_is_enabled(info));

	cell_info_control_unref(ctl);
	ofono_cell_info_unref(info);
}

/* ==== update_interval ==== */

static void test_update_interval(void)
{
	CellInfoControl *ctl = cell_info_control_get("/test");
	struct ofono_cell_info *info = fake_cell_info_new();
	void* tag1 = &ctl;
	void* tag2 = &info;
	void* wrong_tag = &tag1;

	cell_info_control_set_cell_info(ctl, info);

	cell_info_control_set_update_interval(ctl, tag1, 10);
	g_assert_cmpint(fake_cell_info_update_interval(info), == ,10);
	cell_info_control_set_update_interval(ctl, tag2, 5);
	g_assert_cmpint(fake_cell_info_update_interval(info), == ,5);
	cell_info_control_set_update_interval(ctl, tag2, INT_MAX);
	g_assert_cmpint(fake_cell_info_update_interval(info), == ,10);
	cell_info_control_set_update_interval(ctl, tag1, -1);
	g_assert_cmpint(fake_cell_info_update_interval(info), == ,INT_MAX);
	cell_info_control_set_update_interval(ctl, tag1, -1);
	g_assert_cmpint(fake_cell_info_update_interval(info), == ,INT_MAX);

	/* Do it again and then drop the requests one by one */
	cell_info_control_set_update_interval(ctl, tag1, 5);
	cell_info_control_set_update_interval(ctl, tag2, 10);
	g_assert_cmpint(fake_cell_info_update_interval(info), == ,5);
	cell_info_control_drop_requests(ctl, NULL); /* Ignored */
	cell_info_control_drop_requests(ctl, wrong_tag); /* Wasn't there */
	g_assert_cmpint(fake_cell_info_update_interval(info), == ,5);
	cell_info_control_drop_requests(ctl, tag1);
	g_assert_cmpint(fake_cell_info_update_interval(info), == ,10);
	cell_info_control_drop_requests(ctl, tag2);
	g_assert_cmpint(fake_cell_info_update_interval(info), == ,INT_MAX);

	/* These have no effect as all requests are already dropped */
	cell_info_control_drop_requests(ctl, tag1);
	g_assert_cmpint(fake_cell_info_update_interval(info), == ,INT_MAX);
	cell_info_control_drop_requests(ctl, tag2);
	g_assert_cmpint(fake_cell_info_update_interval(info), == ,INT_MAX);

	cell_info_control_unref(ctl);
	ofono_cell_info_unref(info);
}

int main(int argc, char *argv[])
{
	g_test_init(&argc, &argv, NULL);

	gutil_log_timestamp = FALSE;
	gutil_log_default.level = g_test_verbose() ?
		GLOG_LEVEL_VERBOSE : GLOG_LEVEL_NONE;
	__ofono_log_init("test-cell_info_control",
		g_test_verbose() ? "*" : NULL, FALSE, FALSE);

	g_test_add_func(TEST_("null"), test_null);
	g_test_add_func(TEST_("basic"), test_basic);
	g_test_add_func(TEST_("enabled"), test_enabled);
	g_test_add_func(TEST_("update_interval"), test_update_interval);
	return g_test_run();
}

/*
 * Local Variables:
 * mode: C
 * c-basic-offset: 8
 * indent-tabs-mode: t
 * End:
 */
