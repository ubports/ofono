/*
 *
 *  oFono - Open Source Telephony
 *
 *  Copyright (C) 2016  Endocode AG. All rights reserved.
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

#include <string.h>

#include <glib.h>
#include <gatchat.h>

#define OFONO_API_SUBJECT_TO_CHANGE
#include <ofono/plugin.h>
#include <ofono/types.h>
#include <ofono/modem.h>

#include "ubloxmodem.h"

const struct ublox_model ublox_models[] = {
	{
		.name = "SARA-G270",
	},
	/* TOBY L2 series */
	{
		.name = "TOBY-L200",
		.flags = UBLOX_F_TOBY_L2|UBLOX_F_HAVE_USBCONF,
	},
	{
		.name = "TOBY-L201",
		.flags = UBLOX_F_TOBY_L2|UBLOX_F_HAVE_USBCONF,
	},
	{
		.name = "TOBY-L210",
		.flags = UBLOX_F_TOBY_L2|UBLOX_F_HAVE_USBCONF,
	},
	{
		.name = "TOBY-L220",
		.flags = UBLOX_F_TOBY_L2|UBLOX_F_HAVE_USBCONF,
	},
	{
		.name = "TOBY-L280",
		.flags = UBLOX_F_TOBY_L2|UBLOX_F_HAVE_USBCONF,
	},
	/* TOBY L4 series */
	{
		.name = "TOBY-L4006",
		.flags = UBLOX_F_TOBY_L4,
	},
	{
		.name = "TOBY-L4106",
		.flags = UBLOX_F_TOBY_L4,
	},
	{
		.name = "TOBY-L4206",
		.flags = UBLOX_F_TOBY_L4,
	},
	{
		.name = "TOBY-L4906",
		.flags = UBLOX_F_TOBY_L4,
	},
	/* LARA L2 series */
	{
		.name = "LARA-R202",
		.flags = UBLOX_F_LARA_R2,
	},
	{
		.name = "LARA-R211",
		.flags = UBLOX_F_LARA_R2,
	},
	{ /* sentinel */ },
};

const struct ublox_model *ublox_model_from_name(const char *name)
{
	const struct ublox_model *m;

	for (m = ublox_models; m->name; m++) {
		if (!strcmp(name, m->name))
			return m;
	}

	return NULL;
}

const struct ublox_model *ublox_model_from_id(int id)
{
	return ublox_models + id;
}

int ublox_model_to_id(const struct ublox_model *model)
{
	return model - ublox_models;
}

int ublox_is_toby_l2(const struct ublox_model *model)
{
	return model->flags & UBLOX_F_TOBY_L2;
}

int ublox_is_toby_l4(const struct ublox_model *model)
{
	return model->flags & UBLOX_F_TOBY_L4;
}

static int ubloxmodem_init(void)
{
	ublox_gprs_context_init();
	ublox_netreg_init();
	ublox_netmon_init();
	ublox_lte_init();

	return 0;
}

static void ubloxmodem_exit(void)
{
	ublox_gprs_context_exit();
	ublox_netreg_exit();
	ublox_netmon_exit();
	ublox_lte_exit();
}

OFONO_PLUGIN_DEFINE(ubloxmodem, "U-Blox Toby L2 high speed modem driver",
			VERSION, OFONO_PLUGIN_PRIORITY_DEFAULT,
			ubloxmodem_init, ubloxmodem_exit)
