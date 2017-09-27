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

#include <sys/uio.h>
#include <linux/types.h>

#include <ell/ell.h>

#include "mbim-message.h"
#include "mbim-private.h"

#define HEADER_SIZE (sizeof(struct mbim_message_header) + \
					sizeof(struct mbim_fragment_header))

static const char CONTAINER_TYPE_ARRAY	= 'a';
static const char CONTAINER_TYPE_STRUCT	= 'r';

struct mbim_message {
	int ref_count;
	uint8_t header[HEADER_SIZE];
	struct iovec *frags;
	uint32_t n_frags;
	uint8_t uuid[16];
	uint32_t cid;
	uint32_t status;
	uint32_t info_buf_len;

	bool sealed : 1;
};

static const char *_signature_end(const char *signature)
{
	const char *ptr = signature;
	unsigned int indent = 0;
	char expect;

	switch (*signature) {
	case '(':
		expect = ')';
		break;
	case 'a':
		return _signature_end(signature + 1);
	case '0' ... '9':
		expect = 'y';
		break;
	default:
		return signature;
	}

	for (ptr = signature; *ptr != '\0'; ptr++) {
		if (*ptr == *signature)
			indent++;
		else if (*ptr == expect)
			if (!--indent)
				return ptr;
	}

	return NULL;
}

static int get_alignment(const char type)
{
	switch (type) {
	case 'y':
		return 1;
	case 'q':
		return 2;
	case 'u':
	case 's':
		return 4;
	case 'a':
		return 4;
	default:
		return 0;
	}
}

static bool is_fixed_size(const char *sig_start, const char *sig_end)
{
	while (sig_start <= sig_end) {
		if (*sig_start == 'a' || *sig_start == 's')
			return false;

		sig_start++;
	}

	return true;
}

static inline const void *_iter_get_data(struct mbim_message_iter *iter,
						size_t pos)
{
	pos = iter->base_offset + pos;

	while (pos >= iter->cur_iov_offset + iter->iov[iter->cur_iov].iov_len) {
		iter->cur_iov_offset += iter->iov[iter->cur_iov].iov_len;
		iter->cur_iov += 1;
	}

	return iter->iov[iter->cur_iov].iov_base + pos - iter->cur_iov_offset;
}

static bool _iter_copy_string(struct mbim_message_iter *iter,
					uint32_t offset, uint32_t len,
					char **out)
{
	uint8_t buf[len];
	uint8_t *dest = buf;
	uint32_t remaining = len;
	uint32_t iov_start = 0;
	uint32_t i = 0;
	uint32_t tocopy;

	if (!len) {
		*out = NULL;
		return true;
	}

	if (offset + len > iter->len)
		return false;

	offset += iter->base_offset;

	while (offset >= iov_start + iter->iov[i].iov_len)
		iov_start += iter->iov[i++].iov_len;

	tocopy = iter->iov[i].iov_len - (offset - iov_start);

	if (tocopy > remaining)
		tocopy = remaining;

	memcpy(dest, iter->iov[i].iov_base + offset - iov_start, tocopy);
	remaining -= tocopy;
	dest += tocopy;
	i += 1;

	while (remaining) {
		tocopy = remaining;

		if (remaining > iter->iov[i].iov_len)
			tocopy = iter->iov[i].iov_len;

		memcpy(dest, iter->iov[i].iov_base, tocopy);
		remaining -= tocopy;
		dest += tocopy;
	}

	/* Strings are in UTF16-LE, so convert to UTF16-CPU first if needed */
	if (L_CPU_TO_LE16(0x8000) != 0x8000) {
		uint16_t *le = (uint16_t *) buf;

		for (i = 0; i < len; i+= 2)
			le[i] = __builtin_bswap16(le[i]);
	}

	*out = l_utf8_from_utf16(buf, len);
	return true;
}

static inline void _iter_init_internal(struct mbim_message_iter *iter,
					char container_type,
					const char *sig_start,
					const char *sig_end,
					const struct iovec *iov, uint32_t n_iov,
					size_t len, size_t base_offset,
					size_t pos, uint32_t n_elem)
{
	size_t sig_len;

	if (sig_end)
		sig_len = sig_end - sig_start;
	else
		sig_len = strlen(sig_start);

	iter->sig_start = sig_start;
	iter->sig_len = sig_len;
	iter->sig_pos = 0;
	iter->iov = iov;
	iter->n_iov = n_iov;
	iter->cur_iov = 0;
	iter->cur_iov_offset = 0;
	iter->len = len;
	iter->base_offset = base_offset;
	iter->pos = pos;
	iter->n_elem = n_elem;
	iter->container_type = container_type;

	_iter_get_data(iter, iter->pos);
}

