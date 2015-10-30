/*
 *
 *  RIL constants adopted from AOSP's header:
 *
 *  /hardware/ril/reference_ril/ril.h
 *
 *  Copyright (C) 2013 Canonical Ltd.
 *  Copyright (C) 2013-2015 Jolla Ltd.
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License version 2 as
 *  published by the Free Software Foundation.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 */

#ifndef __RIL_CONSTANTS_H
#define __RIL_CONSTANTS_H 1

/* Error Codes */
#define RIL_E_SUCCESS 0
#define RIL_E_RADIO_NOT_AVAILABLE 1
#define RIL_E_GENERIC_FAILURE 2
#define RIL_E_PASSWORD_INCORRECT 3
#define RIL_E_SIM_PIN2 4
#define RIL_E_SIM_PUK2 5
#define RIL_E_REQUEST_NOT_SUPPORTED 6
#define RIL_E_CANCELLED 7
#define RIL_E_OP_NOT_ALLOWED_DURING_VOICE_CALL 8
#define RIL_E_OP_NOT_ALLOWED_BEFORE_REG_TO_NW 9
#define RIL_E_SMS_SEND_FAIL_RETRY 10
#define RIL_E_SIM_ABSENT 11
#define RIL_E_SUBSCRIPTION_NOT_AVAILABLE 12
#define RIL_E_MODE_NOT_SUPPORTED 13
#define RIL_E_FDN_CHECK_FAILURE 14
#define RIL_E_ILLEGAL_SIM_OR_ME 15
#define RIL_E_UNUSED 16
#define RIL_E_DIAL_MODIFIED_TO_USSD 17
#define RIL_E_DIAL_MODIFIED_TO_SS 18
#define RIL_E_DIAL_MODIFIED_TO_DIAL 19
#define RIL_E_USSD_MODIFIED_TO_DIAL 20
#define RIL_E_USSD_MODIFIED_TO_SS 21
#define RIL_E_USSD_MODIFIED_TO_USSD 22
#define RIL_E_SS_MODIFIED_TO_DIAL 23
#define RIL_E_SS_MODIFIED_TO_USSD 24
#define RIL_E_SS_MODIFIED_TO_SS 25
#define RIL_E_SUBSCRIPTION_NOT_SUPPORTED 26
#define RIL_E_MISSING_RESOURCE 27
#define RIL_E_NO_SUCH_ELEMENT 28
#define RIL_E_INVALID_PARAMETER 29

/* call states */
enum ril_call_state {
    RIL_CALL_ACTIVE,
    RIL_CALL_HOLDING,
    RIL_CALL_DIALING,
    RIL_CALL_ALERTING,
    RIL_CALL_INCOMING,
    RIL_CALL_WAITING
};

/* Radio state */
enum ril_radio_state {
    RADIO_STATE_OFF,
    RADIO_STATE_UNAVAILABLE,
    RADIO_STATE_SIM_NOT_READY,
    RADIO_STATE_SIM_LOCKED_OR_ABSENT,
    RADIO_STATE_SIM_READY,
    RADIO_STATE_RUIM_NOT_READY,
    RADIO_STATE_RUIM_READY,
    RADIO_STATE_RUIM_LOCKED_OR_ABSENT,
    RADIO_STATE_NV_NOT_READY,
    RADIO_STATE_NV_READY,
    RADIO_STATE_ON
};

/* Preferred network types */
enum ril_pref_net_type {
    PREF_NET_TYPE_GSM_WCDMA,
    PREF_NET_TYPE_GSM_ONLY,
    PREF_NET_TYPE_WCDMA,
    PREF_NET_TYPE_GSM_WCDMA_AUTO,
    PREF_NET_TYPE_CDMA_EVDO_AUTO,
    PREF_NET_TYPE_CDMA_ONLY,
    PREF_NET_TYPE_EVDO_ONLY,
    PREF_NET_TYPE_GSM_WCDMA_CDMA_EVDO_AUTO,
    PREF_NET_TYPE_LTE_CDMA_EVDO,
    PREF_NET_TYPE_LTE_GSM_WCDMA,
    PREF_NET_TYPE_LTE_CMDA_EVDO_GSM_WCDMA,
    PREF_NET_TYPE_LTE_ONLY,
    PREF_NET_TYPE_LTE_WCDMA
};

