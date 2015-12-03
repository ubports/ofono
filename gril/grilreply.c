/*
 *
 *  RIL library with GLib integration
 *
 *  Copyright (C) 2008-2011  Intel Corporation. All rights reserved.
 *  Copyright (C) 2013 Jolla Ltd
 *  Contact: Jussi Kangas <jussi.kangas@tieto.com>
 *  Copyright (C) 2012-2014  Canonical Ltd.
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

#include <stdio.h>
#include <ctype.h>
#include <errno.h>
#include <string.h>

#include <glib.h>

#include <ofono/log.h>
#include <ofono/modem.h>
#include <ofono/call-forwarding.h>
#include <ofono/gprs-context.h>

#include "common.h"
#include "util.h"
#include "grilreply.h"
#include "grilutil.h"

static gint g_ril_call_compare(gconstpointer a, gconstpointer b)
{
	const struct ofono_call *ca = a;
	const struct ofono_call *cb = b;

	if (ca->id < cb->id)
		return -1;

	if (ca->id > cb->id)
		return 1;

	return 0;
}

GSList *g_ril_reply_parse_get_calls(GRil *gril, const struct ril_msg *message)
{
	struct ofono_call *call;
	struct parcel rilp;
	GSList *l = NULL;
	int num, i;
	gchar *number, *name;

	g_ril_init_parcel(message, &rilp);

	g_ril_append_print_buf(gril, "{");

	/* maguro signals no calls with empty event data */
	if (rilp.size < sizeof(int32_t))
		goto no_calls;

	/* Number of RIL_Call structs */
	num = parcel_r_int32(&rilp);
	for (i = 0; i < num; i++) {
		call = g_try_new(struct ofono_call, 1);
		if (call == NULL)
			break;

		ofono_call_init(call);
		call->status = parcel_r_int32(&rilp);
		call->id = parcel_r_int32(&rilp);
		call->phone_number.type = parcel_r_int32(&rilp);
		parcel_r_int32(&rilp); /* isMpty */
		parcel_r_int32(&rilp); /* isMT */
		parcel_r_int32(&rilp); /* als */
		call->type = parcel_r_int32(&rilp); /* isVoice */
		parcel_r_int32(&rilp); /* isVoicePrivacy */
		number = parcel_r_string(&rilp);
		if (number) {
			strncpy(call->phone_number.number, number,
				OFONO_MAX_PHONE_NUMBER_LENGTH);
			g_free(number);
		}

		parcel_r_int32(&rilp); /* numberPresentation */
		name = parcel_r_string(&rilp);
		if (name) {
			strncpy(call->name, name,
				OFONO_MAX_CALLER_NAME_LENGTH);
			g_free(name);
		}

		parcel_r_int32(&rilp); /* namePresentation */
		parcel_r_int32(&rilp); /* uusInfo */

		if (strlen(call->phone_number.number) > 0)
			call->clip_validity = 0;
		else
			call->clip_validity = 2;

		g_ril_append_print_buf(gril,
					"%s [id=%d,status=%d,type=%d,"
					"number=%s,name=%s]",
					print_buf,
					call->id, call->status, call->type,
					call->phone_number.number, call->name);

		l = g_slist_insert_sorted(l, call, g_ril_call_compare);
	}

no_calls:
	g_ril_append_print_buf(gril, "%s}", print_buf);
	g_ril_print_response(gril, message);

	return l;
}
