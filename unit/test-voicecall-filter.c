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
#include "common.h"

#include <gutil_macros.h>
#include <gutil_log.h>

#include <errno.h>

#define TEST_TIMEOUT_SEC (20)

static gboolean test_debug = FALSE;
static GMainLoop *test_loop = NULL;
static int test_filter_dial_count = 0;
static int test_filter_incoming_count = 0;

/* Fake data structures */

struct ofono_voicecall {
	struct voicecall_filter_chain *chain;
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

static gboolean test_cancel_cb(void* data)
{
	struct voicecall_filter_chain *chain = data;

	DBG("");
	__ofono_voicecall_filter_chain_cancel(chain, NULL);
	g_idle_add(test_quit_cb, NULL);
	return G_SOURCE_REMOVE;
}

static void test_inc(gpointer data)
{
	(*(int*)data)++;
}

static void test_dial_expect_continue_inc
		(enum ofono_voicecall_filter_dial_result result, void *data)
{
	g_assert(result == OFONO_VOICECALL_FILTER_DIAL_CONTINUE);
	if (data) (*(int*)data)++;
}

static void test_dial_expect_continue_and_quit
		(enum ofono_voicecall_filter_dial_result result, void *data)
{
	g_assert(result == OFONO_VOICECALL_FILTER_DIAL_CONTINUE);
	g_main_loop_quit(test_loop);
}

static void test_dial_expect_block_and_quit
		(enum ofono_voicecall_filter_dial_result result, void *data)
{
	g_assert(result == OFONO_VOICECALL_FILTER_DIAL_BLOCK);
	g_main_loop_quit(test_loop);
}

static void test_dial_unexpected
		(enum ofono_voicecall_filter_dial_result result, void *data)
{
	g_assert(FALSE);
}

static void test_incoming_expect_continue_inc
	(enum ofono_voicecall_filter_incoming_result result, void *data)
{
	g_assert(result == OFONO_VOICECALL_FILTER_INCOMING_CONTINUE);
	if (data) (*(int*)data)++;
}

static void test_incoming_expect_continue_and_quit
	(enum ofono_voicecall_filter_incoming_result result, void *data)
{
	g_assert(result == OFONO_VOICECALL_FILTER_INCOMING_CONTINUE);
	g_main_loop_quit(test_loop);
}

static void test_incoming_expect_hangup_and_quit
	(enum ofono_voicecall_filter_incoming_result result, void *data)
{
	g_assert(result == OFONO_VOICECALL_FILTER_INCOMING_HANGUP);
	g_main_loop_quit(test_loop);
}

static void test_incoming_expect_ignore_and_quit
	(enum ofono_voicecall_filter_incoming_result result, void *data)
{
	g_assert(result == OFONO_VOICECALL_FILTER_INCOMING_IGNORE);
	g_main_loop_quit(test_loop);
}

static void test_incoming_unexpected
	(enum ofono_voicecall_filter_incoming_result result, void *data)
{
	g_assert(FALSE);
}

static void test_clear_counts()
{
	test_filter_dial_count = 0;
	test_filter_incoming_count = 0;
}

static void test_common_init()
{
	test_clear_counts();
	test_loop = g_main_loop_new(NULL, FALSE);
	if (!test_debug) {
		g_timeout_add_seconds(TEST_TIMEOUT_SEC, test_timeout_cb, NULL);
	}
}

static void test_voicecall_init(struct ofono_voicecall *vc)
{
	memset(vc, 0, sizeof(*vc));
}

static void test_common_deinit()
{
	g_main_loop_unref(test_loop);
	test_loop = NULL;
}

struct filter_dial_later_data {
	ofono_voicecall_filter_dial_cb_t cb;
	enum ofono_voicecall_filter_dial_result result;
	void *user_data;
};

static gboolean filter_dial_later_cb(gpointer user_data)
{
	struct filter_dial_later_data* later = user_data;

	test_filter_dial_count++;
	later->cb(later->result, later->user_data);
	return G_SOURCE_REMOVE;
}

static unsigned int filter_dial_later(ofono_voicecall_filter_dial_cb_t cb,
	enum ofono_voicecall_filter_dial_result result, void *user_data)
{
	struct filter_dial_later_data* later =
		g_new0(struct filter_dial_later_data, 1);

	later->cb = cb;
	later->result = result;
	later->user_data = user_data;

