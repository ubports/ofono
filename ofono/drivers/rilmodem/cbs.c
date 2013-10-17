/*
 *
 *  oFono - Open Source Telephony
 *
 *  Copyright (C) 2008-2011  Intel Corporation. All rights reserved.
 *  Copyright (C) 2013 Jolla Ltd
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

#define _GNU_SOURCE
#include <string.h>
#include <stdio.h>

#include <glib.h>

#include <ofono/log.h>
#include <ofono/modem.h>
#include <ofono/cbs.h>

#include "gril.h"
#include "grilutil.h"

#include "rilmodem.h"
#include "ril_constants.h"

struct cbs_data {
	GRil *ril;
	guint timer_id;
};

static void ril_set_topics(struct ofono_cbs *cbs, const char *topics,
				ofono_cbs_set_cb_t cb, void *user_data)
{
	/*
	 * Although this does not do anything real
	 * towards network or modem, it is needed
	 * because without it ofono core does not
	 * change powered flag and it would reject
	 * incoming cb messages.
	 */
	CALLBACK_WITH_SUCCESS(cb, user_data);
}

static void ril_clear_topics(struct ofono_cbs *cbs,
				ofono_cbs_set_cb_t cb, void *user_data)
{
	/*
	 * Although this does not do anything real
	 * towards network or modem, it is needed
	 * because without it ofono core does not
	 * change powered flag and it would allow
	 * incoming cb messages.
	 */
	CALLBACK_WITH_SUCCESS(cb, user_data);
}

static void ril_cbs_notify(struct ril_msg *message, gpointer user_data)
{
	struct ofono_cbs *cbs = user_data;

	/*
	 * Ofono does not support UMTS CB - see
	 * src/smsutil.c method cbs_decode.
	 * But let's let the core to make
	 * the rejection reserve memory here
	 * for maximum UMTS CB length
	 */

	unsigned char pdu[1252];
	char *resp;
	struct parcel rilp;

	ril_util_init_parcel(message, &rilp);

	resp = parcel_r_string(&rilp);

	memcpy(resp, pdu, strlen((char *)resp));

	ofono_cbs_notify(cbs, pdu, strlen((char *)resp));
}

static gboolean ril_delayed_register(gpointer user_data)
{
	struct ofono_cbs *cbs = user_data;
	struct cbs_data *cd = ofono_cbs_get_data(cbs);

	cd->timer_id = 0;

	ofono_cbs_register(cbs);

	g_ril_register(cd->ril, RIL_UNSOL_RESPONSE_NEW_BROADCAST_SMS,
			ril_cbs_notify,	cbs);

	return FALSE;
}

static int ril_cbs_probe(struct ofono_cbs *cbs, unsigned int vendor,
				void *user)
{
	GRil *ril = user;

	struct cbs_data *cd = g_try_new0(struct cbs_data, 1);

	cd->ril = g_ril_clone(ril);

	ofono_cbs_set_data(cbs, cd);

	cd->timer_id = g_timeout_add_seconds(2, ril_delayed_register, cbs);

	return 0;
}

static void ril_cbs_remove(struct ofono_cbs *cbs)
{
	struct cbs_data *cd = ofono_cbs_get_data(cbs);
	ofono_cbs_set_data(cbs, NULL);

	if (cd->timer_id > 0)
		g_source_remove(cd->timer_id);

	g_ril_unref(cd->ril);
	g_free(cd);
}

static struct ofono_cbs_driver driver = {
	.name			= "rilmodem",
	.probe			= ril_cbs_probe,
	.remove			= ril_cbs_remove,
	.set_topics		= ril_set_topics,
	.clear_topics	= ril_clear_topics
};

void ril_cbs_init(void)
{
	ofono_cbs_driver_register(&driver);
}

void ril_cbs_exit(void)
{
	ofono_cbs_driver_unregister(&driver);
}

