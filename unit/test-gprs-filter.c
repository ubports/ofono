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

#include <gutil_macros.h>
#include <gutil_log.h>

#include <errno.h>

#define TEST_TIMEOUT_SEC (20)

static gboolean test_debug = FALSE;
static GMainLoop *test_loop = NULL;
static int test_filter_cancel_count;
static int test_filter_activate_count;
static int test_filter_check_count;

struct test_later_data {
	ofono_gprs_filter_activate_cb_t cb;
	struct ofono_gprs_primary_context* ctx;
	void *user_data;
};

struct test_check_later_data {
	ofono_bool_t allow;	
	ofono_gprs_filter_check_cb_t cb;
	void *user_data;
};

/* Fake data structures */

struct ofono_gprs_context {
	struct ofono_gprs_primary_context ctx;
	struct ofono_gprs *gprs;
};

struct ofono_gprs {
	struct gprs_filter_chain *chain;
};

/* Code shared by all tests */

static gboolean test_timeout_cb(gpointer user_data)
{
	g_assert(FALSE);
	return G_SOURCE_REMOVE;
}

static gboolean test_quit_cb(gpointer user_data)
{
	g_main_loop_quit(test_loop);
	return G_SOURCE_REMOVE;
}

static void test_inc(gpointer data)
{
	(*(int*)data)++;
}

static void test_activate_expect_allow
		(const struct ofono_gprs_primary_context *ctx, void *data)
{
	g_assert(ctx);
	if (data) (*(int*)data)++;
}

static void test_activate_expect_allow_and_quit
		(const struct ofono_gprs_primary_context *ctx, void *data)
{
	g_assert(ctx);
	if (data) (*(int*)data)++;
	g_main_loop_quit(test_loop);
}

static void test_activate_expect_disallow
		(const struct ofono_gprs_primary_context *ctx, void *data)
{
	g_assert(!ctx);
	if (data) (*(int*)data)++;
}

static void test_activate_expect_disallow_and_quit
		(const struct ofono_gprs_primary_context *ctx, void *data)
{
	g_assert(!ctx);
	if (data) (*(int*)data)++;
	g_main_loop_quit(test_loop);
}

static void test_check_expect_allow(ofono_bool_t allow, void *data)
{
	g_assert(allow);
	if (data) (*(int*)data)++;
}

static void test_check_expect_allow_and_quit(ofono_bool_t allow, void *data)
{
	g_assert(allow);
	if (data) (*(int*)data)++;
	g_main_loop_quit(test_loop);
}

static void test_check_expect_disallow_and_quit(ofono_bool_t allow, void *data)
{
	g_assert(!allow);
	if (data) (*(int*)data)++;
	g_main_loop_quit(test_loop);
}

static void test_clear_counts()
{
	test_filter_cancel_count = 0;
	test_filter_activate_count = 0;
	test_filter_check_count = 0;
}

static void test_common_init()
{
	test_clear_counts();
	test_loop = g_main_loop_new(NULL, FALSE);
	if (!test_debug) {
		g_timeout_add_seconds(TEST_TIMEOUT_SEC, test_timeout_cb, NULL);
	}
}

static void test_gc_init(struct ofono_gprs *gprs,
					struct ofono_gprs_context *gc)
{
	memset(gc, 0, sizeof(*gc));
	gc->gprs = gprs;
}

static void test_gprs_init(struct ofono_gprs *gprs,
					struct ofono_gprs_context *gc)
{
	memset(gprs, 0, sizeof(*gprs));
	test_gc_init(gprs, gc);
}

static void test_common_deinit()
{
	g_main_loop_unref(test_loop);
	test_loop = NULL;
}

static gboolean filter_later_cb(gpointer user_data)
{
	struct test_later_data* later = user_data;

	later->cb(later->ctx, later->user_data);
	return G_SOURCE_REMOVE;
}

static void filter_free_cb(gpointer user_data)
{
	struct test_later_data* later = user_data;

	g_free(later->ctx);
	g_free(later);
}

static unsigned int filter_later(ofono_gprs_filter_activate_cb_t cb,
		const struct ofono_gprs_primary_context *ctx, void *user_data)
{
	struct test_later_data* later = g_new0(struct test_later_data, 1);

	later->cb = cb;
	later->ctx = g_memdup(ctx, sizeof(*ctx));
	later->user_data = user_data;

	return g_idle_add_full(G_PRIORITY_DEFAULT_IDLE, filter_later_cb,
							later, filter_free_cb);
}

static unsigned int filter_activate_cancel(struct ofono_gprs_context *gc,
			const struct ofono_gprs_primary_context *ctx,
			ofono_gprs_filter_activate_cb_t cb, void *user_data)
{
	test_filter_cancel_count++;
	cb(NULL, user_data);
	return 0;
}

