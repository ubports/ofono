/*
 *
 *  RIL library with GLib integration
 *
 *  Copyright (C) 2008-2011  Intel Corporation. All rights reserved.
 *  Copyright (C) 2012 Canonical Ltd.
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

#ifndef __GRIL_H
#define __GRIL_H

#ifdef __cplusplus
extern "C" {
#endif

#include "grilresponse.h"
#include "grilutil.h"
#include "grilio.h"
#include "ril_constants.h"

struct _GRil;

typedef struct _GRil GRil;

/*
 * This struct represents an entire RIL message read
 * from the command socket.  It can hold responses or
 * unsolicited requests from RILD.
 */
struct ril_msg {
	gchar *buf;
	gsize buf_len;
	gboolean unsolicited;
	int req;
	int serial_no;
	int error;
};

typedef void (*GRilResponseFunc)(struct ril_msg *message, gpointer user_data);

typedef void (*GRilNotifyFunc)(struct ril_msg *message, gpointer user_data);

GRil *g_ril_new();

GIOChannel *g_ril_get_channel(GRil *ril);
GRilIO *g_ril_get_io(GRil *ril);

GRil *g_ril_ref(GRil *ril);
void g_ril_unref(GRil *ril);

GRil *g_ril_clone(GRil *ril);

void g_ril_suspend(GRil *ril);
void g_ril_resume(GRil *ril);

gboolean g_ril_set_disconnect_function(GRil *ril, GRilDisconnectFunc disconnect,
					gpointer user_data);

/*!
 * If the function is not NULL, then on every read/write from the GIOChannel
 * provided to GRil the logging function will be called with the
 * input/output string and user data
 */
gboolean g_ril_set_debug(GRil *ril, GRilDebugFunc func, gpointer user_data);

/*!
 * Queue an RIL request for execution.  The request contents are given
 * in data.  Once the command executes, the callback function given by
 * func is called with user provided data in user_data.
 *
 * Returns an id of the queued command which can be canceled using
 * g_ril_cancel.  If an error occurred, an id of 0 is returned.
 *
 */
guint g_ril_send(GRil *ril, const guint req, const char *data, const gsize data_len,
		GRilResponseFunc func, gpointer user_data, GDestroyNotify notify);

guint g_ril_register(GRil *ril, const int req,
			GRilNotifyFunc func, gpointer user_data);

gboolean g_ril_unregister(GRil *ril, guint id);
gboolean g_ril_unregister_all(GRil *ril);

#ifdef __cplusplus
}
#endif

#endif /* __GRIL_H */
