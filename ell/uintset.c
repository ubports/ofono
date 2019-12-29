/*
 *
 *  Embedded Linux library
 *
 *  Copyright (C) 2015  Intel Corporation. All rights reserved.
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
#include <limits.h>

#include "uintset.h"
#include "private.h"

#define BITS_PER_LONG (sizeof(unsigned long) * 8)

static inline int __ffz(unsigned long word)
{
	return __builtin_ctzl(~word);
}

static inline int __fls(unsigned long word)
{
	return word ? sizeof(word) * 8 - __builtin_clzl(word) : 0;
}

static inline int __ffs(unsigned long word)
{
	return __builtin_ctzl(word);
}

static unsigned long find_first_zero_bit(const unsigned long *addr,
							unsigned long size,
							unsigned long start)
{
	unsigned long i;
	unsigned long mask = ~0UL << (start & (BITS_PER_LONG - 1));
	unsigned long p;

	for (i = start / BITS_PER_LONG; i * BITS_PER_LONG < size; i++) {
		p = addr[i];

		if (mask) {
			p |= ~mask;
			mask = 0;
		}

		if (p == ~0UL)
			continue;

		return i * BITS_PER_LONG + __ffz(p);
	}

	return size;
}

static unsigned long find_first_bit(const unsigned long *addr,
							unsigned long size)
{
	unsigned long result = 0;
	unsigned long tmp;

	while (size >= BITS_PER_LONG) {
		tmp = *addr;
		addr += 1;

		if (tmp)
			goto found;

		result += BITS_PER_LONG;
		size -= BITS_PER_LONG;
	}

	if (!size)
		return result;

	tmp = *addr;
	if (!tmp)
		return result + size;

found:
	return result + __ffs(tmp);
}

static unsigned long find_last_bit(const unsigned long *addr, unsigned int size)
{
	unsigned long words;
	unsigned long tmp;
	long i;

	/* Bits out of bounds are always zero, start at last word */
	words = (size + BITS_PER_LONG - 1) / BITS_PER_LONG;

	for (i = words - 1; i >= 0; i -= 1) {
		tmp = addr[i];

		if (!tmp)
			continue;

		return i * BITS_PER_LONG + __fls(tmp) - 1;
	}

	/* Not found */
	return size;
}

struct l_uintset {
	unsigned long *bits;
	uint16_t size;
	uint32_t min;
	uint32_t max;
};

/**
 * l_uintset_new_from_range:
 * @min: The minimum value of the set of numbers contained in the set
 * @max: The maximum value of the set of numbers contained
 *
 * Creates a new empty collection of unsigned integers.  The size of the set
 * is limited to roughly 2^16 entries.  @min and @max give the minimum and
 * maximum elements of the set.
 *
 * Returns: A newly allocated l_uintset object, and NULL otherwise.
 **/
LIB_EXPORT struct l_uintset *l_uintset_new_from_range(uint32_t min,
								uint32_t max)
{
	struct l_uintset *ret;
	unsigned int size = max - min + 1;

	if (size > USHRT_MAX)
		return NULL;

	ret = l_new(struct l_uintset, 1);
	ret->bits = l_new(unsigned long,
				(size + BITS_PER_LONG - 1) / BITS_PER_LONG);
	ret->size = size;
	ret->min = min;
	ret->max = max;

	return ret;
}

/**
 * l_uintset_new:
 * @size: The maximum size of the set
 *
 * Creates a new empty collection of unsigned integers.  The size of the set
 * is limited to roughly 2^16 entries.  The set is created with minimum value
 * of 1 and maximum value equal to size.
 *
 * Returns: A newly allocated l_uintset object, and NULL otherwise.
 **/
LIB_EXPORT struct l_uintset *l_uintset_new(unsigned int size)
{
	return l_uintset_new_from_range(1, size);
}

/**
 * l_uintset_free:
 * @set: The set to destroy
 *
 * De-allocated the set object.
 **/
LIB_EXPORT void l_uintset_free(struct l_uintset *set)
{
	if (unlikely(!set))
		return;

	l_free(set->bits);
	l_free(set);
}

/**
 * l_uintset_take:
 * @set: The set of numbers
 * @number: The number to remove from the set
 *
 * Removes the @number from the @set.  No checking is performed whether the
 * number is actually contained in the set.  However, basic bounds checking
 * is performed to make sure the number taken can actually exist in the set.
 *
 * Returns: true if the number was removed, and false otherwise.
 **/
LIB_EXPORT bool l_uintset_take(struct l_uintset *set, uint32_t number)
{
	uint32_t offset = (number - set->min) / BITS_PER_LONG;

	if (unlikely(!set))
		return false;

	number -= set->min;

	if (number > set->size)
		return false;

	number %= BITS_PER_LONG;

	set->bits[offset] &= ~(1UL << number);

	return true;
}

