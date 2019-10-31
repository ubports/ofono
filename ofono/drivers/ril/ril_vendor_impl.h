/*
 *  oFono - Open Source Telephony - RIL-based devices
 *
 *  Copyright (C) 2019 Jolla Ltd.
 *  Copyright (C) 2019 Open Mobile Platform LLC.
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

#ifndef RIL_VENDOR_IMPL_H
#define RIL_VENDOR_IMPL_H

#include "ril_vendor.h"

#include <glib-object.h>

typedef struct ril_vendor {
	GObject parent;
	GRilIoChannel *io;
	struct ril_network *network;
} RilVendor;

typedef struct ril_vendor_class {
	GObjectClass parent;
	void (*set_network)(RilVendor *vendor, struct ril_network *network);
	const char *(*request_to_string)(RilVendor *vendor, guint request);
	const char *(*event_to_string)(RilVendor *vendor, guint event);
	GRilIoRequest *(*set_attach_apn_req)(RilVendor *vendor,
			const char *apn, const char *username,
			const char *password, enum ril_auth auth,
			const char *proto);
	GRilIoRequest *(*data_call_req)(RilVendor *vendor, int tech,
			enum ril_data_profile profile, const char *apn,
			const char *username, const char *password,
			enum ril_auth auth, const char *proto);
	gboolean (*data_call_parse)(RilVendor *vendor,
			struct ril_data_call *call, int version,
			GRilIoParser *rilp);
	gboolean (*signal_strength_parse)(RilVendor *vendor,
			struct ril_vendor_signal_strength *signal_strength,
			GRilIoParser *rilp);
} RilVendorClass;

GType ril_vendor_get_type(void);
#define RIL_VENDOR_TYPE (ril_vendor_get_type())
#define RIL_VENDOR(obj)	G_TYPE_CHECK_INSTANCE_CAST((obj), \
	RIL_VENDOR_TYPE, RilVendor)
#define RIL_VENDOR_GET_CLASS(obj) G_TYPE_INSTANCE_GET_CLASS((obj), \
	RIL_VENDOR_TYPE, RilVendorClass)
#define RIL_VENDOR_CLASS(klass) G_TYPE_CHECK_CLASS_CAST((klass), \
	RIL_VENDOR_TYPE, RilVendorClass)

void ril_vendor_init_base(RilVendor *vendor, GRilIoChannel *io);

#endif /* RIL_VENDOR_IMPL_H */

/*
 * Local Variables:
 * mode: C
 * c-basic-offset: 8
 * indent-tabs-mode: t
 * End:
 */
