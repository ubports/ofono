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

const uint8_t mbim_context_type_none[] = {
	0xB4, 0x3F, 0x75, 0x8C, 0xA5, 0x60, 0x4B, 0x46, 0xB3, 0x5E,
	0xC5, 0x86, 0x96, 0x41, 0xFB, 0x54,
};

const uint8_t mbim_context_type_internet[] = {
	0x7E, 0x5E, 0x2A, 0x7E, 0x4E, 0x6F, 0x72, 0x72, 0x73, 0x6B,
	0x65, 0x6E, 0x7E, 0x5E, 0x2A, 0x7E,
};

const uint8_t mbim_context_type_vpn[] = {
	0x9B, 0x9F, 0x7B, 0xBE, 0x89, 0x52, 0x44, 0xB7, 0x83, 0xAC,
	0xCA, 0x41, 0x31, 0x8D, 0xF7, 0xA0,
};

const uint8_t mbim_context_type_voice[] = {
	0x88, 0x91, 0x82, 0x94, 0x0E, 0xF4, 0x43, 0x96, 0x8C, 0xCA,
	0xA8, 0x58, 0x8F, 0xBC, 0x02, 0xB2,
};

const uint8_t mbim_context_type_video_share[] = {
	0x05, 0xA2, 0xA7, 0x16, 0x7C, 0x34, 0x4B, 0x4D, 0x9A, 0x91,
	0xC5, 0xEF, 0x0C, 0x7A, 0xAA, 0xCC,
};

const uint8_t mbim_context_type_purchase[] = {
	0xB3, 0x27, 0x24, 0x96, 0xAC, 0x6C, 0x42, 0x2B, 0xA8, 0xC0,
	0xAC, 0xF6, 0x87, 0xA2, 0x72, 0x17,
};

const uint8_t mbim_context_type_ims[] = {
	0x21, 0x61, 0x0D, 0x01, 0x30, 0x74, 0x4B, 0xCE, 0x94, 0x25,
	0xB5, 0x3A, 0x07, 0xD6, 0x97, 0xD6,
};

const uint8_t mbim_context_type_mms[] = {
	0x46, 0x72, 0x66, 0x64, 0x72, 0x69, 0x6B, 0xC6, 0x96, 0x24,
	0xD1, 0xD3, 0x53, 0x89, 0xAC, 0xA9,
};

const uint8_t mbim_context_type_local[] = {
	0xA5, 0x7A, 0x9A, 0xFC, 0xB0, 0x9F, 0x45, 0xD7, 0xBB, 0x40,
	0x03, 0x3C, 0x39, 0xF6, 0x0D, 0xB9,
};

struct message_assembly_node {
	struct mbim_message_header msg_hdr;
	struct mbim_fragment_header frag_hdr;
	struct iovec *iov;
	size_t n_iov;
	size_t cur_iov;
} __attribute((packed))__;

struct message_assembly {
	struct l_queue *transactions;
};

static bool message_assembly_node_match_tid(const void *a, const void *b)
{
	const struct message_assembly_node *node = a;
	uint32_t tid = L_PTR_TO_UINT(b);

	return L_LE32_TO_CPU(node->msg_hdr.tid) == tid;
}

static void message_assembly_node_free(void *data)
{
	struct message_assembly_node *node = data;
	size_t i;

	for (i = 0; i < node->n_iov; i++)
		l_free(node->iov[i].iov_base);

	l_free(node->iov);
	l_free(node);
}

static struct message_assembly *message_assembly_new()
{
	struct message_assembly *assembly = l_new(struct message_assembly, 1);

	assembly->transactions = l_queue_new();

	return assembly;
}

static void message_assembly_free(struct message_assembly *assembly)
{
	l_queue_destroy(assembly->transactions, message_assembly_node_free);
	l_free(assembly);
}

