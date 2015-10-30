/*
 *  oFono - Open Source Telephony - RIL-based devices
 *
 *  Copyright (C) 2015 Jolla Ltd.
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License version 2 as
 *  published by the Free Software Foundation.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 */

#ifndef RIL_PLUGIN_H
#define RIL_PLUGIN_H

#include "ril_types.h"

#include <ofono/modem.h>
#include <ofono/call-barring.h>
#include <ofono/call-forwarding.h>
#include <ofono/call-settings.h>
#include <ofono/call-volume.h>
#include <ofono/cbs.h>
#include <ofono/devinfo.h>
#include <ofono/gprs-context.h>
#include <ofono/gprs.h>
#include <ofono/netreg.h>
#include <ofono/oemraw.h>
#include <ofono/phonebook.h>
#include <ofono/radio-settings.h>
#include <ofono/sim.h>
#include <ofono/sms.h>
#include <ofono/stk.h>
#include <ofono/ussd.h>
#include <ofono/voicecall.h>

#include <grilio_queue.h>
#include <grilio_request.h>
#include <grilio_parser.h>

#define RILMODEM_DRIVER         "ril"
#define RIL_RETRY_SECS          (2)
#define MAX_SIM_STATUS_RETRIES  (15)

struct ril_plugin {
	const char *default_voice_imsi;
	const char *default_data_imsi;
	const char *default_voice_path;
	const char *default_data_path;
	char **available_slots;
	char **enabled_slots;
};

struct ril_modem_config {
	guint slot;
	gboolean enable_4g;
	const char *default_name;
};

#define RIL_PLUGIN_SIGNAL_VOICE_IMSI    (0x01)
#define RIL_PLUGIN_SIGNAL_DATA_IMSI     (0x02)
#define RIL_PLUGIN_SIGNAL_VOICE_PATH    (0x04)
#define RIL_PLUGIN_SIGNAL_DATA_PATH     (0x10)
#define RIL_PLUGIN_SIGNAL_ENABLED_SLOTS (0x20)

struct ril_modem;
struct ril_plugin_dbus;
typedef void (*ril_modem_cb_t)(struct ril_modem *modem, void *data);

void ril_plugin_set_enabled_slots(struct ril_plugin *plugin, char **slots);
void ril_plugin_set_default_voice_imsi(struct ril_plugin *plugin,
							const char *imsi);
void ril_plugin_set_default_data_imsi(struct ril_plugin *plugin,
							const char *imsi);

struct ril_sim_dbus *ril_sim_dbus_new(struct ril_modem *modem);
const char *ril_sim_dbus_imsi(struct ril_sim_dbus *dbus);
void ril_sim_dbus_free(struct ril_sim_dbus *dbus);

struct ril_plugin_dbus *ril_plugin_dbus_new(struct ril_plugin *plugin);
void ril_plugin_dbus_free(struct ril_plugin_dbus *dbus);
void ril_plugin_dbus_signal(struct ril_plugin_dbus *dbus, int mask);

struct ril_modem *ril_modem_create(GRilIoChannel *io, const char *dev,
					const struct ril_modem_config *config);
void ril_modem_delete(struct ril_modem *modem);
void ril_modem_allow_data(struct ril_modem *modem);
GRilIoChannel *ril_modem_io(struct ril_modem *modem);
const struct ril_modem_config *ril_modem_config(struct ril_modem *modem);
struct ofono_sim *ril_modem_ofono_sim(struct ril_modem *modem);
struct ofono_gprs *ril_modem_ofono_gprs(struct ril_modem *modem);
struct ofono_netreg *ril_modem_ofono_netreg(struct ril_modem *modem);
struct ofono_modem *ril_modem_ofono_modem(struct ril_modem *modem);
void ril_modem_set_error_cb(struct ril_modem *modem, ril_modem_cb_t cb,
								void *data);
void ril_modem_set_removed_cb(struct ril_modem *modem, ril_modem_cb_t cb,
								void *data);

#define ril_modem_slot(md) (ril_modem_config(modem)->slot)
#define ril_modem_4g_enabled(md) (ril_modem_config(modem)->enable_4g)
#define ril_modem_get_path(md) ofono_modem_get_path(ril_modem_ofono_modem(md))

void ril_sim_read_file_linear(struct ofono_sim *sim, int fileid,
		int record, int length, const unsigned char *path,
		unsigned int path_len, ofono_sim_read_cb_t cb, void *data);
void ril_sim_read_file_cyclic(struct ofono_sim *sim, int fileid,
		int record, int length, const unsigned char *path,
		unsigned int path_len, ofono_sim_read_cb_t cb, void *data);
void ril_sim_read_file_transparent(struct ofono_sim *sim, int fileid,
		int start, int length, const unsigned char *path,
		unsigned int path_len, ofono_sim_read_cb_t cb, void *data);
void ril_sim_read_file_info(struct ofono_sim *sim, int fileid,
		const unsigned char *path, unsigned int path_len,
		ofono_sim_file_info_cb_t cb, void *data);

int ril_sim_app_type(struct ofono_sim *sim);
int ril_gprs_ril_data_tech(struct ofono_gprs *gprs);
int ril_netreg_check_if_really_roaming(struct ofono_netreg *netreg,
								gint status);

extern const struct ofono_call_barring_driver ril_call_barring_driver;
extern const struct ofono_call_forwarding_driver ril_call_forwarding_driver;
extern const struct ofono_call_settings_driver ril_call_settings_driver;
extern const struct ofono_call_volume_driver ril_call_volume_driver;
extern const struct ofono_cbs_driver ril_cbs_driver;
extern const struct ofono_devinfo_driver ril_devinfo_driver;
extern const struct ofono_gprs_context_driver ril_gprs_context_driver;
extern const struct ofono_gprs_driver ril_gprs_driver;
extern const struct ofono_modem_driver ril_modem_driver;
extern const struct ofono_netreg_driver ril_netreg_driver;
extern /* const */ struct ofono_oem_raw_driver ril_oem_raw_driver;
extern const struct ofono_phonebook_driver ril_phonebook_driver;
extern const struct ofono_radio_settings_driver ril_radio_settings_driver;
extern const struct ofono_sim_driver ril_sim_driver;
extern const struct ofono_sms_driver ril_sms_driver;
extern const struct ofono_stk_driver ril_stk_driver;
extern const struct ofono_ussd_driver ril_ussd_driver;
extern const struct ofono_voicecall_driver ril_voicecall_driver;

#endif /* RIL_PLUGIN_H */

/*
 * Local Variables:
 * mode: C
 * c-basic-offset: 8
 * indent-tabs-mode: t
 * End:
 */
