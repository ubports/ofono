/*
 *  oFono - Open Source Telephony
 *
 *  Copyright (C) 2017 Jolla Ltd.
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

#include <sailfish_cell_info.h>

#include <gutil_log.h>

/* Fake sailfish_cell_info */

#define FAKE_HANDLER_ID (1)

static int fake_sailfish_cell_info_ref_count = 0;

static void fake_sailfish_cell_info_ref(struct sailfish_cell_info *info)
{
	g_assert(fake_sailfish_cell_info_ref_count >= 0);
	fake_sailfish_cell_info_ref_count++;
}

static void fake_sailfish_cell_info_unref(struct sailfish_cell_info *info)
{
	g_assert(fake_sailfish_cell_info_ref_count > 0);
	fake_sailfish_cell_info_ref_count--;
}

static gulong fake_sailfish_cell_info_add_cells_changed_handler
	(struct sailfish_cell_info *info, sailfish_cell_info_cb_t cb, void *arg)
{
	return FAKE_HANDLER_ID;
}

static void fake_sailfish_cell_info_remove_handler
				(struct sailfish_cell_info *info, gulong id)
{
	g_assert(id == FAKE_HANDLER_ID);
}

static const struct sailfish_cell_info_proc fake_sailfish_cell_info_proc = {
	fake_sailfish_cell_info_ref,
	fake_sailfish_cell_info_unref,
	fake_sailfish_cell_info_add_cells_changed_handler,
	fake_sailfish_cell_info_remove_handler
};

static struct sailfish_cell_info fake_sailfish_cell_info = {
	&fake_sailfish_cell_info_proc,
	NULL
};

/* ==== basic ==== */

static void test_basic(void)
{
	/* NULL resistance */
	g_assert(!sailfish_cell_info_ref(NULL));
	sailfish_cell_info_unref(NULL);
	g_assert(!sailfish_cell_compare_func(NULL, NULL));
	g_assert(!sailfish_cell_info_add_cells_changed_handler(NULL, NULL,
								NULL));
	sailfish_cell_info_remove_handler(NULL, 0);

	/* Make sure that callbacks are being invoked */
	g_assert(sailfish_cell_info_ref(&fake_sailfish_cell_info) ==
						&fake_sailfish_cell_info);
	g_assert(fake_sailfish_cell_info_ref_count == 1);
	g_assert(sailfish_cell_info_add_cells_changed_handler(
		&fake_sailfish_cell_info, NULL, NULL) == FAKE_HANDLER_ID);
	sailfish_cell_info_remove_handler(&fake_sailfish_cell_info,
							FAKE_HANDLER_ID);
	sailfish_cell_info_unref(&fake_sailfish_cell_info);
	g_assert(!fake_sailfish_cell_info_ref_count);
}

/* ==== compare ==== */