static struct mbim_message *message_assembly_add(
					struct message_assembly *assembly,
					const void *header,
					void *frag, size_t frag_len)
{
	const struct mbim_message_header *msg_hdr = header;
	const struct mbim_fragment_header *frag_hdr = header +
					sizeof(struct mbim_message_header);
	uint32_t tid = L_LE32_TO_CPU(msg_hdr->tid);
	uint32_t type = L_LE32_TO_CPU(msg_hdr->type);
	uint32_t n_frags = L_LE32_TO_CPU(frag_hdr->num_frags);
	uint32_t cur_frag = L_LE32_TO_CPU(frag_hdr->cur_frag);
	struct message_assembly_node *node;
	struct mbim_message *message;

	if (unlikely(type != MBIM_COMMAND_DONE &&
				type != MBIM_INDICATE_STATUS_MSG))
		return NULL;

	node = l_queue_find(assembly->transactions,
				message_assembly_node_match_tid,
				L_UINT_TO_PTR(tid));

	if (!node) {
		if (cur_frag != 0)
			return NULL;

		if (n_frags == 1) {
			struct iovec *iov = l_new(struct iovec, 1);

			iov[0].iov_base = frag;
			iov[0].iov_len = frag_len;

			return _mbim_message_build(header, iov, 1);
		}

		node = l_new(struct message_assembly_node, 1);
		memcpy(&node->msg_hdr, msg_hdr, sizeof(*msg_hdr));
		memcpy(&node->frag_hdr, frag_hdr, sizeof(*frag_hdr));
		node->iov = l_new(struct iovec, n_frags);
		node->n_iov = n_frags;
		node->cur_iov = cur_frag;
		node->iov[node->cur_iov].iov_base = frag;
		node->iov[node->cur_iov].iov_len = frag_len;

		l_queue_push_head(assembly->transactions, node);

		return NULL;
	}

	if (node->n_iov != n_frags)
		return NULL;

	if (node->cur_iov + 1 != cur_frag)
		return NULL;

	node->cur_iov = cur_frag;
	node->iov[node->cur_iov].iov_base = frag;
	node->iov[node->cur_iov].iov_len = frag_len;

	if (node->cur_iov + 1 < node->n_iov)
		return NULL;

	l_queue_remove(assembly->transactions, node);
	message = _mbim_message_build(&node->msg_hdr, node->iov, node->n_iov);

	if (!message)
		message_assembly_node_free(node);
	else
		l_free(node);

	return message;
}

struct mbim_device {
	int ref_count;
	struct l_io *io;
	uint32_t max_segment_size;
	uint32_t max_outstanding;
	uint32_t next_tid;
	uint32_t next_notification;
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
	void *segment;
	struct l_queue *pending_commands;
	struct l_queue *sent_commands;
	struct l_queue *notifications;
	struct message_assembly *assembly;
	struct l_idle *close_io;

	bool is_ready : 1;
	bool in_notify : 1;
};

struct pending_command {
	uint32_t tid;
	uint32_t gid;
	struct mbim_message *message;
	mbim_device_reply_func_t callback;
	mbim_device_destroy_func_t destroy;
	void *user_data;
};

static bool pending_command_match_tid(const void *a, const void *b)
{
	const struct pending_command *pending = a;
	uint32_t tid = L_PTR_TO_UINT(b);

	return pending->tid == tid;
}

/*
 * Since we have to track how many outstanding requests we have issued, we
 * have to keep a pending_command structure around until it is replied to
 * by the function.  However, all resources associated with the command
 * can be freed
 */
static void pending_command_cancel(void *data)
{
	struct pending_command *pending = data;

	mbim_message_unref(pending->message);
	pending->message = NULL;

	if (pending->destroy)
		pending->destroy(pending->user_data);

	pending->callback = NULL;
	pending->user_data = NULL;
	pending->destroy = NULL;
}

static void pending_command_free(void *pending)
{
	pending_command_cancel(pending);
	l_free(pending);
}

