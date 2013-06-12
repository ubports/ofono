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
#include <ofono/radio-settings.h>

#include "gril.h"
#include "grilutil.h"

#include "rilmodem.h"

struct radio_data {
	GRil *ril;
};

static gboolean ril_delayed_register(gpointer user_data)
{
	struct ofono_radio_settings *rs = user_data;
	ofono_radio_settings_register(rs);
	return FALSE;
}

static int ril_radio_settings_probe(struct ofono_radio_settings *rs,
					unsigned int vendor,
					void *user)
{
	struct radio_data *rd = g_try_new0(struct radio_data, 1);
	g_timeout_add_seconds(2, ril_delayed_register, rd);
	return 0;
}

static void ril_radio_settings_remove(struct ofono_radio_settings *rs)
{
	ofono_radio_settings_set_data(rs, NULL);
}

static struct ofono_radio_settings_driver driver = {
	.name			= "rilmodem",
	.probe			= ril_radio_settings_probe,
	.remove			= ril_radio_settings_remove,
};

void ril_radio_settings_init(void)
{
	ofono_radio_settings_driver_register(&driver);
}

void ril_radio_settings_exit(void)
{
	ofono_radio_settings_driver_unregister(&driver);
}