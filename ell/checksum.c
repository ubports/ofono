/*
 *
 *  Embedded Linux library
 *
 *  Copyright (C) 2011-2014  Intel Corporation. All rights reserved.
 *
 *  This library is free software; you can redistribute it and/or
 *  modify it under the terms of the GNU Lesser General Public
 *  License as published by the Free Software Foundation; either
 *  version 2.1 of the License, or (at your option) any later version.
 *
 *  This library is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 *  Lesser General Public License for more details.
 *
 *  You should have received a copy of the GNU Lesser General Public
 *  License along with this library; if not, write to the Free Software
 *  Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
 *
 */

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#define _GNU_SOURCE
#include <unistd.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <stdio.h>

#include "util.h"
#include "checksum.h"
#include "private.h"

#ifndef HAVE_LINUX_IF_ALG_H
#ifndef HAVE_LINUX_TYPES_H
typedef uint8_t __u8;
typedef uint16_t __u16;
typedef uint32_t __u32;
#else
#include <linux/types.h>
#endif

#ifndef AF_ALG
#define AF_ALG	38
#define PF_ALG	AF_ALG
#endif

struct sockaddr_alg {
	__u16	salg_family;
	__u8	salg_type[14];
	__u32	salg_feat;
	__u32	salg_mask;
	__u8	salg_name[64];
};

/* Socket options */
#define ALG_SET_KEY	1

#else
#include <linux/if_alg.h>
#endif

#ifndef SOL_ALG
#define SOL_ALG 279
#endif

static struct {
	const char *name;
	uint8_t digest_len;
	bool supported;
} checksum_info_table[] = {
	{ .name = "md4", .digest_len = 16 },
	{ .name = "md5", .digest_len = 16 },
	{ .name = "sha1", .digest_len = 20 },
	{ .name = "sha256", .digest_len = 32 },
	{ .name = "sha384", .digest_len = 48 },
	{ .name = "sha512", .digest_len = 64 },
	{ .name = "cmac(aes)", .digest_len = 16 },
	{ .name = "hmac(md4)", .digest_len = 16 },
	{ .name = "hmac(md5)", .digest_len = 16 },
	{ .name = "hmac(sha1)", .digest_len = 20 },
	{ .name = "hmac(sha256)", .digest_len = 32 },
	{ .name = "hmac(sha384)", .digest_len = 48 },
	{ .name = "hmac(sha512)", .digest_len = 64 },
	{ .name = NULL, .digest_len = 0 },
};

/**
 * SECTION:checksum
 * @short_description: Checksum handling
 *
 * Checksum handling
 */

#define is_valid_type(type)  ((type) > L_CHECKSUM_NONE && \
					(type) <= L_CHECKSUM_SHA512)

/**
 * l_checksum:
 *
 * Opague object representing the checksum.
 */
struct l_checksum {
	int sk;
	char alg_name[sizeof(((struct sockaddr_alg *) 0)->salg_name)];
};

static int create_alg(const char *alg)
{
	struct sockaddr_alg salg;
	int sk;

	sk = socket(PF_ALG, SOCK_SEQPACKET | SOCK_CLOEXEC, 0);
	if (sk < 0)
		return -1;

	memset(&salg, 0, sizeof(salg));
	salg.salg_family = AF_ALG;
	strcpy((char *) salg.salg_type, "hash");
	strcpy((char *) salg.salg_name, alg);

	if (bind(sk, (struct sockaddr *) &salg, sizeof(salg)) < 0) {
		close(sk);
		return -1;
	}

	return sk;
}

static const char *checksum_type_to_name(enum l_checksum_type type)
{
	switch (type) {
	case L_CHECKSUM_NONE:
		return NULL;
	case L_CHECKSUM_MD4:
		return "md4";
	case L_CHECKSUM_MD5:
		return "md5";
	case L_CHECKSUM_SHA1:
		return "sha1";
	case L_CHECKSUM_SHA224:
		return "sha224";
	case L_CHECKSUM_SHA256:
		return "sha256";
	case L_CHECKSUM_SHA384:
		return "sha384";
	case L_CHECKSUM_SHA512:
		return "sha512";
	}

	return NULL;
}

/**
 * l_checksum_new:
 * @type: checksum type
 *
 * Creates new #l_checksum, using the checksum algorithm @type.
 *
 * Returns: a newly allocated #l_checksum object.
 **/
