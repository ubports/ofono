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

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "log.h"
#include "test.h"
#include "private.h"

/**
 * SECTION:test
 * @short_description: Unit test framework
 *
 * Unit test framework
 */

struct test {
	const char *name;
	l_test_func_t function;
	const void *test_data;
	struct test *next;
};

static struct test *test_head;
static struct test *test_tail;

/**
 * l_test_init:
 * @argc: pointer to @argc parameter of main() function
 * @argv: pointer to @argv parameter of main() function
 *
 * Initialize testing framework.
 **/
LIB_EXPORT void l_test_init(int *argc, char ***argv)
{
	test_head = NULL;
	test_tail = NULL;

	l_log_set_stderr();
}

/**
 * l_test_run:
 *
 * Run all configured tests.
 *
 * Returns: 0 on success
 **/
LIB_EXPORT int l_test_run(void)
{
	struct test *test = test_head;

	while (test) {
		struct test *tmp = test;

		printf("TEST: %s\n", test->name);

		test->function(test->test_data);

		test = test->next;

		free(tmp);
	}

	test_head = NULL;
	test_tail = NULL;

	return 0;
}

/**
 * l_test_add:
 * @name: test name
 * @function: test function
 * @test_data: test data
 *
 * Add new test.
 **/
LIB_EXPORT void l_test_add(const char *name, l_test_func_t function,
						const void *test_data)
{
	struct test *test;

	if (unlikely(!name || !function))
		return;

	test = malloc(sizeof(struct test));
	if (!test)
		return;

	memset(test, 0, sizeof(struct test));
	test->name = name;
	test->function = function;
	test->test_data = test_data;
	test->next = NULL;

	if (test_tail)
		test_tail->next = test;

	test_tail = test;

	if (!test_head)
		test_head = test;
}
