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

#include <stdint.h>
#include <sys/socket.h>
#include <netinet/in.h>

struct mbim_message;
struct mbim_message_iter;

enum mbim_command_type {
	MBIM_COMMAND_TYPE_QUERY = 0,
	MBIM_COMMAND_TYPE_SET = 1,
};

struct mbim_message_iter {
	const char *sig_start;
	uint8_t sig_len;
	uint8_t sig_pos;
	const struct iovec *iov;
	uint32_t n_iov;
	uint32_t cur_iov;
	size_t cur_iov_offset;
	size_t len;
	size_t pos;
	size_t base_offset;
	uint32_t n_elem;
	char container_type;
};

struct mbim_message *mbim_message_new(const uint8_t *uuid, uint32_t cid,
					enum mbim_command_type type);
struct mbim_message *mbim_message_ref(struct mbim_message *msg);
void mbim_message_unref(struct mbim_message *msg);

uint32_t mbim_message_get_error(struct mbim_message *message);
uint32_t mbim_message_get_cid(struct mbim_message *message);
const uint8_t *mbim_message_get_uuid(struct mbim_message *message);
bool mbim_message_get_arguments(struct mbim_message *message,
						const char *signature, ...);

bool mbim_message_get_ipv4_address(struct mbim_message *message,
					uint32_t offset,
					struct in_addr *addr);
bool mbim_message_get_ipv4_element(struct mbim_message *message,
					uint32_t offset,
					uint32_t *prefix_len,
					struct in_addr *addr);
bool mbim_message_get_ipv6_address(struct mbim_message *essage,
					uint32_t offset,
					struct in6_addr *addr);
bool mbim_message_get_ipv6_element(struct mbim_message *message,
					uint32_t offset,
					uint32_t *prefix_len,
					struct in6_addr *addr);

bool mbim_message_iter_next_entry(struct mbim_message_iter *iter, ...);

struct mbim_message_builder *mbim_message_builder_new(struct mbim_message *msg);
void mbim_message_builder_free(struct mbim_message_builder *builder);
bool mbim_message_builder_append_basic(struct mbim_message_builder *builder,
					char type, const void *value);
bool mbim_message_builder_append_bytes(struct mbim_message_builder *builder,
					size_t len, const uint8_t *bytes);
bool mbim_message_builder_enter_struct(struct mbim_message_builder *builder,
					const char *signature);
bool mbim_message_builder_leave_struct(struct mbim_message_builder *builder);
bool mbim_message_builder_enter_array(struct mbim_message_builder *builder,
					const char *signature);
bool mbim_message_builder_leave_array(struct mbim_message_builder *builder);
bool mbim_message_builder_enter_databuf(struct mbim_message_builder *builder,
					const char *signature);
bool mbim_message_builder_leave_databuf(struct mbim_message_builder *builder);
struct mbim_message *mbim_message_builder_finalize(
					struct mbim_message_builder *builder);

bool mbim_message_set_arguments(struct mbim_message *message,
						const char *signature, ...);