static void pending_command_cancel_by_gid(void *data, void *user_data)
{
	struct pending_command *pending = data;
	uint32_t gid = L_PTR_TO_UINT(user_data);

	if (pending->gid != gid)
		return;

	pending_command_cancel(pending);
}

static bool pending_command_free_by_gid(void *data, void *user_data)
{
	struct pending_command *pending = data;
	uint32_t gid = L_PTR_TO_UINT(user_data);

	if (pending->gid != gid)
		return false;

	pending_command_free(pending);
	return true;
}

struct notification {
	uint32_t id;
	uint32_t gid;
	uint8_t uuid[16];
	uint32_t cid;
	mbim_device_reply_func_t notify;
	mbim_device_destroy_func_t destroy;
	void *user_data;

	bool destroyed : 1;
};

static bool notification_match_id(const void *a, const void *b)
{
	const struct notification *notification = a;
	uint32_t id = L_PTR_TO_UINT(b);

	return notification->id == id;
}

static void notification_free(void *data)
{
	struct notification *notification = data;

	if (notification->destroy)
		notification->destroy(notification->user_data);

	notification->notify = NULL;
	notification->user_data = NULL;
	notification->destroy = NULL;
	l_free(notification);
}

static bool notification_free_by_gid(void *data, void *user_data)
{
	struct notification *notification = data;
	uint32_t gid = L_PTR_TO_UINT(user_data);

	if (notification->gid != gid)
		return false;

	notification_free(notification);
	return true;
}

static bool notification_free_destroyed(void *data, void *user_data)
{
	struct notification *notification = data;

	if (!notification->destroyed)
		return false;

	notification_free(notification);
	return true;
}

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
	struct mbim_device *device = user_data;
	struct mbim_message *message;
	struct pending_command *pending;
	void *header;
	size_t header_size;
	size_t info_buf_len;
	size_t n_iov;
	struct iovec *body;
	int fd;
	ssize_t written;

	/*
	 * For now assume we write out the entire command in one go without
	 * hitting an EAGAIN
	 */
	pending = l_queue_pop_head(device->pending_commands);
	if (!pending)
		return false;

	message = pending->message;
	_mbim_message_set_tid(message, pending->tid);

	header = _mbim_message_get_header(message, &header_size);
	body = _mbim_message_get_body(message, &n_iov, &info_buf_len);

	fd = l_io_get_fd(io);

	if (info_buf_len + header_size < device->max_segment_size) {
		/*
		 * cdc-wdm* doesn't seem to support scatter-gather writes
		 * properly.  So copy into a temporary buffer instead
		 */
		uint8_t buf[device->max_segment_size];
		size_t pos;
		unsigned int i;

		memcpy(buf, header, header_size);
		pos = header_size;

		for (i = 0; i < n_iov; i++) {
			memcpy(buf + pos, body[i].iov_base, body[i].iov_len);
			pos += body[i].iov_len;
		}

		written = TEMP_FAILURE_RETRY(write(fd, buf, pos));

		l_info("n_iov: %lu, %lu", n_iov + 1, (size_t) written);

		if (written < 0)
			return false;

		l_util_hexdump(false, buf, written, device->debug_handler,
				device->debug_data);
	} else {
		/* TODO: Handle fragmented writes */
		l_util_debug(device->debug_handler, device->debug_data,
				"fragment me");
	}

	l_queue_push_tail(device->sent_commands, pending);

	if (l_queue_isempty(device->pending_commands))
		return false;

	if (l_queue_length(device->sent_commands) >= device->max_outstanding)
		return false;

	/* Only continue sending messages if the connection is ready */
	return device->is_ready;
}

static void dispatch_command_done(struct mbim_device *device,
					struct mbim_message *message)
{
	struct mbim_message_header *hdr =
			_mbim_message_get_header(message, NULL);
	struct pending_command *pending;

	pending = l_queue_remove_if(device->sent_commands,
					pending_command_match_tid,
					L_UINT_TO_PTR(L_LE32_TO_CPU(hdr->tid)));
	if (!pending)
		goto done;