static bool _iter_next_entry_basic(struct mbim_message_iter *iter,
							char type, void *out)
{
	uint8_t uint8_val;
	uint16_t uint16_val;
	uint32_t uint32_val;
	uint32_t offset, length;
	const void *data;
	size_t pos;

	if (iter->container_type == CONTAINER_TYPE_ARRAY && !iter->n_elem)
		return false;

	if (iter->pos >= iter->len)
		return false;

	pos = align_len(iter->pos, get_alignment(type));

	switch (type) {
	case 'y':
		if (pos + 1 > iter->len)
			return false;

		data = _iter_get_data(iter, pos);
		uint8_val = l_get_u8(data);
		*(uint8_t *) out = uint8_val;
		iter->pos = pos + 1;
		break;
	case 'q':
		if (pos + 2 > iter->len)
			return false;
		data = _iter_get_data(iter, pos);
		uint16_val = l_get_le16(data);
		*(uint16_t *) out = uint16_val;
		iter->pos = pos + 2;
		break;
	case 'u':
		if (pos + 4 > iter->len)
			return false;
		data = _iter_get_data(iter, pos);
		uint32_val = l_get_le32(data);
		*(uint32_t *) out = uint32_val;
		iter->pos = pos + 4;
		break;
	case 's':
		/*
		 * String consists of two uint32_t values:
		 * offset followed by length
		 */
		if (pos + 8 > iter->len)
			return false;

		data = _iter_get_data(iter, pos);
		offset = l_get_le32(data);
		data = _iter_get_data(iter, pos + 4);
		length = l_get_le32(data);

		if (!_iter_copy_string(iter, offset, length, out))
			return false;

		iter->pos = pos + 8;
		break;
	default:
		return false;
	}

	if (iter->container_type != CONTAINER_TYPE_ARRAY)
		iter->sig_pos += 1;

	return true;
}

static bool _iter_enter_array(struct mbim_message_iter *iter,
					struct mbim_message_iter *array)
{
	size_t pos;
	uint32_t n_elem;
	const char *sig_start;
	const char *sig_end;
	const void *data;
	bool fixed;
	uint32_t offset;

	if (iter->sig_start[iter->sig_pos] != 'a')
		return false;

	sig_start = iter->sig_start + iter->sig_pos + 1;
	sig_end = _signature_end(sig_start) + 1;

	/*
	 * Two possibilities:
	 * 1. Element Count, followed by OL_PAIR_LIST
	 * 2. Offset, followed by element length or size for raw buffers
	 */
	fixed = is_fixed_size(sig_start, sig_end);

	if (fixed) {
		pos = align_len(iter->pos, 4);
		if (pos + 4 > iter->len)
			return false;

		data = _iter_get_data(iter, pos);
		offset = l_get_le32(data);
		pos += 4;
	}

	pos = align_len(iter->pos, 4);
	if (pos + 4 > iter->len)
		return false;

	data = _iter_get_data(iter, pos);
	n_elem = l_get_le32(data);
	pos += 4;

	if (iter->container_type != CONTAINER_TYPE_ARRAY)
		iter->sig_pos += sig_end - sig_start + 1;

	if (fixed) {
		_iter_init_internal(array, CONTAINER_TYPE_ARRAY,
					sig_start, sig_end,
					iter->iov, iter->n_iov,
					iter->len, iter->base_offset,
					offset, n_elem);
		return true;
	}

	_iter_init_internal(array, CONTAINER_TYPE_ARRAY, sig_start, sig_end,
				iter->iov, iter->n_iov,
				iter->len, iter->base_offset, pos, n_elem);

	iter->pos = pos + 8 * n_elem;

	return true;
}