/* Radio technologies */
enum ril_radio_tech {
    RADIO_TECH_UNKNOWN,
    RADIO_TECH_GPRS,
    RADIO_TECH_EDGE,
    RADIO_TECH_UMTS,
    RADIO_TECH_IS95A,
    RADIO_TECH_IS95B,
    RADIO_TECH_1xRTT ,
    RADIO_TECH_EVDO_0,
    RADIO_TECH_EVDO_A,
    RADIO_TECH_HSDPA,
    RADIO_TECH_HSUPA ,
    RADIO_TECH_HSPA,
    RADIO_TECH_EVDO_B,
    RADIO_TECH_EHRPD,
    RADIO_TECH_LTE,
    RADIO_TECH_HSPAP,
    RADIO_TECH_GSM,
    RADIO_TECH_TD_SCDMA,
    RADIO_TECH_DC_HSDPA
};

/* See RIL_REQUEST_LAST_CALL_FAIL_CAUSE */
#define CALL_FAIL_UNOBTAINABLE_NUMBER 1
#define CALL_FAIL_NORMAL 16
#define CALL_FAIL_BUSY 17
#define CALL_FAIL_CONGESTION 34
#define CALL_FAIL_ACM_LIMIT_EXCEEDED 68
#define CALL_FAIL_CALL_BARRED 240
#define CALL_FAIL_FDN_BLOCKED 241
#define CALL_FAIL_IMSI_UNKNOWN_IN_VLR 242
#define CALL_FAIL_IMEI_NOT_ACCEPTED 243
#define CALL_FAIL_DIAL_MODIFIED_TO_USSD 244
#define CALL_FAIL_DIAL_MODIFIED_TO_SS 245
#define CALL_FAIL_DIAL_MODIFIED_TO_DIAL 246
#define CALL_FAIL_CDMA_LOCKED_UNTIL_POWER_CYCLE 1000
#define CALL_FAIL_CDMA_DROP 1001
#define CALL_FAIL_CDMA_INTERCEPT 1002
#define CALL_FAIL_CDMA_REORDER 1003
#define CALL_FAIL_CDMA_SO_REJECT 1004
#define CALL_FAIL_CDMA_RETRY_ORDER 1005
#define CALL_FAIL_CDMA_ACCESS_FAILURE 1006
#define CALL_FAIL_CDMA_PREEMPTED 1007
#define CALL_FAIL_CDMA_NOT_EMERGENCY 1008
#define CALL_FAIL_CDMA_ACCESS_BLOCKED 1009
#define CALL_FAIL_ERROR_UNSPECIFIED 0xffff

/* see RIL_REQUEST_DEACTIVATE_DATA_CALL parameter*/
#define RIL_DEACTIVATE_DATA_CALL_NO_REASON 0
#define RIL_DEACTIVATE_DATA_CALL_RADIO_SHUTDOWN 1

/* See RIL_REQUEST_SETUP_DATA_CALL */

#define RIL_DATA_PROFILE_DEFAULT 0
#define RIL_DATA_PROFILE_TETHERED 1
#define RIL_DATA_PROFILE_IMS 2
#define RIL_DATA_PROFILE_FOTA 3           /* FOTA = Firmware Over the Air */
#define RIL_DATA_PROFILE_CBS 4
#define RIL_DATA_PROFILE_OEM_BASE 1000    /* Start of OEM-specific profiles */

#define RIL_AUTH_NONE 0
#define RIL_AUTH_PAP 1
#define RIL_AUTH_CHAP 2
#define RIL_AUTH_BOTH 3

/* SIM card states */
#define RIL_CARDSTATE_ABSENT 0
#define RIL_CARDSTATE_PRESENT 1
#define RIL_CARDSTATE_ERROR 2

