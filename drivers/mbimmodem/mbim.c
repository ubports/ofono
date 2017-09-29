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

const uint8_t mbim_uuid_basic_connect[] = {
	0xa2, 0x89, 0xcc, 0x33, 0xbc, 0xbb, 0x8b, 0x4f, 0xb6, 0xb0,
	0x13, 0x3e, 0xc2, 0xaa, 0xe6, 0xdf
};

const uint8_t mbim_uuid_sms[] = {
	0x53, 0x3f, 0xbe, 0xeb, 0x14, 0xfe, 0x44, 0x67, 0x9f, 0x90,
	0x33, 0xa2, 0x23, 0xe5, 0x6c, 0x3f
};

const uint8_t mbim_uuid_ussd[] = {
	0xe5, 0x50, 0xa0, 0xc8, 0x5e, 0x82, 0x47, 0x9e, 0x82, 0xf7,
	0x10, 0xab, 0xf4, 0xc3, 0x35, 0x1f
};

const uint8_t mbim_uuid_phonebook[] = {
	0x4b, 0xf3, 0x84, 0x76, 0x1e, 0x6a, 0x41, 0xdb, 0xb1, 0xd8,
	0xbe, 0xd2, 0x89, 0xc2, 0x5b, 0xdb
};

const uint8_t mbim_uuid_stk[] = {
	0xd8, 0xf2, 0x01, 0x31, 0xfc, 0xb5, 0x4e, 0x17, 0x86, 0x02,
	0xd6, 0xed, 0x38, 0x16, 0x16, 0x4c
};

const uint8_t mbim_uuid_auth[] = {
	0x1d, 0x2b, 0x5f, 0xf7, 0x0a, 0xa1, 0x48, 0xb2, 0xaa, 0x52,
	0x50, 0xf1, 0x57, 0x67, 0x17, 0x4e
};

const uint8_t mbim_uuid_dss[] = {
	0xc0, 0x8a, 0x26, 0xdd, 0x77, 0x18, 0x43, 0x82, 0x84, 0x82,
	0x6e, 0x0d, 0x58, 0x3c, 0x4d ,0x0e
};

struct mbim_device {
	int ref_count;
	struct l_io *io;
	uint32_t max_segment_size;
	uint32_t max_outstanding;
	mbim_device_debug_func_t debug_handler;
	void *debug_data;
	mbim_device_destroy_func_t debug_destroy;
	mbim_device_disconnect_func_t disconnect_handler;
	void *disconnect_data;
	mbim_device_destroy_func_t disconnect_destroy;
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

struct mbim_device *mbim_device_new(int fd, uint32_t max_segment_size)
{
	struct mbim_device *device;

	if (unlikely(fd < 0))
		return NULL;

	device = l_new(struct mbim_device, 1);
	device->max_segment_size = max_segment_size;
	device->max_outstanding = 1;

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

	if (device->debug_destroy)
		device->debug_destroy(device->debug_data);

	if (device->disconnect_destroy)
		device->disconnect_destroy(device->disconnect_data);

	l_free(device);
}

bool mbim_device_set_max_outstanding(struct mbim_device *device, uint32_t max)
{
	if (unlikely(!device))
		return false;

	device->max_outstanding = max;
	return true;
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

	if (!device->io)
		return false;

	l_io_set_close_on_destroy(device->io, do_close);
	return true;
}