static unsigned int filter_activate_cancel_later(struct ofono_gprs_context *gc,
			const struct ofono_gprs_primary_context *ctx,
			ofono_gprs_filter_activate_cb_t cb, void *user_data)
{
	test_filter_cancel_count++;
	return filter_later(cb, NULL, user_data);
}

static unsigned int filter_activate_continue(struct ofono_gprs_context *gc,
			const struct ofono_gprs_primary_context *ctx,
			ofono_gprs_filter_activate_cb_t cb, void *user_data)
{
	test_filter_activate_count++;
	cb(ctx, user_data);
	return 0;
}

static unsigned int filter_activate_continue_later
		(struct ofono_gprs_context *gc,
			const struct ofono_gprs_primary_context *ctx,
			ofono_gprs_filter_activate_cb_t cb, void *user_data)
{
	test_filter_activate_count++;
	return filter_later(cb, ctx, user_data);
}

static gboolean filter_check_later_cb(gpointer user_data)
{
	struct test_check_later_data* later = user_data;

	later->cb(later->allow, later->user_data);
	return G_SOURCE_REMOVE;
}

static unsigned int filter_check_later(ofono_bool_t allow,
			ofono_gprs_filter_check_cb_t cb, void *user_data)
{
	struct test_check_later_data* later =
		g_new0(struct test_check_later_data, 1);

	later->allow = allow;
	later->cb = cb;
	later->user_data = user_data;

	return g_idle_add_full(G_PRIORITY_DEFAULT_IDLE, filter_check_later_cb,
							later, g_free);
}

static unsigned int filter_check_allow(struct ofono_gprs *gprs,
				ofono_gprs_filter_check_cb_t cb, void *data)
{
	test_filter_check_count++;
	cb(TRUE, data);
	return 0;
}

static unsigned int filter_check_disallow(struct ofono_gprs *gprs,
				ofono_gprs_filter_check_cb_t cb, void *data)
{
	test_filter_check_count++;
	cb(FALSE, data);
	return 0;
}

static unsigned int filter_check_disallow_later(struct ofono_gprs *gprs,
				ofono_gprs_filter_check_cb_t cb, void *data)
{
	test_filter_check_count++;
	return filter_check_later(FALSE, cb, data);
}

static void filter_cancel(unsigned int id)
{
	g_source_remove(id);
}

/* Test cases */

/* ==== misc ==== */

static void test_misc(void)
{
	static struct ofono_gprs_filter noname = {
		.api_version = OFONO_GPRS_FILTER_API_VERSION
	};

	static struct ofono_gprs_filter misc = {
		.name = "misc",
		.api_version = OFONO_GPRS_FILTER_API_VERSION,
	};

	int count = 0;
	struct ofono_gprs_primary_context ctx;

	memset(&ctx, 0, sizeof(ctx));

	g_assert(ofono_gprs_filter_register(NULL) == -EINVAL);
	g_assert(ofono_gprs_filter_register(&noname) == -EINVAL);
	g_assert(ofono_gprs_filter_register(&misc) == 0);
	g_assert(ofono_gprs_filter_register(&misc) == 0);
	__ofono_gprs_filter_chain_activate(NULL, NULL, NULL, NULL, NULL, NULL);
	__ofono_gprs_filter_chain_activate(NULL, NULL, &ctx,
				test_activate_expect_allow, NULL, NULL);
	__ofono_gprs_filter_chain_activate(NULL, NULL, NULL,
				test_activate_expect_disallow, NULL, NULL);
	__ofono_gprs_filter_chain_activate(NULL, NULL, NULL, NULL,
				test_inc, &count);
	g_assert(count == 1);
	count = 0;

	__ofono_gprs_filter_chain_check(NULL, NULL, NULL, NULL);
	__ofono_gprs_filter_chain_check(NULL, test_check_expect_allow,
							test_inc, &count);
	g_assert(count == 2);
	count = 0;

	g_assert(!__ofono_gprs_filter_chain_new(NULL));
	__ofono_gprs_filter_chain_cancel(NULL, NULL);
	__ofono_gprs_filter_chain_free(NULL);
	ofono_gprs_filter_unregister(&misc);
	ofono_gprs_filter_unregister(&misc);
	ofono_gprs_filter_unregister(&misc);
	ofono_gprs_filter_unregister(NULL);
}

/* ==== activate_allow ==== */

static void test_activate_allow_cb(const struct ofono_gprs_primary_context *ctx,
								void *data)
{
	struct ofono_gprs_context *gc = data;

	g_assert(ctx);
	g_assert(!memcmp(ctx, &gc->ctx, sizeof(*ctx)));
	g_main_loop_quit(test_loop);
}

