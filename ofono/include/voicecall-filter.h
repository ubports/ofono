/*
 *  oFono - Open Source Telephony
 *
 *  Copyright (C) 2018 Jolla Ltd.
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

#ifndef __OFONO_VOICECALL_FILTER_H
#define __OFONO_VOICECALL_FILTER_H

#ifdef __cplusplus
extern "C" {
#endif

#include <ofono/voicecall.h>

/* 27.007 Section 7.6 */
enum ofono_clip_validity {
	OFONO_CLIP_VALIDITY_VALID = 0,
	OFONO_CLIP_VALIDITY_WITHHELD,
	OFONO_CLIP_VALIDITY_NOT_AVAILABLE
};

/* 27.007 Section 7.18 */
enum ofono_call_status {
	OFONO_CALL_STATUS_ACTIVE = 0,
	OFONO_CALL_STATUS_HELD,
	OFONO_CALL_STATUS_DIALING,
	OFONO_CALL_STATUS_ALERTING,
	OFONO_CALL_STATUS_INCOMING,
	OFONO_CALL_STATUS_WAITING,
	OFONO_CALL_STATUS_DISCONNECTED
};

/* 27.007 Section 7.18 */
enum ofono_call_direction {
	OFONO_CALL_DIRECTION_MOBILE_ORIGINATED = 0,
	OFONO_CALL_DIRECTION_MOBILE_TERMINATED
};

/* 27.007 Section 7.30 */
enum ofono_cnap_validity {
	OFONO_CNAP_VALIDITY_VALID = 0,
	OFONO_CNAP_VALIDITY_WITHHELD,
	OFONO_CNAP_VALIDITY_NOT_AVAILABLE
};

enum ofono_voicecall_filter_dial_result {
	OFONO_VOICECALL_FILTER_DIAL_CONTINUE,     /* Run the next filter */
	OFONO_VOICECALL_FILTER_DIAL_BLOCK         /* Don't dial*/
};

enum ofono_voicecall_filter_incoming_result {
	OFONO_VOICECALL_FILTER_INCOMING_CONTINUE, /* Run the next filter */
	OFONO_VOICECALL_FILTER_INCOMING_HANGUP,   /* Hangup incoming call */
	OFONO_VOICECALL_FILTER_INCOMING_IGNORE    /* Ignore incoming call */
};

typedef void (*ofono_voicecall_filter_dial_cb_t)
			(enum ofono_voicecall_filter_dial_result result,
				void *data);

typedef void (*ofono_voicecall_filter_incoming_cb_t)
			(enum ofono_voicecall_filter_incoming_result result,
				void *data);

#define OFONO_VOICECALL_FILTER_PRIORITY_LOW     (-100)
#define OFONO_VOICECALL_FILTER_PRIORITY_DEFAULT (0)
#define OFONO_VOICECALL_FILTER_PRIORITY_HIGH    (100)

/*
 * The api_version field makes it possible to keep using old plugins
 * even if struct ofono_voicecall_filter gets extended with new callbacks.
 */

#define OFONO_VOICECALL_FILTER_API_VERSION      (0)

/*
 * The filter callbacks either invoke the completion callback directly
 * or return the id of the cancellable asynchronous operation (but never
 * both). If non-zero value is returned, the completion callback has to
 * be invoked later on a fresh stack. Once the asynchronous filtering
 * operation is cancelled, the associated completion callback must not
 * be invoked.
 *
 * Please avoid making blocking D-Bus calls from the filter callbacks.
 */
struct ofono_voicecall_filter {
	const char *name;
	int api_version;        /* OFONO_VOICECALL_FILTER_API_VERSION */
	int priority;
	void (*filter_cancel)(unsigned int id);
	unsigned int (*filter_dial)(struct ofono_voicecall *vc,
				const struct ofono_phone_number *number,
				enum ofono_clir_option clir,
				ofono_voicecall_filter_dial_cb_t cb,
				void *data);
	unsigned int (*filter_incoming)(struct ofono_voicecall *vc,
				const struct ofono_call *call,
				ofono_voicecall_filter_incoming_cb_t cb,
				void *data);
};

void ofono_voicecall_filter_notify(struct ofono_voicecall *vc);
int ofono_voicecall_filter_register(const struct ofono_voicecall_filter *f);
void ofono_voicecall_filter_unregister(const struct ofono_voicecall_filter *f);

#ifdef __cplusplus
}
#endif

#endif /* __OFONO_VOICECALL_FILTER_H */

/*
 * Local Variables:
 * mode: C
 * c-basic-offset: 8
 * indent-tabs-mode: t
 * End:
 */
