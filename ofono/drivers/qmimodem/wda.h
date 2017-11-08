/*
 *
 *  oFono - Open Source Telephony
 *
 *  Copyright (C) 2017  Kerlink SA. All rights reserved.
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
 */

#define QMI_WDA_SET_DATA_FORMAT	32	/* Set data format */
#define QMI_WDA_GET_DATA_FORMAT	33	/* Get data format */

/* Get and set data format interface */
#define QMI_WDA_LL_PROTOCOL	0x11	/* uint32_t */
#define QMI_WDA_DATA_LINK_PROTOCOL_UNKNOWN	0
#define QMI_WDA_DATA_LINK_PROTOCOL_802_3	1
#define QMI_WDA_DATA_LINK_PROTOCOL_RAW_IP	2
