/*
 *  oFono - Open Source Telephony
 *
 *  Copyright (C) 2017-2021 Jolla Ltd.
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

#include <ofono/cell-info.h>

#include <gutil_macros.h>
#include <gutil_log.h>

#include "ofono.h"

/* Fake cell_info */

#define FAKE_HANDLER_ID (1)

struct test_cell_info {
	struct ofono_cell_info info;
	int refcount;
	int interval;
	gboolean enabled;
};

static void test_cell_info_ref(struct ofono_cell_info *info)
{
	DBG("");
	G_CAST(info, struct test_cell_info, info)->refcount++;
}

static void test_cell_info_unref(struct ofono_cell_info *info)
{
	DBG("");
	G_CAST(info, struct test_cell_info, info)->refcount--;
}

static gulong test_cell_info_add_change_handler
	(struct ofono_cell_info *info, ofono_cell_info_cb_t cb, void *arg)
{
	DBG("");
	return FAKE_HANDLER_ID;
}

static void test_cell_info_remove_handler(struct ofono_cell_info *info,
	gulong id)
{
	DBG("%lu", id);
	g_assert_cmpuint(id, == ,FAKE_HANDLER_ID);
}

static void test_cell_info_set_update_interval(struct ofono_cell_info *info,
	int ms)
{
	G_CAST(info, struct test_cell_info, info)->interval = ms;
}

static void test_cell_info_set_enabled(struct ofono_cell_info *info,
	ofono_bool_t enabled)
{
	DBG("%d", enabled);
	G_CAST(info, struct test_cell_info, info)->enabled = enabled;
}

static const struct ofono_cell_info_proc test_cell_info_proc = {
	test_cell_info_ref,
	test_cell_info_unref,
	test_cell_info_add_change_handler,
	test_cell_info_remove_handler,
	test_cell_info_set_update_interval,
	test_cell_info_set_enabled
};

static const struct ofono_cell_info_proc dummy_cell_info_proc = {};

/* ==== basic ==== */

static void test_basic_cb(struct ofono_cell_info *ci, void *data)
{
	g_assert_not_reached();
}

static void test_basic(void)
{
	struct test_cell_info test = {
		{ &test_cell_info_proc, NULL }, 0, 0, FALSE
	};

	struct ofono_cell_info dummy = {
		&dummy_cell_info_proc, NULL
	};

	/* NULL resistance */
	g_assert(!ofono_cell_info_ref(NULL));
	g_assert(ofono_cell_info_ref(&dummy) == &dummy);
	ofono_cell_info_unref(NULL);
	ofono_cell_info_unref(&dummy);
	g_assert(!ofono_cell_info_add_change_handler(NULL, NULL, NULL));
	g_assert(!ofono_cell_info_add_change_handler(&dummy, NULL, NULL));
	ofono_cell_info_remove_handler(NULL, 0);
	ofono_cell_info_remove_handler(&dummy, 0);
	ofono_cell_info_set_update_interval(NULL, 0);
	ofono_cell_info_set_update_interval(&dummy, 0);
	ofono_cell_info_set_enabled(NULL, TRUE);
	ofono_cell_info_set_enabled(&dummy, TRUE);

	/* Make sure that callbacks are being invoked */
	g_assert(ofono_cell_info_ref(&test.info) == &test.info);
	g_assert_cmpint(test.refcount, == ,1);
	g_assert(!ofono_cell_info_add_change_handler(&test.info, NULL, NULL));
	g_assert_cmpuint(ofono_cell_info_add_change_handler(&test.info,
		test_basic_cb, NULL), == ,FAKE_HANDLER_ID);
	ofono_cell_info_remove_handler(&test.info, 0);
	ofono_cell_info_remove_handler(&test.info, FAKE_HANDLER_ID);

	g_assert_cmpint(test.interval, == ,0);
	ofono_cell_info_set_update_interval(&test.info, 10);
	g_assert_cmpint(test.interval, == ,10);

	g_assert(!test.enabled);
	ofono_cell_info_set_enabled(&test.info, TRUE);
	g_assert(test.enabled);
	ofono_cell_info_unref(&test.info);
	g_assert_cmpint(test.refcount, == ,0);
}

/* ==== compare ==== */