/* SIM personalization substates */
#define RIL_PERSOSUBSTATE_UNKNOWN 0
#define RIL_PERSOSUBSTATE_IN_PROGRESS 1
#define RIL_PERSOSUBSTATE_READY 2
#define RIL_PERSOSUBSTATE_SIM_NETWORK 3
#define RIL_PERSOSUBSTATE_SIM_NETWORK_SUBSET 4
#define RIL_PERSOSUBSTATE_SIM_CORPORATE 5
#define RIL_PERSOSUBSTATE_SIM_SERVICE_PROVIDER 6
#define RIL_PERSOSUBSTATE_SIM_SIM 7
#define RIL_PERSOSUBSTATE_SIM_NETWORK_PUK 8
#define RIL_PERSOSUBSTATE_SIM_NETWORK_SUBSET_PUK 9
#define RIL_PERSOSUBSTATE_SIM_CORPORATE_PUK 10
#define RIL_PERSOSUBSTATE_SIM_SERVICE_PROVIDER_PUK 11
#define RIL_PERSOSUBSTATE_SIM_SIM_PUK 12
#define RIL_PERSOSUBSTATE_RUIM_NETWORK1 13
#define RIL_PERSOSUBSTATE_RUIM_NETWORK2 14
#define RIL_PERSOSUBSTATE_RUIM_HRPD 15
#define RIL_PERSOSUBSTATE_RUIM_CORPORATE 16
#define RIL_PERSOSUBSTATE_RUIM_SERVICE_PROVIDER 17
#define RIL_PERSOSUBSTATE_RUIM_RUIM 18
#define RIL_PERSOSUBSTATE_RUIM_NETWORK1_PUK 19
#define RIL_PERSOSUBSTATE_RUIM_NETWORK2_PUK 20
#define RIL_PERSOSUBSTATE_RUIM_HRPD_PUK 21
#define RIL_PERSOSUBSTATE_RUIM_CORPORATE_PUK 22
#define RIL_PERSOSUBSTATE_RUIM_SERVICE_PROVIDER_PUK 23
#define RIL_PERSOSUBSTATE_RUIM_RUIM_PUK 24

/* SIM - App states */
#define RIL_APPSTATE_ILLEGAL -1
#define RIL_APPSTATE_UNKNOWN 0
#define RIL_APPSTATE_DETECTED 1
#define RIL_APPSTATE_PIN 2
#define RIL_APPSTATE_PUK 3
#define RIL_APPSTATE_SUBSCRIPTION_PERSO 4
#define RIL_APPSTATE_READY 5

/* SIM - PIN states */
#define RIL_PINSTATE_UNKNOWN 0
#define RIL_PINSTATE_ENABLED_NOT_VERIFIED 1
#define RIL_PINSTATE_ENABLED_VERIFIED 2
#define RIL_PINSTATE_DISABLED 3
#define RIL_PINSTATE_ENABLED_BLOCKED 4
#define RIL_PINSTATE_ENABLED_PERM_BLOCKED 5

/* SIM - App types */
#define RIL_APPTYPE_UNKNOWN 0
#define RIL_APPTYPE_SIM 1
#define RIL_APPTYPE_USIM 2
#define RIL_APPTYPE_RUIM 3
#define RIL_APPTYPE_CSIM 4
#define RIL_APPTYPE_ISIM 5

