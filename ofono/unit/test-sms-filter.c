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

#define OFONO_API_SUBJECT_TO_CHANGE
#include "ofono.h"
#include "common.h"
#include "smsutil.h"

#include <gutil_log.h>

#include <errno.h>

#define TEST_TIMEOUT_SEC (20)

static GMainLoop *test_loop = NULL;
static guint test_timeout_id = 0;

/* Fake data structures */

struct ofono_sms {
	int dg_count;
	int msg_count;
};

struct ofono_modem {
	int filter_dg_count;
	int filter_msg_count;
};

/* Code shared by all tests */

static gboolean test_no_timeout_cb(gpointer data)
{
	g_assert(FALSE);
	return G_SOURCE_REMOVE;
}

static gboolean test_timeout_cb(gpointer user_data)
{
	ofono_error("Timeout!");
	g_main_loop_quit(test_loop);
	test_timeout_id = 0;
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

static void test_send_text_inc(struct ofono_sms *sms,
		const struct sms_address *addr, const char *text, void *data)
{
	(*(int*)data)++;
}

static void test_common_init()
{
	test_loop = g_main_loop_new(NULL, FALSE);
	test_timeout_id = g_timeout_add_seconds(TEST_TIMEOUT_SEC,
						test_timeout_cb, NULL);
}

static void test_common_deinit()
{
	g_assert(test_timeout_id);
	g_source_remove(test_timeout_id);
	g_main_loop_unref(test_loop);
	test_timeout_id = 0;
	test_loop = NULL;
}

static void test_default_send_message(struct ofono_sms *sms,
		const struct sms_address *addr, const char *text, void *data)
{
	sms->msg_count++;
	g_main_loop_quit(test_loop);
}

static void test_default_dispatch_datagram(struct ofono_sms *sms,
		const struct ofono_uuid *uuid, int dst, int src,
		const unsigned char *buf, unsigned int len,
		const struct sms_address *addr,
		const struct sms_scts *scts)
{
	sms->dg_count++;
	g_main_loop_quit(test_loop);
}

static void test_default_dispatch_recv_message(struct ofono_sms *sms,
		const struct ofono_uuid *uuid, const char *message,
		enum sms_class cls, const struct sms_address *addr,
		const struct sms_scts *scts)
{
	sms->msg_count++;
	g_main_loop_quit(test_loop);
}

/* Test cases */

/* ==== misc ==== */

static void test_misc(void)
{
	static struct ofono_sms_filter noname = { 0 };
	static struct ofono_sms_filter misc = {
		.name = "misc"
	};
	int count = 0;

	g_assert(ofono_sms_filter_register(NULL) == -EINVAL);
	g_assert(ofono_sms_filter_register(&noname) == -EINVAL);
	g_assert(ofono_sms_filter_register(&misc) == 0);
	g_assert(ofono_sms_filter_register(&misc) == 0);
	__ofono_sms_filter_chain_send_text(NULL, NULL, NULL, NULL, NULL, NULL);
	__ofono_sms_filter_chain_send_text(NULL, NULL, NULL, NULL,
						test_inc, &count);
	g_assert(count == 1);
	__ofono_sms_filter_chain_recv_text(NULL, NULL, NULL, 0,  NULL, NULL,
									NULL);
	__ofono_sms_filter_chain_recv_datagram(NULL, NULL, 0, 0, NULL, 0, NULL,
								NULL, NULL);
	__ofono_sms_filter_chain_free(NULL);
	ofono_sms_filter_unregister(&misc);
	ofono_sms_filter_unregister(&misc);
	ofono_sms_filter_unregister(&misc);
	ofono_sms_filter_unregister(NULL);
}

/* ==== no_default ==== */

static void test_no_default(void)
{
	struct sms_filter_chain *chain;
	struct ofono_modem modem;
	struct ofono_sms sms;
	struct ofono_uuid uuid;
	struct sms_address addr;
	struct sms_scts scts;
	int count = 0;

	memset(&modem, 0, sizeof(modem));
	memset(&sms, 0, sizeof(sms));
	memset(&uuid, 0, sizeof(uuid));
	memset(&addr, 0, sizeof(addr));
	memset(&scts, 0, sizeof(scts));

	/* These calls just deallocate the buffer we pass in. One can
	 * verify that with valgrind */
	chain = __ofono_sms_filter_chain_new(&sms, &modem);
	__ofono_sms_filter_chain_send_text(chain, &addr, "1", NULL, NULL,
								NULL);
	__ofono_sms_filter_chain_send_text(chain, &addr, "1", NULL,
						test_inc, &count);
	g_assert(count == 1);
	count = 0;
	__ofono_sms_filter_chain_send_text(chain, &addr, "1",
				test_send_text_inc, test_inc, &count);
	g_assert(count == 2);
	__ofono_sms_filter_chain_recv_text(chain, &uuid, g_strdup("1"), 0,
					   &addr, &scts, NULL);
	__ofono_sms_filter_chain_recv_datagram(chain, &uuid, 0, 0,
					g_malloc0(1), 1, &addr, &scts, NULL);
	__ofono_sms_filter_chain_free(chain);
}

/* ==== send_message ==== */

struct test_send_message_data {
	struct ofono_modem modem;
	struct ofono_sms sms;
	struct sms_filter_chain *chain;
	int destroy_count;
};

static void test_send_message_destroy(void *data)
{
	struct test_send_message_data *test = data;

	test->destroy_count++;
	DBG("%d", test->destroy_count);
}

static void test_send_message_destroy_quit(void *data)
{
	struct test_send_message_data *test = data;

	test->destroy_count++;
	DBG("%d", test->destroy_count);
	g_main_loop_quit(test_loop);
}

static unsigned int test_send_message_filter(struct ofono_modem *modem,
		const struct ofono_sms_address *addr, const char *text,
		ofono_sms_filter_send_text_cb_t cb, void *data)
{
	modem->filter_msg_count++;
	DBG("%d", modem->filter_msg_count);
	cb(OFONO_SMS_FILTER_CONTINUE, addr, text, data);
	return 0;
}

static unsigned int test_send_message_filter2(struct ofono_modem *modem,
		const struct ofono_sms_address *addr, const char *text,
		ofono_sms_filter_send_text_cb_t cb, void *data)
{
	struct ofono_sms_address addr2 = *addr;

	modem->filter_msg_count++;
	DBG("%d", modem->filter_msg_count);
	cb(OFONO_SMS_FILTER_CONTINUE, &addr2, "foo", data);
	return 0;
}

static gboolean test_send_message_start(gpointer data)
{
	struct test_send_message_data *test = data;
	struct sms_address addr;

	memset(&addr, 0, sizeof(addr));
	__ofono_sms_filter_chain_send_text(test->chain, &addr, "test",
		test_default_send_message, test_send_message_destroy, test);
	return G_SOURCE_REMOVE;
}

static void test_send_message(void)
{
	static struct ofono_sms_filter send_message = {
		.name = "send_message",
		.filter_send_text = test_send_message_filter
	};

	static struct ofono_sms_filter send_message2 = {
		.name = "send_message2",
		.filter_send_text = test_send_message_filter2
	};

	struct test_send_message_data test;

	test_common_init();
	memset(&test, 0, sizeof(test));
	g_assert(ofono_sms_filter_register(&send_message) == 0);
	g_assert(ofono_sms_filter_register(&send_message2) == 0);
	test.chain = __ofono_sms_filter_chain_new(&test.sms, &test.modem);

	g_idle_add(test_send_message_start, &test);
	g_main_loop_run(test_loop);

	g_assert(test.destroy_count == 1);
	g_assert(test.sms.msg_count == 1);
	g_assert(test.modem.filter_msg_count == 2);
	__ofono_sms_filter_chain_free(test.chain);
	ofono_sms_filter_unregister(&send_message);
	ofono_sms_filter_unregister(&send_message2);
	test_common_deinit();
}

/* ==== send_message_free ==== */

static void test_send_message_free_handler(struct ofono_sms *sms,
		const struct sms_address *addr, const char *text, void *data)
{
	struct test_send_message_data *test = data;

	sms->msg_count++;
	__ofono_sms_filter_chain_free(test->chain);
	test->chain = NULL;

	g_main_loop_quit(test_loop);
}

static gboolean test_send_message_free_start(gpointer data)
{
	struct test_send_message_data *test = data;
	struct sms_address addr;

	memset(&addr, 0, sizeof(addr));
	__ofono_sms_filter_chain_send_text(test->chain, &addr, "test",
		test_send_message_free_handler, test_send_message_destroy,
		test);

	return G_SOURCE_REMOVE;
}

static void test_send_message_free(void)
{
	static struct ofono_sms_filter send_message_free = {
		.name = "send_message_free",
		.filter_send_text = test_send_message_filter
	};

	struct test_send_message_data test;

	test_common_init();
	memset(&test, 0, sizeof(test));
	g_assert(ofono_sms_filter_register(&send_message_free) == 0);
	test.chain = __ofono_sms_filter_chain_new(&test.sms, &test.modem);

	g_idle_add(test_send_message_free_start, &test);
	g_main_loop_run(test_loop);

	g_assert(test.destroy_count == 1);
	g_assert(test.sms.msg_count == 1);
	g_assert(test.modem.filter_msg_count == 1);
	ofono_sms_filter_unregister(&send_message_free);
	test_common_deinit();
}

/* ==== send_message_nd ==== */

static gboolean test_send_message_nd_start(gpointer data)
{
	struct test_send_message_data *test = data;
	struct sms_address addr;

	memset(&addr, 0, sizeof(addr));
	__ofono_sms_filter_chain_send_text(test->chain, &addr, "test", NULL,
				test_send_message_destroy_quit, test);
	return G_SOURCE_REMOVE;
}

static void test_send_message_nd(void)
{
	static struct ofono_sms_filter send_message = {
		.name = "send_message_nd",
		.filter_send_text = test_send_message_filter
	};

	struct test_send_message_data test;

	test_common_init();
	memset(&test, 0, sizeof(test));
	g_assert(ofono_sms_filter_register(&send_message) == 0);
	test.chain = __ofono_sms_filter_chain_new(&test.sms, &test.modem);

	g_idle_add(test_send_message_nd_start, &test);
	g_main_loop_run(test_loop);

	g_assert(test.destroy_count == 1);
	g_assert(test.modem.filter_msg_count == 1);
	__ofono_sms_filter_chain_free(test.chain);
	ofono_sms_filter_unregister(&send_message);
	test_common_deinit();
}

/* ==== recv_datagram_nd ==== */

static gboolean test_recv_datagram_nd_start(gpointer data)
{
	struct sms_filter_chain *chain = data;
	struct ofono_uuid uuid;
	struct sms_address addr;
	struct sms_scts scts;

	memset(&uuid, 0, sizeof(uuid));
	memset(&addr, 0, sizeof(addr));
	memset(&scts, 0, sizeof(scts));
	__ofono_sms_filter_chain_recv_datagram(chain, &uuid, 0, 0, NULL, 0,
				&addr, &scts, test_default_dispatch_datagram);
	return G_SOURCE_REMOVE;
}

static void test_recv_datagram_nd(void)
{
	struct sms_filter_chain *chain;
	struct ofono_modem modem;
	struct ofono_sms sms;

	test_common_init();
	memset(&modem, 0, sizeof(modem));
	memset(&sms, 0, sizeof(sms));
	chain = __ofono_sms_filter_chain_new(&sms, &modem);

	g_idle_add(test_recv_datagram_nd_start, chain);
	g_main_loop_run(test_loop);

	g_assert(sms.dg_count == 1);
	g_assert(!sms.msg_count);
	__ofono_sms_filter_chain_free(chain);
	test_common_deinit();
}

/* ==== recv_datagram_nc ==== */

static void test_recv_datagram_nc(void)
{
	static struct ofono_sms_filter recv_datagram_nc = {
		.name = "recv_datagram_nc",
	};

	struct sms_filter_chain *chain;
	struct ofono_modem modem;
	struct ofono_sms sms;

	test_common_init();
	memset(&modem, 0, sizeof(modem));
	memset(&sms, 0, sizeof(sms));
	g_assert(ofono_sms_filter_register(&recv_datagram_nc) == 0);
	chain = __ofono_sms_filter_chain_new(&sms, &modem);

	g_idle_add(test_recv_datagram_nd_start, chain);
	g_main_loop_run(test_loop);

	/* The driver has no callbacks, the default handler is invoked */
	g_assert(sms.dg_count == 1);
	g_assert(!sms.msg_count);
	__ofono_sms_filter_chain_free(chain);
	ofono_sms_filter_unregister(&recv_datagram_nc);
	test_common_deinit();
}

/* ==== recv_datagram ==== */

static int test_recv_datagram_filter_count = 0;

static unsigned int test_recv_datagram_filter(struct ofono_modem *modem,
		const struct ofono_uuid *uuid, int dst_port, int src_port,
		const unsigned char *buf, unsigned int len,
		const struct ofono_sms_address *addr,
		const struct ofono_sms_scts *scts,
		ofono_sms_filter_recv_datagram_cb_t cb, void *data)
{
	test_recv_datagram_filter_count++;
	DBG("%d", test_recv_datagram_filter_count);
	cb(OFONO_SMS_FILTER_CONTINUE, uuid, dst_port, src_port, buf, len,
							addr, scts, data);
	return 0;
}

static gboolean test_recv_datagram_start(gpointer data)
{
	struct sms_filter_chain *chain = data;
	struct ofono_uuid uuid;
	struct sms_address addr;
	struct sms_scts scts;
	guint len = 4;
	void *buf = g_malloc0(len);

	memset(&uuid, 0, sizeof(uuid));
	memset(&addr, 0, sizeof(addr));
	memset(&scts, 0, sizeof(scts));
	__ofono_sms_filter_chain_recv_datagram(chain, &uuid, 0, 0, buf, len,
				&addr, &scts, test_default_dispatch_datagram);
	return G_SOURCE_REMOVE;
}

static void test_recv_datagram(void)
{
	static struct ofono_sms_filter recv_datagram = {
		.name = "recv_datagram",
		.filter_recv_datagram = test_recv_datagram_filter
	};

	struct sms_filter_chain *chain;
	struct ofono_modem modem;
	struct ofono_sms sms;

	test_common_init();
	test_recv_datagram_filter_count = 0;
	memset(&modem, 0, sizeof(modem));
	memset(&sms, 0, sizeof(sms));
	g_assert(ofono_sms_filter_register(&recv_datagram) == 0);
	chain = __ofono_sms_filter_chain_new(&sms, &modem);

	g_idle_add(test_recv_datagram_start, chain);
	g_main_loop_run(test_loop);

	g_assert(test_recv_datagram_filter_count == 1);
	g_assert(sms.dg_count == 1);
	g_assert(!sms.msg_count);
	__ofono_sms_filter_chain_free(chain);
	ofono_sms_filter_unregister(&recv_datagram);
	test_common_deinit();
}

/* ==== recv_datagram2 ==== */

static int test_recv_datagram_filter2_count = 0;

static unsigned int test_recv_datagram_filter2(struct ofono_modem *modem,
		const struct ofono_uuid *uuid, int dst_port, int src_port,
		const unsigned char *buf, unsigned int len,
		const struct ofono_sms_address *addr,
		const struct ofono_sms_scts *scts,
		ofono_sms_filter_recv_datagram_cb_t cb, void *data)
{
	unsigned char buf2[8];

	/* Change the contents of the datagram */
	memset(buf2, 0xff, sizeof(buf2));
	test_recv_datagram_filter2_count++;
	DBG("%d", test_recv_datagram_filter2_count);
	/* This filter is supposed to be invoked after the first one */
	g_assert(test_recv_datagram_filter_count >=
		test_recv_datagram_filter2_count);
	cb(OFONO_SMS_FILTER_CONTINUE, uuid, dst_port, src_port,
				buf2, sizeof(buf2), addr, scts, data);
	return 0;
}

static void test_recv_datagram2(void)
{
	static struct ofono_sms_filter recv_datagram1 = {
		.name = "recv_datagram",
		.priority = 2,
		.filter_recv_datagram = test_recv_datagram_filter
	};
	static struct ofono_sms_filter recv_datagram2 = {
		.name = "recv_datagram2",
		.priority = 1,
		.filter_recv_datagram = test_recv_datagram_filter2
	};

	struct sms_filter_chain *chain;
	struct ofono_modem modem;
	struct ofono_sms sms;

	test_common_init();
	test_recv_datagram_filter_count = 0;
	test_recv_datagram_filter2_count = 0;
	memset(&modem, 0, sizeof(modem));
	memset(&sms, 0, sizeof(sms));
	/* Register two drivers */
	g_assert(ofono_sms_filter_register(&recv_datagram2) == 0);
	g_assert(ofono_sms_filter_register(&recv_datagram1) == 0);
	chain = __ofono_sms_filter_chain_new(&sms, &modem);

	g_idle_add(test_recv_datagram_start, chain);
	g_main_loop_run(test_loop);

	g_assert(test_recv_datagram_filter_count == 1);
	g_assert(test_recv_datagram_filter2_count == 1);
	g_assert(sms.dg_count == 1);
	g_assert(!sms.msg_count);
	__ofono_sms_filter_chain_free(chain);
	ofono_sms_filter_unregister(&recv_datagram1);
	ofono_sms_filter_unregister(&recv_datagram2);
	test_common_deinit();
}

/* ==== recv_datagram3 ==== */

static int test_recv_datagram_filter3_count = 0;
static int test_recv_datagram_cancel3_count = 0;

static void test_recv_datagram_cancel3_notify(gpointer data)
{
	test_recv_datagram_cancel3_count++;
	DBG("%d", test_recv_datagram_cancel3_count);
}

static unsigned int test_recv_datagram_filter3(struct ofono_modem *modem,
		const struct ofono_uuid *uuid, int dst_port, int src_port,
		const unsigned char *buf, unsigned int len,
		const struct ofono_sms_address *addr,
		const struct ofono_sms_scts *scts,
		ofono_sms_filter_recv_datagram_cb_t cb, void *data)
{
	test_recv_datagram_filter3_count++;
	DBG("%d", test_recv_datagram_filter3_count);
	if (test_recv_datagram_filter3_count == 1) {
		/* The first request will confinue immediately */
		struct ofono_uuid uuid2;
		struct ofono_sms_address addr2;
		struct ofono_sms_scts scts2;

		memset(&uuid2, 0xff, sizeof(uuid2));
		memset(&addr2, 0xff, sizeof(addr2));
		memset(&scts2, 0xff, sizeof(scts2));

		cb(OFONO_SMS_FILTER_CONTINUE, &uuid2, dst_port, src_port,
					buf, len, &addr2, &scts2, data);
		return 0;
	} else {
		/* The other requests will remain pending */
		return g_timeout_add_seconds_full(G_PRIORITY_DEFAULT,
			2*TEST_TIMEOUT_SEC, test_no_timeout_cb,
			NULL, test_recv_datagram_cancel3_notify);
	}
}

static void test_recv_datagram_cancel3(unsigned int id)
{
	DBG("%d", test_recv_datagram_cancel3_count);
	g_source_remove(id);
}

static gboolean test_recv_datagram3_start(gpointer data)
{
	struct sms_filter_chain *chain = data;
	struct ofono_uuid uuid;
	struct sms_address addr;
	struct sms_scts scts;

	memset(&uuid, 0, sizeof(uuid));
	memset(&addr, 0, sizeof(addr));
	memset(&scts, 0, sizeof(scts));

	/* Submit 3 datagrams */
	__ofono_sms_filter_chain_recv_datagram(chain, &uuid, 0, 0,
		g_malloc0(1), 1, &addr, &scts, test_default_dispatch_datagram);
	__ofono_sms_filter_chain_recv_datagram(chain, &uuid, 0, 0,
		g_malloc0(2), 2, &addr, &scts, test_default_dispatch_datagram);
	__ofono_sms_filter_chain_recv_datagram(chain, &uuid, 0, 0,
		g_malloc0(2), 3, &addr, &scts, test_default_dispatch_datagram);
	return G_SOURCE_REMOVE;
}

static void test_recv_datagram3(void)
{
	static struct ofono_sms_filter recv_datagram3 = {
		.name = "recv_datagram3",
		.priority = 3,
		.filter_recv_datagram = test_recv_datagram_filter3,
		.cancel = test_recv_datagram_cancel3
	};
	static struct ofono_sms_filter recv_datagram1 = {
		.name = "recv_datagram",
		.priority = 2,
		.filter_recv_datagram = test_recv_datagram_filter
	};
	static struct ofono_sms_filter recv_datagram2 = {
		.name = "recv_datagram2",
		.priority = 1,
		.filter_recv_datagram = test_recv_datagram_filter2
	};

	struct sms_filter_chain *chain;
	struct ofono_modem modem;
	struct ofono_sms sms;

	test_common_init();
	test_recv_datagram_filter_count = 0;
	test_recv_datagram_filter2_count = 0;
	test_recv_datagram_filter3_count = 0;
	test_recv_datagram_cancel3_count = 0;
	memset(&modem, 0, sizeof(modem));
	memset(&sms, 0, sizeof(sms));

	/* Register 3 drivers */
	g_assert(ofono_sms_filter_register(&recv_datagram1) == 0);
	g_assert(ofono_sms_filter_register(&recv_datagram2) == 0);
	g_assert(ofono_sms_filter_register(&recv_datagram3) == 0);
	chain = __ofono_sms_filter_chain_new(&sms, &modem);

	g_idle_add(test_recv_datagram3_start, chain);
	g_main_loop_run(test_loop);

	g_assert(test_recv_datagram_filter_count == 1);
	g_assert(test_recv_datagram_filter2_count == 1);
	g_assert(test_recv_datagram_filter3_count == 3);
	g_assert(!test_recv_datagram_cancel3_count);
	g_assert(sms.dg_count == 1);
	g_assert(!sms.msg_count);

	/* The last 2 requests are cancelled when we free the filter */
	__ofono_sms_filter_chain_free(chain);
	g_assert(test_recv_datagram_cancel3_count == 2);

	ofono_sms_filter_unregister(&recv_datagram1);
	ofono_sms_filter_unregister(&recv_datagram2);
	ofono_sms_filter_unregister(&recv_datagram3);
	test_common_deinit();
}

/* ==== recv_datagram_drop ==== */

static int test_recv_datagram_drop_filter_count = 0;

static unsigned int test_recv_datagram_drop_filter(struct ofono_modem *modem,
		const struct ofono_uuid *uuid, int dst_port, int src_port,
		const unsigned char *buf, unsigned int len,
		const struct ofono_sms_address *addr,
		const struct ofono_sms_scts *scts,
		ofono_sms_filter_recv_datagram_cb_t cb, void *data)
{
	test_recv_datagram_drop_filter_count++;
	DBG("%d", test_recv_datagram_drop_filter_count);
	cb(OFONO_SMS_FILTER_DROP, uuid, dst_port, src_port, buf, len,
							addr, scts, data);
	g_idle_add(test_quit_cb, NULL);
	return 0;
}

static void test_recv_datagram_drop(void)
{
	static struct ofono_sms_filter recv_datagram_drop = {
		.name = "recv_datagram_drop",
		.filter_recv_datagram = test_recv_datagram_drop_filter
	};

	struct sms_filter_chain *chain;
	struct ofono_modem modem;
	struct ofono_sms sms;

	test_common_init();
	test_recv_datagram_drop_filter_count = 0;
	memset(&modem, 0, sizeof(modem));
	memset(&sms, 0, sizeof(sms));
	g_assert(ofono_sms_filter_register(&recv_datagram_drop) == 0);
	chain = __ofono_sms_filter_chain_new(&sms, &modem);

	g_idle_add(test_recv_datagram_start, chain);
	g_main_loop_run(test_loop);

	g_assert(test_recv_datagram_drop_filter_count == 1);
	g_assert(!sms.dg_count);
	g_assert(!sms.msg_count);
	__ofono_sms_filter_chain_free(chain);
	ofono_sms_filter_unregister(&recv_datagram_drop);
	test_common_deinit();
}

/* ==== recv_message_nd ==== */

static gboolean test_recv_message_nd_start(gpointer data)
{
	struct sms_filter_chain *chain = data;
	struct ofono_uuid uuid;
	struct sms_address addr;
	struct sms_scts scts;

	memset(&uuid, 0, sizeof(uuid));
	memset(&addr, 0, sizeof(addr));
	memset(&scts, 0, sizeof(scts));
	__ofono_sms_filter_chain_recv_text(chain, &uuid, NULL, 0, &addr,
				&scts, test_default_dispatch_recv_message);
	return G_SOURCE_REMOVE;
}

static void test_recv_message_nd(void)
{
	struct sms_filter_chain *chain;
	struct ofono_modem modem;
	struct ofono_sms sms;

	test_common_init();
	memset(&modem, 0, sizeof(modem));
	memset(&sms, 0, sizeof(sms));
	chain = __ofono_sms_filter_chain_new(&sms, &modem);

	g_idle_add(test_recv_message_nd_start, chain);
	g_main_loop_run(test_loop);

	g_assert(sms.msg_count == 1);
	g_assert(!sms.dg_count);
	__ofono_sms_filter_chain_free(chain);
	test_common_deinit();
}

/* ==== recv_message_nc ==== */

static void test_recv_message_nc(void)
{
	static struct ofono_sms_filter recv_message_nc = {
		.name = "recv_message_nc",
	};

	struct sms_filter_chain *chain;
	struct ofono_modem modem;
	struct ofono_sms sms;

	test_common_init();
	memset(&modem, 0, sizeof(modem));
	memset(&sms, 0, sizeof(sms));
	g_assert(ofono_sms_filter_register(&recv_message_nc) == 0);
	chain = __ofono_sms_filter_chain_new(&sms, &modem);

	g_idle_add(test_recv_message_nd_start, chain);
	g_main_loop_run(test_loop);

	/* The driver has no callbacks, the default handler is invoked */
	g_assert(!sms.dg_count);
	g_assert(sms.msg_count == 1);
	__ofono_sms_filter_chain_free(chain);
	ofono_sms_filter_unregister(&recv_message_nc);
	test_common_deinit();
}

/* ==== recv_message ==== */

static int test_recv_message_filter_count = 0;

static unsigned int test_recv_message_filter(struct ofono_modem *modem,
		const struct ofono_uuid *uuid, const char *message,
		enum ofono_sms_class cls, const struct ofono_sms_address *addr,
		const struct ofono_sms_scts *scts,
		ofono_sms_filter_recv_text_cb_t cb, void *data)
{
	test_recv_message_filter_count++;
	DBG("%d", test_recv_message_filter_count);
	cb(OFONO_SMS_FILTER_CONTINUE, uuid, message, cls, addr, scts, data);
	return 0;
}

static gboolean test_recv_message_start(gpointer data)
{
	struct sms_filter_chain *chain = data;
	struct ofono_uuid uuid;
	struct sms_address addr;
	struct sms_scts scts;
	char *msg = g_strdup("test");

	memset(&uuid, 0, sizeof(uuid));
	memset(&addr, 0, sizeof(addr));
	memset(&scts, 0, sizeof(scts));
	__ofono_sms_filter_chain_recv_text(chain, &uuid, msg, 0, &addr, &scts,
					test_default_dispatch_recv_message);
	return G_SOURCE_REMOVE;
}

static void test_recv_message(void)
{
	static struct ofono_sms_filter recv_message = {
		.name = "recv_message",
		.filter_recv_text = test_recv_message_filter
	};

	struct sms_filter_chain *chain;
	struct ofono_modem modem;
	struct ofono_sms sms;

	test_common_init();
	test_recv_message_filter_count = 0;
	memset(&modem, 0, sizeof(modem));
	memset(&sms, 0, sizeof(sms));
	g_assert(ofono_sms_filter_register(&recv_message) == 0);
	chain = __ofono_sms_filter_chain_new(&sms, &modem);

	g_idle_add(test_recv_message_start, chain);
	g_main_loop_run(test_loop);

	g_assert(test_recv_message_filter_count == 1);
	g_assert(sms.msg_count == 1);
	g_assert(!sms.dg_count);
	__ofono_sms_filter_chain_free(chain);
	ofono_sms_filter_unregister(&recv_message);
	test_common_deinit();
}

/* ==== recv_message2 ==== */

static int test_recv_message_filter2_count = 0;

static unsigned int test_recv_message_filter2(struct ofono_modem *modem,
		const struct ofono_uuid *uuid, const char *message,
		enum ofono_sms_class cls, const struct ofono_sms_address *addr,
		const struct ofono_sms_scts *scts,
		ofono_sms_filter_recv_text_cb_t cb, void *data)
{
	test_recv_message_filter2_count++;
	DBG("%d", test_recv_message_filter2_count);
	cb(OFONO_SMS_FILTER_CONTINUE, uuid, "test2", cls, addr, scts, data);
	return 0;
}

static void test_recv_message2(void)
{
	static struct ofono_sms_filter recv_message = {
		.name = "recv_message",
		.priority = 2,
		.filter_recv_text = test_recv_message_filter
	};

	static struct ofono_sms_filter recv_message2 = {
		.name = "recv_message2",
		.priority = 1,
		.filter_recv_text = test_recv_message_filter2
	};

	struct sms_filter_chain *chain;
	struct ofono_modem modem;
	struct ofono_sms sms;

	test_common_init();
	test_recv_message_filter_count = 0;
	test_recv_message_filter2_count = 0;
	memset(&modem, 0, sizeof(modem));
	memset(&sms, 0, sizeof(sms));
	/* Register two drivers */
	g_assert(ofono_sms_filter_register(&recv_message2) == 0);
	g_assert(ofono_sms_filter_register(&recv_message) == 0);
	chain = __ofono_sms_filter_chain_new(&sms, &modem);

	g_idle_add(test_recv_message_start, chain);
	g_main_loop_run(test_loop);

	g_assert(test_recv_message_filter_count == 1);
	g_assert(test_recv_message_filter2_count == 1);
	g_assert(sms.msg_count == 1);
	g_assert(!sms.dg_count);
	__ofono_sms_filter_chain_free(chain);
	ofono_sms_filter_unregister(&recv_message);
	ofono_sms_filter_unregister(&recv_message2);
	test_common_deinit();
}

/* ==== recv_message3 ==== */

static int test_recv_message_filter3_count = 0;
static int test_recv_message_cancel3_count = 0;

static void test_recv_message_cancel3_notify(gpointer data)
{
	test_recv_message_cancel3_count++;
	DBG("%d", test_recv_message_cancel3_count);
}

static unsigned int test_recv_message_filter3(struct ofono_modem *modem,
		const struct ofono_uuid *uuid, const char *message,
		enum ofono_sms_class cls, const struct ofono_sms_address *addr,
		const struct ofono_sms_scts *scts,
		ofono_sms_filter_recv_text_cb_t cb, void *data)
{
	test_recv_message_filter3_count++;
	DBG("\"%s\" %d", message, test_recv_message_filter3_count);
	if (test_recv_message_filter3_count == 1) {
		/* The first request will confinue immediately */
		struct ofono_uuid uuid2;
		struct ofono_sms_address addr2;
		struct ofono_sms_scts scts2;

		memset(&uuid2, 0xff, sizeof(uuid2));
		memset(&addr2, 0xff, sizeof(addr2));
		memset(&scts2, 0xff, sizeof(scts2));

		cb(OFONO_SMS_FILTER_CONTINUE, &uuid2, message, cls, &addr2,
								&scts2, data);
		return 0;
	} else {
		/* The other two will remain pending */
		return g_timeout_add_seconds_full(G_PRIORITY_DEFAULT,
			2*TEST_TIMEOUT_SEC, test_no_timeout_cb,
			NULL, test_recv_message_cancel3_notify);
	}
}

static void test_recv_message_cancel3(unsigned int id)
{
	DBG("%d", test_recv_message_cancel3_count);
	g_source_remove(id);
}

static gboolean test_recv_message3_start(gpointer data)
{
	struct sms_filter_chain *chain = data;
	struct ofono_uuid uuid;
	struct sms_address addr;
	struct sms_scts scts;

	memset(&uuid, 0, sizeof(uuid));
	memset(&addr, 0, sizeof(addr));
	memset(&scts, 0, sizeof(scts));

	/* Submit 3 datagrams */
	__ofono_sms_filter_chain_recv_text(chain, &uuid, g_strdup("1"), 0,
			&addr, &scts, test_default_dispatch_recv_message);
	__ofono_sms_filter_chain_recv_text(chain, &uuid, g_strdup("2"), 0,
			&addr, &scts, test_default_dispatch_recv_message);
	__ofono_sms_filter_chain_recv_text(chain, &uuid, g_strdup("3"), 0,
			&addr, &scts, test_default_dispatch_recv_message);
	return G_SOURCE_REMOVE;
}

static void test_recv_message3(void)
{
	static struct ofono_sms_filter recv_message3 = {
		.name = "recv_message3",
		.priority = 3,
		.filter_recv_text = test_recv_message_filter3,
		.cancel = test_recv_message_cancel3
	};
	static struct ofono_sms_filter recv_message1 = {
		.name = "recv_message",
		.priority = 2,
		.filter_recv_text = test_recv_message_filter
	};
	static struct ofono_sms_filter recv_message2 = {
		.name = "recv_message2",
		.priority = 1,
		.filter_recv_text = test_recv_message_filter2
	};

	struct sms_filter_chain *chain;
	struct ofono_modem modem;
	struct ofono_sms sms;

	test_common_init();
	test_recv_message_filter_count = 0;
	test_recv_message_filter2_count = 0;
	test_recv_message_filter3_count = 0;
	test_recv_message_cancel3_count = 0;
	memset(&modem, 0, sizeof(modem));
	memset(&sms, 0, sizeof(sms));

	/* Register 3 drivers */
	g_assert(ofono_sms_filter_register(&recv_message1) == 0);
	g_assert(ofono_sms_filter_register(&recv_message2) == 0);
	g_assert(ofono_sms_filter_register(&recv_message3) == 0);
	chain = __ofono_sms_filter_chain_new(&sms, &modem);

	g_idle_add(test_recv_message3_start, chain);
	g_main_loop_run(test_loop);

	g_assert(test_recv_message_filter_count == 1);
	g_assert(test_recv_message_filter2_count == 1);
	g_assert(test_recv_message_filter3_count == 3);
	g_assert(!test_recv_message_cancel3_count);
	g_assert(sms.msg_count == 1);
	g_assert(!sms.dg_count);

	/* The last 2 requests are cancelled when we free the filter */
	__ofono_sms_filter_chain_free(chain);
	g_assert(test_recv_message_cancel3_count == 2);

	ofono_sms_filter_unregister(&recv_message1);
	ofono_sms_filter_unregister(&recv_message2);
	ofono_sms_filter_unregister(&recv_message3);
	test_common_deinit();
}

/* ==== recv_message_drop ==== */

static int test_recv_message_drop_filter_count = 0;

static unsigned int test_recv_message_drop_filter(struct ofono_modem *modem,
		const struct ofono_uuid *uuid, const char *message,
		enum ofono_sms_class cls, const struct ofono_sms_address *addr,
		const struct ofono_sms_scts *scts,
		ofono_sms_filter_recv_text_cb_t cb, void *data)
{
	test_recv_message_drop_filter_count++;
	DBG("\"%s\" %d", message, test_recv_message_drop_filter_count);
	cb(OFONO_SMS_FILTER_DROP, uuid, message, cls, addr, scts, data);
	g_idle_add(test_quit_cb, NULL);
	return 0;
}

static void test_recv_message_drop(void)
{
	static struct ofono_sms_filter recv_message_drop = {
		.name = "recv_message_drop",
		.filter_recv_text = test_recv_message_drop_filter
	};

	struct sms_filter_chain *chain;
	struct ofono_modem modem;
	struct ofono_sms sms;

	test_common_init();
	test_recv_message_drop_filter_count = 0;
	memset(&modem, 0, sizeof(modem));
	memset(&sms, 0, sizeof(sms));
	g_assert(ofono_sms_filter_register(&recv_message_drop) == 0);
	chain = __ofono_sms_filter_chain_new(&sms, &modem);

	g_idle_add(test_recv_message_start, chain);
	g_main_loop_run(test_loop);

	g_assert(test_recv_message_drop_filter_count == 1);
	g_assert(!sms.dg_count);
	g_assert(!sms.msg_count);
	__ofono_sms_filter_chain_free(chain);
	ofono_sms_filter_unregister(&recv_message_drop);
	test_common_deinit();
}

/* ==== early_free ==== */

static void test_early_free(void)
{
	/* First driver has no callbacks */
	static struct ofono_sms_filter early_free2 = {
		.name = "early_free2",
		.priority = 2
	};

	static struct ofono_sms_filter early_free = {
		.name = "early_free",
		.priority = 1,
		.filter_recv_datagram = test_recv_datagram_filter,
		.filter_recv_text = test_recv_message_filter
	};

	struct sms_filter_chain *chain;
	struct ofono_modem modem;
	struct ofono_sms sms;
	struct ofono_uuid uuid;
	struct sms_address addr;
	struct sms_scts scts;

	test_common_init();
	test_recv_datagram_filter_count = 0;
	test_recv_message_filter_count = 0;
	memset(&modem, 0, sizeof(modem));
	memset(&sms, 0, sizeof(sms));
	memset(&uuid, 0, sizeof(uuid));
	memset(&addr, 0, sizeof(addr));
	memset(&scts, 0, sizeof(scts));

	g_assert(ofono_sms_filter_register(&early_free) == 0);
	g_assert(ofono_sms_filter_register(&early_free2) == 0);
	chain = __ofono_sms_filter_chain_new(&sms, &modem);

	/* Submit the datagrams and immediately free the filter */
	__ofono_sms_filter_chain_recv_text(chain, &uuid, NULL, 0, &addr, &scts,
				test_default_dispatch_recv_message);
	__ofono_sms_filter_chain_recv_datagram(chain, &uuid, 0, 0, NULL, 0,
				&addr, &scts, test_default_dispatch_datagram);
	__ofono_sms_filter_chain_free(chain);

	/* Filter callback is getting invoked but not the default callback */
	g_assert(test_recv_datagram_filter_count == 1);
	g_assert(test_recv_message_filter_count == 1);
	g_assert(!sms.msg_count);
	g_assert(!sms.dg_count);

	ofono_sms_filter_unregister(&early_free);
	ofono_sms_filter_unregister(&early_free2);
	test_common_deinit();
}

#define TEST_(name) "/smsfilter/" name

int main(int argc, char *argv[])
{
	g_test_init(&argc, &argv, NULL);

	gutil_log_timestamp = FALSE;
	gutil_log_default.level = g_test_verbose() ?
		GLOG_LEVEL_VERBOSE : GLOG_LEVEL_NONE;
	__ofono_log_init("test-smsfilter",
		g_test_verbose() ? "*" : NULL,
		FALSE, FALSE);

	g_test_add_func(TEST_("misc"), test_misc);
	g_test_add_func(TEST_("no_default"), test_no_default);
	g_test_add_func(TEST_("send_message"), test_send_message);
	g_test_add_func(TEST_("send_message_free"), test_send_message_free);
	g_test_add_func(TEST_("send_message_nd"), test_send_message_nd);
	g_test_add_func(TEST_("recv_datagram_nd"), test_recv_datagram_nd);
	g_test_add_func(TEST_("recv_datagram_nc"), test_recv_datagram_nc);
	g_test_add_func(TEST_("recv_datagram"), test_recv_datagram);
	g_test_add_func(TEST_("recv_datagram2"), test_recv_datagram2);
	g_test_add_func(TEST_("recv_datagram3"), test_recv_datagram3);
	g_test_add_func(TEST_("recv_datagram_drop"), test_recv_datagram_drop);
	g_test_add_func(TEST_("recv_message_nd"), test_recv_message_nd);
	g_test_add_func(TEST_("recv_message_nc"), test_recv_message_nc);
	g_test_add_func(TEST_("recv_message"), test_recv_message);
	g_test_add_func(TEST_("recv_message2"), test_recv_message2);
	g_test_add_func(TEST_("recv_message3"), test_recv_message3);
	g_test_add_func(TEST_("recv_message_drop"), test_recv_message_drop);
	g_test_add_func(TEST_("early_free"), test_early_free);

	return g_test_run();
}

/*
 * Local Variables:
 * mode: C
 * c-basic-offset: 8
 * indent-tabs-mode: t
 * End:
 */
