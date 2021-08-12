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

#include <ofono/log.h>
#include <ofono/ril-constants.h>

#include <mce_battery.h>
#include <mce_charger.h>
#include <mce_display.h>

#include <grilio_channel.h>
#include <grilio_request.h>

#include <gutil_macros.h>

enum ril_devmon_ss_battery_event {
	BATTERY_EVENT_VALID,
	BATTERY_EVENT_STATUS,
	BATTERY_EVENT_COUNT
};

enum ril_devmon_ss_charger_event {
	CHARGER_EVENT_VALID,
	CHARGER_EVENT_STATE,
	CHARGER_EVENT_COUNT
};

enum ril_devmon_ss_display_event {
	DISPLAY_EVENT_VALID,
	DISPLAY_EVENT_STATE,
	DISPLAY_EVENT_COUNT
};

typedef struct ril_devmon_ss {
	struct ril_devmon pub;
	MceBattery *battery;
	MceCharger *charger;
	MceDisplay *display;
	int cell_info_interval_short_ms;
	int cell_info_interval_long_ms;
} DevMon;

typedef struct ril_devmon_ss_io {
	struct ril_devmon_io pub;
	struct ofono_cell_info *cell_info;
	MceBattery *battery;
	MceCharger *charger;
	MceDisplay *display;
	GRilIoChannel *io;
	gboolean display_on;
	gboolean screen_state_supported;
	gulong battery_event_id[BATTERY_EVENT_COUNT];
	gulong charger_event_id[CHARGER_EVENT_COUNT];
	gulong display_event_id[DISPLAY_EVENT_COUNT];
	guint req_id;
	int cell_info_interval_short_ms;
	int cell_info_interval_long_ms;
} DevMonIo;

inline static DevMon *ril_devmon_ss_cast(struct ril_devmon *pub)
{
	return G_CAST(pub, DevMon, pub);
}

inline static DevMonIo *ril_devmon_ss_io_cast(struct ril_devmon_io *pub)
{
	return G_CAST(pub, DevMonIo, pub);
}

static inline gboolean ril_devmon_ss_battery_ok(MceBattery *battery)
{
	return battery->valid && battery->status >= MCE_BATTERY_OK;
}

static inline gboolean ril_devmon_ss_charging(MceCharger *charger)
{
	return charger->valid && charger->state == MCE_CHARGER_ON;
}

static gboolean ril_devmon_ss_display_on(MceDisplay *display)
{
	return display->valid && display->state != MCE_DISPLAY_STATE_OFF;
}

static void ril_devmon_ss_io_state_sent(GRilIoChannel *io, int status,
				const void *data, guint len, void *user_data)
{
	DevMonIo *self = user_data;

	self->req_id = 0;
	if (status == RIL_E_REQUEST_NOT_SUPPORTED) {
		/* This is a permanent failure */
		DBG("RIL_REQUEST_SCREEN_STATE is not supported");
		self->screen_state_supported = FALSE;
	}
}

static void ril_devmon_ss_io_send_screen_state(DevMonIo *self)
{
	/*
	 * RIL_REQUEST_SCREEN_STATE (deprecated on 2017-01-10)
	 *
	 * ((int *)data)[0] is == 1 for "Screen On"
	 * ((int *)data)[0] is == 0 for "Screen Off"
	 */
	if (self->screen_state_supported) {
		GRilIoRequest *req = grilio_request_array_int32_new(1,
							self->display_on);

		grilio_channel_cancel_request(self->io, self->req_id, FALSE);
		self->req_id = grilio_channel_send_request_full(self->io, req,
			RIL_REQUEST_SCREEN_STATE, ril_devmon_ss_io_state_sent,
			NULL, self);
		grilio_request_unref(req);
	}
}

static void ril_devmon_ss_io_set_cell_info_update_interval(DevMonIo *self)
{
	ofono_cell_info_set_update_interval(self->cell_info,
		(self->display_on && (ril_devmon_ss_charging(self->charger) ||
				ril_devmon_ss_battery_ok(self->battery))) ?
					self->cell_info_interval_short_ms :
					self->cell_info_interval_long_ms);
}