static bool message_iter_next_entry_valist(struct mbim_message_iter *orig,
						va_list args)
{
	static const char *simple_types = "syqu";
	struct mbim_message_iter *iter = orig;
	const char *signature = orig->sig_start + orig->sig_pos;
	const char *end;
	uint32_t *out_n_elem;
	struct mbim_message_iter *sub_iter;
	void *arg;

	while (signature < orig->sig_start + orig->sig_len) {
		if (strchr(simple_types, *signature)) {
			arg = va_arg(args, void *);
			if (!_iter_next_entry_basic(iter, *signature, arg))
				return false;

			signature += 1;
			continue;
		}

		switch (*signature) {
		case '0' ... '9':
		{
			uint32_t i;
			uint32_t n_elem;
			size_t pos;
			const void *src;

			if (iter->pos >= iter->len)
				return false;

			pos = align_len(iter->pos, 4);
			end = _signature_end(signature);
			n_elem = strtol(signature, NULL, 10);

			if (pos + n_elem > iter->len)
				return false;

			arg = va_arg(args, uint8_t *);

			for (i = 0; i + 4 < n_elem; i += 4) {
				src = _iter_get_data(iter, pos + i);
				memcpy(arg + i, src, 4);
			}

			src = _iter_get_data(iter, pos + i);
			memcpy(arg + i, src, n_elem - i);
			iter->pos = pos + n_elem;
			signature = end + 1;
			break;
		}
		case 'a':
			out_n_elem = va_arg(args, uint32_t *);
			sub_iter = va_arg(args, void *);

			if (!_iter_enter_array(iter, sub_iter))
				return false;

			*out_n_elem = sub_iter->n_elem;

			end = _signature_end(signature + 1);
			signature = end + 1;
			break;
		default:
			return false;
		}
	}

	if (iter->container_type == CONTAINER_TYPE_ARRAY)
		iter->n_elem -= 1;

	return true;
}

bool mbim_message_iter_next_entry(struct mbim_message_iter *iter, ...)
{
	va_list args;
	bool result;

	if (unlikely(!iter))
		return false;

	va_start(args, iter);
	result = message_iter_next_entry_valist(iter, args);
	va_end(args);

	return result;
}

uint32_t _mbim_information_buffer_offset(uint32_t type)
{
	switch (type) {
	case MBIM_COMMAND_MSG:
	case MBIM_COMMAND_DONE:
		return 28;
	case MBIM_INDICATE_STATUS_MSG:
		return 24;
	}

	return 0;
}

struct mbim_message *mbim_message_new(const uint8_t *uuid, uint32_t cid)
{
	struct mbim_message *msg;

	msg = l_new(struct mbim_message, 1);

	return mbim_message_ref(msg);
}

struct mbim_message *mbim_message_ref(struct mbim_message *msg)
{
	if (unlikely(!msg))
		return NULL;

	__sync_fetch_and_add(&msg->ref_count, 1);

	return msg;
}

void mbim_message_unref(struct mbim_message *msg)
{
	unsigned int i;

	if (unlikely(!msg))
		return;

	if (__sync_sub_and_fetch(&msg->ref_count, 1))
		return;

	for (i = 0; i < msg->n_frags; i++)
		l_free(msg->frags[i].iov_base);

	l_free(msg->frags);
	l_free(msg);
}

struct mbim_message *_mbim_message_build(const void *header,
						struct iovec *frags,
						uint32_t n_frags)
{
	struct mbim_message *msg;
	struct mbim_message_header *hdr = (struct mbim_message_header *) header;
	struct mbim_message_iter iter;
	bool r = false;

	msg = l_new(struct mbim_message, 1);

	msg->ref_count = 1;
	memcpy(msg->header, header, HEADER_SIZE);
	msg->frags = frags;
	msg->n_frags = n_frags;
	msg->sealed = true;

	switch (L_LE32_TO_CPU(hdr->type)) {
	case MBIM_COMMAND_DONE:
		_iter_init_internal(&iter, CONTAINER_TYPE_STRUCT,
						"16yuuu", NULL,
						frags, n_frags,
						frags[0].iov_len, 0, 0, 0);
		r = mbim_message_iter_next_entry(&iter, msg->uuid, &msg->cid,
						&msg->status,
						&msg->info_buf_len);
		break;
	case MBIM_INDICATE_STATUS_MSG:
		break;
	default:
		break;
	}

	if (!r) {
		l_free(msg);
		msg = NULL;
	}

	return msg;
}

bool mbim_message_get_arguments(struct mbim_message *message,
						const char *signature, ...)
{
	struct mbim_message_iter iter;
	va_list args;
	bool result;
	struct mbim_message_header *hdr;
	uint32_t type;
	size_t begin;

	if (unlikely(!message))
		return false;

	if (unlikely(!message->sealed))
		return false;

	hdr = (struct mbim_message_header *) message->header;
	type = L_LE32_TO_CPU(hdr->type);
	begin = _mbim_information_buffer_offset(type);

	_iter_init_internal(&iter, CONTAINER_TYPE_STRUCT,
				signature, NULL,
				message->frags, message->n_frags,
				message->info_buf_len, begin, 0, 0);

	va_start(args, signature);
	result = message_iter_next_entry_valist(&iter, args);
	va_end(args);

	return result;
}
