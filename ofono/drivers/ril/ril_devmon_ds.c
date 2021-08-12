/*
 *  oFono - Open Source Telephony - RIL-based devices
 *
 *  Copyright (C) 2019-2021 Jolla Ltd.
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

#include "ril_devmon.h"
#include "ril_connman.h"

#include <ofono/log.h>
#include <ofono/ril-constants.h>

#include <mce_battery.h>
#include <mce_charger.h>
#include <mce_display.h>

#include <grilio_channel.h>
#include <grilio_request.h>

#include <gutil_macros.h>

enum device_state_type {
	/* Mirrors RIL_DeviceStateType from ril.h */
	POWER_SAVE_MODE,
	CHARGING_STATE,
	LOW_DATA_EXPECTED
};

enum ril_devmon_ds_battery_event {
	BATTERY_EVENT_VALID,
	BATTERY_EVENT_STATUS,
	BATTERY_EVENT_COUNT
};

enum ril_devmon_ds_charger_event {
	CHARGER_EVENT_VALID,
	CHARGER_EVENT_STATE,
	CHARGER_EVENT_COUNT
};

enum ril_devmon_ds_display_event {
	DISPLAY_EVENT_VALID,
	DISPLAY_EVENT_STATE,
	DISPLAY_EVENT_COUNT
};

enum ril_devmon_ds_connman_event {
	CONNMAN_EVENT_VALID,
	CONNMAN_EVENT_TETHERING,
	CONNMAN_EVENT_COUNT
};

typedef struct ril_devmon_ds {
	struct ril_devmon pub;
	struct ril_connman *connman;
	MceBattery *battery;
	MceCharger *charger;
	MceDisplay *display;
	int cell_info_interval_short_ms;
	int cell_info_interval_long_ms;
} DevMon;

typedef struct ril_devmon_ds_io {
	struct ril_devmon_io pub;
	struct ril_connman *connman;
	struct ofono_cell_info *cell_info;
	MceBattery *battery;
	MceCharger *charger;
	MceDisplay *display;
	GRilIoChannel *io;
	guint low_data_req_id;
	guint charging_req_id;
	gboolean low_data;
	gboolean charging;
	gboolean low_data_supported;
	gboolean charging_supported;
	gulong connman_event_id[CONNMAN_EVENT_COUNT];
	gulong battery_event_id[BATTERY_EVENT_COUNT];
	gulong charger_event_id[CHARGER_EVENT_COUNT];
	gulong display_event_id[DISPLAY_EVENT_COUNT];
	guint req_id;
	int cell_info_interval_short_ms;
	int cell_info_interval_long_ms;
} DevMonIo;

#define DBG_(self,fmt,args...) DBG("%s: " fmt, (self)->io->name, ##args)

static inline DevMon *ril_devmon_ds_cast(struct ril_devmon *pub)
{
	return G_CAST(pub, DevMon, pub);
}

static inline DevMonIo *ril_devmon_ds_io_cast(struct ril_devmon_io *pub)
{
	return G_CAST(pub, DevMonIo, pub);
}

static inline gboolean ril_devmon_ds_tethering_on(struct ril_connman *connman)
{
	return connman->valid && connman->tethering;
}

static inline gboolean ril_devmon_ds_battery_ok(MceBattery *battery)
{
	return battery->valid && battery->status >= MCE_BATTERY_OK;
}

static inline gboolean ril_devmon_ds_charging(MceCharger *charger)
{
	return charger->valid && charger->state == MCE_CHARGER_ON;
}

static inline gboolean ril_devmon_ds_display_on(MceDisplay *display)
{
	return display->valid && display->state != MCE_DISPLAY_STATE_OFF;
}

static guint ril_devmon_ds_io_send_device_state(DevMonIo *self,
				enum device_state_type type, gboolean state,
				GRilIoChannelResponseFunc callback)
{
	GRilIoRequest *req = grilio_request_array_int32_new(2, type, state);
	const guint id = grilio_channel_send_request_full(self->io, req,
			RIL_REQUEST_SEND_DEVICE_STATE, callback, NULL, self);

	grilio_request_unref(req);
	return id;
}