LIB_EXPORT struct l_checksum *l_checksum_new(enum l_checksum_type type)
{
	struct l_checksum *checksum;
	const char *name;
	int fd;

	if (!is_valid_type(type))
		return NULL;

	checksum = l_new(struct l_checksum, 1);

	name = checksum_type_to_name(type);

	fd = create_alg(name);
	if (fd < 0)
		goto error;

	checksum->sk = accept4(fd, NULL, 0, SOCK_CLOEXEC);
	close(fd);

	if (checksum->sk < 0)
		goto error;

	strcpy(checksum->alg_name, name);

	return checksum;

error:
	l_free(checksum);
	return NULL;
}

LIB_EXPORT struct l_checksum *l_checksum_new_cmac_aes(const void *key,
							size_t key_len)
{
	struct l_checksum *checksum;
	int fd;

	fd = create_alg("cmac(aes)");
	if (fd < 0)
		return NULL;

	if (setsockopt(fd, SOL_ALG, ALG_SET_KEY, key, key_len) < 0) {
		close(fd);
		return NULL;
	}

	checksum = l_new(struct l_checksum, 1);
	checksum->sk = accept4(fd, NULL, 0, SOCK_CLOEXEC);
	close(fd);

	if (checksum->sk < 0) {
		l_free(checksum);
		return NULL;
	}

	strcpy(checksum->alg_name, "cmac(aes)");
	return checksum;
}

LIB_EXPORT struct l_checksum *l_checksum_new_hmac(enum l_checksum_type type,
					  const void *key, size_t key_len)
{
	struct l_checksum *checksum;
	int fd;
	char name[sizeof(((struct sockaddr_alg *)0)->salg_name)];
	unsigned int r;

	if (!is_valid_type(type))
		return NULL;

	r = snprintf(name, sizeof(name), "hmac(%s)",
					checksum_type_to_name(type));
	if (r >= sizeof(name))
		return NULL;

	fd = create_alg(name);
	if (fd < 0)
		return NULL;

	if (setsockopt(fd, SOL_ALG, ALG_SET_KEY, key, key_len) < 0) {
		close(fd);
		return NULL;
	}

	checksum = l_new(struct l_checksum, 1);
	checksum->sk = accept4(fd, NULL, 0, SOCK_CLOEXEC);
	close(fd);

	if (checksum->sk < 0) {
		l_free(checksum);
		return NULL;
	}

	strcpy(checksum->alg_name, name);
	return checksum;
}

/**
 * l_checksum_clone:
 * @checksum: parent checksum object
 *
 * Creates a new checksum with an independent copy of parent @checksum's
 * state.  l_checksum_get_digest can then be called on the parent or the
 * clone without affecting the state of the other object.
 **/
LIB_EXPORT struct l_checksum *l_checksum_clone(struct l_checksum *checksum)
{
	struct l_checksum *clone;

	if (unlikely(!checksum))
		return NULL;

	clone = l_new(struct l_checksum, 1);
	clone->sk = accept4(checksum->sk, NULL, 0, SOCK_CLOEXEC);

	if (clone->sk < 0) {
		l_free(clone);
		return NULL;
	}

	strcpy(clone->alg_name, checksum->alg_name);
	return clone;
}

/**
 * l_checksum_free:
 * @checksum: checksum object
 *
 * Frees the memory allocated for @checksum.
 **/
LIB_EXPORT void l_checksum_free(struct l_checksum *checksum)
{
	if (unlikely(!checksum))
		return;

	close(checksum->sk);
	l_free(checksum);
}

/**
 * l_checksum_reset:
 * @checksum: checksum object
 *
 * Resets the internal state of @checksum.
 **/
LIB_EXPORT void l_checksum_reset(struct l_checksum *checksum)
{
	if (unlikely(!checksum))
		return;

	send(checksum->sk, NULL, 0, 0);
}

/**
 * l_checksum_update:
 * @checksum: checksum object
 * @data: data pointer
 * @len: length of data
 *
 * Updates checksum from @data pointer with @len bytes.
 *
 * Returns: true if the operation succeeded, false otherwise.
 **/