static void test_compare(void)
{
	struct sailfish_cell c1, c2;

	memset(&c1, 0, sizeof(c1));
	memset(&c2, 0, sizeof(c2));

	g_assert(!sailfish_cell_compare_location(NULL, NULL));
	g_assert(sailfish_cell_compare_location(&c1, NULL) > 0);
	g_assert(sailfish_cell_compare_location(NULL, &c2) < 0);

	c1.type = SAILFISH_CELL_TYPE_GSM;
	c2.type = SAILFISH_CELL_TYPE_WCDMA;
	g_assert(sailfish_cell_compare_location(&c1, &c2) < 0);
	g_assert(sailfish_cell_compare_location(&c2, &c1) > 0);

	/* GSM */
	c1.type = SAILFISH_CELL_TYPE_GSM;
	c2 = c1;
	g_assert(!sailfish_cell_compare_location(&c1, &c2));
	c2 = c1; c2.info.gsm.mcc++;
	g_assert(sailfish_cell_compare_location(&c1, &c2) < 0);
	c2 = c1; c2.info.gsm.mnc++;
	g_assert(sailfish_cell_compare_location(&c1, &c2) < 0);
	c2 = c1; c2.info.gsm.lac++;
	g_assert(sailfish_cell_compare_location(&c1, &c2) < 0);
	c2 = c1; c2.info.gsm.cid++;
	g_assert(sailfish_cell_compare_location(&c1, &c2) < 0);
	/* Other attributes are not being compared */
	c2 = c1; c2.info.gsm.arfcn++;
	g_assert(!sailfish_cell_compare_location(&c1, &c2));
	c2 = c1; c2.info.gsm.bsic++;
	g_assert(!sailfish_cell_compare_location(&c1, &c2));
	c2 = c1; c2.info.gsm.signalStrength++;
	g_assert(!sailfish_cell_compare_location(&c1, &c2));
	c2 = c1; c2.info.gsm.bitErrorRate++;
	g_assert(!sailfish_cell_compare_location(&c1, &c2));
	c2 = c1; c2.info.gsm.bitErrorRate++;
	g_assert(!sailfish_cell_compare_location(&c1, &c2));

	/* WCDMA */
	c1.type = SAILFISH_CELL_TYPE_WCDMA;
	c2 = c1;
	g_assert(!sailfish_cell_compare_location(&c1, &c2));
	c2 = c1; c2.info.wcdma.mcc++;
	g_assert(sailfish_cell_compare_location(&c1, &c2) < 0);
	c2 = c1; c2.info.wcdma.mnc++;
	g_assert(sailfish_cell_compare_location(&c1, &c2) < 0);
	c2 = c1; c2.info.wcdma.lac++;
	g_assert(sailfish_cell_compare_location(&c1, &c2) < 0);
	c2 = c1; c2.info.wcdma.cid++;
	g_assert(sailfish_cell_compare_location(&c1, &c2) < 0);
	/* Other attributes are not being compared */
	c2 = c1; c2.info.wcdma.psc++;
	g_assert(!sailfish_cell_compare_location(&c1, &c2));
	c2 = c1; c2.info.wcdma.uarfcn++;
	g_assert(!sailfish_cell_compare_location(&c1, &c2));
	c2 = c1; c2.info.wcdma.signalStrength++;
	g_assert(!sailfish_cell_compare_location(&c1, &c2));
	c2 = c1; c2.info.wcdma.bitErrorRate++;
	g_assert(!sailfish_cell_compare_location(&c1, &c2));

	/* LTE */
	c1.type = SAILFISH_CELL_TYPE_LTE;
	c2 = c1;
	g_assert(!sailfish_cell_compare_location(&c1, &c2));
	c2 = c1; c2.info.lte.mcc++;
	g_assert(sailfish_cell_compare_location(&c1, &c2) < 0);
	c2 = c1; c2.info.lte.mnc++;
	g_assert(sailfish_cell_compare_location(&c1, &c2) < 0);
	c2 = c1; c2.info.lte.ci++;
	g_assert(sailfish_cell_compare_location(&c1, &c2) < 0);
	c2 = c1; c2.info.lte.pci++;
	g_assert(sailfish_cell_compare_location(&c1, &c2) < 0);
	c2 = c1; c2.info.lte.tac++;
	g_assert(sailfish_cell_compare_location(&c1, &c2) < 0);
	/* Other attributes are not being compared */
	c2 = c1; c2.info.lte.earfcn++;
	g_assert(!sailfish_cell_compare_location(&c1, &c2));
	c2 = c1; c2.info.lte.signalStrength++;
	g_assert(!sailfish_cell_compare_location(&c1, &c2));
	c2 = c1; c2.info.lte.rsrp++;
	g_assert(!sailfish_cell_compare_location(&c1, &c2));
	c2 = c1; c2.info.lte.rsrq++;
	g_assert(!sailfish_cell_compare_location(&c1, &c2));
	c2 = c1; c2.info.lte.rssnr++;
	g_assert(!sailfish_cell_compare_location(&c1, &c2));
	c2 = c1; c2.info.lte.cqi++;
	g_assert(!sailfish_cell_compare_location(&c1, &c2));
	c2 = c1; c2.info.lte.timingAdvance++;
	g_assert(!sailfish_cell_compare_location(&c1, &c2));
}

#define TEST_(name) "/sailfish_cell_info/" name

int main(int argc, char *argv[])
{
	g_test_init(&argc, &argv, NULL);

	gutil_log_timestamp = FALSE;
	gutil_log_default.level = g_test_verbose() ?
		GLOG_LEVEL_VERBOSE : GLOG_LEVEL_NONE;

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
