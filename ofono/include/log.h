/*
 *
 *  oFono - Open Telephony stack for Linux
 *
 *  Copyright (C) 2008-2011  Intel Corporation. All rights reserved.
 *  Copyright (C) 2013-2016  Jolla Ltd.
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

#ifndef __OFONO_LOG_H
#define __OFONO_LOG_H

#include <stdarg.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * SECTION:log
 * @title: Logging premitives
 * @short_description: Functions for logging error and debug information
 */

extern void ofono_info(const char *format, ...)
				__attribute__((format(printf, 1, 2)));
extern void ofono_warn(const char *format, ...)
				__attribute__((format(printf, 1, 2)));
extern void ofono_error(const char *format, ...)
				__attribute__((format(printf, 1, 2)));
extern void ofono_debug(const char *format, ...)
				__attribute__((format(printf, 1, 2)));

#define OFONO_DEBUG_ALIGN 8
#define OFONO_DEBUG_ATTR \
	__attribute__((used, section("__debug"), aligned(OFONO_DEBUG_ALIGN)))

struct ofono_debug_desc {
	const char *name;
	const char *file;
#define OFONO_DEBUG_FLAG_DEFAULT (0)
#define OFONO_DEBUG_FLAG_PRINT   (1 << 0)
#define OFONO_DEBUG_FLAG_HIDE_NAME (1 << 1)
	unsigned int flags;
	void (*notify)(struct ofono_debug_desc* desc);
} __attribute__((aligned(OFONO_DEBUG_ALIGN)));

/**
 * DBG:
 * @fmt: format string
 * @arg...: list of arguments
 *
 * Simple macro around ofono_debug() which also include the function
 * name it is called in.
 */
#define DBG(fmt, arg...) do { \
	static struct ofono_debug_desc __ofono_debug_desc OFONO_DEBUG_ATTR = { \
		.file = __FILE__, .flags = OFONO_DEBUG_FLAG_DEFAULT, \
	}; \
	if (__ofono_debug_desc.flags & OFONO_DEBUG_FLAG_PRINT) \
		ofono_dbg(&__ofono_debug_desc, "%s() " fmt, \
					 __FUNCTION__ , ## arg); \
} while (0)

extern void ofono_dbg(const struct ofono_debug_desc *desc,
				const char *format, ...)
				__attribute__((format(printf, 2, 3)));

typedef void (*ofono_log_hook_cb_t)(const struct ofono_debug_desc *desc,
			int priority, const char *format, va_list va);

extern ofono_log_hook_cb_t ofono_log_hook;
extern struct ofono_debug_desc __start___debug[];
extern struct ofono_debug_desc __stop___debug[];

#ifdef __cplusplus
}
#endif

#endif /* __OFONO_LOG_H */