	if (pending->callback)
		pending->callback(message, pending->user_data);

	pending_command_free(pending);

	if (l_queue_isempty(device->pending_commands))
		goto done;

	l_io_set_write_handler(device->io, command_write_handler, device, NULL);
done:
	mbim_message_unref(message);
}

static void dispatch_notification(struct mbim_device *device,
						struct mbim_message *message)
{
	const struct l_queue_entry *entry =
				l_queue_get_entries(device->notifications);
	uint32_t cid = mbim_message_get_cid(message);
	const uint8_t *uuid = mbim_message_get_uuid(message);
	bool handled = false;

	device->in_notify = true;

	while (entry) {
		struct notification *notification = entry->data;

		if (notification->cid != cid)
			goto next;

		if (memcmp(notification->uuid, uuid, 16))
			goto next;

		if (notification->notify)
			notification->notify(message, notification->user_data);

		handled = true;

next:
		entry = entry->next;
	}

	device->in_notify = false;

	l_queue_foreach_remove(device->notifications,
					notification_free_destroyed, NULL);

	if (!handled) {
		char uuidstr[37];

		if (!l_uuid_to_string(uuid, uuidstr, sizeof(uuidstr)))
			memset(uuidstr, 0, sizeof(uuidstr));

		l_util_debug(device->debug_handler, device->debug_data,
				"Unhandled notification (%s) %u",
				uuidstr, cid);
	}

	mbim_message_unref(message);
}

static void dispatch_message(struct mbim_device *device, uint32_t type,
						struct mbim_message *message)
{
	switch (type) {
	case MBIM_COMMAND_DONE:
		dispatch_command_done(device, message);
		break;
	case MBIM_INDICATE_STATUS_MSG:
		dispatch_notification(device, message);
		break;
	default:
		mbim_message_unref(message);
	}
}

static bool command_read_handler(struct l_io *io, void *user_data)
{
	struct mbim_device *device = user_data;
	ssize_t len;
	uint32_t type;
	int fd;
	struct mbim_message_header *hdr;
	struct iovec iov[2];
	uint32_t n_iov = 0;
	uint32_t header_size;
	struct mbim_message *message;
	uint32_t i;

	fd = l_io_get_fd(io);

	if (device->header_offset < sizeof(struct mbim_message_header)) {
		if (!receive_header(device, fd))
			return false;

		if (device->header_offset != sizeof(struct mbim_message_header))
			return true;
	}

	hdr = (struct mbim_message_header *) device->header;
	type = L_LE32_TO_CPU(hdr->type);

	if (device->segment_bytes_remaining == 0)
		device->segment_bytes_remaining =
					L_LE32_TO_CPU(hdr->len) -
					sizeof(struct mbim_message_header);

	if (type == MBIM_COMMAND_DONE || type == MBIM_INDICATE_STATUS_MSG)
		header_size = HEADER_SIZE;
	else
		header_size = sizeof(struct mbim_message_header);

	/* Put the rest of the header into the first chunk */
	if (device->header_offset < header_size) {
		iov[n_iov].iov_base = device->header + device->header_offset;
		iov[n_iov].iov_len = header_size - device->header_offset;
		n_iov += 1;
	}

	l_info("hdr->len: %u", L_LE32_TO_CPU(hdr->len));
	l_info("header_size: %u", header_size);
	l_info("header_offset: %lu", device->header_offset);
	l_info("segment_bytes_remaining: %lu", device->segment_bytes_remaining);

	iov[n_iov].iov_base = device->segment + L_LE32_TO_CPU(hdr->len) -
				device->header_offset -
				device->segment_bytes_remaining;
	iov[n_iov].iov_len = device->segment_bytes_remaining -
				(header_size - device->header_offset);
	n_iov += 1;

	len = TEMP_FAILURE_RETRY(readv(fd, iov, n_iov));
	if (len < 0) {
		if (errno == EAGAIN)
			return true;

		return false;
	}

	device->segment_bytes_remaining -= len;

	if (n_iov == 2) {
		if ((size_t) len >= iov[0].iov_len)
			device->header_offset += iov[0].iov_len;
		else
			device->header_offset += len;
	}

	for (i = 0; i < n_iov; i++) {
		if ((size_t) len < iov[i].iov_len) {
			iov[i].iov_len = len;
			n_iov = i;
			break;
		}

		len -= iov[i].iov_len;
	}

	l_util_hexdumpv(true, iov, n_iov,
				device->debug_handler, device->debug_data);

	if (device->segment_bytes_remaining > 0)
		return true;

	device->header_offset = 0;
	message = message_assembly_add(device->assembly, device->header,
					device->segment,
					L_LE32_TO_CPU(hdr->len) - header_size);
	device->segment = l_malloc(device->max_segment_size - HEADER_SIZE);

	if (!message)
		return true;

	dispatch_message(device, type, message);
	return true;
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

	device->is_ready = true;

	l_io_set_read_handler(device->io, command_read_handler, device, NULL);

	if (l_queue_length(device->pending_commands) > 0)
		l_io_set_write_handler(device->io, command_write_handler,
								device, NULL);

	return true;
}