static void test_activate_allow(void)
{
	static struct ofono_gprs_filter filter = {
		.name = "activate_allow",
		.api_version = OFONO_GPRS_FILTER_API_VERSION,
		.filter_activate = filter_activate_continue
	};

	int count = 0;
	struct ofono_gprs gprs;
	struct ofono_gprs_context gc;
	struct ofono_gprs_primary_context *ctx = &gc.ctx;

	test_common_init();
	test_gprs_init(&gprs, &gc);

	g_assert((gprs.chain = __ofono_gprs_filter_chain_new(&gprs)) != NULL);
	g_assert(ofono_gprs_filter_register(&filter) == 0);

	/* This one gets rejected because there's no callback */
	__ofono_gprs_filter_chain_activate(gprs.chain, &gc, ctx, NULL,
							test_inc, &count);
	g_assert(count == 1);
	count = 0;

	/* This one immediately gets completed because there's no context */
	__ofono_gprs_filter_chain_activate(gprs.chain, NULL, NULL,
			test_activate_expect_disallow, test_inc, &count);
	g_assert(count == 2);
	count = 0;

	/* test_activate_allow_cb will compare these */
	strcpy(ctx->username, "foo");
	strcpy(ctx->password, "bar");

	/* Completion callback will terminate the loop */
	__ofono_gprs_filter_chain_activate(gprs.chain, &gc, ctx,
					test_activate_allow_cb, NULL, &gc);
	g_main_loop_run(test_loop);

	/* Nothing to cancel */
	__ofono_gprs_filter_chain_cancel(gprs.chain, NULL);
	g_assert(!count);

	__ofono_gprs_filter_chain_free(gprs.chain);
	ofono_gprs_filter_unregister(&filter);
	test_common_deinit();
}

/* ==== activate_allow_async ==== */

static void test_activate_allow_async(void)
{
	static struct ofono_gprs_filter allow = {
		.name = "activate_allow",
		.api_version = OFONO_GPRS_FILTER_API_VERSION,
		.priority = OFONO_GPRS_FILTER_PRIORITY_DEFAULT,
		.filter_activate = filter_activate_continue_later,
		.cancel = filter_cancel
	};

	static struct ofono_gprs_filter dummy = {
		.name = "dummy",
		.api_version = OFONO_GPRS_FILTER_API_VERSION,
		.priority = OFONO_GPRS_FILTER_PRIORITY_LOW
	};

	int count = 0;
	struct ofono_gprs gprs;
	struct ofono_gprs_context gc;
	struct ofono_gprs_primary_context *ctx = &gc.ctx;

	test_common_init();
	test_gprs_init(&gprs, &gc);

	g_assert((gprs.chain = __ofono_gprs_filter_chain_new(&gprs)) != NULL);
	g_assert(ofono_gprs_filter_register(&allow) == 0);
	g_assert(ofono_gprs_filter_register(&dummy) == 0);

	/* Completion callback will terminate the loop */
	__ofono_gprs_filter_chain_activate(gprs.chain, &gc, ctx,
			test_activate_expect_allow_and_quit, test_inc, &count);
	g_main_loop_run(test_loop);
	g_assert(count == 2); /* test_activate_expect_allow_and_quit+test_inc */
	g_assert(test_filter_activate_count == 1);
	__ofono_gprs_filter_chain_free(gprs.chain);
	ofono_gprs_filter_unregister(&allow);
	ofono_gprs_filter_unregister(&dummy);
	test_common_deinit();
}

/* ==== activate_change ==== */

#define TEST_CHANGE_USERNAME "username"
#define TEST_CHANGE_PASSWORD "password"

static void test_activate_change_cb
		(const struct ofono_gprs_primary_context *ctx, void *data)
{
	g_assert(ctx);
	g_assert(!g_strcmp0(ctx->username, TEST_CHANGE_USERNAME));
	g_assert(!g_strcmp0(ctx->password, TEST_CHANGE_PASSWORD));
	(*(int*)data)++;
	g_main_loop_quit(test_loop);
}

static unsigned int test_activate_change_filter(struct ofono_gprs_context *gc,
			const struct ofono_gprs_primary_context *ctx,
			ofono_gprs_filter_activate_cb_t cb, void *user_data)
{
	struct ofono_gprs_primary_context updated = *ctx;

	g_assert(!memcmp(ctx, &gc->ctx, sizeof(*ctx)));

	strcpy(updated.username, TEST_CHANGE_USERNAME);
	strcpy(updated.password, TEST_CHANGE_PASSWORD);
	cb(&updated, user_data);
	return 0;
}

