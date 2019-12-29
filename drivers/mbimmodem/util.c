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

#include <stdint.h>
#include <stdbool.h>

#include "src/common.h"
#include "mbim.h"
#include "util.h"

int mbim_data_class_to_tech(uint32_t n)
{
	if (n & MBIM_DATA_CLASS_LTE)
		return ACCESS_TECHNOLOGY_EUTRAN;

	if (n & (MBIM_DATA_CLASS_HSUPA | MBIM_DATA_CLASS_HSDPA))
		return ACCESS_TECHNOLOGY_UTRAN_HSDPA_HSUPA;

	if (n & MBIM_DATA_CLASS_HSUPA)
		return ACCESS_TECHNOLOGY_UTRAN_HSUPA;

	if (n & MBIM_DATA_CLASS_HSDPA)
		return ACCESS_TECHNOLOGY_UTRAN_HSDPA;

	if (n & MBIM_DATA_CLASS_UMTS)
		return ACCESS_TECHNOLOGY_UTRAN;

	if (n & MBIM_DATA_CLASS_EDGE)
		return ACCESS_TECHNOLOGY_GSM_EGPRS;

	if (n & MBIM_DATA_CLASS_GPRS)
		return ACCESS_TECHNOLOGY_GSM;

	return -1;
}

