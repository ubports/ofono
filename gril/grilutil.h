/*
 *
 *  RIL library with GLib integration
 *
 *  Copyright (C) 2008-2011  Intel Corporation. All rights reserved.
 *  Copyright (C) 2012  Canonical Ltd.
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

#ifndef __GRILUTIL_H
#define __GRILUTIL_H

#ifdef __cplusplus
extern "C" {
#endif

#include "gfunc.h"
#include "parcel.h"
#include "gril.h"

int ril_protocol_string_to_ofono_protocol(gchar *protocol_str);
const char *ril_error_to_string(int error);
const char *ril_radio_state_to_string(int radio_state);
const char *ril_request_id_to_string(int req);
const char *ril_unsol_request_to_string(int request);
const char *ril_pdp_fail_to_string(int status);

void g_ril_util_debug_hexdump(gboolean in, const unsigned char *buf, gsize len,
				GRilDebugFunc debugf, gpointer user_data);

gboolean g_ril_util_setup_io(GIOChannel *io, GIOFlags flags);

#ifdef __cplusplus
}
#endif

#endif /* __GRILUTIL_H */
