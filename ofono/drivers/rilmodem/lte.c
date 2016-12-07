/*
 *
 *  oFono - Open Source Telephony
 *
 *  Copyright (C) 2016  Intel Corporation. All rights reserved.
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
#include <stdlib.h>
#include <stdio.h>
#include <errno.h>

#include <glib.h>

#include <ofono/modem.h>
#include <ofono/gprs-context.h>
#include <ofono/log.h>
#include <ofono/lte.h>

#include <gril/gril.h>
#include <gril/grilutil.h>

#include "rilmodem.h"

struct ril_lte_data {
	GRil *ril;
};

static void ril_lte_set_default_attach_info_cb(struct ril_msg *message,
							gpointer user_data)
{
	struct cb_data *cbd = user_data;
	ofono_lte_cb_t cb = cbd->cb;
	struct ofono_lte *lte = cbd->user;
	struct ril_lte_data *ld = ofono_lte_get_data(lte);
	DBG("");

	if (message->error == RIL_E_SUCCESS) {
		g_ril_print_response_no_args(ld->ril, message);
		CALLBACK_WITH_SUCCESS(cb, cbd->data);
	} else {
		ofono_error("%s: RIL error %s", __func__,
				ril_error_to_string(message->error));
		CALLBACK_WITH_FAILURE(cb, cbd->data);
	}
}

static void ril_lte_set_default_attach_info(const struct ofono_lte *lte,
			const struct ofono_lte_default_attach_info *info,
			ofono_lte_cb_t cb, void *data)
{
	struct ril_lte_data *ld = ofono_lte_get_data(lte);
	struct cb_data *cbd = cb_data_new(cb, data, (struct ofono_lte *)lte);
	struct parcel rilp;
	char buf[OFONO_GPRS_MAX_APN_LENGTH + 1];

	DBG("%s", info->apn);

	parcel_init(&rilp);
	parcel_w_int32(&rilp, 5);

	if (strlen(info->apn) > 0) {
		sprintf(buf, "%s", info->apn);
		parcel_w_string(&rilp, buf);
	} else
		parcel_w_string(&rilp, "");	/* apn */

	parcel_w_string(&rilp, "ip");		/* protocol */
	parcel_w_int32(&rilp, 0);		/* auth type */
	parcel_w_string(&rilp, "");		/* username */
	parcel_w_string(&rilp, "");		/* password */

	if (g_ril_send(ld->ril, RIL_REQUEST_SET_INITIAL_ATTACH_APN, &rilp,
			ril_lte_set_default_attach_info_cb, cbd, g_free) > 0)
		return;

	g_free(cbd);
	CALLBACK_WITH_FAILURE(cb, data);
}

static gboolean lte_delayed_register(gpointer user_data)
{
	struct ofono_lte *lte = user_data;

	DBG("");

	ofono_lte_register(lte);

	return FALSE;
}

static int ril_lte_probe(struct ofono_lte *lte, void *user_data)
{
	GRil *ril = user_data;
	struct ril_lte_data *ld;

	DBG("");

	ld = g_try_new0(struct ril_lte_data, 1);
	if (ld == NULL)
		return -ENOMEM;

	ld->ril = g_ril_clone(ril);

	ofono_lte_set_data(lte, ld);

	g_idle_add(lte_delayed_register, lte);

	return 0;
}

static void ril_lte_remove(struct ofono_lte *lte)
{
	struct ril_lte_data *ld = ofono_lte_get_data(lte);

	DBG("");

	ofono_lte_set_data(lte, NULL);

	g_ril_unref(ld->ril);
	g_free(ld);
}

static struct ofono_lte_driver driver = {
	.name				= RILMODEM,
	.probe				= ril_lte_probe,
	.remove				= ril_lte_remove,
	.set_default_attach_info	= ril_lte_set_default_attach_info,
};

void ril_lte_init(void)
{
	ofono_lte_driver_register(&driver);
}

void ril_lte_exit(void)
{
	ofono_lte_driver_unregister(&driver);
}
