/*
 *  oFono - Open Source Telephony
 *
 *  Copyright (C) 2017 Jolla Ltd.
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License version 2 as
 *  published by the Free Software Foundation.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 *  GNU General Public License for more details.
 */

#ifndef __OFONO_SMS_FILTER_H
#define __OFONO_SMS_FILTER_H

#ifdef __cplusplus
extern "C" {
#endif

#include <ofono/types.h>

struct ofono_modem;

/* 23.040 Section 9.1.2.5 */
enum ofono_sms_number_type {
	OFONO_SMS_NUMBER_TYPE_UNKNOWN = 0,
	OFONO_SMS_NUMBER_TYPE_INTERNATIONAL = 1,
	OFONO_SMS_NUMBER_TYPE_NATIONAL = 2,
	OFONO_SMS_NUMBER_TYPE_NETWORK_SPECIFIC = 3,
	OFONO_SMS_NUMBER_TYPE_SUBSCRIBER = 4,
	OFONO_SMS_NUMBER_TYPE_ALPHANUMERIC = 5,
	OFONO_SMS_NUMBER_TYPE_ABBREVIATED = 6,
	OFONO_SMS_NUMBER_TYPE_RESERVED = 7
};

/* 23.040 Section 9.1.2.5 */
enum ofono_sms_numbering_plan {
	OFONO_SMS_NUMBERING_PLAN_UNKNOWN = 0,
	OFONO_SMS_NUMBERING_PLAN_ISDN = 1,
	OFONO_SMS_NUMBERING_PLAN_DATA = 3,
	OFONO_SMS_NUMBERING_PLAN_TELEX = 4,
	OFONO_SMS_NUMBERING_PLAN_SC1 = 5,
	OFONO_SMS_NUMBERING_PLAN_SC2 = 6,
	OFONO_SMS_NUMBERING_PLAN_NATIONAL = 8,
	OFONO_SMS_NUMBERING_PLAN_PRIVATE = 9,
	OFONO_SMS_NUMBERING_PLAN_ERMES = 10,
	OFONO_SMS_NUMBERING_PLAN_RESERVED = 15
};

enum ofono_sms_class {
	OFONO_SMS_CLASS_0 = 0,
	OFONO_SMS_CLASS_1 = 1,
	OFONO_SMS_CLASS_2 = 2,
	OFONO_SMS_CLASS_3 = 3,
	OFONO_SMS_CLASS_UNSPECIFIED = 4,
};

struct ofono_sms_address {
	enum ofono_sms_number_type number_type;
	enum ofono_sms_numbering_plan numbering_plan;
	/*
	 * An alphanum TP-OA is 10 7-bit coded octets, which can carry
	 * 11 8-bit characters. 22 bytes + terminator in UTF-8.
	 */
	char address[23];
};

struct ofono_sms_scts {
	unsigned char year;
	unsigned char month;
	unsigned char day;
	unsigned char hour;
	unsigned char minute;
	unsigned char second;
	ofono_bool_t has_timezone;
	unsigned char timezone;
};

enum ofono_sms_filter_result {
	OFONO_SMS_FILTER_DROP,      /* Stop processing and drop the message */
	OFONO_SMS_FILTER_CONTINUE   /* Run the next filter */
};

typedef void (*ofono_sms_filter_send_text_cb_t)
			(enum ofono_sms_filter_result result,
				const struct ofono_sms_address *addr,
				const char *message,
				void *data);

typedef void (*ofono_sms_filter_send_datagram_cb_t)
			(enum ofono_sms_filter_result result,
				const struct ofono_sms_address *addr,
				int dst_port, int src_port,
				const unsigned char *buf, unsigned int len,
				void *data);

typedef void (*ofono_sms_filter_recv_text_cb_t)
			(enum ofono_sms_filter_result result,
				const struct ofono_uuid *uuid,
				const char *message,
				enum ofono_sms_class cls,
				const struct ofono_sms_address *addr,
				const struct ofono_sms_scts *scts,
				void *data);

typedef void (*ofono_sms_filter_recv_datagram_cb_t)
			(enum ofono_sms_filter_result result,
				const struct ofono_uuid *uuid,
				int dst_port, int src_port,
				const unsigned char *buf, unsigned int len,
				const struct ofono_sms_address *addr,
				const struct ofono_sms_scts *scts,
				void *data);

#define OFONO_SMS_FILTER_PRIORITY_LOW     (-100)
#define OFONO_SMS_FILTER_PRIORITY_DEFAULT (0)
#define OFONO_SMS_FILTER_PRIORITY_HIGH    (100)

/*
 * The api_version field makes it possible to keep using old plugins
 * even if struct ofono_sms_filter gets extended with new callbacks.
 */

#define OFONO_SMS_FILTER_API_VERSION      (0)

/*
 * The filter callbacks either invoke the completion callback directly
 * or return the id of the cancellable asynchronous operation (but never
 * both). If non-zero value is returned, the completion callback has to
 * be invoked later on a fresh stack. Once the asynchronous filtering
 * operation is cancelled, the associated completion callback must not
 * be invoked.
 *
 * The pointers passed to the filter callbacks are guaranteed to be
 * valid until the filter calls the completion callback. The completion
 * callback is never NULL.
 *
 * Please avoid making blocking D-Bus calls from the filter callbacks.
 */
struct ofono_sms_filter {
	const char *name;
	int api_version;        /* OFONO_SMS_FILTER_API_VERSION */
	int priority;
	unsigned int (*filter_send_text)(struct ofono_modem *modem,
				const struct ofono_sms_address *addr,
				const char *message,
				ofono_sms_filter_send_text_cb_t cb,
				void *data);
	unsigned int (*filter_send_datagram)(struct ofono_modem *modem,
				const struct ofono_sms_address *addr,
				int dst_port, int src_port,
				const unsigned char *buf, unsigned int len,
				ofono_sms_filter_send_datagram_cb_t cb,
				void *data);
	unsigned int (*filter_recv_text)(struct ofono_modem *modem,
				const struct ofono_uuid *uuid,
				const char *message,
				enum ofono_sms_class cls,
				const struct ofono_sms_address *addr,
				const struct ofono_sms_scts *scts,
				ofono_sms_filter_recv_text_cb_t cb,
				void *data);
	unsigned int (*filter_recv_datagram)(struct ofono_modem *modem,
				const struct ofono_uuid *uuid,
				int dst_port, int src_port,
				const unsigned char *buf, unsigned int len,
				const struct ofono_sms_address *addr,
				const struct ofono_sms_scts *scts,
				ofono_sms_filter_recv_datagram_cb_t cb,
				void *data);
	void (*cancel)(unsigned int id);
};

int ofono_sms_filter_register(const struct ofono_sms_filter *filter);
void ofono_sms_filter_unregister(const struct ofono_sms_filter *filter);

#ifdef __cplusplus
}
#endif

#endif /* __OFONO_SMS_FILTER_H */

/*
 * Local Variables:
 * mode: C
 * c-basic-offset: 8
 * indent-tabs-mode: t
 * End:
 */
