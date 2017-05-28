/*
 *  oFono - Open Source Telephony
 *
 *  Copyright (C) 2017 Jolla Ltd. All rights reserved.
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License version 2 as
 *  published by the Free Software Foundation.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 */

#ifndef OFONO_DBUS_QUEUE_H
#define OFONO_DBUS_QUEUE_H

#include <ofono/types.h>
#include <ofono/dbus.h>

struct ofono_dbus_queue;

typedef DBusMessage * (* ofono_dbus_cb_t) (DBusMessage *msg, void *data);
typedef DBusMessage * (* ofono_dbus_reply_cb_t) (DBusMessage *msg);

struct ofono_dbus_queue *__ofono_dbus_queue_new(void);
void __ofono_dbus_queue_free(struct ofono_dbus_queue *q);
void __ofono_dbus_queue_request(struct ofono_dbus_queue *q,
			ofono_dbus_cb_t fn, DBusMessage *msg, void *data);
ofono_bool_t __ofono_dbus_queue_pending(struct ofono_dbus_queue *q);
ofono_bool_t __ofono_dbus_queue_set_pending(struct ofono_dbus_queue *q,
						DBusMessage *msg);
void __ofono_dbus_queue_reply_msg(struct ofono_dbus_queue *q,
						DBusMessage *reply);
void __ofono_dbus_queue_reply_ok(struct ofono_dbus_queue *q);
void __ofono_dbus_queue_reply_failed(struct ofono_dbus_queue *q);
void __ofono_dbus_queue_reply_fn(struct ofono_dbus_queue *q,
						ofono_dbus_reply_cb_t fn);
void __ofono_dbus_queue_reply_all_ok(struct ofono_dbus_queue *q);
void __ofono_dbus_queue_reply_all_failed(struct ofono_dbus_queue *q);
void __ofono_dbus_queue_reply_all_fn(struct ofono_dbus_queue *q,
						ofono_dbus_reply_cb_t fn);
void __ofono_dbus_queue_reply_all_fn_param(struct ofono_dbus_queue *q,
					ofono_dbus_cb_t fn, void *data);

#endif /* OFONO_DBUS_QUEUE_H */
