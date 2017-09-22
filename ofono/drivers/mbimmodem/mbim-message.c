/*
 *
 *  oFono - Open Source Telephony
 *
 *  Copyright (C) 2017  Intel Corporation. All rights reserved.
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

#include <ell/ell.h>

#include "mbim-message.h"

struct mbim_message {
	int ref_count;
};

struct mbim_message *mbim_message_new(const uint8_t *uuid, uint32_t cid)
{
	struct mbim_message *msg;

	msg = l_new(struct mbim_message, 1);

	return mbim_message_ref(msg);
}

struct mbim_message *mbim_message_ref(struct mbim_message *msg)
{
	if (unlikely(!msg))
		return NULL;

	__sync_fetch_and_add(&msg->ref_count, 1);

	return msg;
}

void mbim_message_unref(struct mbim_message *msg)
{
	if (unlikely(!msg))
		return;

	if (__sync_sub_and_fetch(&msg->ref_count, 1))
		return;

	l_free(msg);
}
