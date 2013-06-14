/*
 *
 *  oFono - Open Source Telephony
 *
 *  Copyright (C) 2008-2011  Intel Corporation. All rights reserved.
 *  Copyright (C) 2013 Jolla Ltd
 *  Contact: Jussi Kangas <jussi.kangas@tieto.com>
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
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

#include <glib.h>

#include <ofono/log.h>
#include <ofono/modem.h>
#include <ofono/phonebook.h>

#include "gril.h"
#include "grilutil.h"

#include "rilmodem.h"

#include "ril_constants.h"

struct pb_data {
	GRil *ril;
};

static gboolean ril_delayed_register(gpointer user_data)
{
	struct ofono_phonebook *pb = user_data;
	ofono_phonebook_register(pb);
	return FALSE;
}

static int ril_phonebook_probe(struct ofono_phonebook *pb,
			unsigned int vendor, void *user)
{
	GRil *ril = user;
	struct pb_data *pd = g_try_new0(struct pb_data, 1);
	pd->ril = g_ril_clone(ril);
	ofono_phonebook_set_data(pb, pd);
	g_timeout_add_seconds(2, ril_delayed_register, pb);

	return 0;
}

static void ril_phonebook_remove(struct ofono_phonebook *pb)
{
	struct pb_data *pd = ofono_phonebook_get_data(pb);
	ofono_phonebook_set_data(pb, NULL);
	g_ril_unref(pd->ril);
	g_free(pd);
}

static struct ofono_phonebook_driver driver = {
	.name				= "rilmodem",
	.probe				= ril_phonebook_probe,
	.remove				= ril_phonebook_remove,
};

void ril_phonebook_init(void)
{
	ofono_phonebook_driver_register(&driver);
}

void ril_phonebook_exit(void)
{
	ofono_phonebook_driver_unregister(&driver);
}