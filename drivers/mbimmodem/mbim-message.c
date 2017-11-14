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

#define MAX_NESTING 2 /* a(uss) */
#define HEADER_SIZE (sizeof(struct mbim_message_header) + \
					sizeof(struct mbim_fragment_header))

static const char CONTAINER_TYPE_ARRAY	= 'a';
static const char CONTAINER_TYPE_STRUCT	= 'r';
static const char CONTAINER_TYPE_DATABUF = 'd';
static const char *simple_types = "syqut";

struct mbim_message {
	int ref_count;
	uint8_t header[HEADER_SIZE];
	struct iovec *frags;
	uint32_t n_frags;
	uint8_t uuid[16];
	uint32_t cid;
	union {
		uint32_t status;
		uint32_t command_type;
	};
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
	case 't':
		return 4;
	case 'a':
		return 4;
	case 'v':
		return 4;
	default:
		return 0;
	}
}

static int get_basic_size(const char type)
{
	switch (type) {
	case 'y':
		return 1;
	case 'q':
		return 2;
	case 'u':
		return 4;
	case 't':
		return 8;
	default:
		return 0;
	}
}

static bool is_fixed_size(const char *sig_start, const char *sig_end)
{
	while (sig_start <= sig_end) {
		if (*sig_start == 'a' || *sig_start == 's' || *sig_start == 'v')
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
}

static bool _iter_next_entry_basic(struct mbim_message_iter *iter,
							char type, void *out)
{
	uint8_t uint8_val;
	uint16_t uint16_val;
	uint32_t uint32_val;
	uint64_t uint64_val;
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
	case 't':
		if (pos + 8 > iter->len)
			return false;
		data = _iter_get_data(iter, pos);
		uint64_val = l_get_le64(data);
		*(uint64_t *) out = uint64_val;
		iter->pos = pos + 8;
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

	if (iter->container_type == CONTAINER_TYPE_ARRAY && !iter->n_elem)
		return false;

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
		iter->pos += 4;
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

static bool _iter_enter_struct(struct mbim_message_iter *iter,
					struct mbim_message_iter *structure)
{
	size_t offset;
	size_t len;
	size_t pos;
	const char *sig_start;
	const char *sig_end;
	const void *data;

	if (iter->container_type == CONTAINER_TYPE_ARRAY && !iter->n_elem)
		return false;

	if (iter->sig_start[iter->sig_pos] != '(')
		return false;

	sig_start = iter->sig_start + iter->sig_pos + 1;
	sig_end = _signature_end(iter->sig_start + iter->sig_pos);

	/* TODO: support fixed size structures */
	if (is_fixed_size(sig_start, sig_end))
		return false;

	pos = align_len(iter->pos, 4);
	if (pos + 8 > iter->len)
		return false;

	data = _iter_get_data(iter, pos);
	offset = l_get_le32(data);
	pos += 4;
	data = _iter_get_data(iter, pos);
	len = l_get_le32(data);

	_iter_init_internal(structure, CONTAINER_TYPE_STRUCT,
				sig_start, sig_end, iter->iov, iter->n_iov,
				len, iter->base_offset + offset, 0, 0);

	if (iter->container_type != CONTAINER_TYPE_ARRAY)
		iter->sig_pos += sig_end - sig_start + 2;

	iter->pos = pos + 4;

	return true;
}

static bool _iter_enter_databuf(struct mbim_message_iter *iter,
					const char *signature,
					struct mbim_message_iter *databuf)
{
	if (iter->container_type != CONTAINER_TYPE_STRUCT)
		return false;

	_iter_init_internal(databuf, CONTAINER_TYPE_DATABUF,
				signature, NULL, iter->iov, iter->n_iov,
				iter->len - iter->pos,
				iter->base_offset + iter->pos, 0, 0);

	iter->pos = iter->len;

	return true;
}

static bool message_iter_next_entry_valist(struct mbim_message_iter *orig,
						va_list args)
{
	struct mbim_message_iter *iter = orig;
	const char *signature = orig->sig_start + orig->sig_pos;
	const char *end;
	uint32_t *out_n_elem;
	struct mbim_message_iter *sub_iter;
	struct mbim_message_iter stack[MAX_NESTING];
	unsigned int indent = 0;
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
		case '(':
			signature += 1;
			indent += 1;

			if (unlikely(indent > MAX_NESTING))
				return false;

			if (!_iter_enter_struct(iter, &stack[indent - 1]))
				return false;

			iter = &stack[indent - 1];

			break;
		case ')':
			if (unlikely(indent == 0))
				return false;

			signature += 1;
			indent -= 1;

			if (indent == 0)
				iter = orig;
			else
				iter = &stack[indent - 1];
			break;
		case 'a':
			out_n_elem = va_arg(args, uint32_t *);
			sub_iter = va_arg(args, void *);

			if (!_iter_enter_array(iter, sub_iter))
				return false;

			*out_n_elem = sub_iter->n_elem;

			end = _signature_end(signature + 1);
			signature = end + 1;
			break;
		case 'd':
		{
			const char *s = va_arg(args, const char *);
			sub_iter = va_arg(args, void *);

			if (!_iter_enter_databuf(iter, s, sub_iter))
				return false;

			signature += 1;
			break;
		}

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

static struct mbim_message *_mbim_message_new_common(uint32_t type,
							const uint8_t *uuid,
							uint32_t cid)
{
	struct mbim_message *msg;
	struct mbim_message_header *hdr;
	struct mbim_fragment_header *frag;

	msg = l_new(struct mbim_message, 1);
	hdr = (struct mbim_message_header *) msg->header;
	hdr->type = L_CPU_TO_LE32(type);

	frag = (struct mbim_fragment_header *) (msg->header + sizeof(*hdr));
	frag->num_frags = L_CPU_TO_LE32(1);
	frag->cur_frag = L_CPU_TO_LE32(0);

	memcpy(msg->uuid, uuid, 16);
	msg->cid = cid;

	return mbim_message_ref(msg);
}

struct mbim_message *_mbim_message_new_command_done(const uint8_t *uuid,
					uint32_t cid, uint32_t status)
{
	struct mbim_message *message =
		_mbim_message_new_common(MBIM_COMMAND_DONE, uuid, cid);

	if (!message)
		return NULL;

	message->status = status;

	return message;
}

void _mbim_message_set_tid(struct mbim_message *message, uint32_t tid)
{
	struct mbim_message_header *hdr =
				(struct mbim_message_header *) message->header;

	hdr->tid = L_CPU_TO_LE32(tid);
}

void *_mbim_message_to_bytearray(struct mbim_message *message, size_t *out_len)
{
	unsigned int i;
	struct mbim_message_header *hdr;
	void *binary;
	size_t pos;
	size_t len;

	if (!message->sealed)
		return NULL;

	hdr = (struct mbim_message_header *) message->header;
	len = L_LE32_TO_CPU(hdr->len);
	binary = l_malloc(len);

	memcpy(binary, message->header, HEADER_SIZE);
	pos = HEADER_SIZE;

	for (i = 0; i < message->n_frags; i++) {
		memcpy(binary + pos, message->frags[i].iov_base,
				message->frags[i].iov_len);
		pos += message->frags[i].iov_len;
	}

	if (out_len)
		*out_len = len;

	return binary;
}

struct mbim_message *mbim_message_new(const uint8_t *uuid, uint32_t cid,
					enum mbim_command_type type)
{
	struct mbim_message *message =
		_mbim_message_new_common(MBIM_COMMAND_MSG, uuid, cid);

	if (!message)
		return NULL;

	message->command_type = type;

	return message;
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
	case MBIM_COMMAND_MSG:
		_iter_init_internal(&iter, CONTAINER_TYPE_STRUCT,
						"16yuuu", NULL,
						frags, n_frags,
						frags[0].iov_len, 0, 0, 0);
		r = mbim_message_iter_next_entry(&iter, msg->uuid, &msg->cid,
						&msg->command_type,
						&msg->info_buf_len);
		break;
	case MBIM_INDICATE_STATUS_MSG:
		_iter_init_internal(&iter, CONTAINER_TYPE_STRUCT,
						"16yuu", NULL,
						frags, n_frags,
						frags[0].iov_len, 0, 0, 0);
		r = mbim_message_iter_next_entry(&iter, msg->uuid, &msg->cid,
						&msg->info_buf_len);
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

uint32_t mbim_message_get_error(struct mbim_message *message)
{
	struct mbim_message_header *hdr;

	if (unlikely(!message))
		return false;

	if (unlikely(!message->sealed))
		return false;

	hdr = (struct mbim_message_header *) message->header;

	if (L_LE32_TO_CPU(hdr->type) != MBIM_COMMAND_DONE)
		return 0;

	return message->status;
}

uint32_t mbim_message_get_cid(struct mbim_message *message)
{
	if (unlikely(!message))
		return false;

	return message->cid;
}

const uint8_t *mbim_message_get_uuid(struct mbim_message *message)
{
	if (unlikely(!message))
		return false;

	return message->uuid;
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

static bool _mbim_message_get_data(struct mbim_message *message,
					uint32_t offset,
					void *dest, size_t len)
{
	struct mbim_message_iter iter;
	struct mbim_message_header *hdr;
	uint32_t type;
	size_t begin;
	const void *src;
	size_t pos;
	uint32_t i;

	if (unlikely(!message))
		return false;

	if (unlikely(!message->sealed))
		return false;

	hdr = (struct mbim_message_header *) message->header;
	type = L_LE32_TO_CPU(hdr->type);
	begin = _mbim_information_buffer_offset(type);

	_iter_init_internal(&iter, CONTAINER_TYPE_STRUCT,
				"", NULL,
				message->frags, message->n_frags,
				message->info_buf_len, begin, offset, 0);

	pos = align_len(iter.pos, 4);
	if (pos + len > iter.len)
		return false;

	for (i = 0; i + 4 < len; i += 4) {
		src = _iter_get_data(&iter, pos + i);
		memcpy(dest + i, src, 4);
	}

	src = _iter_get_data(&iter, pos + i);
	memcpy(dest + i, src, len - i);

	return true;
}

bool mbim_message_get_ipv4_address(struct mbim_message *message,
					uint32_t offset,
					struct in_addr *addr)
{
	return _mbim_message_get_data(message, offset, &addr->s_addr, 4);
}

bool mbim_message_get_ipv4_element(struct mbim_message *message,
					uint32_t offset,
					uint32_t *prefix_len,
					struct in_addr *addr)
{
	uint8_t buf[8];

	if (!_mbim_message_get_data(message, offset, buf, 8))
		return false;

	*prefix_len = l_get_le32(buf);
	memcpy(&addr->s_addr, buf + 4, 4);

	return true;
}

bool mbim_message_get_ipv6_address(struct mbim_message *message,
					uint32_t offset,
					struct in6_addr *addr)
{
	return _mbim_message_get_data(message, offset, addr->s6_addr, 16);
}

bool mbim_message_get_ipv6_element(struct mbim_message *message,
					uint32_t offset,
					uint32_t *prefix_len,
					struct in6_addr *addr)
{
	uint8_t buf[20];

	if (!_mbim_message_get_data(message, offset, buf, 20))
		return false;

	*prefix_len = l_get_le32(buf);
	memcpy(&addr->s6_addr, buf + 4, 16);

	return true;
}

struct container {
	void *sbuf;		/* static buffer */
	size_t sbuf_size;
	size_t sbuf_pos;
	void *dbuf;		/* data buffer */
	size_t dbuf_size;
	size_t dbuf_pos;
	void *obuf;		/* offset buffer */
	size_t obuf_size;
	size_t obuf_pos;
	char container_type;
	char signature[64];
	uint8_t sigindex;
	uint32_t base_offset;
	uint32_t array_start;
};

static void container_update_offsets(struct container *container)
{
	size_t i;

	if (!container->obuf)
		return;

	for (i = 0; i < container->obuf_pos; i += 4) {
		uint32_t sbuf_offset = l_get_u32(container->obuf + i);
		uint32_t dbuf_offset = l_get_u32(container->sbuf + sbuf_offset);

		dbuf_offset += container->sbuf_pos - container->base_offset;
		l_put_le32(dbuf_offset, container->sbuf + sbuf_offset);
	}

	l_free(container->obuf);
	container->obuf = NULL;
	container->obuf_pos = 0;
	container->obuf_size = 0;
}

struct mbim_message_builder {
	struct mbim_message *message;
	struct container stack[MAX_NESTING + 1];
	uint32_t index;
};

static inline size_t grow_buf(void **buf, size_t *buf_size, size_t *pos,
				size_t len, unsigned int alignment)
{
	size_t size = align_len(*pos, alignment);

	if (size + len > *buf_size) {
		*buf = l_realloc(*buf, size + len);
		*buf_size = size + len;
	}

	if (size - *pos > 0)
		memset(*buf + *pos, 0, size - *pos);

	*pos = size + len;
	return size;
}

#define GROW_SBUF(c, len, alignment) \
	grow_buf(&c->sbuf, &c->sbuf_size, &c->sbuf_pos, \
			len, alignment)

#define GROW_DBUF(c, len, alignment) \
	grow_buf(&c->dbuf, &c->dbuf_size, &c->dbuf_pos, \
			len, alignment)

#define GROW_OBUF(c) \
	grow_buf(&c->obuf, &c->obuf_size, &c->obuf_pos, 4, 4)

static void add_offset_and_length(struct container *container,
					uint32_t offset, uint32_t len)
{
	size_t start;
	/*
	 * note the relative offset in the data buffer.  Store it in native
	 * endian order for now.  It will be fixed up later once we finalize
	 * the structure
	 */
	start = GROW_SBUF(container, 8, 4);
	l_put_u32(offset, container->sbuf + start);
	l_put_le32(len, container->sbuf + start + 4);

	/* Make a note in offset buffer to update the offset at this position */
	offset = start;
	start = GROW_OBUF(container);
	l_put_u32(offset, container->obuf + start);
}

struct mbim_message_builder *mbim_message_builder_new(struct mbim_message *msg)
{
	struct mbim_message_builder *ret;
	struct mbim_message_header *hdr;
	uint32_t type;
	struct container *container;

	if (unlikely(!msg))
		return NULL;

	if (msg->sealed)
		return NULL;

	hdr = (struct mbim_message_header *) msg->header;
	type = L_LE32_TO_CPU(hdr->type);

	ret = l_new(struct mbim_message_builder, 1);
	ret->message = mbim_message_ref(msg);

	/* Reserve space in the static buffer for UUID, CID, Status, etc */
	container = &ret->stack[ret->index];
	container->base_offset = _mbim_information_buffer_offset(type);
	container->container_type = CONTAINER_TYPE_STRUCT;
	GROW_SBUF(container, container->base_offset, 0);

	return ret;
}

void mbim_message_builder_free(struct mbim_message_builder *builder)
{
	uint32_t i;

	if (unlikely(!builder))
		return;

	mbim_message_unref(builder->message);

	for (i = 0; i <= builder->index; i++) {
		if (builder->stack[i].container_type == CONTAINER_TYPE_ARRAY)
			continue;

		l_free(builder->stack[i].sbuf);
		l_free(builder->stack[i].dbuf);
		l_free(builder->stack[i].obuf);
	}

	l_free(builder);
}

bool mbim_message_builder_append_basic(struct mbim_message_builder *builder,
					char type, const void *value)
{
	struct container *container = &builder->stack[builder->index];
	struct container *array = NULL;
	size_t start;
	unsigned int alignment;
	size_t len;
	uint16_t *utf16;

	if (unlikely(!builder))
		return false;

	if (unlikely(!strchr(simple_types, type)))
		return false;

	alignment = get_alignment(type);
	if (!alignment)
		return false;

	if (builder->index > 0 &&
			container->signature[container->sigindex] != type)
		return false;

	len = get_basic_size(type);

	if (container->container_type == CONTAINER_TYPE_ARRAY) {
		array = container;
		container = &builder->stack[builder->index - 1];
	}

	if (len) {
		uint16_t swapped_u16;
		uint32_t swapped_u32;
		uint64_t swapped_u64;

		switch (len) {
		case 2:
			swapped_u16 = L_CPU_TO_LE16(l_get_u16(value));
			value = &swapped_u16;
			break;
		case 4:
			swapped_u32 = L_CPU_TO_LE32(l_get_u32(value));
			value = &swapped_u32;
			break;
		case 8:
			swapped_u64 = L_CPU_TO_LE64(l_get_u64(value));
			value = &swapped_u64;
			break;
		}

		if (array) {
			uint32_t n_elem = l_get_le32(container->sbuf +
					array->array_start + 4);
			start = GROW_DBUF(container, len, alignment);
			memcpy(container->dbuf + start, value, len);
			l_put_le32(n_elem + 1,
				container->sbuf + array->array_start + 4);
		} else {
			start = GROW_SBUF(container, len, alignment);
			memcpy(container->sbuf + start, value, len);
		}

		goto done;
	}

	/* Null string? */
	if (!value) {
		start = GROW_SBUF(container, 8, 4);
		l_put_le32(0, container->sbuf + start);
		l_put_le32(0, container->sbuf + start + 4);
		goto done;
	}

	utf16 = l_utf8_to_utf16(value, &len);
	if (!utf16)
		return false;

	/* Strings are in UTF16-LE, so convert if needed */
	if (L_CPU_TO_LE16(0x8000) != 0x8000) {
		size_t i;

		for (i = 0; i < len - 2; i += 2)
			utf16[i] = __builtin_bswap16(utf16[i]);
	}

	/*
	 * First grow the data buffer.
	 * MBIM v1.0-errata1, Section 10.3:
	 * "If the size of the payload in the variable field is not a multiple
	 * of 4 bytes, the field shall be padded up to the next 4 byte multiple.
	 * This shall be true even for the last payload in DataBuffer."
	 */
	start = GROW_DBUF(container, len - 2, 4);
	memcpy(container->dbuf + start, utf16, len - 2);
	l_free(utf16);

	add_offset_and_length(container, start, len - 2);

	if (array) {
		uint32_t n_elem = l_get_le32(container->sbuf +
						array->array_start);
		l_put_le32(n_elem + 1,
				container->sbuf + array->array_start);
	}
done:
	if (!array)
		container->sigindex += 1;

	return true;
}

bool mbim_message_builder_append_bytes(struct mbim_message_builder *builder,
					size_t len, const uint8_t *bytes)
{
	struct container *container = &builder->stack[builder->index];
	size_t start;

	if (unlikely(!builder))
		return false;

	if (container->container_type == CONTAINER_TYPE_ARRAY) {
		struct container *array;

		if (unlikely(container->sigindex != 0))
			return false;

		if (unlikely(container->signature[container->sigindex] != 'y'))
			return false;

		array = container;
		container = &builder->stack[builder->index - 1];

		start = GROW_DBUF(container, len, 1);
		memcpy(container->dbuf + start, bytes, len);
		l_put_le32(len, container->sbuf + array->array_start + 4);

		return true;
	} else if (container->container_type == CONTAINER_TYPE_STRUCT) {
		if (builder->index > 0) {
			unsigned int i = container->sigindex;
			const char *sig = container->signature + i;
			size_t n_elem;
			const char *sigend;

			if (*sig < '0' || *sig > '9')
				return false;

			n_elem = strtol(sig, NULL, 10);
			if (n_elem != len)
				return false;

			sigend = _signature_end(sig);
			if (!sigend)
				return false;

			container->sigindex += sigend - sig + 1;
		}

		start = GROW_SBUF(container, len, 1);
		memcpy(container->sbuf + start, bytes, len);

		return true;
	}

	return false;
}

bool mbim_message_builder_enter_struct(struct mbim_message_builder *builder,
					const char *signature)
{
	struct container *container;

	if (strlen(signature) > sizeof(((struct container *) 0)->signature) - 1)
		return false;

	if (builder->index == L_ARRAY_SIZE(builder->stack) - 1)
		return false;

	builder->index += 1;

	container = &builder->stack[builder->index];
	memset(container, 0, sizeof(*container));
	strcpy(container->signature, signature);
	container->sigindex = 0;
	container->container_type = CONTAINER_TYPE_STRUCT;

	return true;
}

bool mbim_message_builder_leave_struct(struct mbim_message_builder *builder)
{
	struct container *container;
	struct container *parent;
	struct container *array = NULL;
	size_t start;

	if (unlikely(builder->index == 0))
		return false;

	container = &builder->stack[builder->index];

	if (unlikely(container->container_type != CONTAINER_TYPE_STRUCT))
		return false;

	builder->index -= 1;
	parent = &builder->stack[builder->index];
	GROW_DBUF(container, 0, 4);
	container_update_offsets(container);

	if (parent->container_type == CONTAINER_TYPE_ARRAY) {
		array = parent;
		parent = &builder->stack[builder->index - 1];
	}

	/*
	 * Copy the structure buffers into parent's buffers
	 */
	start = GROW_DBUF(parent, container->sbuf_pos + container->dbuf_pos, 4);
	memcpy(parent->dbuf + start, container->sbuf, container->sbuf_pos);
	memcpy(parent->dbuf + start + container->sbuf_pos,
				container->dbuf, container->dbuf_pos);
	l_free(container->sbuf);
	l_free(container->dbuf);

	add_offset_and_length(parent, start,
			container->sbuf_pos + container->dbuf_pos);

	if (array) {
		uint32_t n_elem = l_get_le32(parent->sbuf +
						array->array_start);
		l_put_le32(n_elem + 1,
				parent->sbuf + array->array_start);
	}

	memset(container, 0, sizeof(*container));

	return true;
}

bool mbim_message_builder_enter_array(struct mbim_message_builder *builder,
					const char *signature)
{
	struct container *parent;
	struct container *container;

	if (strlen(signature) > sizeof(((struct container *) 0)->signature) - 1)
		return false;

	if (builder->index == L_ARRAY_SIZE(builder->stack) - 1)
		return false;

	/*
	 * TODO: validate that arrays consist of a single simple type or
	 * a single struct
	 */
	parent = &builder->stack[builder->index++];
	container = &builder->stack[builder->index];

	/* Arrays add on to the parent's buffers */
	container->container_type = CONTAINER_TYPE_ARRAY;
	strcpy(container->signature, signature);
	container->sigindex = 0;

	/* First grow the body enough to cover preceding length */
	container->array_start = GROW_SBUF(parent, 4, 4);
	l_put_le32(0, parent->sbuf + container->array_start);

	/* For arrays of fixed-size elements, it is offset followed by length */
	if (is_fixed_size(container->signature,
				_signature_end(container->signature))) {
		/* Note down offset into the data buffer */
		size_t start = GROW_DBUF(parent, 0, 4);
		l_put_u32(start, parent->sbuf + container->array_start);
		/* Set length to 0 */
		start = GROW_SBUF(parent, 4, 4);
		l_put_le32(0, parent->sbuf + start);
		/* Note down offset position to recalculate */
		start = GROW_OBUF(parent);
		l_put_u32(container->array_start, parent->obuf + start);
	}

	return true;
}

bool mbim_message_builder_leave_array(struct mbim_message_builder *builder)
{
	struct container *container;

	if (unlikely(builder->index == 0))
		return false;

	container = &builder->stack[builder->index];

	if (unlikely(container->container_type != CONTAINER_TYPE_ARRAY))
		return false;

	builder->index -= 1;
	memset(container, 0, sizeof(*container));

	return true;
}

bool mbim_message_builder_enter_databuf(struct mbim_message_builder *builder,
					const char *signature)
{
	struct container *container;

	if (strlen(signature) > sizeof(((struct container *) 0)->signature) - 1)
		return false;

	if (builder->index != 0)
		return false;

	builder->index += 1;

	container = &builder->stack[builder->index];
	memset(container, 0, sizeof(*container));
	strcpy(container->signature, signature);
	container->sigindex = 0;
	container->container_type = CONTAINER_TYPE_DATABUF;

	return true;
}

bool mbim_message_builder_leave_databuf(struct mbim_message_builder *builder)
{
	struct container *container;
	struct container *parent;
	size_t start;

	if (unlikely(builder->index == 0))
		return false;

	container = &builder->stack[builder->index];

	if (unlikely(container->container_type != CONTAINER_TYPE_DATABUF))
		return false;

	builder->index -= 1;
	parent = &builder->stack[builder->index];
	GROW_DBUF(container, 0, 4);
	container_update_offsets(container);

	/*
	 * Copy the structure buffers into parent's buffers
	 */
	start = GROW_SBUF(parent, container->sbuf_pos + container->dbuf_pos, 4);
	memcpy(parent->sbuf + start, container->sbuf, container->sbuf_pos);
	memcpy(parent->sbuf + start + container->sbuf_pos,
				container->dbuf, container->dbuf_pos);
	l_free(container->sbuf);
	l_free(container->dbuf);

	memset(container, 0, sizeof(*container));

	return true;
}

struct mbim_message *mbim_message_builder_finalize(
					struct mbim_message_builder *builder)
{
	struct container *root;
	struct mbim_message_header *hdr;

	if (unlikely(!builder))
		return NULL;

	if (builder->index != 0)
		return NULL;

	hdr = (struct mbim_message_header *) builder->message->header;

	root = &builder->stack[0];
	GROW_DBUF(root, 0, 4);
	container_update_offsets(root);

	memcpy(root->sbuf, builder->message->uuid, 16);
	l_put_le32(builder->message->cid, root->sbuf + 16);

	switch (L_LE32_TO_CPU(hdr->type)) {
	case MBIM_COMMAND_DONE:
		l_put_le32(builder->message->status, root->sbuf + 20);
		break;
	case MBIM_COMMAND_MSG:
		l_put_le32(builder->message->command_type, root->sbuf + 20);
		break;
	default:
		break;
	}

	builder->message->info_buf_len = root->dbuf_pos + root->sbuf_pos -
						root->base_offset;
	l_put_le32(builder->message->info_buf_len,
					root->sbuf + root->base_offset - 4);

	builder->message->n_frags = 2;
	builder->message->frags = l_new(struct iovec, 2);
	builder->message->frags[0].iov_base = root->sbuf;
	builder->message->frags[0].iov_len = root->sbuf_pos;
	builder->message->frags[1].iov_base = root->dbuf;
	builder->message->frags[1].iov_len = root->dbuf_pos;

	root->sbuf = NULL;
	root->dbuf = NULL;

	hdr->len = L_CPU_TO_LE32(HEADER_SIZE + root->dbuf_pos + root->sbuf_pos);

	builder->message->sealed = true;

	return builder->message;
}

static bool append_arguments(struct mbim_message *message,
					const char *signature, va_list args)
{
	struct mbim_message_builder *builder;
	char subsig[64];
	const char *sigend;
	struct {
		char type;
		const char *sig_start;
		const char *sig_end;
		unsigned int n_items;
	} stack[MAX_NESTING + 1];
	unsigned int stack_index = 0;

	if (strlen(signature) > sizeof(subsig) - 1)
		return false;

	builder = mbim_message_builder_new(message);

	stack[stack_index].type = CONTAINER_TYPE_STRUCT;
	stack[stack_index].sig_start = signature;
	stack[stack_index].sig_end = signature + strlen(signature);
	stack[stack_index].n_items = 0;

	while (stack_index != 0 || stack[0].sig_start != stack[0].sig_end) {
		const char *s;
		const char *str;

		if (stack[stack_index].type == CONTAINER_TYPE_ARRAY &&
				stack[stack_index].n_items == 0)
			stack[stack_index].sig_start =
				stack[stack_index].sig_end;

		if (stack[stack_index].sig_start ==
				stack[stack_index].sig_end) {
			bool r = false;

			if (stack_index == 0)
				goto error;

			if (stack[stack_index].type == CONTAINER_TYPE_ARRAY)
				r = mbim_message_builder_leave_array(builder);
			if (stack[stack_index].type == CONTAINER_TYPE_STRUCT)
				r = mbim_message_builder_leave_struct(builder);
			if (stack[stack_index].type == CONTAINER_TYPE_DATABUF)
				r = mbim_message_builder_leave_databuf(builder);

			if (!r)
				goto error;

			stack_index -= 1;
			continue;
		}

		s = stack[stack_index].sig_start;

		if (stack[stack_index].type != CONTAINER_TYPE_ARRAY)
			stack[stack_index].sig_start += 1;
		else
			stack[stack_index].n_items -= 1;

		switch (*s) {
		case '0' ... '9':
		{
			uint32_t n_elem = strtol(s, NULL, 10);
			const uint8_t *arg = va_arg(args, const uint8_t *);

			sigend = _signature_end(s);
			if (!sigend)
				goto error;

			if (!mbim_message_builder_append_bytes(builder,
								n_elem, arg))
				goto error;

			stack[stack_index].sig_start = sigend + 1;
			break;
		}
		case 's':
			str = va_arg(args, const char *);

			if (!mbim_message_builder_append_basic(builder,
								*s, str))
				goto error;
			break;
		case 'y':
		{
			uint8_t y = (uint8_t) va_arg(args, int);

			if (!mbim_message_builder_append_basic(builder, *s, &y))
				goto error;

			break;
		}
		case 'q':
		{
			uint16_t n = (uint16_t) va_arg(args, int);

			if (!mbim_message_builder_append_basic(builder, *s, &n))
				goto error;

			break;
		}
		case 'u':
		{
			uint32_t u = va_arg(args, uint32_t);

			if (!mbim_message_builder_append_basic(builder, *s, &u))
				goto error;

			break;
		}
		case 't':
		{
			uint64_t u = va_arg(args, uint64_t);

			if (!mbim_message_builder_append_basic(builder, *s, &u))
				goto error;

			break;
		}
		case 'v': /* Structure with variable signature */
		{
			if (stack_index == MAX_NESTING)
				goto error;

			str = va_arg(args, const char *);
			if (!str)
				goto error;

			if (!mbim_message_builder_enter_struct(builder, str))
				goto error;

			stack_index += 1;
			stack[stack_index].sig_start = str;
			stack[stack_index].sig_end = str + strlen(str);
			stack[stack_index].n_items = 0;
			stack[stack_index].type = CONTAINER_TYPE_STRUCT;

			break;
		}
		case 'd':
		{
			if (stack_index == MAX_NESTING)
				goto error;

			str = va_arg(args, const char *);
			if (!str)
				goto error;

			if (!mbim_message_builder_enter_databuf(builder, str))
				goto error;

			stack_index += 1;
			stack[stack_index].sig_start = str;
			stack[stack_index].sig_end = str + strlen(str);
			stack[stack_index].n_items = 0;
			stack[stack_index].type = CONTAINER_TYPE_DATABUF;

			break;
		}
		case '(':
			if (stack_index == MAX_NESTING)
				goto error;

			sigend = _signature_end(s);
			memcpy(subsig, s + 1, sigend - s - 1);
			subsig[sigend - s - 1] = '\0';

			if (!mbim_message_builder_enter_struct(builder, subsig))
				goto error;

			if (stack[stack_index].type !=
					CONTAINER_TYPE_ARRAY)
				stack[stack_index].sig_start = sigend + 1;

			stack_index += 1;
			stack[stack_index].sig_start = s + 1;
			stack[stack_index].sig_end = sigend;
			stack[stack_index].n_items = 0;
			stack[stack_index].type = CONTAINER_TYPE_STRUCT;

			break;
		case 'a':
			if (stack_index == MAX_NESTING)
				goto error;

			sigend = _signature_end(s + 1) + 1;
			memcpy(subsig, s + 1, sigend - s - 1);
			subsig[sigend - s - 1] = '\0';

			if (!mbim_message_builder_enter_array(builder, subsig))
				goto error;

			if (stack[stack_index].type != CONTAINER_TYPE_ARRAY)
				stack[stack_index].sig_start = sigend;

			stack_index += 1;
			stack[stack_index].sig_start = s + 1;
			stack[stack_index].sig_end = sigend;
			stack[stack_index].n_items = va_arg(args, unsigned int);
			stack[stack_index].type = CONTAINER_TYPE_ARRAY;

			/* Special case of byte arrays, just copy the data */
			if (!strcmp(subsig, "y")) {
				const uint8_t *bytes =
						va_arg(args, const uint8_t *);

				if (!mbim_message_builder_append_bytes(builder,
						stack[stack_index].n_items,
						bytes))
					goto error;

				stack[stack_index].n_items = 0;
			}

			break;
		default:
			goto error;
		}
	}

	mbim_message_builder_finalize(builder);
	mbim_message_builder_free(builder);

	return true;

error:
	mbim_message_builder_free(builder);
	return false;
}

bool mbim_message_set_arguments(struct mbim_message *message,
						const char *signature, ...)
{
	va_list args;
	bool result;

	if (unlikely(!message))
		return false;

	if (unlikely(message->sealed))
		return false;

	if (!signature)
		return true;

	va_start(args, signature);
	result = append_arguments(message, signature, args);
	va_end(args);

	return result;
}

void *_mbim_message_get_header(struct mbim_message *message, size_t *out_len)
{
	if (out_len)
		*out_len = HEADER_SIZE;

	return message->header;
}

struct iovec *_mbim_message_get_body(struct mbim_message *message,
					size_t *out_n_iov, size_t *out_len)
{
	if (out_len)
		*out_len = message->info_buf_len;

	if (out_n_iov)
		*out_n_iov = message->info_buf_len ? message->n_frags :
							message->n_frags - 1;

	return message->frags;
}
