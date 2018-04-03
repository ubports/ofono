/*
 *  oFono - Open Source Telephony - RIL-based devices
 *
 *  Copyright (C) 2016-2018 Jolla Ltd.
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

#ifndef RIL_VENDOR_H
#define RIL_VENDOR_H

#include "ril_types.h"

struct ril_vendor_defaults {
	gboolean empty_pin_query;
	gboolean legacy_imei_query;
	gboolean enable_cbs;
	gboolean query_available_band_mode;
};

struct ril_vendor_driver {
	const char *name;
	const void *driver_data;
	const struct ril_vendor_driver *base;
	void (*get_defaults)(struct ril_vendor_defaults *defaults);
	struct ril_vendor_hook *(*create_hook)(const void *driver_data,
				GRilIoChannel *io, const char *path,
				const struct ril_slot_config *cfg,
				struct ril_network *network);
};

struct ril_vendor_hook_proc {
	const struct ril_vendor_hook_proc *base;
	const char *(*request_to_string)(struct ril_vendor_hook *hook,
							guint request);
	const char *(*event_to_string)(struct ril_vendor_hook *hook,
							guint event);
	GRilIoRequest *(*data_call_req)(struct ril_vendor_hook *hook,
			int tech, const char *profile, const char *apn,
			const char *username, const char *password,
			enum ril_auth auth, const char *proto);
	gboolean (*data_call_parse)(struct ril_vendor_hook *hook,
			struct ril_data_call *call, int version,
			GRilIoParser *rilp);
};

typedef void (*ril_vendor_hook_free_proc)(struct ril_vendor_hook *hook);
struct ril_vendor_hook {
	const struct ril_vendor_hook_proc *proc;
	ril_vendor_hook_free_proc free;
	gint ref_count;
};

struct ril_vendor_hook *ril_vendor_create_hook
		(const struct ril_vendor_driver *vendor, GRilIoChannel *io,
			const char *path, const struct ril_slot_config *cfg,
			struct ril_network *network);
void ril_vendor_get_defaults(const struct ril_vendor_driver *vendor,
					struct ril_vendor_defaults *defaults);

struct ril_vendor_hook *ril_vendor_hook_init(struct ril_vendor_hook *hook,
				const struct ril_vendor_hook_proc *proc,
				ril_vendor_hook_free_proc free);
struct ril_vendor_hook *ril_vendor_hook_ref(struct ril_vendor_hook *hook);
void ril_vendor_hook_unref(struct ril_vendor_hook *hook);

const char *ril_vendor_hook_request_to_string(struct ril_vendor_hook *hook,
					guint request);
const char *ril_vendor_hook_event_to_string(struct ril_vendor_hook *hook,
					guint event);
GRilIoRequest *ril_vendor_hook_data_call_req(struct ril_vendor_hook *hook,
			int tech, const char *profile, const char *apn,
			const char *username, const char *password,
			enum ril_auth auth, const char *proto);
gboolean ril_vendor_hook_data_call_parse(struct ril_vendor_hook *hook,
			struct ril_data_call *call, int version,
			GRilIoParser *rilp);

/* Put vendor driver descriptors to the "__vendor" section */
#define RIL_VENDOR_DRIVER_DEFINE(name) \
	const struct ril_vendor_driver name \
	__attribute__((used, section("__vendor"))) =
#define RIL_VENDOR_DRIVER_FOREACH(var) \
	for ((var) = __start___vendor; (var) < __stop___vendor; (var)++)
extern const struct ril_vendor_driver __start___vendor[];
extern const struct ril_vendor_driver __stop___vendor[];

#endif /* RIL_VENDOR_H */

/*
 * Local Variables:
 * mode: C
 * c-basic-offset: 8
 * indent-tabs-mode: t
 * End:
 */
