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

struct mbim_device;
struct mbim_message;

#define MBIM_CID_DEVICE_CAPS			1
#define MBIM_CID_SUBSCRIBER_READY_STATUS	2
#define MBIM_CID_RADIO_STATE			3
#define MBIM_CID_PIN				4
#define MBIM_CID_PIN_LIST			5
#define MBIM_CID_HOME_PROVIDER			6
#define MBIM_CID_PREFERRED_PROVIDERS		7
#define MBIM_CID_VISIBLE_PROVIDERS		8
#define MBIM_CID_REGISTER_STATE			9
#define MBIM_CID_PACKET_SERVICE			10
#define MBIM_CID_SIGNAL_STATE			11
#define MBIM_CID_CONNECT			12
#define MBIM_CID_PROVISIONED_CONTEXTS		13
#define MBIM_CID_SERVICE_ACTIVATION		14
#define MBIM_CID_IP_CONFIGURATION		15
#define MBIM_CID_DEVICE_SERVICES		16
#define MBIM_CID_DEVICE_SERVICE_SUBSCRIBE_LIST	19
#define MBIM_CID_PACKET_STATISTICS		20
#define MBIM_CID_NETWORK_IDLE_HINT		21
#define MBIM_CID_EMERGENCY_MODE			22
#define MBIM_CID_IP_PACKET_FILTERS		23
#define MBIM_CID_MULTICARRIER_PROVIDERS		24

#define MBIM_CID_SMS_CONFIGURATION		1
#define MBIM_CID_SMS_READ			2
#define MBIM_CID_SMS_SEND			3
#define MBIM_CID_SMS_DELETE			4
#define MBIM_CID_SMS_MESSAGE_STORE_STATUS	5

#define MBIM_CID_USSD				1

#define MBIM_CID_PHONEBOOK_CONFIGURATION	1
#define MBIM_CID_PHONEBOOK_READ			2
#define MBIM_CID_PHONEBOOK_DELETE		3
#define MBIM_CID_PHONEBOOK_WRITE		4

#define MBIM_CID_STK_PAC			1
#define MBIM_CID_STK_TERMINAL_RESPONSE		2
#define MBIM_CID_STK_ENVELOPE			3

#define MBIM_CID_AKA_AUTH			1
#define MBIM_CID_AKAP_AUTH			2
#define MBIM_CID_SIM_AUTH			3

#define MBIM_CID_DSS_CONNECT			1

/* Table 10-11 */
enum mbim_data_class {
	MBIM_DATA_CLASS_NONE		= 0x00,
	MBIM_DATA_CLASS_GPRS		= 0x01,
	MBIM_DATA_CLASS_EDGE		= 0x02,
	MBIM_DATA_CLASS_UMTS		= 0x04,
	MBIM_DATA_CLASS_HSDPA		= 0x08,
	MBIM_DATA_CLASS_HSUPA		= 0x10,
	MBIM_DATA_CLASS_LTE		= 0x20,
	MBIM_DATA_CLASS_1XRTT		= 0x10000,
	MBIM_DATA_CLASS_EVDO		= 0x20000,
	MBIM_DATA_CLASS_EVDO_REVA	= 0x40000,
	MBIM_DATA_CLASS_1XEVDV		= 0x80000,
	MBIM_DATA_CLASS_3XRTT		= 0x100000,
	MBIM_DATA_CLASS_1XEVDO_REVB	= 0x200000,
	MBIM_DATA_CLASS_UMB		= 0x400000,
	MBIM_DATA_CLASS_CUSTOM		= 0x80000000,
};

typedef void (*mbim_device_debug_func_t) (const char *str, void *user_data);
typedef void (*mbim_device_disconnect_func_t) (void *user_data);
typedef void (*mbim_device_destroy_func_t) (void *user_data);
typedef void (*mbim_device_ready_func_t) (void *user_data);
typedef void (*mbim_device_reply_func_t) (struct mbim_message *message,
							void *user_data);

extern const uint8_t mbim_uuid_basic_connect[];
extern const uint8_t mbim_uuid_sms[];
extern const uint8_t mbim_uuid_ussd[];
extern const uint8_t mbim_uuid_phonebook[];
extern const uint8_t mbim_uuid_stk[];
extern const uint8_t mbim_uuid_auth[];
extern const uint8_t mbim_uuid_dss[];

extern const uint8_t mbim_context_type_none[];
extern const uint8_t mbim_context_type_internet[];
extern const uint8_t mbim_context_type_vpn[];
extern const uint8_t mbim_context_type_voice[];
extern const uint8_t mbim_context_type_video_share[];
extern const uint8_t mbim_context_type_purchase[];
extern const uint8_t mbim_context_type_ims[];
extern const uint8_t mbim_context_type_mms[];
extern const uint8_t mbim_context_type_local[];

struct mbim_device *mbim_device_new(int fd, uint32_t max_segment_size);
bool mbim_device_set_close_on_unref(struct mbim_device *device, bool do_close);
struct mbim_device *mbim_device_ref(struct mbim_device *device);
void mbim_device_unref(struct mbim_device *device);
bool mbim_device_shutdown(struct mbim_device *device);

bool mbim_device_set_max_outstanding(struct mbim_device *device, uint32_t max);

bool mbim_device_set_debug(struct mbim_device *device,
				mbim_device_debug_func_t func, void *user_data,
				mbim_device_destroy_func_t destroy);
bool mbim_device_set_disconnect_handler(struct mbim_device *device,
					mbim_device_disconnect_func_t function,
					void *user_data,
					mbim_device_destroy_func_t destroy);
bool mbim_device_set_ready_handler(struct mbim_device *device,
					mbim_device_ready_func_t function,
					void *user_data,
					mbim_device_destroy_func_t destroy);

uint32_t mbim_device_send(struct mbim_device *device, uint32_t gid,
				struct mbim_message *message,
				mbim_device_reply_func_t function,
				void *user_data,
				mbim_device_destroy_func_t destroy);
bool mbim_device_cancel(struct mbim_device *device, uint32_t tid);
bool mbim_device_cancel_group(struct mbim_device *device, uint32_t gid);

uint32_t mbim_device_register(struct mbim_device *device, uint32_t gid,
				const uint8_t *uuid, uint32_t cid,
				mbim_device_reply_func_t notify,
				void *user_data,
				mbim_device_destroy_func_t destroy);
bool mbim_device_unregister(struct mbim_device *device, uint32_t id);
bool mbim_device_unregister_group(struct mbim_device *device, uint32_t gid);