static void ril_devmon_ds_io_low_data_state_sent(GRilIoChannel *io, int status,
				const void *data, guint len, void *user_data)
{
	DevMonIo *self = user_data;

	self->low_data_req_id = 0;
	if (status == RIL_E_REQUEST_NOT_SUPPORTED) {
		DBG_(self, "LOW_DATA_EXPECTED state is not supported");
		self->low_data_supported = FALSE;
	}
}

static void ril_devmon_ds_io_charging_state_sent(GRilIoChannel *io, int status,
				const void *data, guint len, void *user_data)
{
	DevMonIo *self = user_data;

	self->charging_req_id = 0;
	if (status == RIL_E_REQUEST_NOT_SUPPORTED) {
		DBG_(self, "CHARGING state is not supported");
		self->charging_supported = FALSE;
	}
}

static void ril_devmon_ds_io_update_charging(DevMonIo *self)
{
	const gboolean charging = ril_devmon_ds_charging(self->charger);

	if (self->charging != charging) {
		self->charging = charging;
		DBG_(self, "Charging %s", charging ? "on" : "off");
		if (self->charging_supported) {
			grilio_channel_cancel_request(self->io,
					self->charging_req_id, FALSE);
			self->charging_req_id =
				ril_devmon_ds_io_send_device_state(self,
					CHARGING_STATE, charging,
					ril_devmon_ds_io_charging_state_sent);
		}
	}
}

static void ril_devmon_ds_io_update_low_data(DevMonIo *self)
{
	const gboolean low_data =
		!ril_devmon_ds_tethering_on(self->connman) &&
		!ril_devmon_ds_charging(self->charger) &&
		!ril_devmon_ds_display_on(self->display);

	if (self->low_data != low_data) {
		self->low_data = low_data;
		DBG_(self, "Low data is%s expected", low_data ? "" : " not");
		if (self->low_data_supported) {
			grilio_channel_cancel_request(self->io,
					self->low_data_req_id, FALSE);
			self->low_data_req_id =
				ril_devmon_ds_io_send_device_state(self,
					LOW_DATA_EXPECTED, low_data,
					ril_devmon_ds_io_low_data_state_sent);
		}
	}
}

static void ril_devmon_ds_io_set_cell_info_update_interval(DevMonIo *self)
{
	ofono_cell_info_set_update_interval(self->cell_info,
		(ril_devmon_ds_display_on(self->display) &&
			(ril_devmon_ds_charging(self->charger) ||
				ril_devmon_ds_battery_ok(self->battery))) ?
					self->cell_info_interval_short_ms :
					self->cell_info_interval_long_ms);
}

static void ril_devmon_ds_io_connman_cb(struct ril_connman *connman,
			enum ril_connman_property property, void *user_data)
{
	ril_devmon_ds_io_update_low_data((DevMonIo *)user_data);
}

static void ril_devmon_ds_io_battery_cb(MceBattery *battery, void *user_data)
{
	ril_devmon_ds_io_set_cell_info_update_interval(user_data);
}

static void ril_devmon_ds_io_display_cb(MceDisplay *display, void *user_data)
{
	DevMonIo *self = user_data;

	ril_devmon_ds_io_update_low_data(self);
	ril_devmon_ds_io_set_cell_info_update_interval(self);
}

static void ril_devmon_ds_io_charger_cb(MceCharger *charger, void *user_data)
{
	DevMonIo *self = user_data;

	ril_devmon_ds_io_update_low_data(self);
	ril_devmon_ds_io_update_charging(self);
	ril_devmon_ds_io_set_cell_info_update_interval(self);
}

static void ril_devmon_ds_io_free(struct ril_devmon_io *devmon_io)
{
	DevMonIo *self = ril_devmon_ds_io_cast(devmon_io);

	ril_connman_remove_all_handlers(self->connman, self->connman_event_id);
	ril_connman_unref(self->connman);

	mce_battery_remove_all_handlers(self->battery, self->battery_event_id);
	mce_battery_unref(self->battery);

	mce_charger_remove_all_handlers(self->charger, self->charger_event_id);
	mce_charger_unref(self->charger);

	mce_display_remove_all_handlers(self->display, self->display_event_id);
	mce_display_unref(self->display);

	grilio_channel_cancel_request(self->io, self->low_data_req_id, FALSE);
	grilio_channel_cancel_request(self->io, self->charging_req_id, FALSE);
	grilio_channel_unref(self->io);

	ofono_cell_info_unref(self->cell_info);
	g_free(self);
}