static void test_activate_change(void)
{
	static struct ofono_gprs_filter filter = {
		.name = "activate_change",
		.api_version = OFONO_GPRS_FILTER_API_VERSION,
		.filter_activate = test_activate_change_filter
	};

	int count = 0;
	struct ofono_gprs gprs;
	struct ofono_gprs_context gc;
	struct ofono_gprs_primary_context *ctx = &gc.ctx;

	test_common_init();
	test_gprs_init(&gprs, &gc);

	g_assert((gprs.chain = __ofono_gprs_filter_chain_new(&gprs)) != NULL);
	g_assert(ofono_gprs_filter_register(&filter) == 0);

	/* These will be changed by test_activate_change_filter */
	strcpy(ctx->username, "foo");
	strcpy(ctx->password, "bar");

	/* test_activate_change_cb will terminate the loop */
	__ofono_gprs_filter_chain_activate(gprs.chain, &gc, ctx,
					test_activate_change_cb, NULL, &count);
	g_main_loop_run(test_loop);
	g_assert(count == 1);

	__ofono_gprs_filter_chain_free(gprs.chain);
	ofono_gprs_filter_unregister(&filter);
	test_common_deinit();
}

/* ==== activate_disallow ==== */

static void test_activate_disallow(void)
{
	static struct ofono_gprs_filter filter = {
		.name = "activate_disallow",
		.api_version = OFONO_GPRS_FILTER_API_VERSION,
		.filter_activate = filter_activate_cancel
	};

	int count = 0;
	struct ofono_gprs gprs;
	struct ofono_gprs_context gc;
	struct ofono_gprs_primary_context *ctx = &gc.ctx;

	test_common_init();
	test_gprs_init(&gprs, &gc);

	g_assert((gprs.chain = __ofono_gprs_filter_chain_new(&gprs)) != NULL);
	/* If we have no drivers registered, everything is allowed: */
	__ofono_gprs_filter_chain_activate(gprs.chain, &gc, ctx,
				test_activate_expect_allow, NULL, NULL);
	g_assert(ofono_gprs_filter_register(&filter) == 0);
	/* Completion callback will terminate the loop */
	__ofono_gprs_filter_chain_activate(gprs.chain, &gc, ctx,
			test_activate_expect_disallow_and_quit, NULL, &count);
	g_main_loop_run(test_loop);
	g_assert(count == 1); /* test_activate_expect_disallow_and_quit */
	g_assert(test_filter_cancel_count == 1);
	__ofono_gprs_filter_chain_free(gprs.chain);
	ofono_gprs_filter_unregister(&filter);
	test_common_deinit();
}

/* ==== check_v0 ==== */

static void test_check_v0(void)
{
	static struct ofono_gprs_filter filter = {
		.name = "check_v0",
		.api_version = 0,
		/* filter_check_disallow never gets invoked because
		 * api_version is less than 1 */
		.filter_check = filter_check_disallow
	};

	int count = 0;
	struct ofono_gprs gprs;

	test_common_init();

	g_assert(ofono_gprs_filter_register(&filter) == 0);
	g_assert((gprs.chain = __ofono_gprs_filter_chain_new(&gprs)) != NULL);

	/* The request gets completed immediately */
	__ofono_gprs_filter_chain_check(gprs.chain, test_check_expect_allow,
							test_inc, &count);

	/* test_check_expect_allow + test_inc = 2 */
	g_assert(count == 2);
	g_assert(!test_filter_check_count);

	__ofono_gprs_filter_chain_free(gprs.chain);
	ofono_gprs_filter_unregister(&filter);
	test_common_deinit();
}

/* ==== check_default ==== */

static void test_check_default(void)
{
	static struct ofono_gprs_filter filter = {
		.name = "check_default",
		.api_version = OFONO_GPRS_FILTER_API_VERSION
	};

	int count = 0;
	struct ofono_gprs gprs;

	test_common_init();

	g_assert(ofono_gprs_filter_register(&filter) == 0);
	g_assert((gprs.chain = __ofono_gprs_filter_chain_new(&gprs)) != NULL);

	/* The request gets completed immediately, default = allow */
	__ofono_gprs_filter_chain_check(gprs.chain, test_check_expect_allow,
							test_inc, &count);

	/* test_check_expect_allow + test_inc = 2 */
	g_assert(count == 2);
	g_assert(!test_filter_check_count);

	__ofono_gprs_filter_chain_free(gprs.chain);
	ofono_gprs_filter_unregister(&filter);
	test_common_deinit();
}

/* ==== check_allow ==== */

