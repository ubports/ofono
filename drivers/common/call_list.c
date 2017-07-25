/*
 *
 *  oFono - Open Source Telephony
 *
 *  Copyright (C) 2008-2011  Intel Corporation. All rights reserved.
 *  Copyright (C) 2019 Alexander Couzens <lynxis@fe80.eu>
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
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
 *
 */

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <glib.h>
#include <ofono/types.h>

#include <ofono/types.h>
#include <ofono/log.h>
#include <ofono/voicecall.h>

#include "src/common.h"

#include <drivers/common/call_list.h>

gint ofono_call_compare(gconstpointer a, gconstpointer b)
{
	const struct ofono_call *ca = a;
	const struct ofono_call *cb = b;

	if (ca->id < cb->id)
		return -1;

	if (ca->id > cb->id)
		return 1;

	return 0;
}

gint ofono_call_compare_by_status(gconstpointer a, gconstpointer b)
{
	const struct ofono_call *call = a;
	int status = GPOINTER_TO_INT(b);

	if (status != call->status)
		return 1;

	return 0;
}

gint ofono_call_compare_by_id(gconstpointer a, gconstpointer b)
{
	const struct ofono_call *call = a;
	unsigned int id = GPOINTER_TO_UINT(b);

	if (id < call->id)
		return -1;

	if (id > call->id)
		return 1;

	return 0;
}

void ofono_call_list_dial_callback(struct ofono_voicecall *vc,
				   GSList **call_list,
				   const struct ofono_phone_number *ph,
				   int call_id)
{
	struct ofono_call *call;
	GSList *list;

	/* check if call_id already present */
	list = g_slist_find_custom(*call_list,
				GINT_TO_POINTER(call_id),
				ofono_call_compare_by_id);

	if (list) {
		return;
	}

	call = g_new0(struct ofono_call, 1);
	call->id = call_id;

	memcpy(&call->called_number, ph, sizeof(*ph));
	call->direction = CALL_DIRECTION_MOBILE_ORIGINATED;
	call->status = CALL_STATUS_DIALING;
	call->type = 0; /* voice */

	*call_list = g_slist_insert_sorted(*call_list,
					    call,
					    ofono_call_compare);
	ofono_voicecall_notify(vc, call);
}

void ofono_call_list_notify(struct ofono_voicecall *vc,
			    GSList **call_list,
			    GSList *calls)
{
	GSList *old_calls = *call_list;
	GSList *new_calls = calls;
	struct ofono_call *new_call, *old_call;

	while (old_calls || new_calls) {
		old_call = old_calls ? old_calls->data : NULL;
		new_call = new_calls ? new_calls->data : NULL;

		/* we drop disconnected calls and treat them as not existent */
		if (new_call && new_call->status == CALL_STATUS_DISCONNECTED) {
			new_calls = new_calls->next;
			calls = g_slist_remove(calls, new_call);
			g_free(new_call);
			continue;
		}

		if (old_call &&
				(new_call == NULL ||
				(new_call->id > old_call->id))) {
			ofono_voicecall_disconnected(
						vc,
						old_call->id,
						OFONO_DISCONNECT_REASON_UNKNOWN,
						NULL);
			old_calls = old_calls->next;
		} else if (new_call &&
			   (old_call == NULL ||
			   (new_call->id < old_call->id))) {

			/* new call, signal it */
			if (new_call->type == 0)
				ofono_voicecall_notify(vc, new_call);

			new_calls = new_calls->next;
		} else {
			if (memcmp(new_call, old_call, sizeof(*new_call))
					&& new_call->type == 0)
				ofono_voicecall_notify(vc, new_call);

			new_calls = new_calls->next;
			old_calls = old_calls->next;
		}
	}

	g_slist_free_full(*call_list, g_free);
	*call_list = calls;
}
