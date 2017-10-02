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

#define _GNU_SOURCE
#include <unistd.h>
#include <limits.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <linux/types.h>

#include <ell/ell.h>

#include "mbim.h"
#include "mbim-message.h"
#include "mbim-private.h"

#define MAX_CONTROL_TRANSFER 4096
#define HEADER_SIZE (sizeof(struct mbim_message_header) + \
					sizeof(struct mbim_fragment_header))

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
	uint32_t next_tid;
	mbim_device_debug_func_t debug_handler;
	void *debug_data;
	mbim_device_destroy_func_t debug_destroy;
	mbim_device_disconnect_func_t disconnect_handler;
	void *disconnect_data;
	mbim_device_destroy_func_t disconnect_destroy;
	mbim_device_ready_func_t ready_handler;
	mbim_device_destroy_func_t ready_destroy;
	void *ready_data;
	uint8_t header[HEADER_SIZE];
	size_t header_offset;
	size_t segment_bytes_remaining;
};

static inline uint32_t _mbim_device_get_next_tid(struct mbim_device *device)
{
	uint32_t tid = device->next_tid;

	if (device->next_tid == UINT_MAX)
		device->next_tid = 1;
	else
		device->next_tid += 1;

	return tid;
}

static void disconnect_handler(struct l_io *io, void *user_data)
{
	struct mbim_device *device = user_data;

	l_util_debug(device->debug_handler, device->debug_data, "disconnect");

	if (device->disconnect_handler)
		device->disconnect_handler(device->disconnect_data);
}

static int receive_header(struct mbim_device *device, int fd)
{
	size_t to_read = sizeof(struct mbim_message_header) -
							device->header_offset;
	ssize_t len = TEMP_FAILURE_RETRY(read(fd,
					device->header + device->header_offset,
					to_read));

	if (len < 0) {
		if (errno == EAGAIN)
			return true;

		return false;
	}

	l_util_hexdump(true, device->header + device->header_offset, len,
				device->debug_handler, device->debug_data);
	device->header_offset += len;

	return true;
}

static bool command_write_handler(struct l_io *io, void *user_data)
{
	return false;
}

static bool command_read_handler(struct l_io *io, void *user_data)
{
	return false;
}

static bool open_write_handler(struct l_io *io, void *user_data)
{
	struct mbim_device *device = user_data;
	ssize_t written;
	int fd;
	uint32_t buf[4];

	/* Fill out buf with a MBIM_OPEN_MSG pdu */
	buf[0] = L_CPU_TO_LE32(MBIM_OPEN_MSG);
	buf[1] = L_CPU_TO_LE32(sizeof(buf));
	buf[2] = L_CPU_TO_LE32(_mbim_device_get_next_tid(device));
	buf[3] = L_CPU_TO_LE32(device->max_segment_size);

	fd = l_io_get_fd(io);

	written = TEMP_FAILURE_RETRY(write(fd, buf, sizeof(buf)));
	if (written < 0)
		return false;

	l_util_hexdump(false, buf, written,
				device->debug_handler, device->debug_data);

	return false;
}

static bool open_read_handler(struct l_io *io, void *user_data)
{
	struct mbim_device *device = user_data;
	uint8_t buf[MAX_CONTROL_TRANSFER];
	ssize_t len;
	uint32_t type;
	int fd;
	struct mbim_message_header *hdr;

	fd = l_io_get_fd(io);

	if (device->header_offset < sizeof(struct mbim_message_header)) {
		if (!receive_header(device, fd))
			return false;

		if (device->header_offset != sizeof(struct mbim_message_header))
			return true;
	}

	hdr = (struct mbim_message_header *) device->header;
	type = L_LE32_TO_CPU(hdr->type);

	if (device->segment_bytes_remaining == 0) {
		if (type == MBIM_OPEN_DONE)
			device->segment_bytes_remaining = 4;
		else
			device->segment_bytes_remaining =
					L_LE32_TO_CPU(hdr->len) -
					sizeof(struct mbim_message_header);
	}

	len = TEMP_FAILURE_RETRY(read(fd, buf,
					device->segment_bytes_remaining));
	if (len < 0) {
		if (errno == EAGAIN)
			return true;

		return false;
	}

	l_util_hexdump(true, buf, len,
				device->debug_handler, device->debug_data);
	device->segment_bytes_remaining -= len;

	/* Ready to read next packet */
	if (!device->segment_bytes_remaining)
		device->header_offset = 0;

	if (type != MBIM_OPEN_DONE)
		return true;

	/* Grab OPEN_DONE Status field */
	if (l_get_le32(buf) != 0) {
		close(fd);
		return false;
	}

	if (device->ready_handler)
		device->ready_handler(device->ready_data);

	l_io_set_read_handler(device->io, command_read_handler, device, NULL);
	l_io_set_write_handler(device->io, command_write_handler, device, NULL);

	return true;
}

struct mbim_device *mbim_device_new(int fd, uint32_t max_segment_size)
{
	struct mbim_device *device;

	if (unlikely(fd < 0))
		return NULL;

	device = l_new(struct mbim_device, 1);

	if (max_segment_size > MAX_CONTROL_TRANSFER)
		max_segment_size = MAX_CONTROL_TRANSFER;

	device->max_segment_size = max_segment_size;
	device->max_outstanding = 1;
	device->next_tid = 1;


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

	if (device->io) {
		l_io_destroy(device->io);
		device->io = NULL;
	}

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

bool mbim_device_set_ready_handler(struct mbim_device *device,
					mbim_device_ready_func_t function,
					void *user_data,
					mbim_device_destroy_func_t destroy)
{
	if (unlikely(!device))
		return false;

	if (device->ready_destroy)
		device->ready_destroy(device->ready_data);

	device->ready_handler = function;
	device->ready_destroy = destroy;
	device->ready_data = user_data;

	return true;
}