static void test_compare(void)
{
	struct ofono_cell c1, c2;

	memset(&c1, 0, sizeof(c1));
	memset(&c2, 0, sizeof(c2));

	g_assert(!ofono_cell_compare_location(NULL, NULL));
	g_assert(ofono_cell_compare_location(&c1, NULL) > 0);
	g_assert(ofono_cell_compare_location(NULL, &c2) < 0);

	c1.type = OFONO_CELL_TYPE_GSM;
	c2.type = OFONO_CELL_TYPE_WCDMA;
	g_assert(ofono_cell_compare_location(&c1, &c2) < 0);
	g_assert(ofono_cell_compare_location(&c2, &c1) > 0);

	/* GSM */
	c1.type = OFONO_CELL_TYPE_GSM;
	c2 = c1;
	g_assert(!ofono_cell_compare_location(&c1, &c2));
	c2 = c1; c2.info.gsm.mcc++;
	g_assert(ofono_cell_compare_location(&c1, &c2) < 0);
	c2 = c1; c2.info.gsm.mnc++;
	g_assert(ofono_cell_compare_location(&c1, &c2) < 0);
	c2 = c1; c2.info.gsm.lac++;
	g_assert(ofono_cell_compare_location(&c1, &c2) < 0);
	c2 = c1; c2.info.gsm.cid++;
	g_assert(ofono_cell_compare_location(&c1, &c2) < 0);
	/* Other attributes are not being compared */
	c2 = c1; c2.info.gsm.arfcn++;
	g_assert(!ofono_cell_compare_location(&c1, &c2));
	c2 = c1; c2.info.gsm.bsic++;
	g_assert(!ofono_cell_compare_location(&c1, &c2));
	c2 = c1; c2.info.gsm.signalStrength++;
	g_assert(!ofono_cell_compare_location(&c1, &c2));
	c2 = c1; c2.info.gsm.bitErrorRate++;
	g_assert(!ofono_cell_compare_location(&c1, &c2));
	c2 = c1; c2.info.gsm.bitErrorRate++;
	g_assert(!ofono_cell_compare_location(&c1, &c2));

	/* WCDMA */
	c1.type = OFONO_CELL_TYPE_WCDMA;
	c2 = c1;
	g_assert(!ofono_cell_compare_location(&c1, &c2));
	c2 = c1; c2.info.wcdma.mcc++;
	g_assert(ofono_cell_compare_location(&c1, &c2) < 0);
	c2 = c1; c2.info.wcdma.mnc++;
	g_assert(ofono_cell_compare_location(&c1, &c2) < 0);
	c2 = c1; c2.info.wcdma.lac++;
	g_assert(ofono_cell_compare_location(&c1, &c2) < 0);
	c2 = c1; c2.info.wcdma.cid++;
	g_assert(ofono_cell_compare_location(&c1, &c2) < 0);
	/* Other attributes are not being compared */
	c2 = c1; c2.info.wcdma.psc++;
	g_assert(!ofono_cell_compare_location(&c1, &c2));
	c2 = c1; c2.info.wcdma.uarfcn++;
	g_assert(!ofono_cell_compare_location(&c1, &c2));
	c2 = c1; c2.info.wcdma.signalStrength++;
	g_assert(!ofono_cell_compare_location(&c1, &c2));
	c2 = c1; c2.info.wcdma.bitErrorRate++;
	g_assert(!ofono_cell_compare_location(&c1, &c2));

	/* LTE */
	c1.type = OFONO_CELL_TYPE_LTE;
	c2 = c1;
	g_assert(!ofono_cell_compare_location(&c1, &c2));
	c2 = c1; c2.info.lte.mcc++;
	g_assert(ofono_cell_compare_location(&c1, &c2) < 0);
	c2 = c1; c2.info.lte.mnc++;
	g_assert(ofono_cell_compare_location(&c1, &c2) < 0);
	c2 = c1; c2.info.lte.ci++;
	g_assert(ofono_cell_compare_location(&c1, &c2) < 0);
	c2 = c1; c2.info.lte.pci++;
	g_assert(ofono_cell_compare_location(&c1, &c2) < 0);
	c2 = c1; c2.info.lte.tac++;
	g_assert(ofono_cell_compare_location(&c1, &c2) < 0);
	/* Other attributes are not being compared */
	c2 = c1; c2.info.lte.earfcn++;
	g_assert(!ofono_cell_compare_location(&c1, &c2));
	c2 = c1; c2.info.lte.signalStrength++;
	g_assert(!ofono_cell_compare_location(&c1, &c2));
	c2 = c1; c2.info.lte.rsrp++;
	g_assert(!ofono_cell_compare_location(&c1, &c2));
	c2 = c1; c2.info.lte.rsrq++;
	g_assert(!ofono_cell_compare_location(&c1, &c2));
	c2 = c1; c2.info.lte.rssnr++;
	g_assert(!ofono_cell_compare_location(&c1, &c2));
	c2 = c1; c2.info.lte.cqi++;
	g_assert(!ofono_cell_compare_location(&c1, &c2));
	c2 = c1; c2.info.lte.timingAdvance++;
	g_assert(!ofono_cell_compare_location(&c1, &c2));

	/* Unknown type */
	c1.type = c2.type = (enum ofono_cell_type)-1;
	g_assert(!ofono_cell_compare_location(&c1, &c2));
}

#define TEST_(name) "/cell-info/" name

int main(int argc, char *argv[])
{
	g_test_init(&argc, &argv, NULL);

	gutil_log_timestamp = FALSE;
	gutil_log_default.level = g_test_verbose() ?
		GLOG_LEVEL_VERBOSE : GLOG_LEVEL_NONE;
	__ofono_log_init("test-cell-info",
				g_test_verbose() ? "*" : NULL,
				FALSE, FALSE);

	g_test_add_func(TEST_("basic"), test_basic);
	g_test_add_func(TEST_("compare"), test_compare);

	return g_test_run();
}

/*
 * Local Variables:
 * mode: C
 * c-basic-offset: 8
 * indent-tabs-mode: t
 * End:
 */