static void test_check_allow(void)
{
	static struct ofono_gprs_filter filter = {
		.name = "check_allow",
		.api_version = OFONO_GPRS_FILTER_API_VERSION,
		.filter_check = filter_check_allow
	};

	int count = 0;
	struct ofono_gprs gprs;

	test_common_init();

	/* This one immediately gets completed because there's no chain */
	__ofono_gprs_filter_chain_check(NULL, test_check_expect_allow,
							test_inc, &count);
	g_assert(count == 2);
	count = 0;

	/* Create the chain */
	g_assert((gprs.chain = __ofono_gprs_filter_chain_new(&gprs)) != NULL);

	/* This one immediately gets completed because there are no filters */
	__ofono_gprs_filter_chain_check(gprs.chain, test_check_expect_allow,
							test_inc, &count);
	g_assert(count == 2);
	count = 0;

	/* Register the filter */
	g_assert(ofono_gprs_filter_register(&filter) == 0);

	/* This one gets rejected because there's no callback */
	__ofono_gprs_filter_chain_check(gprs.chain, NULL, test_inc, &count);
	g_assert(!test_filter_check_count);
	g_assert(count == 1);
	count = 0;

	/* Completion callback will terminate the loop */
	__ofono_gprs_filter_chain_check(gprs.chain,
			test_check_expect_allow_and_quit, test_inc, &count);
	g_main_loop_run(test_loop);

	/* test_check_expect_allow_and_quit + test_inc = 2 */
	g_assert(count == 2);
	g_assert(test_filter_check_count == 1);
	count = 0;

	/* Nothing to cancel */
	__ofono_gprs_filter_chain_cancel(gprs.chain, NULL);
	g_assert(!count);

	__ofono_gprs_filter_chain_free(gprs.chain);
	ofono_gprs_filter_unregister(&filter);
	test_common_deinit();
}

/* ==== check_disallow ==== */

static void test_check_disallow(void)
{
	static struct ofono_gprs_filter filter = {
		.name = "check_disallow",
		.api_version = OFONO_GPRS_FILTER_API_VERSION,
		.filter_check = filter_check_disallow_later
	};

	int count = 0;
	struct ofono_gprs gprs;

	test_common_init();

	g_assert((gprs.chain = __ofono_gprs_filter_chain_new(&gprs)) != NULL);
	g_assert(ofono_gprs_filter_register(&filter) == 0);

	/* Completion callback will terminate the loop */
	__ofono_gprs_filter_chain_check(gprs.chain,
			test_check_expect_disallow_and_quit, test_inc, &count);
	g_main_loop_run(test_loop);

	/* test_check_expect_disallow_and_quit + test_inc = 2 */
	g_assert(count == 2);
	g_assert(test_filter_check_count == 1);

	__ofono_gprs_filter_chain_free(gprs.chain);
	ofono_gprs_filter_unregister(&filter);
	test_common_deinit();
}

/* ==== cancel1 ==== */

static void test_cancel1(void)
{
	static struct ofono_gprs_filter filter = {
		.name = "disallow",
		.api_version = OFONO_GPRS_FILTER_API_VERSION,
		.priority = OFONO_GPRS_FILTER_PRIORITY_DEFAULT,
		.filter_activate = filter_activate_cancel_later,
		.cancel = filter_cancel
	};

	int count = 0;
	struct ofono_gprs gprs;
	struct ofono_gprs_context gc;
	struct ofono_gprs_primary_context *ctx = &gc.ctx;

	test_clear_counts();
	test_gprs_init(&gprs, &gc);

	g_assert((gprs.chain = __ofono_gprs_filter_chain_new(&gprs)) != NULL);
	g_assert(ofono_gprs_filter_register(&filter) == 0);

	/* This schedules asynchronous callback */
	__ofono_gprs_filter_chain_activate(gprs.chain, &gc, ctx,
				test_activate_expect_allow, test_inc, &count);

	/* And this cancels it */
	__ofono_gprs_filter_chain_free(gprs.chain);
	g_assert(test_filter_cancel_count == 1);
	g_assert(count == 1); /* test_inc */

	ofono_gprs_filter_unregister(&filter);
}

/* ==== cancel2 ==== */

static gboolean test_cancel2_free_chain(void* data)
{
	struct ofono_gprs *gprs = data;

	DBG("");
	__ofono_gprs_filter_chain_free(gprs->chain);
	gprs->chain = NULL;
	g_idle_add(test_quit_cb, NULL);
	return G_SOURCE_REMOVE;
}

static unsigned int test_cancel2_activate(struct ofono_gprs_context *gc,
			const struct ofono_gprs_primary_context *ctx,
			ofono_gprs_filter_activate_cb_t cb, void *user_data)
{
	DBG("");

	/* Request gets cancelled before completion. */
	g_idle_add(test_cancel2_free_chain, gc->gprs);
	cb(NULL, user_data);
	return 0;
}

static void test_cancel2(void)
{
	static struct ofono_gprs_filter filter = {
		.name = "cancel",
		.api_version = OFONO_GPRS_FILTER_API_VERSION,
		.priority = OFONO_GPRS_FILTER_PRIORITY_DEFAULT,
		.filter_activate = test_cancel2_activate,
		.cancel = filter_cancel
	};

	int count = 0;
	struct ofono_gprs gprs;
	struct ofono_gprs_context gc;
	struct ofono_gprs_primary_context *ctx = &gc.ctx;

	test_common_init();
	test_gprs_init(&gprs, &gc);

	g_assert((gprs.chain = __ofono_gprs_filter_chain_new(&gprs)) != NULL);
	g_assert(ofono_gprs_filter_register(&filter) == 0);

	/* This schedules asynchronous callback */
	__ofono_gprs_filter_chain_activate(gprs.chain, &gc, ctx,
				test_activate_expect_allow, test_inc, &count);
	g_main_loop_run(test_loop);

	/* Chain is destroyed by test_cancel2_free_chain */
	g_assert(!gprs.chain);
	g_assert(!test_filter_cancel_count);
	g_assert(count == 1); /* test_inc */

	ofono_gprs_filter_unregister(&filter);
	test_common_deinit();
}

