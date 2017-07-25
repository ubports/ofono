/*
 *
 *  oFono - Open Source Telephony
 *
 *  Copyright (C) 2017 Alexander Couzens <lynxis@fe80.eu>
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

#include <stdint.h>

#include "voice.h"
#include "src/common.h"

#define _(X) case X: return #X

const char *qmi_voice_call_state_name(enum qmi_voice_call_state value)
{
	switch (value) {
		_(QMI_CALL_STATE_IDLE);
		_(QMI_CALL_STATE_ORIG);
		_(QMI_CALL_STATE_INCOMING);
		_(QMI_CALL_STATE_CONV);
		_(QMI_CALL_STATE_CC_IN_PROG);
		_(QMI_CALL_STATE_ALERTING);
		_(QMI_CALL_STATE_HOLD);
		_(QMI_CALL_STATE_WAITING);
		_(QMI_CALL_STATE_DISCONNECTING);
		_(QMI_CALL_STATE_END);
		_(QMI_CALL_STATE_SETUP);
	}
	return "QMI_CALL_STATE_<UNKNOWN>";
}

int qmi_to_ofono_status(uint8_t status, int *ret) {
	int err = 0;
	switch (status) {
	case QMI_CALL_STATE_IDLE:
	case QMI_CALL_STATE_END:
	case QMI_CALL_STATE_DISCONNECTING:
		*ret = CALL_STATUS_DISCONNECTED;
		break;
	case QMI_CALL_STATE_HOLD:
		*ret = CALL_STATUS_HELD;
		break;
	case QMI_CALL_STATE_WAITING:
		*ret = CALL_STATUS_WAITING;
		break;
	case QMI_CALL_STATE_ORIG:
		*ret = CALL_STATUS_DIALING;
		break;
	case QMI_CALL_STATE_INCOMING:
		*ret = CALL_STATUS_INCOMING;
		break;
	case QMI_CALL_STATE_CONV:
		*ret = CALL_STATUS_ACTIVE;
		break;
	case QMI_CALL_STATE_CC_IN_PROG:
	case QMI_CALL_STATE_SETUP:
		/* FIXME: unsure if _SETUP is dialing or not */
		*ret = CALL_STATUS_DIALING;
		break;
	case QMI_CALL_STATE_ALERTING:
		*ret = CALL_STATUS_ALERTING;
		break;
	default:
		err = 1;
	}
	return err;
}

uint8_t ofono_to_qmi_direction(enum call_direction ofono_direction) {
	return ofono_direction + 1;
}
enum call_direction qmi_to_ofono_direction(uint8_t qmi_direction) {
	return qmi_direction - 1;
}

