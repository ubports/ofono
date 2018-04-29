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

#ifndef __OFONO_GPRS_FILTER_H
#define __OFONO_GPRS_FILTER_H

#ifdef __cplusplus
extern "C" {
#endif

#include <ofono/types.h>

struct ofono_gprs;
struct ofono_gprs_context;
struct ofono_gprs_primary_context;

/* If ctx is NULL then activation gets cancelled */
typedef void (*ofono_gprs_filter_activate_cb_t)
		(const struct ofono_gprs_primary_context *ctx, void *data);
typedef void (*ofono_gprs_filter_check_cb_t)(ofono_bool_t allow, void *data);

#define OFONO_GPRS_FILTER_PRIORITY_LOW     (-100)
#define OFONO_GPRS_FILTER_PRIORITY_DEFAULT (0)
#define OFONO_GPRS_FILTER_PRIORITY_HIGH    (100)

/*
 * The api_version field makes it possible to keep using old plugins
 * even if struct ofono_gprs_filter gets extended with new callbacks.
 */

#define OFONO_GPRS_FILTER_API_VERSION      (1)

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
struct ofono_gprs_filter {
	const char *name;
	int api_version;        /* OFONO_GPRS_FILTER_API_VERSION */
	int priority;
	void (*cancel)(unsigned int id);
	unsigned int (*filter_activate)(struct ofono_gprs_context *gc,
				const struct ofono_gprs_primary_context *ctx,
				ofono_gprs_filter_activate_cb_t cb,
				void *data);
	/* API version 1 */
	unsigned int (*filter_check)(struct ofono_gprs *gprs,
				ofono_gprs_filter_check_cb_t cb, void *data);
};

int ofono_gprs_filter_register(const struct ofono_gprs_filter *filter);
void ofono_gprs_filter_unregister(const struct ofono_gprs_filter *filter);

#ifdef __cplusplus
}
#endif

#endif /* __OFONO_GPRS_FILTER_H */

/*
 * Local Variables:
 * mode: C
 * c-basic-offset: 8
 * indent-tabs-mode: t
 * End:
 */
