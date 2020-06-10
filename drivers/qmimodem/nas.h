/*
 *
 *  oFono - Open Source Telephony
 *
 *  Copyright (C) 2011-2012  Intel Corporation. All rights reserved.
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

#define QMI_NAS_RESET			0	/* Reset NAS service state variables */
#define QMI_NAS_ABORT			1	/* Abort previously issued NAS command */
#define QMI_NAS_EVENT			2	/* Connection state report indication */
#define QMI_NAS_SET_EVENT		2	/* Set NAS state report conditions */
#define QMI_NAS_SET_REG_EVENT		3	/* Set NAS registration report conditions */

#define QMI_NAS_GET_RSSI		32	/* Get the signal strength */
#define QMI_NAS_SCAN_NETS		33	/* Scan for visible network */
#define QMI_NAS_REGISTER_NET		34	/* Initiate a network registration */
#define QMI_NAS_ATTACH_DETACH		35	/* Initiate an attach or detach action */
#define QMI_NAS_GET_SS_INFO		36	/* Get info about current serving system */
#define QMI_NAS_SS_INFO_IND		36	/* Current serving system info indication */
#define QMI_NAS_GET_HOME_INFO		37	/* Get info about home network */

#define QMI_NAS_SET_SYSTEM_SELECTION_PREF 51
#define QMI_NAS_GET_SYSTEM_SELECTION_PREF 52

/* Set NAS state report conditions */
#define QMI_NAS_PARAM_REPORT_SIGNAL_STRENGTH	0x10
struct qmi_nas_param_event_signal_strength {
	uint8_t report;					/* bool */
	uint8_t count;
	int8_t dbm[5];
} __attribute__((__packed__));
#define QMI_NAS_PARAM_REPORT_RF_INFO		0x11
struct qmi_nas_param_event_rf_info {
	uint8_t report;					/* bool */
} __attribute__((__packed__));

#define QMI_NAS_NOTIFY_SIGNAL_STRENGTH		0x10
struct qmi_nas_signal_strength {
	int8_t dbm;
	uint8_t rat;
} __attribute__((__packed__));

#define QMI_NAS_NOTIFY_RF_INFO			0x11
struct qmi_nas_rf_info {
	uint8_t count;
	struct {
		uint8_t rat;
		uint16_t band;
		uint16_t channel;
	} __attribute__((__packed__)) info[0];
} __attribute__((__packed__));

/* Get the signal strength */
#define QMI_NAS_RESULT_SIGNAL_STRENGTH		0x01

/* Scan for visible network */
#define QMI_NAS_PARAM_NETWORK_MASK		0x10	/* uint8 bitmask */

#define QMI_NAS_NETWORK_MASK_GSM		(1 << 0)
#define QMI_NAS_NETWORK_MASK_UMTS		(1 << 1)
#define QMI_NAS_NETWORK_MASK_LTE		(1 << 2)
#define QMI_NAS_NETWORK_MASK_TDSCDMA		(1 << 3)

#define QMI_NAS_RESULT_NETWORK_LIST		0x10
struct qmi_nas_network_info {
	uint16_t mcc;
	uint16_t mnc;
	uint8_t status;
	uint8_t desc_len;
	char desc[0];
} __attribute__((__packed__));
struct qmi_nas_network_list {
	uint16_t count;
	struct qmi_nas_network_info info[0];
} __attribute__((__packed__));
#define QMI_NAS_RESULT_NETWORK_RAT		0x11
struct qmi_nas_network_rat {
	uint16_t count;
	struct {
		uint16_t mcc;
		uint16_t mnc;
		uint8_t rat;
	} __attribute__((__packed__)) info[0];
} __attribute__((__packed__));

#define QMI_NAS_NETWORK_RAT_NONE		0x00
#define QMI_NAS_NETWORK_RAT_GSM			0x04
#define QMI_NAS_NETWORK_RAT_UMTS		0x05
#define QMI_NAS_NETWORK_RAT_LTE			0x08
#define QMI_NAS_NETWORK_RAT_TDSCDMA		0x09
#define QMI_NAS_NETWORK_RAT_NO_CHANGE		0xff

/* Initiate a network registration */
#define QMI_NAS_PARAM_REGISTER_ACTION		0x01	/* uint8 */
#define QMI_NAS_PARAM_REGISTER_MANUAL_INFO	0x10
struct qmi_nas_param_register_manual_info {
	uint16_t mcc;
	uint16_t mnc;
	uint8_t rat;
} __attribute__((__packed__));

#define QMI_NAS_REGISTER_ACTION_AUTO		0x01
#define QMI_NAS_REGISTER_ACTION_MANUAL		0x02