/* ==== cancel3 ==== */

static gboolean test_cancel3_cb(void* data)
{
	struct ofono_gprs *gprs = data;

	DBG("");
	__ofono_gprs_filter_chain_cancel(gprs->chain, NULL);
	g_idle_add(test_quit_cb, NULL);
	return G_SOURCE_REMOVE;
}

static unsigned int test_cancel3_activate(struct ofono_gprs_context *gc,
			const struct ofono_gprs_primary_context *ctx,
			ofono_gprs_filter_activate_cb_t cb, void *user_data)
{
	DBG("");

	/* Request gets cancelled before completion. */
	g_idle_add(test_cancel3_cb, gc->gprs);
	cb(NULL, user_data);
	return 0;
}

static void test_cancel3(void)
{
	static struct ofono_gprs_filter filter = {
		.name = "cancel",
		.api_version = OFONO_GPRS_FILTER_API_VERSION,
		.priority = OFONO_GPRS_FILTER_PRIORITY_DEFAULT,
		.filter_activate = test_cancel3_activate,
		.cancel = filter_cancel
	};

	int count = 0;
	struct ofono_gprs gprs;
	struct ofono_gprs_context gc;
	struct ofono_gprs_primary_context *ctx = &gc.ctx;

	test_common_init();
	test_gprs_init(&gprs, &gc);

	g_assert((gprs.chain = __ofono_gprs_filter_chain_new(&gprs)) != NULL);
	g_assert(ofono_gprs_filter_register(&filter) == 0);

	/* This schedules asynchronous callback */
	__ofono_gprs_filter_chain_activate(gprs.chain, &gc, ctx,
				test_activate_expect_allow, test_inc, &count);
	g_main_loop_run(test_loop);

	g_assert(!test_filter_cancel_count);
	g_assert(count == 1); /* test_inc */

	ofono_gprs_filter_unregister(&filter);
	__ofono_gprs_filter_chain_free(gprs.chain);
	test_common_deinit();
}

/* ==== cancel4 ==== */

static unsigned int test_cancel4_activate(struct ofono_gprs_context *gc,
			const struct ofono_gprs_primary_context *ctx,
			ofono_gprs_filter_activate_cb_t cb, void *user_data)
{
	struct ofono_gprs *gprs = gc->gprs;
	struct ofono_gprs_context gc2;

	DBG("");
	test_gc_init(gprs, &gc2);

	/* There's no request for this gc, nothing gets canceled */
	__ofono_gprs_filter_chain_cancel(gprs->chain, &gc2);
	cb(ctx, user_data);
	return 0;
}

static void test_cancel4(void)
{
	static struct ofono_gprs_filter filter = {
		.name = "cancel4",
		.api_version = OFONO_GPRS_FILTER_API_VERSION,
		.priority = OFONO_GPRS_FILTER_PRIORITY_DEFAULT,
		.filter_activate = test_cancel4_activate,
		.cancel = filter_cancel
	};

	int count = 0;
	struct ofono_gprs gprs;
	struct ofono_gprs_context gc;
	struct ofono_gprs_primary_context *ctx = &gc.ctx;

	test_common_init();
	test_gprs_init(&gprs, &gc);

	g_assert((gprs.chain = __ofono_gprs_filter_chain_new(&gprs)) != NULL);
	g_assert(ofono_gprs_filter_register(&filter) == 0);

	/* This schedules asynchronous callback */
	__ofono_gprs_filter_chain_activate(gprs.chain, &gc, ctx,
			test_activate_expect_allow_and_quit, test_inc, &count);
	g_main_loop_run(test_loop);

	g_assert(!test_filter_cancel_count);
	g_assert(count == 2); /* test_activate_expect_allow_and_quit+test_inc */

	__ofono_gprs_filter_chain_free(gprs.chain);
	ofono_gprs_filter_unregister(&filter);
	test_common_deinit();
}

/* ==== cancel5 ==== */

static gboolean test_cancel5_cb(void* data)
{
	struct ofono_gprs_context *gc = data;
	struct ofono_gprs *gprs = gc->gprs;

	DBG("");
	__ofono_gprs_filter_chain_cancel(gprs->chain, gc);
	g_idle_add(test_quit_cb, NULL);
	return G_SOURCE_REMOVE;
}