static void ril_devmon_ss_io_battery_cb(MceBattery *battery, void *user_data)
{
	ril_devmon_ss_io_set_cell_info_update_interval(user_data);
}

static void ril_devmon_ss_io_charger_cb(MceCharger *charger, void *user_data)
{
	ril_devmon_ss_io_set_cell_info_update_interval(user_data);
}

static void ril_devmon_ss_io_display_cb(MceDisplay *display, void *user_data)
{
	DevMonIo *self = user_data;
	const gboolean display_on = ril_devmon_ss_display_on(display);

	if (self->display_on != display_on) {
		self->display_on = display_on;
		ril_devmon_ss_io_send_screen_state(self);
		ril_devmon_ss_io_set_cell_info_update_interval(self);
	}
}

static void ril_devmon_ss_io_free(struct ril_devmon_io *devmon_io)
{
	DevMonIo *self = ril_devmon_ss_io_cast(devmon_io);

	mce_battery_remove_all_handlers(self->battery, self->battery_event_id);
	mce_battery_unref(self->battery);

	mce_charger_remove_all_handlers(self->charger, self->charger_event_id);
	mce_charger_unref(self->charger);

	mce_display_remove_all_handlers(self->display, self->display_event_id);
	mce_display_unref(self->display);

	grilio_channel_cancel_request(self->io, self->req_id, FALSE);
	grilio_channel_unref(self->io);

	ofono_cell_info_unref(self->cell_info);
	g_free(self);
}

static struct ril_devmon_io *ril_devmon_ss_start_io(struct ril_devmon *devmon,
		GRilIoChannel *io, struct ofono_cell_info *cell_info)
{
	DevMon *ss = ril_devmon_ss_cast(devmon);
	DevMonIo *self = g_new0(DevMonIo, 1);

	self->pub.free = ril_devmon_ss_io_free;
	self->screen_state_supported = TRUE;
	self->io = grilio_channel_ref(io);
	self->cell_info = ofono_cell_info_ref(cell_info);

	self->battery = mce_battery_ref(ss->battery);
	self->battery_event_id[BATTERY_EVENT_VALID] =
		mce_battery_add_valid_changed_handler(self->battery,
			ril_devmon_ss_io_battery_cb, self);
	self->battery_event_id[BATTERY_EVENT_STATUS] =
		mce_battery_add_status_changed_handler(self->battery,
			ril_devmon_ss_io_battery_cb, self);

	self->charger = mce_charger_ref(ss->charger);
	self->charger_event_id[CHARGER_EVENT_VALID] =
		mce_charger_add_valid_changed_handler(self->charger,
			ril_devmon_ss_io_charger_cb, self);
	self->charger_event_id[CHARGER_EVENT_STATE] =
		mce_charger_add_state_changed_handler(self->charger,
			ril_devmon_ss_io_charger_cb, self);

	self->display = mce_display_ref(ss->display);
	self->display_on = ril_devmon_ss_display_on(self->display);
	self->display_event_id[DISPLAY_EVENT_VALID] =
		mce_display_add_valid_changed_handler(self->display,
			ril_devmon_ss_io_display_cb, self);
	self->display_event_id[DISPLAY_EVENT_STATE] =
		mce_display_add_state_changed_handler(self->display,
			ril_devmon_ss_io_display_cb, self);

	self->cell_info_interval_short_ms =
			ss->cell_info_interval_short_ms;
	self->cell_info_interval_long_ms =
			ss->cell_info_interval_long_ms;

	ril_devmon_ss_io_send_screen_state(self);
	ril_devmon_ss_io_set_cell_info_update_interval(self);
	return &self->pub;
}

static void ril_devmon_ss_free(struct ril_devmon *devmon)
{
	DevMon *self = ril_devmon_ss_cast(devmon);

	mce_battery_unref(self->battery);
	mce_charger_unref(self->charger);
	mce_display_unref(self->display);
	g_free(self);
}

struct ril_devmon *ril_devmon_ss_new(const struct ril_slot_config *config)
{
	DevMon *self = g_new0(DevMon, 1);

	self->pub.free = ril_devmon_ss_free;
	self->pub.start_io = ril_devmon_ss_start_io;
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
