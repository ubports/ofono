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

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

#include "mbim-desc.h"

/*
 * Attempts to find MBIM specific descriptors.
 *
 * Returns true if the MBIM Function descriptor was found, false otherwise.
 */
bool mbim_find_descriptors(const uint8_t *data, size_t data_len,
				const struct mbim_desc **out_desc,
				const struct mbim_extended_desc **out_ext_desc)
{
	bool r = false;

	while (data_len > 3) {
		uint8_t len = data[0];

		if (data[1] != 0x24)
			goto next;

		/* MBIM v1.0, Table 4-3 */
		switch (data[2]) {
		case 0x1b:
			if (!out_desc)
				break;

			if (len != sizeof(struct mbim_desc) || data_len < len)
				break;

			*out_desc = (const struct mbim_desc *) data;
			r = true;
			break;
		case 0x1c:
			if (!out_ext_desc)
				break;

			if (len != sizeof(struct mbim_extended_desc) ||
					data_len < len)
				break;

			*out_ext_desc =
				(const struct mbim_extended_desc *) data;
			break;
		}

next:
		data_len -= len;
		data += len;
	}

	return r;
}
