/*
 *
 *  oFono - Open Source Telephony
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

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#define _GNU_SOURCE
#include <stdio.h>
#include <unistd.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <syslog.h>
#ifdef __GLIBC__
#include <execinfo.h>
#endif
#include <dlfcn.h>

#include "ofono.h"

ofono_log_hook_cb_t ofono_log_hook;
static GString *ofono_debug_str;

static const char *program_exec;
static const char *program_path;

/**
 * ofono_info:
 * @format: format string
 * @Varargs: list of arguments
 *
 * Output general information
 */
void ofono_info(const char *format, ...)
{
	va_list ap;

	va_start(ap, format);

	vsyslog(LOG_INFO, format, ap);

	va_end(ap);

	if (ofono_log_hook) {
		va_start(ap, format);
		ofono_log_hook(NULL, LOG_INFO, format, ap);
		va_end(ap);
	}
}

/**
 * ofono_warn:
 * @format: format string
 * @Varargs: list of arguments
 *
 * Output warning messages
 */
void ofono_warn(const char *format, ...)
{
	va_list ap;

	va_start(ap, format);

	vsyslog(LOG_WARNING, format, ap);

	va_end(ap);

	if (ofono_log_hook) {
		va_start(ap, format);
		ofono_log_hook(NULL, LOG_WARNING, format, ap);
		va_end(ap);
	}
}

/**
 * ofono_error:
 * @format: format string
 * @varargs: list of arguments
 *
 * Output error messages
 */
void ofono_error(const char *format, ...)
{
	va_list ap;

	va_start(ap, format);

	vsyslog(LOG_ERR, format, ap);

	va_end(ap);

	if (ofono_log_hook) {
		va_start(ap, format);
		ofono_log_hook(NULL, LOG_ERR, format, ap);
		va_end(ap);
	}
}

/**
 * ofono_debug:
 * @format: format string
 * @varargs: list of arguments
 *
 * Output debug message
 *
 * The actual output of the debug message is controlled via a command line
 * switch. If not enabled, these messages will be ignored.
 */
void ofono_debug(const char *format, ...)
{
	va_list ap;

	va_start(ap, format);

	vsyslog(LOG_DEBUG, format, ap);

	va_end(ap);

	if (ofono_log_hook) {
		va_start(ap, format);
		ofono_log_hook(NULL, LOG_DEBUG, format, ap);
		va_end(ap);
	}
}

void ofono_dbg(const struct ofono_debug_desc *desc, const char *format, ...)
{
	va_list ap;

	if (!(desc->flags & OFONO_DEBUG_FLAG_PRINT))
		return;

	va_start(ap, format);

	if (ofono_debug_str) {
		g_string_vprintf(ofono_debug_str, format, ap);
		syslog(LOG_DEBUG, "%s:%s", desc->file, ofono_debug_str->str);
	} else {
		vsyslog(LOG_DEBUG, format, ap);
	}

	va_end(ap);

	if (ofono_log_hook) {
		va_start(ap, format);
		ofono_log_hook(desc, LOG_DEBUG, format, ap);
		va_end(ap);
	}
}

#ifdef __GLIBC__
static void print_backtrace(unsigned int offset)
{
	void *frames[99];
	size_t n_ptrs;
	unsigned int i;
	int outfd[2], infd[2];
	int pathlen;
	pid_t pid;

	if (program_exec == NULL)
		return;

	pathlen = strlen(program_path);

	n_ptrs = backtrace(frames, G_N_ELEMENTS(frames));
	if (n_ptrs < offset)
		return;

	if (pipe(outfd) < 0)
		return;

	if (pipe(infd) < 0) {
		close(outfd[0]);
		close(outfd[1]);
		return;
	}

	pid = fork();
	if (pid < 0) {
		close(outfd[0]);
		close(outfd[1]);
		close(infd[0]);
		close(infd[1]);
		return;
	}

	if (pid == 0) {
		close(outfd[1]);
		close(infd[0]);

		dup2(outfd[0], STDIN_FILENO);
		dup2(infd[1], STDOUT_FILENO);

		execlp("addr2line", "-C", "-f", "-e", program_exec, NULL);

		exit(EXIT_FAILURE);
	}

	close(outfd[0]);
	close(infd[1]);

	ofono_error("++++++++ backtrace ++++++++");

	for (i = offset; i < n_ptrs - 1; i++) {
		Dl_info info;
		char addr[20], buf[PATH_MAX * 2];
		int len, written;
		char *ptr, *pos;

		dladdr(frames[i], &info);

		len = snprintf(addr, sizeof(addr), "%p\n", frames[i]);
		if (len < 0)
			break;

		written = write(outfd[1], addr, len);
		if (written < 0)
			break;

		len = read(infd[0], buf, sizeof(buf));
		if (len < 0)
			break;

		buf[len] = '\0';

		pos = strchr(buf, '\n');
		if (!pos)
			break;

		*pos++ = '\0';

		if (strcmp(buf, "??") == 0) {
			ofono_error("#%-2u %p in %s", i - offset,
						frames[i], info.dli_fname);
			continue;
		}

		ptr = strchr(pos, '\n');
		*ptr++ = '\0';

		if (strncmp(pos, program_path, pathlen) == 0)
			pos += pathlen + 1;

		ofono_error("#%-2u %p in %s() at %s", i - offset,
						frames[i], buf, pos);
	}

	ofono_error("+++++++++++++++++++++++++++");

	kill(pid, SIGTERM);

	close(outfd[1]);
	close(infd[0]);
}

