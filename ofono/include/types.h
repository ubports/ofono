/*
 *
 *  oFono - Open Source Telephony
 *
 *  Copyright (C) 2008-2011  Intel Corporation. All rights reserved.
 *  Copyright (C) 2015-2021  Jolla Ltd.
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

#ifndef __OFONO_TYPES_H
#define __OFONO_TYPES_H

#ifdef __cplusplus
extern "C" {
#endif

#ifndef	FALSE
#define	FALSE	(0)
#endif

#ifndef	TRUE
#define	TRUE	(!FALSE)
#endif

typedef int		ofono_bool_t;

/* MCC is always three digits. MNC is either two or three digits */
#define OFONO_MAX_MCC_LENGTH 3
#define OFONO_MAX_MNC_LENGTH 3

typedef void (*ofono_destroy_func)(void *data);

enum ofono_access_technology {
	OFONO_ACCESS_TECHNOLOGY_NONE = -1,
	/* 27.007 Section 7.3 <AcT> */
	OFONO_ACCESS_TECHNOLOGY_GSM = 0,
	OFONO_ACCESS_TECHNOLOGY_GSM_COMPACT = 1,
	OFONO_ACCESS_TECHNOLOGY_UTRAN = 2,
	OFONO_ACCESS_TECHNOLOGY_GSM_EGPRS = 3,
	OFONO_ACCESS_TECHNOLOGY_UTRAN_HSDPA = 4,
	OFONO_ACCESS_TECHNOLOGY_UTRAN_HSUPA = 5,
	OFONO_ACCESS_TECHNOLOGY_UTRAN_HSDPA_HSUPA = 6,
	OFONO_ACCESS_TECHNOLOGY_EUTRAN = 7
};

/* 27.007 Section 6.2 */
enum ofono_clir_option {
	OFONO_CLIR_OPTION_DEFAULT = 0,
	OFONO_CLIR_OPTION_INVOCATION = 1,
	OFONO_CLIR_OPTION_SUPPRESSION = 2,
};

/* 27.007 Section 7.6 */
enum ofono_clip_validity {
	OFONO_CLIP_VALIDITY_VALID = 0,
	OFONO_CLIP_VALIDITY_WITHHELD = 1,
	OFONO_CLIP_VALIDITY_NOT_AVAILABLE = 2
};

/* 27.007 Section 7.30 */
enum ofono_cnap_validity {
	OFONO_CNAP_VALIDITY_VALID = 0,
	OFONO_CNAP_VALIDITY_WITHHELD = 1,
	OFONO_CNAP_VALIDITY_NOT_AVAILABLE = 2
};

/* 27.007 Section 7.18 */
enum ofono_call_status {
	OFONO_CALL_STATUS_ACTIVE = 0,
	OFONO_CALL_STATUS_HELD = 1,
	OFONO_CALL_STATUS_DIALING = 2,
	OFONO_CALL_STATUS_ALERTING = 3,
	OFONO_CALL_STATUS_INCOMING = 4,
	OFONO_CALL_STATUS_WAITING = 5,
	OFONO_CALL_STATUS_DISCONNECTED
};

/* 27.007 Section 7.18 */
enum ofono_call_direction {
	OFONO_CALL_DIRECTION_MOBILE_ORIGINATED = 0,
	OFONO_CALL_DIRECTION_MOBILE_TERMINATED = 1
};

enum ofono_sms_charset {
	OFONO_SMS_CHARSET_7BIT = 0,
	OFONO_SMS_CHARSET_8BIT = 1,
	OFONO_SMS_CHARSET_UCS2 = 2
};

enum ofono_error_type {
	OFONO_ERROR_TYPE_NO_ERROR = 0,
	OFONO_ERROR_TYPE_CME,
	OFONO_ERROR_TYPE_CMS,
	OFONO_ERROR_TYPE_CEER,
	OFONO_ERROR_TYPE_SIM,
	OFONO_ERROR_TYPE_FAILURE,
	OFONO_ERROR_TYPE_ERRNO
};

enum ofono_disconnect_reason {
	OFONO_DISCONNECT_REASON_UNKNOWN = 0,
	OFONO_DISCONNECT_REASON_LOCAL_HANGUP,
	OFONO_DISCONNECT_REASON_REMOTE_HANGUP,
	OFONO_DISCONNECT_REASON_ERROR,
};

struct ofono_error {
	enum ofono_error_type type;
	int error;
};

#define OFONO_MAX_PHONE_NUMBER_LENGTH 80
#define OFONO_MAX_CALLER_NAME_LENGTH 80

/* Number types, 3GPP TS 24.008 subclause 10.5.4.7, octect 3 */
/* Unknown, ISDN numbering plan */
#define OFONO_NUMBER_TYPE_UNKNOWN 129
/* International, ISDN numbering plan */
#define OFONO_NUMBER_TYPE_INTERNATIONAL 145

struct ofono_phone_number {
	char number[OFONO_MAX_PHONE_NUMBER_LENGTH + 1];
	int type;
};

/* Length of NUM_FIELDS in 3GPP2 C.S0005-E v2.0 */
#define OFONO_CDMA_MAX_PHONE_NUMBER_LENGTH 256

struct ofono_cdma_phone_number {
	/* char maps to max size of CHARi (8 bit) in 3GPP2 C.S0005-E v2.0 */
	char number[OFONO_CDMA_MAX_PHONE_NUMBER_LENGTH];
};

struct ofono_call {
	unsigned int id;
	int type;
	enum ofono_call_direction direction;
	enum ofono_call_status status;
	struct ofono_phone_number phone_number;
	struct ofono_phone_number called_number;
	char name[OFONO_MAX_CALLER_NAME_LENGTH + 1];
	enum ofono_clip_validity clip_validity;
	enum ofono_cnap_validity cnap_validity;
};

struct ofono_network_time {
	int sec;	/* Seconds [0..59], -1 if unavailable */
	int min;	/* Minutes [0..59], -1 if unavailable */
	int hour;	/* Hours [0..23], -1 if unavailable */
	int mday;	/* Day of month [1..31], -1 if unavailable */
	int mon;	/* Month [1..12], -1 if unavailable */
	int year;	/* Current year, -1 if unavailable */
	int dst;	/* Current adjustment, in hours */
	int utcoff;	/* Offset from UTC in seconds */
};

#define OFONO_SHA1_UUID_LEN 20

struct ofono_uuid {
	unsigned char uuid[OFONO_SHA1_UUID_LEN];
};

const char *ofono_uuid_to_str(const struct ofono_uuid *uuid);
void ofono_call_init(struct ofono_call *call);

#ifdef __cplusplus
}
#endif

#endif /* __OFONO_TYPES_H */