LIB_EXPORT bool l_checksum_update(struct l_checksum *checksum,
					const void *data, size_t len)
{
	ssize_t written;

	if (unlikely(!checksum))
		return false;

	written = send(checksum->sk, data, len, MSG_MORE);
	if (written < 0)
		return false;

	return true;
}

/**
 * l_checksum_updatev:
 * @checksum: checksum object
 * @iov: iovec pointer
 * @iov_len: Number of iovec entries
 *
 * This is a iovec based version of l_checksum_update; it updates the checksum
 * based on contents of @iov and @iov_len.
 *
 * Returns: true if the operation succeeded, false otherwise.
 **/
LIB_EXPORT bool l_checksum_updatev(struct l_checksum *checksum,
					const struct iovec *iov, size_t iov_len)
{
	struct msghdr msg;
	ssize_t written;

	if (unlikely(!checksum))
		return false;

	if (unlikely(!iov) || unlikely(!iov_len))
		return false;

	memset(&msg, 0, sizeof(msg));
	msg.msg_iov = (struct iovec *) iov;
	msg.msg_iovlen = iov_len;

	written = sendmsg(checksum->sk, &msg, MSG_MORE);
	if (written < 0)
		return false;

	return true;
}

/**
 * l_checksum_get_digest:
 * @checksum: checksum object
 * @digest: digest data pointer
 * @len: length of digest data
 *
 * Gets the digest from @checksum as raw binary data.
 *
 * Returns: Number of bytes read, or negative value if an error occurred.
 **/
LIB_EXPORT ssize_t l_checksum_get_digest(struct l_checksum *checksum,
						void *digest, size_t len)
{
	ssize_t result;

	if (unlikely(!checksum))
		return -EINVAL;

	if (unlikely(!digest))
		return -EFAULT;

	if (unlikely(!len))
		return -EINVAL;

	result = recv(checksum->sk, digest, len, 0);
	if (result < 0)
		return -errno;

	return result;
}

/**
 * l_checksum_get_string:
 * @checksum: checksum object
 *
 * Gets the digest from @checksum as hex encoded string.
 *
 * Returns: a newly allocated hex string
 **/
LIB_EXPORT char *l_checksum_get_string(struct l_checksum *checksum)
{
	unsigned char digest[64];
	unsigned int i;

	if (unlikely(!checksum))
		return NULL;

	l_checksum_get_digest(checksum, digest, sizeof(digest));

	for (i = 0; checksum_info_table[i].name; i++) {
		if (strcmp(checksum_info_table[i].name, checksum->alg_name))
			continue;

		return l_util_hexstring(digest,
					checksum_info_table[i].digest_len);
	}

	return NULL;
}

static void init_supported()
{
	static bool initialized = false;
	struct sockaddr_alg salg;
	int sk;
	int i;

	if (likely(initialized))
		return;

	initialized = true;

	sk = socket(PF_ALG, SOCK_SEQPACKET | SOCK_CLOEXEC, 0);
	if (sk < 0)
		return;

	memset(&salg, 0, sizeof(salg));
	salg.salg_family = AF_ALG;
	strcpy((char *) salg.salg_type, "hash");

	for (i = 0; checksum_info_table[i].name; i++) {
		strcpy((char *) salg.salg_name, checksum_info_table[i].name);

		if (bind(sk, (struct sockaddr *) &salg, sizeof(salg)) < 0)
			continue;

		checksum_info_table[i].supported = true;
	}

	close(sk);
}

static inline bool is_supported(const char *alg)
{
	int i;

	for (i = 0; checksum_info_table[i].name; i++) {
		if (strcmp(checksum_info_table[i].name, alg))
			continue;

		return checksum_info_table[i].supported;
	}

	return false;
}

LIB_EXPORT bool l_checksum_is_supported(enum l_checksum_type type,
							bool check_hmac)
{
	const char *name;
	char hmac[sizeof(((struct sockaddr_alg *)0)->salg_name)];

	init_supported();

	name = checksum_type_to_name(type);
	if (!name)
		return false;

	if (!is_supported(name))
		return false;

	if (!check_hmac)
		return true;

	snprintf(hmac, sizeof(hmac) - 1, "hmac(%s)", name);
	return is_supported(hmac);
}

LIB_EXPORT bool l_checksum_cmac_aes_supported()
{
	init_supported();

	return is_supported("cmac(aes)");
}
