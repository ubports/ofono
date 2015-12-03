/*
 *
 *  RIL library with GLib integration
 *
 *  Copyright (C) 2008-2011  Intel Corporation. All rights reserved.
 *  Copyright (C) 2012-2014  Canonical Ltd.
 *  Copyright (C) 2015 Ratchanan Srirattanamet.
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
#include <ofono/gprs-context.h>

#include "grilrequest.h"
#include "simutil.h"
#include "util.h"
#include "common.h"


/* SETUP_DATA_CALL_PARAMS reply parameters */
#define MIN_DATA_CALL_REPLY_SIZE 36

/* Call ID should not really be a big number */
#define MAX_CID_DIGITS 3

#define OFONO_EINVAL(error) do {		\
	error->type = OFONO_ERROR_TYPE_FAILURE;	\
	error->error = -EINVAL;			\
} while (0)

#define OFONO_NO_ERROR(error) do {			\
	error->type = OFONO_ERROR_TYPE_NO_ERROR;	\
	error->error = 0;				\
} while (0)

void g_ril_request_set_initial_attach_apn(GRil *gril, const char *apn,
						int proto,
						const char *user,
						const char *passwd,
						const char *mccmnc,
						struct parcel *rilp)
{
	const char *proto_str;
	const int auth_type = RIL_AUTH_ANY;

	parcel_init(rilp);

	parcel_w_string(rilp, apn);

	proto_str = ril_ofono_protocol_to_ril_string(proto);
	parcel_w_string(rilp, proto_str);

	parcel_w_int32(rilp, auth_type);
	parcel_w_string(rilp, user);
	parcel_w_string(rilp, passwd);

	g_ril_append_print_buf(gril, "(%s,%s,%s,%s,%s", apn, proto_str,
				ril_authtype_to_string(auth_type),
				user, passwd);

	if (g_ril_vendor(gril) == OFONO_RIL_VENDOR_MTK) {
		parcel_w_string(rilp, mccmnc);
		g_ril_append_print_buf(gril, "%s,%s)", print_buf, mccmnc);
	} else {
		g_ril_append_print_buf(gril, "%s)", print_buf);
	}
}

void g_ril_request_set_uicc_subscription(GRil *gril, int slot_id,
					int app_index,
					int sub_id,
					int sub_status,
					struct parcel *rilp)
{
	parcel_init(rilp);

	parcel_w_int32(rilp, slot_id);
	parcel_w_int32(rilp, app_index);
	parcel_w_int32(rilp, sub_id);
	parcel_w_int32(rilp, sub_status);

	g_ril_append_print_buf(gril, "(%d, %d, %d, %d(%s))",
				slot_id,
				app_index,
				sub_id,
				sub_status,
				sub_status ? "ACTIVATE" : "DEACTIVATE");
}
