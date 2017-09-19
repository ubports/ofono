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

#include <unistd.h>

#include <ell/ell.h>

#include "mbim.h"

struct mbim_device {
	int ref_count;
	int fd;
	struct l_io *io;
	mbim_device_debug_func_t debug_handler;
	void *debug_data;
	mbim_device_destroy_func_t debug_destroy;
	mbim_device_disconnect_func_t disconnect_handler;
	void *disconnect_data;
	mbim_device_destroy_func_t disconnect_destroy;

	bool close_on_unref : 1;
};

static void disconnect_handler(struct l_io *io, void *user_data)
{
	struct mbim_device *device = user_data;

	l_util_debug(device->debug_handler, device->debug_data, "disconnect");

	if (device->disconnect_handler)
		device->disconnect_handler(device->disconnect_data);
}

static bool open_write_handler(struct l_io *io, void *user_data)
{
	return false;
}

static bool open_read_handler(struct l_io *io, void *user_data)
{
	return true;
}

struct mbim_device *mbim_device_new(int fd)
{
	struct mbim_device *device;

	if (unlikely(fd < 0))
		return NULL;

	device = l_new(struct mbim_device, 1);

	device->fd = fd;
	device->close_on_unref = false;

	device->io = l_io_new(fd);
	l_io_set_disconnect_handler(device->io, disconnect_handler,
								device, NULL);

	l_io_set_read_handler(device->io, open_read_handler, device, NULL);
	l_io_set_write_handler(device->io, open_write_handler, device, NULL);

	return mbim_device_ref(device);
}

struct mbim_device *mbim_device_ref(struct mbim_device *device)
{
	if (unlikely(!device))
		return NULL;

	__sync_fetch_and_add(&device->ref_count, 1);

	return device;
}

void mbim_device_unref(struct mbim_device *device)
{
	if (unlikely(!device))
		return;

	if (__sync_sub_and_fetch(&device->ref_count, 1))
		return;

	l_io_destroy(device->io);

	if (device->close_on_unref)
		close(device->fd);

	if (device->debug_destroy)
		device->debug_destroy(device->debug_data);

	if (device->disconnect_destroy)
		device->disconnect_destroy(device->disconnect_data);

	l_free(device);
}

bool mbim_device_set_disconnect_handler(struct mbim_device *device,
					mbim_device_disconnect_func_t function,
					void *user_data,
					mbim_device_destroy_func_t destroy)
{
	if (unlikely(!device))
		return false;

	if (device->disconnect_destroy)
		device->disconnect_destroy(device->disconnect_data);

	device->disconnect_handler = function;
	device->disconnect_destroy = destroy;
	device->disconnect_data = user_data;

	return true;
}

bool mbim_device_set_debug(struct mbim_device *device,
				mbim_device_debug_func_t func, void *user_data,
				mbim_device_destroy_func_t destroy)
{
	if (unlikely(!device))
		return false;

	if (device->debug_destroy)
		device->debug_destroy(device->debug_data);

	device->debug_handler = func;
	device->debug_data = user_data;
	device->debug_destroy = destroy;

	return true;
}

bool mbim_device_set_close_on_unref(struct mbim_device *device, bool do_close)
{
	if (unlikely(!device))
		return false;

	device->close_on_unref = do_close;
	return true;
}
