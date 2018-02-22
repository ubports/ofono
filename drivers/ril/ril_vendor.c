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

#include "ril_vendor.h"
#include "ril_log.h"

struct ril_vendor_hook *ril_vendor_create_hook
		(const struct ril_vendor_driver *vendor, GRilIoChannel *io,
			const char *path, const struct ril_slot_config *config,
			struct ril_network *network)
{
	if (vendor) {
		const void *data = vendor->driver_data;

		/*
		 * NOTE: we are looking for the callback in the base but
		 * keeping the original driver data.
		 */
		while (!vendor->create_hook && vendor->base) {
			vendor = vendor->base;
		}
		if (vendor->create_hook) {
			return vendor->create_hook(data, io, path, config,
								network);
		}
	}
	return NULL;
}

struct ril_vendor_hook *ril_vendor_hook_init(struct ril_vendor_hook *self,
				const struct ril_vendor_hook_proc *proc,
				ril_vendor_hook_free_proc free)
{
	self->proc = proc;
	self->free = free;
	g_atomic_int_set(&self->ref_count, 1);
	return self;
}

struct ril_vendor_hook *ril_vendor_hook_ref(struct ril_vendor_hook *self)
{
	if (self) {
		GASSERT(self->ref_count > 0);
		g_atomic_int_inc(&self->ref_count);
	}
	return self;
}

static void ril_vendor_hook_free(struct ril_vendor_hook *self)
{
	if (self->free) {
		self->free(self);
	}
}

void ril_vendor_hook_unref(struct ril_vendor_hook *self)
{
	if (self) {
		GASSERT(self->ref_count > 0);
		if (g_atomic_int_dec_and_test(&self->ref_count)) {
			ril_vendor_hook_free(self);
		}
	}
}

void ril_vendor_get_defaults(const struct ril_vendor_driver *vendor,
					struct ril_vendor_defaults *defaults)
{
	if (vendor) {
		while (!vendor->get_defaults && vendor->base) {
			vendor = vendor->base;
		}
		if (vendor->get_defaults) {
			vendor->get_defaults(defaults);
		}
	}
}

const char *ril_vendor_hook_request_to_string(struct ril_vendor_hook *self,
							guint request)
{
	if (self) {
		const struct ril_vendor_hook_proc *proc = self->proc;

		while (!proc->request_to_string && proc->base) {
			proc = proc->base;
		}
		if (proc->request_to_string) {
			return proc->request_to_string(self, request);
		}
	}
	return NULL;
}

const char *ril_vendor_hook_event_to_string(struct ril_vendor_hook *self,
							guint event)
{
	if (self) {
		const struct ril_vendor_hook_proc *proc = self->proc;

		while (!proc->event_to_string && proc->base) {
			proc = proc->base;
		}
		if (proc->event_to_string) {
			return proc->event_to_string(self, event);
		}
	}
	return NULL;
}

GRilIoRequest *ril_vendor_hook_data_call_req(struct ril_vendor_hook *self,
			int tech, const char *profile, const char *apn,
			const char *username, const char *password,
			enum ril_auth auth, const char *proto)
{
	if (self) {
		const struct ril_vendor_hook_proc *proc = self->proc;

		while (!proc->data_call_req && proc->base) {
			proc = proc->base;
		}
		if (proc->data_call_req) {
			return proc->data_call_req(self, tech, profile, apn,
					username, password, auth, proto);
		}
	}
	return NULL;
}

gboolean ril_vendor_hook_data_call_parse(struct ril_vendor_hook *self,
		struct ril_data_call *call, int ver, GRilIoParser *rilp)
{
	if (self) {
		const struct ril_vendor_hook_proc *proc = self->proc;

		while (!proc->data_call_parse && proc->base) {
			proc = proc->base;
		}
		if (proc->data_call_parse) {
			return proc->data_call_parse(self, call, ver, rilp);
		}
	}
	return FALSE;
}

/*
 * Local Variables:
 * mode: C
 * c-basic-offset: 8
 * indent-tabs-mode: t
 * End:
 */
