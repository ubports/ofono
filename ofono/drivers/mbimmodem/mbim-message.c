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
static const char *simple_types = "syqu";

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

static int get_basic_size(const char type)
{
	switch (type) {
	case 'y':
		return 1;
	case 'q':
		return 2;
	case 'u':
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

static bool _iter_enter_struct(struct mbim_message_iter *iter,
					struct mbim_message_iter *structure)
{
	size_t offset;
	size_t len;
	size_t pos;
	const char *sig_start;
	const char *sig_end;
	const void *data;

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

	iter->pos = pos + len;

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
	struct mbim_message_iter stack[2];
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

			if (!_iter_enter_struct(iter, &stack[indent - 1]))
				return false;

			iter = &stack[indent - 1];

			break;
		case ')':
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
	char signature[256];
	uint8_t sigindex;
	uint32_t base_offset;
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
	struct container stack[3];
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

	if (len) {
		start = GROW_SBUF(container, len, alignment);
		memcpy(container->sbuf + start, value, len);
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

done:
	if (container->container_type != CONTAINER_TYPE_ARRAY)
		container->sigindex += 1;

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

	root = &builder->stack[0];
	GROW_DBUF(root, 0, 4);
	container_update_offsets(root);

	memcpy(root->sbuf, builder->message->uuid, 16);
	l_put_le32(builder->message->cid, root->sbuf + 16);
	l_put_le32(builder->message->status, root->sbuf + 20);

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

	hdr = (struct mbim_message_header *) builder->message->header;
	hdr->len = L_CPU_TO_LE32(HEADER_SIZE + root->dbuf_pos + root->sbuf_pos);

	builder->message->sealed = true;

	return builder->message;
}