/* RIL Request Messages */
#define RIL_REQUEST_GET_SIM_STATUS 1
#define RIL_REQUEST_ENTER_SIM_PIN 2
#define RIL_REQUEST_ENTER_SIM_PUK 3
#define RIL_REQUEST_ENTER_SIM_PIN2 4
#define RIL_REQUEST_ENTER_SIM_PUK2 5
#define RIL_REQUEST_CHANGE_SIM_PIN 6
#define RIL_REQUEST_CHANGE_SIM_PIN2 7
#define RIL_REQUEST_ENTER_NETWORK_DEPERSONALIZATION 8
#define RIL_REQUEST_GET_CURRENT_CALLS 9
#define RIL_REQUEST_DIAL 10
#define RIL_REQUEST_GET_IMSI 11
#define RIL_REQUEST_HANGUP 12
#define RIL_REQUEST_HANGUP_WAITING_OR_BACKGROUND 13
#define RIL_REQUEST_HANGUP_FOREGROUND_RESUME_BACKGROUND 14
#define RIL_REQUEST_SWITCH_HOLDING_AND_ACTIVE 15
#define RIL_REQUEST_CONFERENCE 16
#define RIL_REQUEST_UDUB 17
#define RIL_REQUEST_LAST_CALL_FAIL_CAUSE 18
#define RIL_REQUEST_SIGNAL_STRENGTH 19
#define RIL_REQUEST_VOICE_REGISTRATION_STATE 20
#define RIL_REQUEST_DATA_REGISTRATION_STATE 21
#define RIL_REQUEST_OPERATOR 22
#define RIL_REQUEST_RADIO_POWER 23
#define RIL_REQUEST_DTMF 24
#define RIL_REQUEST_SEND_SMS 25
#define RIL_REQUEST_SEND_SMS_EXPECT_MORE 26
#define RIL_REQUEST_SETUP_DATA_CALL 27
#define RIL_REQUEST_SIM_IO 28
#define RIL_REQUEST_SEND_USSD 29
#define RIL_REQUEST_CANCEL_USSD 30
#define RIL_REQUEST_GET_CLIR 31
#define RIL_REQUEST_SET_CLIR 32
#define RIL_REQUEST_QUERY_CALL_FORWARD_STATUS 33
#define RIL_REQUEST_SET_CALL_FORWARD 34
#define RIL_REQUEST_QUERY_CALL_WAITING 35
#define RIL_REQUEST_SET_CALL_WAITING 36
#define RIL_REQUEST_SMS_ACKNOWLEDGE  37
#define RIL_REQUEST_GET_IMEI 38
#define RIL_REQUEST_GET_IMEISV 39
#define RIL_REQUEST_ANSWER 40
#define RIL_REQUEST_DEACTIVATE_DATA_CALL 41
#define RIL_REQUEST_QUERY_FACILITY_LOCK 42
#define RIL_REQUEST_SET_FACILITY_LOCK 43
#define RIL_REQUEST_CHANGE_BARRING_PASSWORD 44
#define RIL_REQUEST_QUERY_NETWORK_SELECTION_MODE 45
#define RIL_REQUEST_SET_NETWORK_SELECTION_AUTOMATIC 46
#define RIL_REQUEST_SET_NETWORK_SELECTION_MANUAL 47
#define RIL_REQUEST_QUERY_AVAILABLE_NETWORKS 48
#define RIL_REQUEST_DTMF_START 49
#define RIL_REQUEST_DTMF_STOP 50
#define RIL_REQUEST_BASEBAND_VERSION 51
#define RIL_REQUEST_SEPARATE_CONNECTION 52
#define RIL_REQUEST_SET_MUTE 53
#define RIL_REQUEST_GET_MUTE 54
#define RIL_REQUEST_QUERY_CLIP 55
#define RIL_REQUEST_LAST_DATA_CALL_FAIL_CAUSE 56
#define RIL_REQUEST_DATA_CALL_LIST 57
#define RIL_REQUEST_RESET_RADIO 58
#define RIL_REQUEST_OEM_HOOK_RAW 59
#define RIL_REQUEST_OEM_HOOK_STRINGS 60
#define RIL_REQUEST_SCREEN_STATE 61
#define RIL_REQUEST_SET_SUPP_SVC_NOTIFICATION 62
#define RIL_REQUEST_WRITE_SMS_TO_SIM 63
#define RIL_REQUEST_DELETE_SMS_ON_SIM 64
#define RIL_REQUEST_SET_BAND_MODE 65
#define RIL_REQUEST_QUERY_AVAILABLE_BAND_MODE 66
#define RIL_REQUEST_STK_GET_PROFILE 67
#define RIL_REQUEST_STK_SET_PROFILE 68
#define RIL_REQUEST_STK_SEND_ENVELOPE_COMMAND 69
#define RIL_REQUEST_STK_SEND_TERMINAL_RESPONSE 70
#define RIL_REQUEST_STK_HANDLE_CALL_SETUP_REQUESTED_FROM_SIM 71
#define RIL_REQUEST_EXPLICIT_CALL_TRANSFER 72
#define RIL_REQUEST_SET_PREFERRED_NETWORK_TYPE 73
#define RIL_REQUEST_GET_PREFERRED_NETWORK_TYPE 74
#define RIL_REQUEST_GET_NEIGHBORING_CELL_IDS 75
#define RIL_REQUEST_SET_LOCATION_UPDATES 76
#define RIL_REQUEST_CDMA_SET_SUBSCRIPTION_SOURCE 77
#define RIL_REQUEST_CDMA_SET_ROAMING_PREFERENCE 78
#define RIL_REQUEST_CDMA_QUERY_ROAMING_PREFERENCE 79
#define RIL_REQUEST_SET_TTY_MODE 80
#define RIL_REQUEST_QUERY_TTY_MODE 81
#define RIL_REQUEST_CDMA_SET_PREFERRED_VOICE_PRIVACY_MODE 82
#define RIL_REQUEST_CDMA_QUERY_PREFERRED_VOICE_PRIVACY_MODE 83
#define RIL_REQUEST_CDMA_FLASH 84
#define RIL_REQUEST_CDMA_BURST_DTMF 85
#define RIL_REQUEST_CDMA_VALIDATE_AND_WRITE_AKEY 86
#define RIL_REQUEST_CDMA_SEND_SMS 87
#define RIL_REQUEST_CDMA_SMS_ACKNOWLEDGE 88
#define RIL_REQUEST_GSM_GET_BROADCAST_SMS_CONFIG 89
#define RIL_REQUEST_GSM_SET_BROADCAST_SMS_CONFIG 90
#define RIL_REQUEST_GSM_SMS_BROADCAST_ACTIVATION 91
#define RIL_REQUEST_CDMA_GET_BROADCAST_SMS_CONFIG 92
#define RIL_REQUEST_CDMA_SET_BROADCAST_SMS_CONFIG 93
#define RIL_REQUEST_CDMA_SMS_BROADCAST_ACTIVATION 94
#define RIL_REQUEST_CDMA_SUBSCRIPTION 95
#define RIL_REQUEST_CDMA_WRITE_SMS_TO_RUIM 96
#define RIL_REQUEST_CDMA_DELETE_SMS_ON_RUIM 97
#define RIL_REQUEST_DEVICE_IDENTITY 98
#define RIL_REQUEST_EXIT_EMERGENCY_CALLBACK_MODE 99
#define RIL_REQUEST_GET_SMSC_ADDRESS 100
#define RIL_REQUEST_SET_SMSC_ADDRESS 101
#define RIL_REQUEST_REPORT_SMS_MEMORY_STATUS 102
#define RIL_REQUEST_REPORT_STK_SERVICE_IS_RUNNING 103
#define RIL_REQUEST_CDMA_GET_SUBSCRIPTION_SOURCE 104
#define RIL_REQUEST_ISIM_AUTHENTICATION 105
#define RIL_REQUEST_ACKNOWLEDGE_INCOMING_GSM_SMS_WITH_PDU 106
#define RIL_REQUEST_STK_SEND_ENVELOPE_WITH_STATUS 107
#define RIL_REQUEST_VOICE_RADIO_TECH 108
#define RIL_REQUEST_GET_CELL_INFO_LIST 109
#define RIL_REQUEST_SET_UNSOL_CELL_INFO_LIST_RATE 110
#define RIL_REQUEST_SET_INITIAL_ATTACH_APN 111
#define RIL_REQUEST_IMS_REGISTRATION_STATE 112
#define RIL_REQUEST_IMS_SEND_SMS 113
#define RIL_REQUEST_SIM_TRANSMIT_APDU_BASIC 114
#define RIL_REQUEST_SIM_OPEN_CHANNEL 115
#define RIL_REQUEST_SIM_CLOSE_CHANNEL 116
#define RIL_REQUEST_SIM_TRANSMIT_APDU_CHANNEL 117
#define RIL_REQUEST_NV_READ_ITEM 118
#define RIL_REQUEST_NV_WRITE_ITEM 119
#define RIL_REQUEST_NV_WRITE_CDMA_PRL 120
#define RIL_REQUEST_NV_RESET_CONFIG 121
#define RIL_REQUEST_SET_UICC_SUBSCRIPTION  122
#define RIL_REQUEST_ALLOW_DATA  123
#define RIL_REQUEST_GET_HARDWARE_CONFIG 124
#define RIL_REQUEST_SIM_AUTHENTICATION 125
#define RIL_REQUEST_GET_DC_RT_INFO 126
#define RIL_REQUEST_SET_DC_RT_INFO_RATE 127
#define RIL_REQUEST_SET_DATA_PROFILE 128
#define RIL_REQUEST_SHUTDOWN 129
#define RIL_REQUEST_GET_RADIO_CAPABILITY 130
#define RIL_REQUEST_SET_RADIO_CAPABILITY 131