static bool close_write_handler(struct l_io *io, void *user_data)
{
	struct mbim_device *device = user_data;
	ssize_t written;
	int fd;
	uint32_t buf[3];

	/* Fill out buf with a MBIM_CLOSE_MSG pdu */
	buf[0] = L_CPU_TO_LE32(MBIM_CLOSE_MSG);
	buf[1] = L_CPU_TO_LE32(sizeof(buf));
	buf[2] = L_CPU_TO_LE32(_mbim_device_get_next_tid(device));

	fd = l_io_get_fd(io);

	written = TEMP_FAILURE_RETRY(write(fd, buf, sizeof(buf)));
	if (written < 0)
		return false;

	l_util_hexdump(false, buf, written,
				device->debug_handler, device->debug_data);

	return false;
}

static void close_io(struct l_idle *idle, void *user_data)
{
	struct mbim_device *device = user_data;
	struct l_io *io = device->io;

	l_idle_remove(idle);
	device->close_io = NULL;

	device->io = NULL;
	l_io_destroy(io);
}

static bool close_read_handler(struct l_io *io, void *user_data)
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

	if (!device->segment_bytes_remaining) {
		if (type == MBIM_CLOSE_DONE)
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

	if (type == MBIM_CLOSE_DONE) {
		device->close_io = l_idle_create(close_io, device, NULL);
		return false;
	}

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
	device->next_notification = 1;

	device->segment = l_malloc(max_segment_size - HEADER_SIZE);

	device->io = l_io_new(fd);
	l_io_set_disconnect_handler(device->io, disconnect_handler,
								device, NULL);

	l_io_set_read_handler(device->io, open_read_handler, device, NULL);
	l_io_set_write_handler(device->io, open_write_handler, device, NULL);

	device->pending_commands = l_queue_new();
	device->sent_commands = l_queue_new();
	device->notifications = l_queue_new();
	device->assembly = message_assembly_new();

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

	l_idle_remove(device->close_io);

	if (device->io) {
		l_io_destroy(device->io);
		device->io = NULL;
	}

	l_free(device->segment);

	if (device->debug_destroy)
		device->debug_destroy(device->debug_data);

	if (device->disconnect_destroy)
		device->disconnect_destroy(device->disconnect_data);

	l_queue_destroy(device->pending_commands, pending_command_free);
	l_queue_destroy(device->sent_commands, pending_command_free);
	l_queue_destroy(device->notifications, notification_free);
	message_assembly_free(device->assembly);
	l_free(device);
}

