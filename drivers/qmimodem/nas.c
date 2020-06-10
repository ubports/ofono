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

int qmi_nas_cap_to_bearer_tech(int cap_tech)
{

	switch (cap_tech) {
	case QMI_NAS_DATA_CAPABILITY_GSM:
	case QMI_NAS_DATA_CAPABILITY_NONE:
		return PACKET_BEARER_NONE;
	case QMI_NAS_DATA_CAPABILITY_GPRS:
		return PACKET_BEARER_GPRS;
	case QMI_NAS_DATA_CAPABILITY_EDGE:
		return PACKET_BEARER_EGPRS;
	case QMI_NAS_DATA_CAPABILITY_EVDO_REV_0:
	case QMI_NAS_DATA_CAPABILITY_EVDO_REV_A:
	case QMI_NAS_DATA_CAPABILITY_EVDO_REV_B:
		return PACKET_BEARER_UMTS;
	case QMI_NAS_DATA_CAPABILITY_HSDPA:
		return PACKET_BEARER_HSDPA;
	case QMI_NAS_DATA_CAPABILITY_HSUPA:
		return PACKET_BEARER_HSUPA;
    case QMI_NAS_DATA_CAPABILITY_HSDPA_PLUS:
    case QMI_NAS_DATA_CAPABILITY_DC_HSDPA_PLUS:
		/*
		 * HSPAP is HSPA+; which ofono doesn't define;
		 * so, if differentiating HSPA and HSPA+ is
		 * important, then ofono needs to be patched,
		 * and we probably also need to introduce a
		 * new indicator icon.
		 */
		return PACKET_BEARER_HSUPA_HSDPA;
	case QMI_NAS_DATA_CAPABILITY_LTE:
		return PACKET_BEARER_EPS;
	default:
		return PACKET_BEARER_NONE;
	}
}