/* Initiate an attach or detach action */
#define QMI_NAS_PARAM_ATTACH_ACTION		0x10	/* uint8 */

#define QMI_NAS_ATTACH_ACTION_ATTACH		0x01
#define QMI_NAS_ATTACH_ACTION_DETACH		0x02

/* Get info about current serving system */
#define QMI_NAS_RESULT_SERVING_SYSTEM		0x01
struct qmi_nas_serving_system {
	uint8_t status;
	uint8_t cs_state;
	uint8_t ps_state;
	uint8_t network;
	uint8_t radio_if_count;
	uint8_t radio_if[0];
} __attribute__((__packed__));
#define QMI_NAS_RESULT_ROAMING_STATUS		0x10	/* uint8 */

#define QMI_NAS_RESULT_DATA_CAPABILIT_STATUS		0x11	/* uint8 */
struct qmi_nas_data_capability {
	uint8_t cap_count;
	uint8_t cap[0];
} __attribute__((__packed__));

#define QMI_NAS_DATA_CAPABILITY_NONE          0x00
#define QMI_NAS_DATA_CAPABILITY_GPRS          0x01
#define QMI_NAS_DATA_CAPABILITY_EDGE          0x02
#define QMI_NAS_DATA_CAPABILITY_HSDPA         0x03
#define QMI_NAS_DATA_CAPABILITY_HSUPA         0x04
#define QMI_NAS_DATA_CAPABILITY_WCDMA         0x05
#define QMI_NAS_DATA_CAPABILITY_CDMA          0x06
#define QMI_NAS_DATA_CAPABILITY_EVDO_REV_0    0x07
#define QMI_NAS_DATA_CAPABILITY_EVDO_REV_A    0x08
#define QMI_NAS_DATA_CAPABILITY_GSM           0x09
#define QMI_NAS_DATA_CAPABILITY_EVDO_REV_B    0x0A
#define QMI_NAS_DATA_CAPABILITY_LTE           0x0B
#define QMI_NAS_DATA_CAPABILITY_HSDPA_PLUS    0x0C
#define QMI_NAS_DATA_CAPABILITY_DC_HSDPA_PLUS 0x0D

#define QMI_NAS_RESULT_CURRENT_PLMN		0x12
struct qmi_nas_current_plmn {
	uint16_t mcc;
	uint16_t mnc;
	uint8_t desc_len;
	char desc[0];
} __attribute__((__packed__));
#define QMI_NAS_RESULT_LOCATION_AREA_CODE	0x1d	/* uint16 */
#define QMI_NAS_RESULT_CELL_ID			0x1e	/* uint32 */

/* qmi_nas_serving_system.status */
#define QMI_NAS_REGISTRATION_STATE_NOT_REGISTERED	0x00
#define QMI_NAS_REGISTRATION_STATE_REGISTERED		0x01
#define QMI_NAS_REGISTRATION_STATE_SEARCHING		0x02
#define QMI_NAS_REGISTRATION_STATE_DENIED		0x03
#define QMI_NAS_REGISTRATION_STATE_UNKNOWN		0x04

#define QMI_NAS_RESULT_3GGP_DST 0x1b
#define QMI_NAS_RESULT_3GPP_TIME 0x1c
struct qmi_nas_3gpp_time {
	uint16_t year;
	uint8_t month;
	uint8_t day;
	uint8_t hour;
	uint8_t minute;
	uint8_t second;
	uint8_t timezone;
} __attribute__((__packed__));

/* cs_state/ps_state */
#define QMI_NAS_ATTACH_STATE_INVALID		0x00
#define QMI_NAS_ATTACH_STATE_ATTACHED		0x01
#define QMI_NAS_ATTACH_STATE_DETACHED		0x02

/* Get info about home network */
#define QMI_NAS_RESULT_HOME_NETWORK		0x01
struct qmi_nas_home_network {
	uint16_t mcc;
	uint16_t mnc;
	uint8_t desc_len;
	char desc[0];
} __attribute__((__packed__));

#define QMI_NAS_RAT_MODE_PREF_ANY		(-1)
#define QMI_NAS_RAT_MODE_PREF_GSM		(1 << 2)
#define QMI_NAS_RAT_MODE_PREF_UMTS		(1 << 3) | (1 << 2)
#define QMI_NAS_RAT_MODE_PREF_LTE		(1 << 4) | (1 << 3) | (1 << 2)

#define QMI_NAS_PARAM_SYSTEM_SELECTION_PREF_MODE	0x11

#define QMI_NAS_RESULT_SYSTEM_SELECTION_PREF_MODE	0x11

int qmi_nas_rat_to_tech(uint8_t rat);
int qmi_nas_cap_to_bearer_tech(int cap_tech);
