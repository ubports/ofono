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

#include <glib.h>
#include <gatchat.h>

#define OFONO_API_SUBJECT_TO_CHANGE
#include <ofono/plugin.h>
#include <ofono/types.h>
#include <ofono/modem.h>

#include "xmm7modem.h"

static int xmm7modem_init(void)
{
	xmm_radio_settings_init();
	xmm_ims_init();
	xmm_netmon_init();
	return 0;
}

static void xmm7modem_exit(void)
{
	xmm_radio_settings_exit();
	xmm_ims_exit();
	xmm_netmon_exit();
}

OFONO_PLUGIN_DEFINE(xmm7modem, "Intel xmm7xxx series modem driver",
			VERSION, OFONO_PLUGIN_PRIORITY_DEFAULT,
			xmm7modem_init, xmm7modem_exit)
