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

#include <linux/types.h>

/* MBIM v1.0, Section 6.4: MBIM Functional Descriptor */
struct mbim_desc {
	uint8_t bFunctionLength;
	uint8_t bDescriptorType;
	uint8_t bDescriptorSubtype;
	__le16 bcdMBIMVersion;
	__le16 wMaxControlMessage;
	uint8_t bNumberFilters;
	uint8_t bMaxFilterSize;
	__le16 wMaxSegmentSize;
	uint8_t bmNetworkCapabilities;
} __attribute__ ((packed));

/* MBIM v1.0, Section 6.5: MBIM Extended Functional Descriptor */
struct mbim_extended_desc {
	uint8_t	bFunctionLength;
	uint8_t	bDescriptorType;
	uint8_t bDescriptorSubtype;
	__le16 bcdMBIMExtendedVersion;
	uint8_t bMaxOutstandingCommandMessages;
	__le16 wMTU;
} __attribute__ ((packed));

bool mbim_find_descriptors(const uint8_t *data, size_t data_len,
				const struct mbim_desc **out_desc,
				const struct mbim_extended_desc **out_ext_desc);
