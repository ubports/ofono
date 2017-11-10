/*
 *
 *  oFono - Open Source Telephony
 *
 *  Copyright (C) 2017  Jonas Bonn. All rights reserved.
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

#include "nas.h"

#include "src/common.h"

int qmi_nas_rat_to_tech(uint8_t rat)
{
	switch (rat) {
	case QMI_NAS_NETWORK_RAT_GSM:
		return ACCESS_TECHNOLOGY_GSM;
	case QMI_NAS_NETWORK_RAT_UMTS:
		return ACCESS_TECHNOLOGY_UTRAN;
	case QMI_NAS_NETWORK_RAT_LTE:
		return ACCESS_TECHNOLOGY_EUTRAN;
	}

	return -1;
}