static unsigned int test_cancel5_activate(struct ofono_gprs_context *gc,
			const struct ofono_gprs_primary_context *ctx,
			ofono_gprs_filter_activate_cb_t cb, void *user_data)
{
	DBG("");

	/* Request gets cancelled before completion. */
	g_idle_add(test_cancel5_cb, gc);
	cb(NULL, user_data);
	return 0;
}

static void test_cancel5(void)
{
	static struct ofono_gprs_filter filter = {
		.name = "cancel",
		.api_version = OFONO_GPRS_FILTER_API_VERSION,
		.priority = OFONO_GPRS_FILTER_PRIORITY_DEFAULT,
		.filter_activate = test_cancel5_activate,
		.cancel = filter_cancel
	};

	int count = 0;
	struct ofono_gprs gprs;
	struct ofono_gprs_context gc;
	struct ofono_gprs_primary_context *ctx = &gc.ctx;

	test_common_init();
	test_gprs_init(&gprs, &gc);

	g_assert((gprs.chain = __ofono_gprs_filter_chain_new(&gprs)) != NULL);
	g_assert(ofono_gprs_filter_register(&filter) == 0);

	/* This schedules asynchronous callback */
	__ofono_gprs_filter_chain_activate(gprs.chain, &gc, ctx,
				test_activate_expect_allow, test_inc, &count);
	g_main_loop_run(test_loop);

	g_assert(!test_filter_cancel_count);
	g_assert(count == 1); /* test_inc */

	ofono_gprs_filter_unregister(&filter);
	__ofono_gprs_filter_chain_free(gprs.chain);
	test_common_deinit();
}

/* ==== cancel6 ==== */

static void test_cancel6(void)
{
	static struct ofono_gprs_filter filter = {
		.name = "cancel",
		.api_version = OFONO_GPRS_FILTER_API_VERSION,
		.priority = OFONO_GPRS_FILTER_PRIORITY_DEFAULT,
		.filter_activate = filter_activate_continue_later,
		.cancel = filter_cancel
	};

	int count = 0, count2 = 0;
	struct ofono_gprs gprs;
	struct ofono_gprs_context gc, gc2;
	struct ofono_gprs_primary_context *ctx = &gc.ctx;

	test_common_init();
	test_gprs_init(&gprs, &gc);
	test_gc_init(&gprs, &gc2);

	g_assert((gprs.chain = __ofono_gprs_filter_chain_new(&gprs)) != NULL);
	g_assert(ofono_gprs_filter_register(&filter) == 0);

	/* Submit 2 requests */
	__ofono_gprs_filter_chain_activate(gprs.chain, &gc, ctx,
			test_activate_expect_allow_and_quit, test_inc, &count);
	__ofono_gprs_filter_chain_activate(gprs.chain, &gc2, ctx,
			test_activate_expect_allow, test_inc, &count2);

	/* And cancel the second one */
	__ofono_gprs_filter_chain_cancel(gprs.chain, &gc2);

	g_main_loop_run(test_loop);

	g_assert(test_filter_activate_count == 2);
	g_assert(!test_filter_cancel_count);
	g_assert(count == 2); /* test_activate_expect_allow_and_quit+test_inc */
	g_assert(count2 == 1); /* test_inc */

	ofono_gprs_filter_unregister(&filter);
	__ofono_gprs_filter_chain_free(gprs.chain);
	test_common_deinit();
}

/* ==== priorities1 ==== */

static void test_priorities1(void)
{
	static struct ofono_gprs_filter priority_low = {
		.name = "priority_low",
		.api_version = OFONO_GPRS_FILTER_API_VERSION,
		.priority = OFONO_GPRS_FILTER_PRIORITY_LOW,
		.filter_activate = filter_activate_continue_later,
		.cancel = filter_cancel
	};

	static struct ofono_gprs_filter priority_default = {
		.name = "priority_low",
		.api_version = OFONO_GPRS_FILTER_API_VERSION,
		.priority = OFONO_GPRS_FILTER_PRIORITY_DEFAULT,
		.filter_activate = filter_activate_cancel_later,
		.cancel = filter_cancel
	};

	static struct ofono_gprs_filter dummy = {
		.name = "dummy",
		.api_version = OFONO_GPRS_FILTER_API_VERSION,
		.priority = OFONO_GPRS_FILTER_PRIORITY_HIGH
	};

	int count = 0;
	struct ofono_gprs gprs;
	struct ofono_gprs_context gc;
	struct ofono_gprs_primary_context *ctx = &gc.ctx;

	test_common_init();
	test_gprs_init(&gprs, &gc);

	/* priority_default filter will be invoked first */
	g_assert(ofono_gprs_filter_register(&priority_low) == 0);
	g_assert(ofono_gprs_filter_register(&priority_default) == 0);
	g_assert(ofono_gprs_filter_register(&dummy) == 0);
	g_assert((gprs.chain = __ofono_gprs_filter_chain_new(&gprs)) != NULL);

	/* Completion callback will terminate the loop */
	__ofono_gprs_filter_chain_activate(gprs.chain, &gc, ctx,
		test_activate_expect_disallow_and_quit, test_inc, &count);
	g_main_loop_run(test_loop);
	g_assert(count == 2); /* test_activate_expect_disallow_and_quit
			       * and test_inc */
	g_assert(test_filter_cancel_count == 1);
	g_assert(test_filter_activate_count == 0);
	__ofono_gprs_filter_chain_free(gprs.chain);
	ofono_gprs_filter_unregister(&priority_low);
	ofono_gprs_filter_unregister(&priority_default);
	ofono_gprs_filter_unregister(&dummy);
	test_common_deinit();
}

