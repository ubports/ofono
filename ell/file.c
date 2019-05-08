/*
 *
 *  Embedded Linux library
 *
 *  Copyright (C) 2017  Intel Corporation. All rights reserved.
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
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>

#include "file.h"
#include "private.h"

/**
 * l_file_get_contents:
 * @filename: File to load
 * @out_len: Set to the length of the loaded file
 *
 * Attempts to load the contents of a file via sequential read system calls.
 * This can be useful for files that are not mmapable, e.g. sysfs entries.
 *
 * Returns: A newly allocated memory region with the file contents
 **/
LIB_EXPORT void *l_file_get_contents(const char *filename, size_t *out_len)
{
	int fd;
	struct stat st;
	uint8_t *contents;
	size_t bytes_read = 0;
	ssize_t nread;

	fd = open(filename, O_RDONLY);
	if (fd < 0)
		return NULL;

	if (fstat(fd, &st) < 0) {
		close(fd);
		return NULL;
	}

	contents = l_malloc(st.st_size);

	do {
		nread = read(fd, contents + bytes_read, 4096);

		if (nread < 0) {
			if (errno == EINTR)
				continue;

			goto error;
		}

		bytes_read += nread;
	} while (nread != 0);

	if (out_len)
		*out_len = bytes_read;

	close(fd);
	return contents;

error:
	l_free(contents);
	close(fd);
	return NULL;
}