	return g_idle_add_full(G_PRIORITY_DEFAULT_IDLE, filter_dial_later_cb,
							later, g_free);
}

static unsigned int filter_dial_continue(struct ofono_voicecall *vc,
	const struct ofono_phone_number *number, enum ofono_clir_option clir,
	ofono_voicecall_filter_dial_cb_t cb, void *user_data)
{
	test_filter_dial_count++;
	cb(OFONO_VOICECALL_FILTER_DIAL_CONTINUE, user_data);
	return 0;
}

static unsigned int filter_dial_continue_later(struct ofono_voicecall *vc,
	const struct ofono_phone_number *number, enum ofono_clir_option clir,
	ofono_voicecall_filter_dial_cb_t cb, void *user_data)
{
	return filter_dial_later(cb, OFONO_VOICECALL_FILTER_DIAL_CONTINUE,
								user_data);
}

static unsigned int filter_dial_block(struct ofono_voicecall *vc,
	const struct ofono_phone_number *number, enum ofono_clir_option clir,
	ofono_voicecall_filter_dial_cb_t cb, void *user_data)
{
	test_filter_dial_count++;
	cb(OFONO_VOICECALL_FILTER_DIAL_BLOCK, user_data);
	return 0;
}

static unsigned int filter_dial_block_later(struct ofono_voicecall *vc,
	const struct ofono_phone_number *number, enum ofono_clir_option clir,
	ofono_voicecall_filter_dial_cb_t cb, void *user_data)
{
	return filter_dial_later(cb, OFONO_VOICECALL_FILTER_DIAL_BLOCK,
								user_data);
}

struct filter_incoming_later_data {
	ofono_voicecall_filter_incoming_cb_t cb;
	enum ofono_voicecall_filter_incoming_result result;
	void *user_data;
};

static gboolean filter_incoming_later_cb(gpointer user_data)
{
	struct filter_incoming_later_data* later = user_data;

	test_filter_incoming_count++;
	later->cb(later->result, later->user_data);
	return G_SOURCE_REMOVE;
}

static unsigned int filter_incoming_later
		(ofono_voicecall_filter_incoming_cb_t cb,
			enum ofono_voicecall_filter_incoming_result result,
			void *user_data)
{
	struct filter_incoming_later_data* later =
		g_new0(struct filter_incoming_later_data, 1);

	later->cb = cb;
	later->result = result;
	later->user_data = user_data;

	return g_idle_add_full(G_PRIORITY_DEFAULT_IDLE,
			filter_incoming_later_cb, later, g_free);
}

static unsigned int filter_incoming_continue(struct ofono_voicecall *vc,
	const struct ofono_call *call, ofono_voicecall_filter_incoming_cb_t cb,
	void *user_data)
{
	test_filter_incoming_count++;
	cb(OFONO_VOICECALL_FILTER_INCOMING_CONTINUE, user_data);
	return 0;
}

static unsigned int filter_incoming_continue_later(struct ofono_voicecall *vc,
	const struct ofono_call *call, ofono_voicecall_filter_incoming_cb_t cb,
	void *user_data)
{
	return filter_incoming_later(cb,
			OFONO_VOICECALL_FILTER_INCOMING_CONTINUE, user_data);
}

static unsigned int filter_incoming_hangup(struct ofono_voicecall *vc,
	const struct ofono_call *call, ofono_voicecall_filter_incoming_cb_t cb,
	void *user_data)
{
	test_filter_incoming_count++;
	cb(OFONO_VOICECALL_FILTER_INCOMING_HANGUP, user_data);
	return 0;
}

static unsigned int filter_incoming_hangup_later(struct ofono_voicecall *vc,
	const struct ofono_call *call, ofono_voicecall_filter_incoming_cb_t cb,
	void *user_data)
{
	return filter_incoming_later(cb,
			OFONO_VOICECALL_FILTER_INCOMING_HANGUP, user_data);
}

static unsigned int filter_incoming_ignore(struct ofono_voicecall *vc,
	const struct ofono_call *call, ofono_voicecall_filter_incoming_cb_t cb,
	void *user_data)
{
	test_filter_incoming_count++;
	cb(OFONO_VOICECALL_FILTER_INCOMING_IGNORE, user_data);
	return 0;
}

static unsigned int filter_incoming_ignore_later(struct ofono_voicecall *vc,
	const struct ofono_call *call, ofono_voicecall_filter_incoming_cb_t cb,
	void *user_data)
{
	return filter_incoming_later(cb,
			OFONO_VOICECALL_FILTER_INCOMING_IGNORE, user_data);
}

static void filter_cancel(unsigned int id)
{
	DBG("%u", id);
	g_source_remove(id);
}

/* ==== misc ==== */

static void test_misc(void)
{
	static struct ofono_voicecall_filter noname = {
		.api_version = OFONO_VOICECALL_FILTER_API_VERSION
	};

	static struct ofono_voicecall_filter misc = {
		.name = "misc",
		.api_version = OFONO_VOICECALL_FILTER_API_VERSION,
	};

	int count = 0;

	g_assert(ofono_voicecall_filter_register(NULL) == -EINVAL);
	g_assert(ofono_voicecall_filter_register(&noname) == -EINVAL);
	g_assert(ofono_voicecall_filter_register(&misc) == 0);
	g_assert(ofono_voicecall_filter_register(&misc) == 0);

	g_assert(!__ofono_voicecall_filter_chain_new(NULL));
	__ofono_voicecall_filter_chain_cancel(NULL, NULL);
	__ofono_voicecall_filter_chain_free(NULL);

	__ofono_voicecall_filter_chain_dial(NULL, NULL,
			OFONO_CLIR_OPTION_DEFAULT, NULL, test_inc, &count);
	g_assert(count == 1);
	count = 0;

	__ofono_voicecall_filter_chain_dial(NULL, NULL,
			OFONO_CLIR_OPTION_DEFAULT,
			test_dial_expect_continue_inc, NULL, &count);
	g_assert(count == 1);
	count = 0;

	__ofono_voicecall_filter_chain_dial(NULL, NULL,
			OFONO_CLIR_OPTION_DEFAULT,
			test_dial_expect_continue_inc, test_inc, &count);
	g_assert(count == 2);
	count = 0;

	__ofono_voicecall_filter_chain_dial_check(NULL, NULL, NULL,
			test_inc, &count);
	g_assert(count == 1);
	count = 0;

	__ofono_voicecall_filter_chain_dial_check(NULL, NULL,
			test_dial_expect_continue_inc, NULL, &count);
	g_assert(count == 1);
	count = 0;

	__ofono_voicecall_filter_chain_dial_check(NULL, NULL,
			test_dial_expect_continue_inc, test_inc, &count);
	g_assert(count == 2);
	count = 0;

	__ofono_voicecall_filter_chain_incoming(NULL, NULL,
			test_incoming_expect_continue_inc,
			test_inc, &count);
	g_assert(count == 2);
	count = 0;

	ofono_voicecall_filter_unregister(&misc);
	ofono_voicecall_filter_unregister(&misc);
	ofono_voicecall_filter_unregister(&misc);
	ofono_voicecall_filter_unregister(NULL);
}

/* ==== dial_allow ==== */

static void test_dial_allow(void)
{
	static struct ofono_voicecall_filter filter1 = {
		.name = "dial_allow",
		.api_version = OFONO_VOICECALL_FILTER_API_VERSION,
		.priority = OFONO_VOICECALL_FILTER_PRIORITY_DEFAULT,
		.filter_dial = filter_dial_continue
	};

	static struct ofono_voicecall_filter filter2 = {
		.name = "dummy",
		.api_version = OFONO_VOICECALL_FILTER_API_VERSION,
		.priority = OFONO_VOICECALL_FILTER_PRIORITY_LOW
		/* Implicitely allows everything */
	};

	struct ofono_voicecall vc;
	struct ofono_phone_number number;
	struct ofono_call call;
	int count = 0;

	test_common_init();
	test_voicecall_init(&vc);
	string_to_phone_number("112", &number);
	memset(&call, 0, sizeof(call));

	g_assert((vc.chain = __ofono_voicecall_filter_chain_new(&vc)) != NULL);

	/* This one gets ok'ed immediately because there're no filters */
	__ofono_voicecall_filter_chain_dial(vc.chain, &number,
			OFONO_CLIR_OPTION_DEFAULT,
			test_dial_expect_continue_inc,
			test_inc, &count);
	g_assert(count == 2);
	count = 0;

	/* Register the filters */
	g_assert(ofono_voicecall_filter_register(&filter1) == 0);
	g_assert(ofono_voicecall_filter_register(&filter2) == 0);

	/* This one gets ok'ed immediately because there's no number */
	__ofono_voicecall_filter_chain_dial(vc.chain, NULL,
			OFONO_CLIR_OPTION_DEFAULT,
			test_dial_expect_continue_inc,
			test_inc, &count);
	g_assert(count == 2);
	count = 0;

	/* This one does nothing because there's no callback */
	__ofono_voicecall_filter_chain_dial(vc.chain, &number,
			OFONO_CLIR_OPTION_DEFAULT, NULL, test_inc, &count);
	g_assert(count == 1);
	count = 0;

	/* Completion callback will terminate the loop */
	__ofono_voicecall_filter_chain_dial(vc.chain, &number,
			OFONO_CLIR_OPTION_DEFAULT,
			test_dial_expect_continue_and_quit,
			test_inc, &count);

	g_main_loop_run(test_loop);
	g_assert(test_filter_dial_count == 1);

	/* Count is incremented by the request destructor */
	g_assert(count == 1);
	count = 0;

	/* Non-existent call */
	__ofono_voicecall_filter_chain_cancel(vc.chain, &call);

	__ofono_voicecall_filter_chain_free(vc.chain);
	ofono_voicecall_filter_unregister(&filter1);
	ofono_voicecall_filter_unregister(&filter2);
	test_common_deinit();
}

/* ==== dial_allow_async ==== */

static void test_dial_allow_async(void)
{
	static struct ofono_voicecall_filter filter1 = {
		.name = "dial_allow_async",
		.api_version = OFONO_VOICECALL_FILTER_API_VERSION,
		.priority = OFONO_VOICECALL_FILTER_PRIORITY_LOW,
		.filter_dial = filter_dial_continue_later,
		.filter_cancel = filter_cancel
	};

	static struct ofono_voicecall_filter filter2 = {
		.name = "dummy",
		.api_version = OFONO_VOICECALL_FILTER_API_VERSION,
		.priority = OFONO_VOICECALL_FILTER_PRIORITY_DEFAULT
		/* Implicitely allows everything */
	};

	struct ofono_voicecall vc;
	struct ofono_phone_number number;
	int count = 0;

	test_common_init();
	test_voicecall_init(&vc);
	string_to_phone_number("+1234", &number);

	g_assert(ofono_voicecall_filter_register(&filter1) == 0);
	g_assert(ofono_voicecall_filter_register(&filter2) == 0);
	g_assert((vc.chain = __ofono_voicecall_filter_chain_new(&vc)) != NULL);

	/* Completion callback will terminate the loop */
	__ofono_voicecall_filter_chain_dial(vc.chain, &number,
			OFONO_CLIR_OPTION_DEFAULT,
			test_dial_expect_continue_and_quit,
			test_inc, &count);

	g_main_loop_run(test_loop);
	g_assert(test_filter_dial_count == 1);

	/* Count is incremented by the request destructor */
	g_assert(count == 1);
	count = 0;

	__ofono_voicecall_filter_chain_free(vc.chain);
	ofono_voicecall_filter_unregister(&filter1);
	ofono_voicecall_filter_unregister(&filter2);
	test_common_deinit();
}

/* ==== dial_block ==== */

static void test_dial_block(void)
{
	static struct ofono_voicecall_filter filter1 = {
		.name = "dial_block",
		.api_version = OFONO_VOICECALL_FILTER_API_VERSION,
		.priority = OFONO_VOICECALL_FILTER_PRIORITY_DEFAULT,
		.filter_dial = filter_dial_block
	};

	static struct ofono_voicecall_filter filter2 = {
		.name = "dummy",
		.api_version = OFONO_VOICECALL_FILTER_API_VERSION,
		.priority = OFONO_VOICECALL_FILTER_PRIORITY_LOW
		/* Implicitely allows everything */
	};

	struct ofono_voicecall vc;
	struct ofono_phone_number number;
	int count = 0;

	test_common_init();
	test_voicecall_init(&vc);
	string_to_phone_number("112", &number);

	g_assert((vc.chain = __ofono_voicecall_filter_chain_new(&vc)) != NULL);
	g_assert(ofono_voicecall_filter_register(&filter1) == 0);
	g_assert(ofono_voicecall_filter_register(&filter2) == 0);

	/* This one gets ok'ed immediately because there's no number */
	__ofono_voicecall_filter_chain_dial(vc.chain, NULL,
			OFONO_CLIR_OPTION_DEFAULT,
			test_dial_expect_continue_inc,
			test_inc, &count);
	g_assert(count == 2);
	count = 0;

	/* This one does nothing because there's no callback */
	__ofono_voicecall_filter_chain_dial(vc.chain, &number,
			OFONO_CLIR_OPTION_DEFAULT, NULL, test_inc, &count);
	g_assert(count == 1);
	count = 0;

	/* Completion callback will terminate the loop */
	__ofono_voicecall_filter_chain_dial(vc.chain, &number,
			OFONO_CLIR_OPTION_DEFAULT,
			test_dial_expect_block_and_quit,
			test_inc, &count);

	g_main_loop_run(test_loop);
	g_assert(test_filter_dial_count == 1);

	/* Count is incremented by the request destructor */
	g_assert(count == 1);
	count = 0;

	__ofono_voicecall_filter_chain_free(vc.chain);
	ofono_voicecall_filter_unregister(&filter1);
	ofono_voicecall_filter_unregister(&filter2);
	test_common_deinit();
}

/* ==== dial_block_async ==== */

static void test_dial_block_async(void)
{
	static struct ofono_voicecall_filter filter1 = {
		.name = "dial_block_async",
		.api_version = OFONO_VOICECALL_FILTER_API_VERSION,
		.priority = OFONO_VOICECALL_FILTER_PRIORITY_LOW,
		.filter_dial = filter_dial_block_later,
		.filter_cancel = filter_cancel
	};

	static struct ofono_voicecall_filter filter2 = {
		.name = "dummy",
		.api_version = OFONO_VOICECALL_FILTER_API_VERSION,
		.priority = OFONO_VOICECALL_FILTER_PRIORITY_DEFAULT
		/* Implicitely allows everything */
	};

	struct ofono_voicecall vc;
	struct ofono_phone_number number;
	int count = 0;

	test_common_init();
	test_voicecall_init(&vc);
	string_to_phone_number("+1234", &number);

	g_assert(ofono_voicecall_filter_register(&filter1) == 0);
	g_assert(ofono_voicecall_filter_register(&filter2) == 0);
	g_assert((vc.chain = __ofono_voicecall_filter_chain_new(&vc)) != NULL);

	/* Completion callback will terminate the loop */
	__ofono_voicecall_filter_chain_dial(vc.chain, &number,
			OFONO_CLIR_OPTION_DEFAULT,
			test_dial_expect_block_and_quit,
			test_inc, &count);

	g_main_loop_run(test_loop);
	g_assert(test_filter_dial_count == 1);

	/* Count is incremented by the request destructor */
	g_assert(count == 1);
	count = 0;

	__ofono_voicecall_filter_chain_free(vc.chain);
	ofono_voicecall_filter_unregister(&filter1);
	ofono_voicecall_filter_unregister(&filter2);
	test_common_deinit();
}

/* ==== dial_check ==== */

static void test_dial_check(void)
{
	static struct ofono_voicecall_filter filter = {
		.name = "dial_check",
		.api_version = OFONO_VOICECALL_FILTER_API_VERSION,
		.priority = OFONO_VOICECALL_FILTER_PRIORITY_DEFAULT,
		.filter_dial = filter_dial_continue
	};

	struct ofono_voicecall vc;
	struct ofono_phone_number number;
	struct ofono_call call;
	int count = 0;

	test_common_init();
	test_voicecall_init(&vc);
	string_to_phone_number("112", &number);
	memset(&call, 0, sizeof(call));

	g_assert((vc.chain = __ofono_voicecall_filter_chain_new(&vc)) != NULL);

	/* This one gets ok'ed immediately because there're no filters */
	__ofono_voicecall_filter_chain_dial_check(vc.chain, &call,
			test_dial_expect_continue_inc,
			test_inc, &count);
	g_assert(count == 2);
	count = 0;

	/* Register the filter */
	g_assert(ofono_voicecall_filter_register(&filter) == 0);

	/* This one gets ok'ed immediately because there's no call (hmmm?) */
	__ofono_voicecall_filter_chain_dial_check(vc.chain, NULL,
			test_dial_expect_continue_inc,
			test_inc, &count);
	g_assert(count == 2);
	count = 0;

	/* This one does nothing because there's no callback */
	__ofono_voicecall_filter_chain_dial_check(vc.chain, &call,
			NULL, test_inc, &count);
	g_assert(count == 1);
	count = 0;

	/* Completion callback will terminate the loop */
	__ofono_voicecall_filter_chain_dial_check(vc.chain, &call,
			test_dial_expect_continue_and_quit,
			test_inc, &count);

	g_main_loop_run(test_loop);
	g_assert(test_filter_dial_count == 1);

	/* Count is incremented by the request destructor */
	g_assert(count == 1);
	count = 0;

	__ofono_voicecall_filter_chain_free(vc.chain);
	ofono_voicecall_filter_unregister(&filter);
	test_common_deinit();
}

/* ==== incoming_allow ==== */

static void test_incoming_allow(void)
{
	static struct ofono_voicecall_filter filter = {
		.name = "incoming_allow",
		.api_version = OFONO_VOICECALL_FILTER_API_VERSION,
		.filter_incoming = filter_incoming_continue
	};

	struct ofono_voicecall vc;
	struct ofono_call call;
	int count = 0;

	test_common_init();
	test_voicecall_init(&vc);
	ofono_call_init(&call);
	string_to_phone_number("911", &call.phone_number);

	g_assert((vc.chain = __ofono_voicecall_filter_chain_new(&vc)) != NULL);

	/* This one gets ok'ed immediately because there're no filters */
	__ofono_voicecall_filter_chain_incoming(vc.chain, &call,
			test_incoming_expect_continue_inc,
			test_inc, &count);
	g_assert(count == 2);
	count = 0;

	/* Register the filter */
	g_assert(ofono_voicecall_filter_register(&filter) == 0);

	/* This one gets ok'ed immediately because there's no call */
	__ofono_voicecall_filter_chain_incoming(vc.chain, NULL,
			test_incoming_expect_continue_inc,
			test_inc, &count);
	g_assert(count == 2);
	count = 0;

	/* This one does nothing because all callbacks are NULL */
	__ofono_voicecall_filter_chain_incoming(vc.chain, &call, NULL,
			NULL, &count);
	g_assert(!count);

	/* Completion callback will terminate the loop */
	__ofono_voicecall_filter_chain_incoming(vc.chain, &call,
			test_incoming_expect_continue_and_quit,
			test_inc, &count);

	g_main_loop_run(test_loop);
	g_assert(test_filter_incoming_count == 1);

	/* Count is incremented by the request destructor */
	g_assert(count == 1);
	count = 0;

	__ofono_voicecall_filter_chain_free(vc.chain);
	ofono_voicecall_filter_unregister(&filter);
	test_common_deinit();
}

/* ==== incoming_hangup ==== */

static void test_incoming_hangup(void)
{
	static struct ofono_voicecall_filter filter = {
		.name = "incoming_hangup",
		.api_version = OFONO_VOICECALL_FILTER_API_VERSION,
		.filter_incoming = filter_incoming_hangup
	};

	struct ofono_voicecall vc;
	struct ofono_call call;
	int count = 0;

	test_common_init();
	test_voicecall_init(&vc);
	ofono_call_init(&call);
	string_to_phone_number("911", &call.phone_number);

	g_assert(ofono_voicecall_filter_register(&filter) == 0);
	g_assert((vc.chain = __ofono_voicecall_filter_chain_new(&vc)) != NULL);

	/* Completion callback will terminate the loop */
	__ofono_voicecall_filter_chain_incoming(vc.chain, &call,
			test_incoming_expect_hangup_and_quit,
			test_inc, &count);

	g_main_loop_run(test_loop);
	g_assert(test_filter_incoming_count == 1);

	/* Count is incremented by the request destructor */
	g_assert(count == 1);
	count = 0;

	__ofono_voicecall_filter_chain_free(vc.chain);
	ofono_voicecall_filter_unregister(&filter);
	test_common_deinit();
}

/* ==== incoming_ignore ==== */

static void test_incoming_ignore(void)
{
	static struct ofono_voicecall_filter filter = {
		.name = "incoming_ignore",
		.api_version = OFONO_VOICECALL_FILTER_API_VERSION,
		.filter_incoming = filter_incoming_ignore
	};

	struct ofono_voicecall vc;
	struct ofono_call call;
	int count = 0;

	test_common_init();
	test_voicecall_init(&vc);
	ofono_call_init(&call);
	string_to_phone_number("911", &call.phone_number);

	g_assert(ofono_voicecall_filter_register(&filter) == 0);
	g_assert((vc.chain = __ofono_voicecall_filter_chain_new(&vc)) != NULL);

	/* Completion callback will terminate the loop */
	__ofono_voicecall_filter_chain_incoming(vc.chain, &call,
			test_incoming_expect_ignore_and_quit,
			test_inc, &count);

	g_main_loop_run(test_loop);
	g_assert(test_filter_incoming_count == 1);

	/* Count is incremented by the request destructor */
	g_assert(count == 1);
	count = 0;

	__ofono_voicecall_filter_chain_free(vc.chain);
	ofono_voicecall_filter_unregister(&filter);
	test_common_deinit();
}

/* ==== restart ==== */

struct test_restart_data {
	struct ofono_voicecall vc;
	struct ofono_call call;
	gboolean restarted;
};

static gboolean test_restart_cb(gpointer user_data)
{
	struct test_restart_data *test = user_data;

	DBG("");
	test->restarted = TRUE;
	__ofono_voicecall_filter_chain_restart(test->vc.chain, &test->call);
	return G_SOURCE_REMOVE;
}

static void test_restart(void)
{
	static struct ofono_voicecall_filter filter = {
		.name = "incoming_ignore_later",
		.api_version = OFONO_VOICECALL_FILTER_API_VERSION,
		.filter_incoming = filter_incoming_ignore_later
	};

	struct test_restart_data test;
	struct ofono_voicecall *vc = &test.vc;
	struct ofono_call *call = &test.call;
	int count = 0;

	test_common_init();
	memset(&test, 0, sizeof(test));
	test_voicecall_init(vc);
	ofono_call_init(call);
	string_to_phone_number("911", &call->phone_number);

	g_assert(ofono_voicecall_filter_register(&filter) == 0);
	g_assert((vc->chain = __ofono_voicecall_filter_chain_new(vc)) != NULL);

	/* Completion callback will terminate the loop */
	__ofono_voicecall_filter_chain_incoming(vc->chain, call,
			test_incoming_expect_ignore_and_quit,
			test_inc, &count);

	g_idle_add(test_restart_cb, &test);
	g_main_loop_run(test_loop);

	/* Two times because of the restart */
	g_assert(test_filter_incoming_count == 2);
	g_assert(test.restarted);

	/* Count is incremented by the request destructor */
	g_assert(count == 1);
	count = 0;

	__ofono_voicecall_filter_chain_free(vc->chain);
	ofono_voicecall_filter_unregister(&filter);
	test_common_deinit();
}

/* ==== cancel1 ==== */

static void test_cancel1(void)
{
	static struct ofono_voicecall_filter filter = {
		.name = "dial_allow_async",
		.api_version = OFONO_VOICECALL_FILTER_API_VERSION,
		.priority = OFONO_VOICECALL_FILTER_PRIORITY_LOW,
		.filter_dial = filter_dial_continue_later,
		.filter_cancel = filter_cancel
	};

	struct ofono_voicecall vc;
	struct ofono_phone_number number;
	int count = 0;

	test_common_init();
	test_voicecall_init(&vc);
	string_to_phone_number("+1234", &number);

	g_assert(ofono_voicecall_filter_register(&filter) == 0);
	g_assert((vc.chain = __ofono_voicecall_filter_chain_new(&vc)) != NULL);

	/* Submit the request */
	__ofono_voicecall_filter_chain_dial(vc.chain, &number,
			OFONO_CLIR_OPTION_DEFAULT,
			test_dial_unexpected, test_inc, &count);

	/* And immediately cancel it */
	__ofono_voicecall_filter_chain_cancel(vc.chain, NULL);
	g_assert(!test_filter_dial_count);
	g_assert(count == 1);
	count = 0;

	__ofono_voicecall_filter_chain_free(vc.chain);
	ofono_voicecall_filter_unregister(&filter);
	test_common_deinit();
}

/* ==== cancel2 ==== */

static unsigned int filter_dial_cancel2(struct ofono_voicecall *vc,
	const struct ofono_phone_number *number, enum ofono_clir_option clir,
	ofono_voicecall_filter_dial_cb_t cb, void *user_data)
{
	DBG("");
	g_idle_add(test_cancel_cb, vc->chain);
	return filter_dial_continue_later(vc, number, clir, cb, user_data);
}

static void test_cancel2(void)
{
	static struct ofono_voicecall_filter filter = {
		.name = "dial_allow_async",
		.api_version = OFONO_VOICECALL_FILTER_API_VERSION,
		.filter_dial = filter_dial_cancel2,
		.filter_cancel = filter_cancel
	};

	struct ofono_voicecall vc;
	struct ofono_phone_number number;
	int count = 0;

	test_common_init();
	test_voicecall_init(&vc);
	string_to_phone_number("+1234", &number);

	g_assert(ofono_voicecall_filter_register(&filter) == 0);
	g_assert((vc.chain = __ofono_voicecall_filter_chain_new(&vc)) != NULL);

	/* Submit the request */
	__ofono_voicecall_filter_chain_dial(vc.chain, &number,
			OFONO_CLIR_OPTION_DEFAULT,
			test_dial_unexpected,
			test_inc, &count);

	/* It will be cancelled before it's completed */
	g_main_loop_run(test_loop);
	g_assert(!test_filter_dial_count);
	g_assert(count == 1);
	count = 0;

	__ofono_voicecall_filter_chain_free(vc.chain);
	ofono_voicecall_filter_unregister(&filter);
	test_common_deinit();
}

/* ==== cancel3 ==== */

static unsigned int filter_dial_cancel3(struct ofono_voicecall *vc,
	const struct ofono_phone_number *number, enum ofono_clir_option clir,
	ofono_voicecall_filter_dial_cb_t cb, void *user_data)
{
	DBG("");
	g_idle_add(test_cancel_cb, vc->chain);
	cb(OFONO_VOICECALL_FILTER_DIAL_CONTINUE, user_data);
	return 0;
}

static void test_cancel3(void)
{
	static struct ofono_voicecall_filter filter = {
		.name = "dial_allow_async",
		.api_version = OFONO_VOICECALL_FILTER_API_VERSION,
		.filter_dial = filter_dial_cancel3,
		.filter_cancel = filter_cancel
	};

	struct ofono_voicecall vc;
	struct ofono_phone_number number;
	int count = 0;

	test_common_init();
	test_voicecall_init(&vc);
	string_to_phone_number("+1234", &number);

	g_assert(ofono_voicecall_filter_register(&filter) == 0);
	g_assert((vc.chain = __ofono_voicecall_filter_chain_new(&vc)) != NULL);

	/* Submit the request */
	__ofono_voicecall_filter_chain_dial(vc.chain, &number,
			OFONO_CLIR_OPTION_DEFAULT,
			test_dial_unexpected, test_inc, &count);

	/* It will be cancelled before it's completed */
	g_main_loop_run(test_loop);
	g_assert(!test_filter_dial_count);
	g_assert(count == 1);
	count = 0;

	__ofono_voicecall_filter_chain_free(vc.chain);
	ofono_voicecall_filter_unregister(&filter);
	test_common_deinit();
}

/* ==== cancel4 ==== */

static void test_cancel4(void)
{
	static struct ofono_voicecall_filter filter = {
		.name = "dial_allow_async",
		.api_version = OFONO_VOICECALL_FILTER_API_VERSION,
		.filter_dial = filter_dial_cancel3, /* Reuse */
		.filter_cancel = filter_cancel
	};

	struct ofono_voicecall vc;
	struct ofono_call call;
	int count = 0;

	test_common_init();
	test_voicecall_init(&vc);
	ofono_call_init(&call);
	string_to_phone_number("+1234", &call.phone_number);

	g_assert(ofono_voicecall_filter_register(&filter) == 0);
	g_assert((vc.chain = __ofono_voicecall_filter_chain_new(&vc)) != NULL);

	/* Submit the request */
	__ofono_voicecall_filter_chain_dial_check(vc.chain, &call,
			test_dial_unexpected, test_inc, &count);

	/* It will be cancelled before it's completed */
	g_main_loop_run(test_loop);
	g_assert(!test_filter_dial_count);
	g_assert(count == 1);
	count = 0;

	__ofono_voicecall_filter_chain_free(vc.chain);
	ofono_voicecall_filter_unregister(&filter);
	test_common_deinit();
}

/* ==== cancel5 ==== */

static void test_cancel5(void)
{
	static struct ofono_voicecall_filter filter1 = {
		.name = "incoming_allow",
		.api_version = OFONO_VOICECALL_FILTER_API_VERSION,
		.filter_incoming = filter_incoming_continue_later,
		.filter_cancel = filter_cancel
	};

	static struct ofono_voicecall_filter filter2 = {
		.name = "incoming_hangup",
		.api_version = OFONO_VOICECALL_FILTER_API_VERSION,
		.filter_incoming = filter_incoming_hangup_later,
		.filter_cancel = filter_cancel
	};

	struct ofono_voicecall vc;
	struct ofono_call call1, call2;
	int count1 = 0, count2 = 0;

	test_common_init();
	test_voicecall_init(&vc);
	ofono_call_init(&call1);
	ofono_call_init(&call2);
	string_to_phone_number("112", &call1.phone_number);
	string_to_phone_number("911", &call2.phone_number);

	g_assert(ofono_voicecall_filter_register(&filter1) == 0);
	g_assert(ofono_voicecall_filter_register(&filter2) == 0);
	g_assert((vc.chain = __ofono_voicecall_filter_chain_new(&vc)) != NULL);

	/* Completion callback will terminate the loop */
	__ofono_voicecall_filter_chain_incoming(vc.chain, &call1,
			test_incoming_unexpected, test_inc, &count1);
	__ofono_voicecall_filter_chain_incoming(vc.chain, &call2,
			test_incoming_expect_hangup_and_quit,
			test_inc, &count2);

	/* Cancel the first request (twice) */
	__ofono_voicecall_filter_chain_cancel(vc.chain, &call1);
	__ofono_voicecall_filter_chain_cancel(vc.chain, &call1);

	g_main_loop_run(test_loop);
	g_assert(test_filter_incoming_count == 2);

	/* Counts are incremented by the request destructors */
	g_assert(count1 == 1);
	g_assert(count2 == 1);
	count1 = 0;
	count2 = 0;

	__ofono_voicecall_filter_chain_free(vc.chain);
	ofono_voicecall_filter_unregister(&filter1);
	ofono_voicecall_filter_unregister(&filter2);
	test_common_deinit();
}

/* ==== cancel6 ==== */

static void test_cancel6(void)
{
	static struct ofono_voicecall_filter filter1 = {
		.name = "incoming_allow",
		.api_version = OFONO_VOICECALL_FILTER_API_VERSION,
		.filter_incoming = filter_incoming_continue_later,
		.filter_cancel = filter_cancel
	};

	static struct ofono_voicecall_filter filter2 = {
		.name = "incoming_hangup",
		.api_version = OFONO_VOICECALL_FILTER_API_VERSION,
		.filter_incoming = filter_incoming_hangup_later,
		.filter_cancel = filter_cancel
	};

	struct ofono_voicecall vc;
	struct ofono_call call1, call2;
	int count1 = 0, count2 = 0;

	test_common_init();
	test_voicecall_init(&vc);
	ofono_call_init(&call1);
	ofono_call_init(&call2);
	string_to_phone_number("112", &call1.phone_number);
	string_to_phone_number("911", &call2.phone_number);

	g_assert(ofono_voicecall_filter_register(&filter1) == 0);
	g_assert(ofono_voicecall_filter_register(&filter2) == 0);
	g_assert((vc.chain = __ofono_voicecall_filter_chain_new(&vc)) != NULL);

	/* Completion callback will terminate the loop */
	__ofono_voicecall_filter_chain_incoming(vc.chain, &call1,
			test_incoming_expect_hangup_and_quit,
			test_inc, &count1);
	__ofono_voicecall_filter_chain_incoming(vc.chain, &call2,
			test_incoming_unexpected, test_inc, &count2);

	/* Cancel the second request (twice) */
	__ofono_voicecall_filter_chain_cancel(vc.chain, &call2);
	__ofono_voicecall_filter_chain_cancel(vc.chain, &call2);

	g_main_loop_run(test_loop);
	g_assert(test_filter_incoming_count == 2);

	/* Counts are incremented by the request destructors */
	g_assert(count1 == 1);
	g_assert(count2 == 1);
	count1 = 0;
	count2 = 0;

	__ofono_voicecall_filter_chain_free(vc.chain);
	ofono_voicecall_filter_unregister(&filter1);
	ofono_voicecall_filter_unregister(&filter2);
	test_common_deinit();
}

#define TEST_(name) "/voicecall-filter/" name

int main(int argc, char *argv[])
{
	g_test_init(&argc, &argv, NULL);

	gutil_log_timestamp = FALSE;
	gutil_log_default.level = g_test_verbose() ?
		GLOG_LEVEL_VERBOSE : GLOG_LEVEL_NONE;
	__ofono_log_init("test-voicecall-filter",
		g_test_verbose() ? "*" : NULL,
		FALSE, FALSE);

	if (argc > 1 && !strcmp(argv[1] , "-d")) {
		test_debug = TRUE;
		DBG("Debugging on (no timeout)");
	}

	g_test_add_func(TEST_("misc"), test_misc);
	g_test_add_func(TEST_("dial_allow"), test_dial_allow);
	g_test_add_func(TEST_("dial_allow_async"), test_dial_allow_async);
	g_test_add_func(TEST_("dial_block"), test_dial_block);
	g_test_add_func(TEST_("dial_block_async"), test_dial_block_async);
	g_test_add_func(TEST_("dial_check"), test_dial_check);
	g_test_add_func(TEST_("incoming_allow"), test_incoming_allow);
	g_test_add_func(TEST_("incoming_hangup"), test_incoming_hangup);
	g_test_add_func(TEST_("incoming_ignore"), test_incoming_ignore);
	g_test_add_func(TEST_("restart"), test_restart);
	g_test_add_func(TEST_("cancel1"), test_cancel1);
	g_test_add_func(TEST_("cancel2"), test_cancel2);
	g_test_add_func(TEST_("cancel3"), test_cancel3);
	g_test_add_func(TEST_("cancel4"), test_cancel4);
	g_test_add_func(TEST_("cancel5"), test_cancel5);
	g_test_add_func(TEST_("cancel6"), test_cancel6);

	return g_test_run();
}

/*
 * Local Variables:
 * mode: C
 * c-basic-offset: 8
 * indent-tabs-mode: t
 * End:
 */
