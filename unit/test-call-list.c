/*
 *
 *  oFono - Open Source Telephony
 *
 *  Copyright (C) 2017 Alexander Couzens <lynxis@fe80.eu>
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License version 2 as
 *  published by the Free Software Foundation.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 */


#include <glib.h>
#include <string.h>


#include "../src/common.h"
#include <ofono/types.h>
#include <drivers/common/call_list.h>

struct voicecall {
};

struct notified {
	unsigned int id;
	enum call_status status;
};

static struct notified notified_list[32];
static int notified_idx;
static int notified_check;

void reset_notified(void)
{
	notified_idx = 0;
	notified_check = 0;
	memset(&notified_list, 0, sizeof(notified_list));
}

void ofono_voicecall_notify(struct ofono_voicecall *vc,
				struct ofono_call *call)
{
	notified_list[notified_idx].id = call->id;
	notified_list[notified_idx].status = call->status;
	notified_idx++;
}

void ofono_voicecall_disconnected(struct ofono_voicecall *vc, int id,
				enum ofono_disconnect_reason reason,
				const struct ofono_error *error)
{
	notified_list[notified_idx].id = id;
	notified_list[notified_idx].status = CALL_STATUS_DISCONNECTED;
	notified_idx++;
}

static GSList *create_call(
		GSList *calls,
		unsigned int id,
		enum call_status status,
		enum call_direction direction)
{
	struct ofono_call *call = g_new0(struct ofono_call, 1);

	call->id = id;
	call->status = status;
	call->direction = direction;

	calls = g_slist_insert_sorted(calls, call, ofono_call_compare);

	return calls;
}

static void assert_notified(unsigned int call_id, int call_status)
{
	g_assert(notified_idx >= notified_check);
	g_assert(notified_list[notified_check].id == call_id);
	g_assert(notified_list[notified_check].status == call_status);

	notified_check++;
}

static void test_notify_disconnected(void)
{
	struct ofono_voicecall *vc = NULL;
	struct ofono_phone_number ph;
	GSList *call_list;
	GSList *calls;

	strcpy(ph.number, "004888123456");
	ph.type = 0;

	/* reset test */
	reset_notified();
	call_list = NULL;

	/* fill disconnected call*/
	calls = create_call(NULL, 1, CALL_STATUS_DISCONNECTED,
			    CALL_DIRECTION_MOBILE_TERMINATED);
	ofono_call_list_notify(vc, &call_list, calls);

	/* incoming call */
	calls = create_call(NULL, 1, CALL_STATUS_DISCONNECTED,
			    CALL_DIRECTION_MOBILE_TERMINATED);
	calls = create_call(calls, 1, CALL_STATUS_ALERTING,
			   CALL_DIRECTION_MOBILE_TERMINATED);
	ofono_call_list_notify(vc, &call_list, calls);

	/* answer call */
	calls = create_call(NULL, 1, CALL_STATUS_ACTIVE,
			   CALL_DIRECTION_MOBILE_TERMINATED);
	calls = create_call(calls, 1, CALL_STATUS_DISCONNECTED,
			    CALL_DIRECTION_MOBILE_TERMINATED);
	ofono_call_list_notify(vc, &call_list, calls);

	/* another call waiting */
	calls = create_call(NULL, 1, CALL_STATUS_DISCONNECTED,
			    CALL_DIRECTION_MOBILE_TERMINATED);
	calls = create_call(calls, 1, CALL_STATUS_ACTIVE,
			   CALL_DIRECTION_MOBILE_TERMINATED);
	calls = create_call(calls, 2, CALL_STATUS_DISCONNECTED,
			    CALL_DIRECTION_MOBILE_TERMINATED);
	calls = create_call(calls, 2, CALL_STATUS_WAITING,
				   CALL_DIRECTION_MOBILE_TERMINATED);
	calls = create_call(calls, 2, CALL_STATUS_DISCONNECTED,
			    CALL_DIRECTION_MOBILE_TERMINATED);
	ofono_call_list_notify(vc, &call_list, calls);

	/* end all calls */
	ofono_call_list_notify(vc, &call_list, NULL);

	/* verify call history */
	assert_notified(1, CALL_STATUS_ALERTING);
	assert_notified(1, CALL_STATUS_ACTIVE);
	assert_notified(2, CALL_STATUS_WAITING);
	assert_notified(1, CALL_STATUS_DISCONNECTED);
	assert_notified(2, CALL_STATUS_DISCONNECTED);

	g_assert(notified_check == notified_idx);
	g_slist_free_full(call_list, g_free);
}