bool mbim_device_shutdown(struct mbim_device *device)
{
	if (unlikely(!device))
		return false;

	l_io_set_read_handler(device->io, close_read_handler, device, NULL);
	l_io_set_write_handler(device->io, close_write_handler, device, NULL);

	device->is_ready = false;
	return true;
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

uint32_t mbim_device_send(struct mbim_device *device, uint32_t gid,
				struct mbim_message *message,
				mbim_device_reply_func_t function,
				void *user_data,
				mbim_device_destroy_func_t destroy)
{
	struct pending_command *pending;

	if (unlikely(!device || !message))
		return 0;

	pending = l_new(struct pending_command, 1);

	pending->tid = _mbim_device_get_next_tid(device);
	pending->gid = gid;
	pending->message = message;
	pending->callback = function;
	pending->destroy = destroy;
	pending->user_data = user_data;

	l_queue_push_tail(device->pending_commands, pending);

	if (!device->is_ready)
		goto done;

	if (l_queue_length(device->sent_commands) >= device->max_outstanding)
		goto done;

	l_io_set_write_handler(device->io, command_write_handler,
								device, NULL);
done:
	return pending->tid;
}

bool mbim_device_cancel(struct mbim_device *device, uint32_t tid)
{
	struct pending_command *pending;

	if (unlikely(!device))
		return false;

	pending = l_queue_remove_if(device->pending_commands,
					pending_command_match_tid,
					L_UINT_TO_PTR(tid));
	if (pending) {
		pending_command_free(pending);
		return true;
	}

	pending = l_queue_find(device->sent_commands,
					pending_command_match_tid,
					L_UINT_TO_PTR(tid));

	if (!pending)
		return false;

	pending_command_cancel(pending);
	return true;
}

bool mbim_device_cancel_group(struct mbim_device *device, uint32_t gid)
{
	if (unlikely(!device))
		return false;

	l_queue_foreach_remove(device->pending_commands,
					pending_command_free_by_gid,
					L_UINT_TO_PTR(gid));

	l_queue_foreach(device->sent_commands,
					pending_command_cancel_by_gid,
					L_UINT_TO_PTR(gid));

	return true;
}

uint32_t mbim_device_register(struct mbim_device *device, uint32_t gid,
				const uint8_t *uuid, uint32_t cid,
				mbim_device_reply_func_t notify,
				void *user_data,
				mbim_device_destroy_func_t destroy)
{
	struct notification *notification;
	uint32_t id;

	if (unlikely(!device))
		return 0;

	id = device->next_notification;

	if (device->next_notification == UINT_MAX)
		device->next_notification = 1;
	else
		device->next_notification += 1;

	notification = l_new(struct notification, 1);
	notification->id = id;
	notification->gid = gid;
	memcpy(notification->uuid, uuid, sizeof(notification->uuid));
	notification->cid = cid;
	notification->notify = notify;
	notification->destroy = destroy;
	notification->user_data = user_data;

	l_queue_push_tail(device->notifications, notification);

	return notification->id;
}

bool mbim_device_unregister(struct mbim_device *device, uint32_t id)
{
	struct notification *notification;

	if (unlikely(!device))
		return false;

	if (device->in_notify) {
		notification = l_queue_find(device->notifications,
						notification_match_id,
						L_UINT_TO_PTR(id));
		if (!notification)
			return false;

		notification->destroyed = true;
		return true;
	}

	notification = l_queue_remove_if(device->notifications,
					notification_match_id,
					L_UINT_TO_PTR(id));
	if (!notification)
		return false;

	notification_free(notification);
	return true;
}

bool mbim_device_unregister_group(struct mbim_device *device, uint32_t gid)
{
	const struct l_queue_entry *entry;
	bool r;

	if (unlikely(!device))
		return false;

	if (!device->in_notify)
		return l_queue_foreach_remove(device->notifications,
					notification_free_by_gid,
					L_UINT_TO_PTR(gid)) > 0;

	entry = l_queue_get_entries(device->notifications);
	r = false;

	while (entry) {
		struct notification *notification = entry->data;

		if (notification->gid == gid) {
			notification->destroyed = true;
			r = true;
		}

		entry = entry->next;
	}

	return r;
}