static struct ril_devmon_io *ril_devmon_ds_start_io(struct ril_devmon *devmon,
		GRilIoChannel *io, struct ofono_cell_info *cell_info)
{
	DevMon *ds = ril_devmon_ds_cast(devmon);
	DevMonIo *self = g_new0(DevMonIo, 1);

	self->pub.free = ril_devmon_ds_io_free;
	self->low_data_supported = TRUE;
	self->charging_supported = TRUE;
	self->io = grilio_channel_ref(io);
	self->cell_info = ofono_cell_info_ref(cell_info);

	self->connman = ril_connman_ref(ds->connman);
	self->connman_event_id[CONNMAN_EVENT_VALID] =
		ril_connman_add_property_changed_handler(self->connman,
			RIL_CONNMAN_PROPERTY_VALID,
			ril_devmon_ds_io_connman_cb, self);
	self->connman_event_id[CONNMAN_EVENT_TETHERING] =
		ril_connman_add_property_changed_handler(self->connman,
			RIL_CONNMAN_PROPERTY_TETHERING,
			ril_devmon_ds_io_connman_cb, self);

	self->battery = mce_battery_ref(ds->battery);
	self->battery_event_id[BATTERY_EVENT_VALID] =
		mce_battery_add_valid_changed_handler(self->battery,
			ril_devmon_ds_io_battery_cb, self);
	self->battery_event_id[BATTERY_EVENT_STATUS] =
		mce_battery_add_status_changed_handler(self->battery,
			ril_devmon_ds_io_battery_cb, self);

	self->charger = mce_charger_ref(ds->charger);
	self->charger_event_id[CHARGER_EVENT_VALID] =
		mce_charger_add_valid_changed_handler(self->charger,
			ril_devmon_ds_io_charger_cb, self);
	self->charger_event_id[CHARGER_EVENT_STATE] =
		mce_charger_add_state_changed_handler(self->charger,
			ril_devmon_ds_io_charger_cb, self);

	self->display = mce_display_ref(ds->display);
	self->display_event_id[DISPLAY_EVENT_VALID] =
		mce_display_add_valid_changed_handler(self->display,
			ril_devmon_ds_io_display_cb, self);
	self->display_event_id[DISPLAY_EVENT_STATE] =
		mce_display_add_state_changed_handler(self->display,
			ril_devmon_ds_io_display_cb, self);

	self->cell_info_interval_short_ms =
				ds->cell_info_interval_short_ms;
	self->cell_info_interval_long_ms =
				ds->cell_info_interval_long_ms;

	ril_devmon_ds_io_update_low_data(self);
	ril_devmon_ds_io_update_charging(self);
	ril_devmon_ds_io_set_cell_info_update_interval(self);
	return &self->pub;
}

static void ril_devmon_ds_free(struct ril_devmon *devmon)
{
	DevMon *self = ril_devmon_ds_cast(devmon);

	ril_connman_unref(self->connman);
	mce_battery_unref(self->battery);
	mce_charger_unref(self->charger);
	mce_display_unref(self->display);
	g_free(self);
}

struct ril_devmon *ril_devmon_ds_new(const struct ril_slot_config *config)
{
	DevMon *self = g_new0(DevMon, 1);

	self->pub.free = ril_devmon_ds_free;
	self->pub.start_io = ril_devmon_ds_start_io;
	self->connman = ril_connman_new();
	self->battery = mce_battery_new();
	self->charger = mce_charger_new();
	self->display = mce_display_new();
	self->cell_info_interval_short_ms =
				config->cell_info_interval_short_ms;
	self->cell_info_interval_long_ms =
				config->cell_info_interval_long_ms;
	return &self->pub;
}

/*
 * Local Variables:
 * mode: C
 * c-basic-offset: 8
 * indent-tabs-mode: t
 * End:
 */