static void signal_handler(int signo)
{
	ofono_error("Aborting (signal %d) [%s]", signo, program_exec);

	print_backtrace(2);

	exit(EXIT_FAILURE);
}

static void signal_setup(sighandler_t handler)
{
	struct sigaction sa;
	sigset_t mask;

	sigemptyset(&mask);
	sa.sa_handler = handler;
	sa.sa_mask = mask;
	sa.sa_flags = 0;
	sigaction(SIGBUS, &sa, NULL);
	sigaction(SIGILL, &sa, NULL);
	sigaction(SIGFPE, &sa, NULL);
	sigaction(SIGSEGV, &sa, NULL);
	sigaction(SIGABRT, &sa, NULL);
	sigaction(SIGPIPE, &sa, NULL);
}
#endif

extern struct ofono_debug_desc __start___debug[];
extern struct ofono_debug_desc __stop___debug[];

static gchar **enabled = NULL;

static ofono_bool_t is_enabled(struct ofono_debug_desc *desc)
{
	int i;

	if (enabled == NULL)
		return FALSE;

	for (i = 0; enabled[i] != NULL; i++) {
		if (desc->name != NULL && g_pattern_match_simple(enabled[i],
							desc->name) == TRUE)
			return TRUE;
		if (desc->file != NULL && g_pattern_match_simple(enabled[i],
							desc->file) == TRUE)
			return TRUE;
	}

	return FALSE;
}

void __ofono_log_enable(struct ofono_debug_desc *start,
					struct ofono_debug_desc *stop)
{
	struct ofono_debug_desc *desc;
	const char *name = NULL, *file = NULL;

	if (start == NULL || stop == NULL)
		return;

	for (desc = start; desc < stop; desc++) {
		if (file != NULL || name != NULL) {
			if (g_strcmp0(desc->file, file) == 0) {
				if (desc->name == NULL)
					desc->name = name;
			} else
				file = NULL;
		}

		if (is_enabled(desc) &&
				!(desc->flags & OFONO_DEBUG_FLAG_PRINT)) {
			desc->flags |= OFONO_DEBUG_FLAG_PRINT;
			if (desc->notify) {
				desc->notify(desc);
			}
		}
	}
}

int __ofono_log_init(const char *program, const char *debug,
						ofono_bool_t detach,
						ofono_bool_t backtrace)
{
	static char path[PATH_MAX];
	int option = LOG_NDELAY | LOG_PID;

	program_exec = program;
	program_path = getcwd(path, sizeof(path));
	ofono_debug_str = g_string_sized_new(127);

	if (debug != NULL)
		enabled = g_strsplit_set(debug, ":, ", 0);

	__ofono_log_enable(__start___debug, __stop___debug);

	if (detach == FALSE)
		option |= LOG_PERROR;

#ifdef __GLIBC__
	if (backtrace == TRUE)
		signal_setup(signal_handler);
#endif

	openlog(basename(program), option, LOG_DAEMON);

	syslog(LOG_INFO, "oFono version %s", VERSION);

	return 0;
}

void __ofono_log_cleanup(ofono_bool_t backtrace)
{
	syslog(LOG_INFO, "Exit");

	closelog();

#ifdef __GLIBC__
	if (backtrace == TRUE)
		signal_setup(SIG_DFL);
#endif

	g_strfreev(enabled);
	g_string_free(ofono_debug_str, TRUE);
	ofono_debug_str = NULL;
}
