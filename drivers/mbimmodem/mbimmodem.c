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

#define OFONO_API_SUBJECT_TO_CHANGE
#include <ofono/plugin.h>

#include "mbimmodem.h"

static int mbimmodem_init(void)
{
	mbim_devinfo_init();
	mbim_sim_init();
	mbim_netreg_init();
	mbim_sms_exit();
	mbim_gprs_init();
	mbim_gprs_context_init();
	return 0;
}

static void mbimmodem_exit(void)
{
	mbim_gprs_context_exit();
	mbim_gprs_exit();
	mbim_sms_exit();
	mbim_netreg_exit();
	mbim_sim_exit();
	mbim_devinfo_exit();
}

OFONO_PLUGIN_DEFINE(mbimmodem, "MBIM modem driver", VERSION,
		OFONO_PLUGIN_PRIORITY_DEFAULT, mbimmodem_init, mbimmodem_exit)
