/*
 *
 *  Embedded Linux library
 *
 *  Copyright (C) 2019 Geanix. All rights reserved.
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
#include <fcntl.h>
#include <unistd.h>
#include <string.h>
#include <dirent.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <linux/gpio.h>

#include "private.h"
#include "strv.h"
#include "util.h"
#include "gpio.h"

struct l_gpio_chip {
	int fd;
	char *name;
	char *label;
	uint32_t n_lines;
};

struct l_gpio_writer {
	int fd;
	uint32_t n_offsets;
};

struct l_gpio_reader {
	int fd;
	uint32_t n_offsets;
};

static bool chip_has_line_label(const char *chip_name, const char *line_label)
{
	struct l_gpio_chip *chip;
	bool has_label;

	chip = l_gpio_chip_new(chip_name);
	if (!chip)
		return false;

	has_label = l_gpio_chip_find_line_offset(chip, line_label, NULL);

	l_gpio_chip_free(chip);

	return has_label;
}

LIB_EXPORT char **l_gpio_chips_with_line_label(const char *line_label)
{
	struct dirent *entry;
	DIR *dp;
	char **chips = NULL;

	dp = opendir("/sys/bus/gpio/devices");
	if (dp == NULL)
		return NULL;

	while ((entry = readdir(dp))) {
		if (entry->d_type != DT_LNK)
			continue;

		if (!l_str_has_prefix(entry->d_name, "gpiochip"))
			continue;

		if (chip_has_line_label(entry->d_name, line_label))
			chips = l_strv_append(chips, entry->d_name);
	}

	closedir(dp);

	return chips;
}

LIB_EXPORT struct l_gpio_chip *l_gpio_chip_new(const char *chip_name)
{
	struct l_gpio_chip *chip;
	struct gpiochip_info info;
	char *path;
	int fd;
	int ret;

	if (unlikely(!chip_name))
		return NULL;

	path = l_strdup_printf("/dev/%s", chip_name);
	fd = open(path, O_RDONLY | O_CLOEXEC);
	l_free(path);

	if (fd < 0)
		return NULL;

	memset(&info, 0, sizeof(info));

	ret = ioctl(fd, GPIO_GET_CHIPINFO_IOCTL, &info);
	if (ret < 0) {
		close(fd);
		return NULL;
	}

	chip = l_new(struct l_gpio_chip, 1);
	chip->fd = fd;
	chip->n_lines = info.lines;
	chip->label = l_strndup(info.label, sizeof(info.label));
	chip->name = l_strdup(chip_name);

	return chip;
}

LIB_EXPORT const char *l_gpio_chip_get_label(struct l_gpio_chip *chip)
{
	if (unlikely(!chip))
		return NULL;

	return chip->label;
}

LIB_EXPORT const char *l_gpio_chip_get_name(struct l_gpio_chip *chip)
{
	if (unlikely(!chip))
		return NULL;

	return chip->name;
}

LIB_EXPORT uint32_t l_gpio_chip_get_num_lines(struct l_gpio_chip *chip)
{
	if (unlikely(!chip))
		return 0;

	return chip->n_lines;
}

LIB_EXPORT void l_gpio_chip_free(struct l_gpio_chip *chip)
{
	if (unlikely(!chip))
		return;

	if (chip->fd >= 0)
		close(chip->fd);

	l_free(chip->name);
	l_free(chip->label);
	l_free(chip);
}

LIB_EXPORT bool l_gpio_chip_find_line_offset(struct l_gpio_chip *chip,
						const char *line_label,
						uint32_t *line_offset)
{
	struct gpioline_info info;
	uint32_t i;

	if (unlikely(!chip))
		return false;

	if (unlikely(!line_label))
		return false;

	for (i = 0; i < chip->n_lines; i++) {
		memset(&info, 0, sizeof(info));
		info.line_offset = i;

		if (ioctl(chip->fd, GPIO_GET_LINEINFO_IOCTL, &info) < 0)
			return false;

		if (strcmp(info.name, line_label) != 0)
			continue;

		if (line_offset)
			*line_offset = i;

		return true;
	}

	return false;
}

LIB_EXPORT char *l_gpio_chip_get_line_label(struct l_gpio_chip *chip,
						uint32_t offset)
{
	struct gpioline_info info;

	if (unlikely(!chip))
		return NULL;

	if (unlikely(offset >= chip->n_lines))
		return NULL;

	memset(&info, 0, sizeof(info));
	info.line_offset = offset;

	if (ioctl(chip->fd, GPIO_GET_LINEINFO_IOCTL, &info) < 0)
		return NULL;

	return l_strdup(info.name);
}

LIB_EXPORT char *l_gpio_chip_get_line_consumer(struct l_gpio_chip *chip,
						uint32_t offset)
{
	struct gpioline_info info;

	if (unlikely(!chip))
		return NULL;

	if (unlikely(offset >= chip->n_lines))
		return NULL;

	memset(&info, 0, sizeof(info));
	info.line_offset = offset;

	if (ioctl(chip->fd, GPIO_GET_LINEINFO_IOCTL, &info) < 0)
		return NULL;

	return l_strdup(info.consumer);
}

LIB_EXPORT struct l_gpio_writer *l_gpio_writer_new(struct l_gpio_chip *chip,
						const char *consumer,
						uint32_t n_offsets,
						const uint32_t offsets[],
						const uint32_t values[])
{
	struct l_gpio_writer *writer;
	struct gpiohandle_request request;
	uint32_t i;

	if (unlikely(!chip))
		return NULL;

	if (unlikely(n_offsets == 0 || n_offsets > GPIOHANDLES_MAX))
		return NULL;

	if (unlikely(!offsets))
		return NULL;

	memset(&request, 0, sizeof(request));
	l_strlcpy(request.consumer_label, consumer, 32);
	request.lines = n_offsets;
	request.flags = GPIOHANDLE_REQUEST_OUTPUT;

	for (i = 0; i < n_offsets; i++) {
		if (offsets[i] >= chip->n_lines)
			return NULL;

		request.lineoffsets[i] = offsets[i];
		request.default_values[i] = values[i];
	}

	if (ioctl(chip->fd, GPIO_GET_LINEHANDLE_IOCTL, &request) < 0)
		return NULL;

	if (request.fd <= 0)
		return NULL;

	writer = l_new(struct l_gpio_writer, 1);
	writer->fd = request.fd;
	writer->n_offsets = n_offsets;

	return writer;
}

LIB_EXPORT void l_gpio_writer_free(struct l_gpio_writer *writer)
{
	if (unlikely(!writer))
		return;

	if (writer->fd >= 0)
		close(writer->fd);

	l_free(writer);
}

LIB_EXPORT bool l_gpio_writer_set(struct l_gpio_writer *writer, uint32_t n_values,
					const uint32_t values[])
{
	struct gpiohandle_data data;
	uint32_t i;

	if (unlikely(!writer))
		return false;

	if (unlikely(!values))
		return false;

	if (unlikely(n_values != writer->n_offsets))
		return false;

	for (i = 0; i < n_values; i++)
		data.values[i] = values[i];

	if (ioctl(writer->fd, GPIOHANDLE_SET_LINE_VALUES_IOCTL, &data) < 0)
		return false;

	return true;
}

LIB_EXPORT struct l_gpio_reader *l_gpio_reader_new(struct l_gpio_chip *chip,
							const char *consumer,
							uint32_t n_offsets,
							const uint32_t offsets[])
{
	struct l_gpio_reader *reader;
	struct gpiohandle_request request;
	uint32_t i;

	if (unlikely(!chip))
		return NULL;

	if (unlikely(n_offsets == 0 || n_offsets > GPIOHANDLES_MAX))
		return NULL;

	if (unlikely(!offsets))
		return NULL;

	memset(&request, 0, sizeof(request));
	l_strlcpy(request.consumer_label, consumer, 32);
	request.lines = n_offsets;
	request.flags = GPIOHANDLE_REQUEST_INPUT;

	for (i = 0; i < n_offsets; i++) {
		if (offsets[i] >= chip->n_lines)
			return NULL;

		request.lineoffsets[i] = offsets[i];
	}

	if (ioctl(chip->fd, GPIO_GET_LINEHANDLE_IOCTL, &request) < 0)
		return NULL;

	if (request.fd <= 0)
		return NULL;

	reader = l_new(struct l_gpio_reader, 1);
	reader->fd = request.fd;
	reader->n_offsets = n_offsets;

	return reader;
}

LIB_EXPORT void l_gpio_reader_free(struct l_gpio_reader *reader)
{
	if (unlikely(!reader))
		return;

	if (reader->fd >= 0)
		close(reader->fd);

	l_free(reader);
}

LIB_EXPORT bool l_gpio_reader_get(struct l_gpio_reader *reader,
					uint32_t n_values, uint32_t values[])
{
	struct gpiohandle_data data;
	uint32_t i;

	if (unlikely(!reader))
		return false;

	if (unlikely(n_values != reader->n_offsets))
		return false;

	if (unlikely(!values))
		return false;

	if (ioctl(reader->fd, GPIOHANDLE_GET_LINE_VALUES_IOCTL, &data) < 0)
		return false;

	for (i = 0; i < n_values; i++)
		values[i] = data.values[i];

	return true;
}