static void test_notify(void)
{
	struct ofono_voicecall *vc = NULL;
	struct ofono_phone_number ph;
	GSList *call_list;
	GSList *calls;

	strcpy(ph.number, "004888123456");
	ph.type = 0;

	/* reset test */
	reset_notified();
	call_list = NULL;

	/* incoming call */
	calls = create_call(NULL, 1, CALL_STATUS_ALERTING,
			   CALL_DIRECTION_MOBILE_TERMINATED);
	ofono_call_list_notify(vc, &call_list, calls);

	/* answer call */
	calls = create_call(NULL, 1, CALL_STATUS_ACTIVE,
			   CALL_DIRECTION_MOBILE_TERMINATED);
	ofono_call_list_notify(vc, &call_list, calls);

	/* another call waiting */
	calls = create_call(NULL, 1, CALL_STATUS_ACTIVE,
			   CALL_DIRECTION_MOBILE_TERMINATED);
	calls = create_call(calls, 2, CALL_STATUS_WAITING,
				   CALL_DIRECTION_MOBILE_TERMINATED);
	ofono_call_list_notify(vc, &call_list, calls);

	/* end all calls */
	ofono_call_list_notify(vc, &call_list, NULL);

	/* verify call history */
	assert_notified(1, CALL_STATUS_ALERTING);
	assert_notified(1, CALL_STATUS_ACTIVE);
	assert_notified(2, CALL_STATUS_WAITING);
	assert_notified(1, CALL_STATUS_DISCONNECTED);
	assert_notified(2, CALL_STATUS_DISCONNECTED);

	g_assert(notified_check == notified_idx);
	g_slist_free_full(call_list, g_free);
}

static void test_dial_callback(void)
{
	struct ofono_voicecall *vc = NULL;
	struct ofono_phone_number ph;
	struct ofono_call *call;
	GSList *call_list;

	/* reset test */
	reset_notified();
	call_list = NULL;

	strcpy(ph.number, "0099301234567890");
	ph.type = 0;

	ofono_call_list_dial_callback(vc, &call_list, &ph, 33);

	call = call_list->data;

	g_assert(strcmp(call->called_number.number, ph.number) == 0);
	g_slist_free_full(call_list, g_free);
}

static void test_dial_callback_race(void)
{
	struct ofono_voicecall *vc = NULL;
	struct ofono_phone_number ph;
	GSList *call_list, *calls;

	/* reset test */
	reset_notified();
	call_list = NULL;

	strcpy(ph.number, "0099301234567890");
	ph.type = 0;

	/* outgoing call */
	calls = create_call(NULL, 1, CALL_STATUS_DIALING,
			   CALL_DIRECTION_MOBILE_ORIGINATED);
	ofono_call_list_notify(vc, &call_list, calls);
	ofono_call_list_dial_callback(vc, &call_list, &ph, 1);

	g_assert(call_list->next == NULL);

	/* check how many items in the variable */
	g_slist_free_full(call_list, g_free);
}

int main(int argc, char **argv)
{
	g_test_init(&argc, &argv, NULL);

	g_test_add_func("/test-call-list/dial_callback", test_dial_callback);
	g_test_add_func("/test-call-list/dial_callback_race", test_dial_callback_race);
	g_test_add_func("/test-call-list/test_notify", test_notify);
	g_test_add_func("/test-call-list/test_notify_disconnected",
			test_notify_disconnected);
	return g_test_run();
}
