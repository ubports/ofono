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

#include <ofono/log.h>
#include <ofono/modem.h>
#include <ofono/devinfo.h>

#include "drivers/mbimmodem/mbimmodem.h"

struct devinfo_data {
	struct l_idle *delayed_register;
};

static void mbim_query_revision(struct ofono_devinfo *info,
				ofono_devinfo_query_cb_t cb, void *data)
{
	struct ofono_modem *modem = ofono_devinfo_get_modem(info);
	const char *revision = ofono_modem_get_string(modem, "FirmwareInfo");

	if (revision)
		CALLBACK_WITH_SUCCESS(cb, revision, data);
	else
		CALLBACK_WITH_FAILURE(cb, NULL, data);
}

static void mbim_query_serial(struct ofono_devinfo *info,
				ofono_devinfo_query_cb_t cb, void *data)
{
	struct ofono_modem *modem = ofono_devinfo_get_modem(info);
	const char *serial = ofono_modem_get_string(modem, "DeviceId");

	if (serial)
		CALLBACK_WITH_SUCCESS(cb, serial, data);
	else
		CALLBACK_WITH_FAILURE(cb, NULL, data);
}

static void delayed_register(struct l_idle *idle, void *user_data)
{
	struct ofono_devinfo *info = user_data;
	struct devinfo_data *dd = ofono_devinfo_get_data(info);

	l_idle_remove(idle);
	dd->delayed_register = NULL;

	ofono_devinfo_register(info);
}

static int mbim_devinfo_probe(struct ofono_devinfo *info, unsigned int vendor,
				void *data)
{
	struct devinfo_data *dd = l_new(struct devinfo_data, 1);

	dd->delayed_register = l_idle_create(delayed_register, info, NULL);
	ofono_devinfo_set_data(info, dd);

	return 0;
}

static void mbim_devinfo_remove(struct ofono_devinfo *info)
{
	struct devinfo_data *dd = ofono_devinfo_get_data(info);

	ofono_devinfo_set_data(info, NULL);
	l_idle_remove(dd->delayed_register);
	l_free(dd);
}

static const struct ofono_devinfo_driver driver = {
	.name			= "mbim",
	.probe			= mbim_devinfo_probe,
	.remove			= mbim_devinfo_remove,
	.query_revision		= mbim_query_revision,
	.query_serial		= mbim_query_serial,
};

void mbim_devinfo_init(void)
{
	ofono_devinfo_driver_register(&driver);
}

void mbim_devinfo_exit(void)
{
	ofono_devinfo_driver_unregister(&driver);
}
