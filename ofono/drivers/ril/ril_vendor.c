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

#include "ril_vendor.h"
#include "ril_vendor_impl.h"
#include "ril_log.h"

#include <grilio_channel.h>

G_DEFINE_ABSTRACT_TYPE(RilVendor, ril_vendor, G_TYPE_OBJECT)

/* Vendor driver descriptors are in the "__vendor" section */
extern const struct ril_vendor_driver __start___vendor[];
extern const struct ril_vendor_driver __stop___vendor[];

const struct ril_vendor_driver *ril_vendor_find_driver(const char *name)
{
	if (name) {
		const struct ril_vendor_driver *d;

		for (d = __start___vendor; d < __stop___vendor; d++) {
			if (!strcasecmp(d->name, name)) {
				return d;
			}
		}
	}
	return NULL;
}

RilVendor *ril_vendor_create(const struct ril_vendor_driver *driver,
				GRilIoChannel *io, const char *path,
				const struct ril_slot_config *config)
{
	return (driver && driver->create_vendor) ?
		driver->create_vendor(driver->driver_data, io, path, config) :
		NULL;
}

RilVendor *ril_vendor_ref(RilVendor *self)
{
	if (G_LIKELY(self)) {
		g_object_ref(RIL_VENDOR(self));
	}
	return self;
}

void ril_vendor_unref(RilVendor *self)
{
	if (G_LIKELY(self)) {
		g_object_unref(RIL_VENDOR(self));
	}
}

void ril_vendor_get_defaults(const struct ril_vendor_driver *vendor,
					struct ril_vendor_defaults *defaults)
{
	if (vendor && vendor->get_defaults) {
		vendor->get_defaults(defaults);
	}
}

const char *ril_vendor_request_to_string(RilVendor *self, guint request)
{
	return G_LIKELY(self) ? RIL_VENDOR_GET_CLASS(self)->
				request_to_string(self, request) : NULL;
}

const char *ril_vendor_event_to_string(RilVendor *self, guint event)
{
	return G_LIKELY(self) ? RIL_VENDOR_GET_CLASS(self)->
				event_to_string(self, event) : NULL;
}

void ril_vendor_set_network(RilVendor *self, struct ril_network *nw)
{
	if (G_LIKELY(self)) {
		RIL_VENDOR_GET_CLASS(self)->set_network(self, nw);
	}
}

GRilIoRequest *ril_vendor_set_attach_apn_req(RilVendor *self, const char *apn,
			const char *user, const char *password,
			enum ril_auth auth, const char *proto)
{
	return G_LIKELY(self) ? RIL_VENDOR_GET_CLASS(self)->
		set_attach_apn_req(self, apn, user, password, auth, proto) :
		NULL;
}

GRilIoRequest *ril_vendor_data_call_req(RilVendor *self, int tech,
			enum ril_data_profile profile, const char *apn,
			const char *username, const char *password,
			enum ril_auth auth, const char *proto)
{
	return G_LIKELY(self) ? RIL_VENDOR_GET_CLASS(self)->
		data_call_req(self, tech, profile, apn, username, password,
							auth, proto) : NULL;
}

gboolean ril_vendor_data_call_parse(RilVendor *self,
		struct ril_data_call *call, int ver, GRilIoParser *rilp)
{
	return G_LIKELY(self) && RIL_VENDOR_GET_CLASS(self)->
				data_call_parse(self, call, ver, rilp);
}

gboolean ril_vendor_signal_strength_parse(RilVendor *self,
			struct ril_vendor_signal_strength *signal_strength,
			GRilIoParser *rilp)
{
	return G_LIKELY(self) && RIL_VENDOR_GET_CLASS(self)->
			signal_strength_parse(self, signal_strength, rilp);
}

static void ril_vendor_default_set_network(RilVendor *self,
						struct ril_network *network)
{
	if (self->network != network) {
		if (self->network) {
			g_object_remove_weak_pointer(G_OBJECT(self->network),
						(gpointer*) &self->network);
		}
		self->network = network;
		if (self->network) {
			g_object_add_weak_pointer(G_OBJECT(network),
						(gpointer*) &self->network);
		}
	}
}

static const char *ril_vendor_default_id_to_string(RilVendor *self, guint id)
{
	return NULL;
}

static GRilIoRequest *ril_vendor_default_set_attach_apn_req(RilVendor *self,
			const char *apn, const char *username,
			const char *password, enum ril_auth auth,
			const char *proto)
{
	return NULL;
}

static GRilIoRequest *ril_vendor_default_data_call_req(RilVendor *self,
			int tech, enum ril_data_profile profile,
			const char *apn, const char *user, const char *passwd,
			enum ril_auth auth, const char *proto)
{
	return NULL;
}

static gboolean ril_vendor_default_data_call_parse(RilVendor *self,
			struct ril_data_call *call, int version,
			GRilIoParser *rilp)
{
	return FALSE;
}

static gboolean ril_vendor_default_signal_strength_parse(RilVendor *self,
			struct ril_vendor_signal_strength *signal_strength,
			GRilIoParser *rilp)
{
	return FALSE;
}

void ril_vendor_init_base(RilVendor *self, GRilIoChannel *io)
{
	self->io = grilio_channel_ref(io);
}

static void ril_vendor_init(RilVendor *self)
{
}

static void ril_vendor_finalize(GObject* object)
{
	RilVendor *self = RIL_VENDOR(object);

	if (self->network) {
		g_object_remove_weak_pointer(G_OBJECT(self->network),
						(gpointer*) &self->network);
	}
	grilio_channel_unref(self->io);
	G_OBJECT_CLASS(ril_vendor_parent_class)->finalize(object);
}

static void ril_vendor_class_init(RilVendorClass* klass)
{
    G_OBJECT_CLASS(klass)->finalize = ril_vendor_finalize;
    klass->set_network = ril_vendor_default_set_network;
    klass->request_to_string = ril_vendor_default_id_to_string;
    klass->event_to_string = ril_vendor_default_id_to_string;
    klass->set_attach_apn_req = ril_vendor_default_set_attach_apn_req;
    klass->data_call_req = ril_vendor_default_data_call_req;
    klass->data_call_parse = ril_vendor_default_data_call_parse;
    klass->signal_strength_parse = ril_vendor_default_signal_strength_parse;
}

/*
 * Local Variables:
 * mode: C
 * c-basic-offset: 8
 * indent-tabs-mode: t
 * End:
 */
