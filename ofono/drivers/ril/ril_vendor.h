/*
 *  oFono - Open Source Telephony - RIL-based devices
 *
 *  Copyright (C) 2016-2020 Jolla Ltd.
 *  Copyright (C) 2019-2020 Open Mobile Platform LLC.
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
	gboolean enable_stk;
	gboolean replace_strange_oper;
	gboolean query_available_band_mode;
	gboolean use_data_profiles;
	gboolean force_gsm_when_radio_off;
	guint mms_data_profile_id;
};

struct ril_vendor_driver {
	const char *name;
	const void *driver_data;
	void (*get_defaults)(struct ril_vendor_defaults *defaults);
	struct ril_vendor *(*create_vendor)(const void *driver_data,
				GRilIoChannel *io, const char *path,
				const struct ril_slot_config *cfg);
};

struct ril_vendor_signal_strength {
	gint32 gsm;   /* (0-31, 99) per TS 27.007 8.5  */
	gint32 lte;   /* (0-31, 99) per TS 27.007 8.5 */
	gint32 qdbm;  /* 4*dBm, 0 if none */
};

const struct ril_vendor_driver *ril_vendor_find_driver(const char *name);
struct ril_vendor *ril_vendor_create
		(const struct ril_vendor_driver *vendor, GRilIoChannel *io,
			const char *path, const struct ril_slot_config *cfg);
void ril_vendor_get_defaults(const struct ril_vendor_driver *vendor,
					struct ril_vendor_defaults *defaults);

struct ril_vendor *ril_vendor_ref(struct ril_vendor *vendor);
void ril_vendor_unref(struct ril_vendor *vendor);

const char *ril_vendor_request_to_string(struct ril_vendor *vendor,
					guint request);
const char *ril_vendor_event_to_string(struct ril_vendor *vendor,
					guint event);
void ril_vendor_set_network(struct ril_vendor *vendor, struct ril_network *nw);
GRilIoRequest *ril_vendor_set_attach_apn_req(struct ril_vendor *vendor,
			const char *apn, const char *username,
			const char *password, enum ril_auth auth,
			const char *proto);
GRilIoRequest *ril_vendor_data_call_req(struct ril_vendor *vendor, int tech,
			enum ril_data_profile profile, const char *apn,
			const char *username, const char *password,
			enum ril_auth auth, const char *proto);
gboolean ril_vendor_data_call_parse(struct ril_vendor *vendor,
			struct ril_data_call *call, int version,
			GRilIoParser *rilp);
gboolean ril_vendor_signal_strength_parse(struct ril_vendor *vendor,
			struct ril_vendor_signal_strength *signal_strength,
			GRilIoParser *rilp);

/* Put vendor driver descriptors to the "__vendor" section */
#define RIL_VENDOR_DRIVER_DEFINE(name) const struct ril_vendor_driver name \
	__attribute__((used, section("__vendor"))) =

#endif /* RIL_VENDOR_H */

/*
 * Local Variables:
 * mode: C
 * c-basic-offset: 8
 * indent-tabs-mode: t
 * End:
 */