/* RIL Unsolicited Messages */
#define RIL_UNSOL_RESPONSE_BASE 1000
#define RIL_UNSOL_RESPONSE_RADIO_STATE_CHANGED 1000
#define RIL_UNSOL_RESPONSE_CALL_STATE_CHANGED 1001
#define RIL_UNSOL_RESPONSE_VOICE_NETWORK_STATE_CHANGED 1002
#define RIL_UNSOL_RESPONSE_NEW_SMS 1003
#define RIL_UNSOL_RESPONSE_NEW_SMS_STATUS_REPORT 1004
#define RIL_UNSOL_RESPONSE_NEW_SMS_ON_SIM 1005
#define RIL_UNSOL_ON_USSD 1006
#define RIL_UNSOL_ON_USSD_REQUEST 1007
#define RIL_UNSOL_NITZ_TIME_RECEIVED  1008
#define RIL_UNSOL_SIGNAL_STRENGTH  1009
#define RIL_UNSOL_DATA_CALL_LIST_CHANGED 1010
#define RIL_UNSOL_SUPP_SVC_NOTIFICATION 1011
#define RIL_UNSOL_STK_SESSION_END 1012
#define RIL_UNSOL_STK_PROACTIVE_COMMAND 1013
#define RIL_UNSOL_STK_EVENT_NOTIFY 1014
#define RIL_UNSOL_STK_CALL_SETUP 1015
#define RIL_UNSOL_SIM_SMS_STORAGE_FULL 1016
#define RIL_UNSOL_SIM_REFRESH 1017
#define RIL_UNSOL_CALL_RING 1018
#define RIL_UNSOL_RESPONSE_SIM_STATUS_CHANGED 1019
#define RIL_UNSOL_RESPONSE_CDMA_NEW_SMS 1020
#define RIL_UNSOL_RESPONSE_NEW_BROADCAST_SMS 1021
#define RIL_UNSOL_CDMA_RUIM_SMS_STORAGE_FULL 1022
#define RIL_UNSOL_RESTRICTED_STATE_CHANGED 1023
#define RIL_UNSOL_ENTER_EMERGENCY_CALLBACK_MODE 1024
#define RIL_UNSOL_CDMA_CALL_WAITING 1025
#define RIL_UNSOL_CDMA_OTA_PROVISION_STATUS 1026
#define RIL_UNSOL_CDMA_INFO_REC 1027
#define RIL_UNSOL_OEM_HOOK_RAW 1028
#define RIL_UNSOL_RINGBACK_TONE 1029
#define RIL_UNSOL_RESEND_INCALL_MUTE 1030
#define RIL_UNSOL_CDMA_SUBSCRIPTION_SOURCE_CHANGED 1031
#define RIL_UNSOL_CDMA_PRL_CHANGED 1032
#define RIL_UNSOL_EXIT_EMERGENCY_CALLBACK_MODE 1033
#define RIL_UNSOL_RIL_CONNECTED 1034
#define RIL_UNSOL_VOICE_RADIO_TECH_CHANGED 1035
#define RIL_UNSOL_CELL_INFO_LIST 1036
#define RIL_UNSOL_RESPONSE_IMS_NETWORK_STATE_CHANGED 1037
#define RIL_UNSOL_UICC_SUBSCRIPTION_STATUS_CHANGED 1038
#define RIL_UNSOL_SRVCC_STATE_NOTIFY 1039
#define RIL_UNSOL_HARDWARE_CONFIG_CHANGED 1040
#define RIL_UNSOL_DC_RT_INFO_CHANGED 1041
#define RIL_UNSOL_RADIO_CAPABILITY 1042
#define RIL_UNSOL_ON_SS 1043
#define RIL_UNSOL_STK_CC_ALPHA_NOTIFY 1044

/* Suplementary services Service class*/
#define SERVICE_CLASS_NONE 0

/* RIL_FACILITY_LOCK parameters */
#define RIL_FACILITY_UNLOCK "0"
#define RIL_FACILITY_LOCK "1"

#endif /*__RIL_CONSTANTS_H */