/* ==== priorities2 ==== */

static void test_priorities2(void)
{
	static struct ofono_gprs_filter priority_default = {
		.name = "priority_default",
		.api_version = OFONO_GPRS_FILTER_API_VERSION,
		.priority = OFONO_GPRS_FILTER_PRIORITY_DEFAULT,
		.filter_activate = filter_activate_cancel_later,
		.cancel = filter_cancel
	};

	static struct ofono_gprs_filter priority_high = {
		.name = "priority_high",
		.api_version = OFONO_GPRS_FILTER_API_VERSION,
		.priority = OFONO_GPRS_FILTER_PRIORITY_HIGH,
		.filter_activate = filter_activate_continue_later,
		.cancel = filter_cancel
	};

	int count = 0;
	struct ofono_gprs gprs;
	struct ofono_gprs_context gc;
	struct ofono_gprs_primary_context *ctx = &gc.ctx;

	test_common_init();
	test_gprs_init(&gprs, &gc);

	/* priority_default filter will be invoked last */
	g_assert(ofono_gprs_filter_register(&priority_high) == 0);
	g_assert(ofono_gprs_filter_register(&priority_default) == 0);
	g_assert((gprs.chain = __ofono_gprs_filter_chain_new(&gprs)) != NULL);

	/* Completion callback will terminate the loop */
	__ofono_gprs_filter_chain_activate(gprs.chain, &gc, ctx,
		test_activate_expect_disallow_and_quit, test_inc, &count);

	g_main_loop_run(test_loop);
	g_assert(count == 2); /* test_activate_expect_disallow_and_quit
			       * and test_inc */
	g_assert(test_filter_cancel_count == 1);
	g_assert(test_filter_activate_count == 1);
	__ofono_gprs_filter_chain_free(gprs.chain);
	ofono_gprs_filter_unregister(&priority_default);
	ofono_gprs_filter_unregister(&priority_high);
	test_common_deinit();
}

#define TEST_(name) "/gprs-filter/" name

int main(int argc, char *argv[])
{
	g_test_init(&argc, &argv, NULL);

	gutil_log_timestamp = FALSE;
	gutil_log_default.level = g_test_verbose() ?
		GLOG_LEVEL_VERBOSE : GLOG_LEVEL_NONE;
	__ofono_log_init("test-gprs-filter",
		g_test_verbose() ? "*" : NULL,
		FALSE, FALSE);

	if (argc > 1 && !strcmp(argv[1] , "-d")) {
		test_debug = TRUE;
		DBG("Debugging on (no timeout)");
	}

	g_test_add_func(TEST_("misc"), test_misc);
	g_test_add_func(TEST_("activate_allow"), test_activate_allow);
	g_test_add_func(TEST_("activate_allow_async"),
						test_activate_allow_async);
	g_test_add_func(TEST_("activate_change"), test_activate_change);
	g_test_add_func(TEST_("activate_disallow"), test_activate_disallow);
	g_test_add_func(TEST_("check_v0"), test_check_v0);
	g_test_add_func(TEST_("check_default"), test_check_default);
	g_test_add_func(TEST_("check_allow"), test_check_allow);
	g_test_add_func(TEST_("check_disallow"), test_check_disallow);
	g_test_add_func(TEST_("cancel1"), test_cancel1);
	g_test_add_func(TEST_("cancel2"), test_cancel2);
	g_test_add_func(TEST_("cancel3"), test_cancel3);
	g_test_add_func(TEST_("cancel4"), test_cancel4);
	g_test_add_func(TEST_("cancel5"), test_cancel5);
	g_test_add_func(TEST_("cancel6"), test_cancel6);
	g_test_add_func(TEST_("priorities1"), test_priorities1);
	g_test_add_func(TEST_("priorities2"), test_priorities2);

	return g_test_run();
}

/*
 * Local Variables:
 * mode: C
 * c-basic-offset: 8
 * indent-tabs-mode: t
 * End:
 */