/**
 * l_uintset_put:
 * @set: The set of numbers
 * @number: The number to add to the set
 *
 * Adds the @number to the @set.  No checking is performed whether the
 * number is already contained in the set.  However, basic bounds checking
 * is performed to make sure the number being added can actually exist in
 * the set.
 *
 * Returns: true if the number was added, and false otherwise.
 **/
LIB_EXPORT bool l_uintset_put(struct l_uintset *set, uint32_t number)
{
	uint32_t bit = number - set->min;
	uint32_t offset;

	if (unlikely(!set))
		return false;

	if (bit >= set->size)
		return false;

	offset = bit / BITS_PER_LONG;
	set->bits[offset] |= 1UL << (bit % BITS_PER_LONG);

	return true;
}

/**
 * l_uintset_contains:
 * @set: The set of numbers
 * @number: The number to search for
 *
 * Returns: true if the number is in the set, and false otherwise.
 **/
LIB_EXPORT bool l_uintset_contains(struct l_uintset *set, uint32_t number)
{
	uint32_t bit = number - set->min;
	uint32_t offset;

	if (unlikely(!set))
		return false;

	if (bit >= set->size)
		return false;

	offset = bit / BITS_PER_LONG;
	if (set->bits[offset] & (1UL << (bit % BITS_PER_LONG)))
		return true;

	return false;
}

/**
 * l_uintset_get_min:
 * @set: The set of numbers
 *
 * Returns: the minimum possible value of the set.  If @set is NULL returns
 * UINT_MAX.
 **/
LIB_EXPORT uint32_t l_uintset_get_min(struct l_uintset *set)
{
	if (unlikely(!set))
		return UINT_MAX;

	return set->min;
}

/**
 * l_uintset_get_max:
 * @set: The set of numbers
 *
 * Returns: the maximum possible value of the set.  If @set is NULL returns
 * UINT_MAX.
 **/
LIB_EXPORT uint32_t l_uintset_get_max(struct l_uintset *set)
{
	if (unlikely(!set))
		return UINT_MAX;

	return set->max;
}

/**
 * l_uintset_find_unused_min:
 * @set: The set of numbers
 *
 * Returns: The minimum number that is not preset in the set.  If the set of
 * numbers is fully populated, returns l_uintset_get_max(set) + 1. If @set is
 * NULL returns UINT_MAX.
 **/
LIB_EXPORT uint32_t l_uintset_find_unused_min(struct l_uintset *set)
{
	unsigned int bit;

	if (unlikely(!set))
		return UINT_MAX;

	bit = find_first_zero_bit(set->bits, set->size, 0);

	if (bit >= set->size)
		return set->max + 1;

	return bit + set->min;
}

/**
 * l_uintset_find_unused:
 * @set: The set of numbers
 * @start: The starting point
 *
 * Returns: First number not in the set starting at position @start (inclusive).
 * If all numbers in the set starting at @start until l_uintset_get_max(set)
 * are taken, the starting position is set to the minimum and the search starts
 * again.  If the set of numbers is fully populated, this function returns
 * l_uintset_get_max(set) + 1. If @set is NULL returns UINT_MAX.
 **/
LIB_EXPORT uint32_t l_uintset_find_unused(struct l_uintset *set, uint32_t start)
{
	unsigned int bit;

	if (unlikely(!set))
		return UINT_MAX;

	if (start < set->min || start > set->max)
		return set->max + 1;

	bit = find_first_zero_bit(set->bits, set->size, start - set->min);
	if (bit >= set->size)
		bit = find_first_zero_bit(set->bits, set->size, 0);

	if (bit >= set->size)
		return set->max + 1;

	return bit + set->min;
}

/**
 * l_uintset_find_max:
 * @set: The set of numbers
 *
 * Returns: The maximum number preset in the set.  If the set of numbers is
 * empty returns l_uintset_get_max(set) + 1. If @set is NULL returns UINT_MAX.
 **/
LIB_EXPORT uint32_t l_uintset_find_max(struct l_uintset *set)
{
	unsigned int bit;

	if (unlikely(!set))
		return UINT_MAX;

	bit = find_last_bit(set->bits, set->size);

	if (bit >= set->size)
		return set->max + 1;

	return bit + set->min;
}

/**
 * l_uintset_find_min:
 * @set: The set of numbers
 *
 * Returns: The minimum number preset in the set.  If the set of numbers is
 * empty returns l_uintset_get_max(set) + 1. If @set is NULL returns UINT_MAX.
 **/
LIB_EXPORT uint32_t l_uintset_find_min(struct l_uintset *set)
{
	unsigned int bit;

	if (unlikely(!set))
		return UINT_MAX;

	bit = find_first_bit(set->bits, set->size);

	if (bit >= set->size)
		return set->max + 1;

	return bit + set->min;
}
