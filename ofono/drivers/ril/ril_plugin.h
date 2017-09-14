/*
 *  oFono - Open Source Telephony - RIL-based devices
 *
 *  Copyright (C) 2015-2017 Jolla Ltd.
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
#include "sailfish_manager.h"

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
#include <ofono/phonebook.h>
#include <ofono/radio-settings.h>
#include <ofono/sim.h>
#include <ofono/sms.h>
#include <ofono/stk.h>
#include <ofono/ussd.h>
#include <ofono/voicecall.h>
#include <ofono/netmon.h>

#include <grilio_queue.h>
#include <grilio_request.h>
#include <grilio_parser.h>

#define RILMODEM_DRIVER         "ril"

struct ril_modem {
	GRilIoChannel *io;
	const char *imei;
	const char *imeisv;
	const char *log_prefix;
	const char *ecclist_file;
	struct ofono_modem *ofono;
	struct sailfish_cell_info *cell_info;
	struct ril_radio *radio;
	struct ril_data *data;
	struct ril_network *network;
	struct ril_sim_card *sim_card;
	struct ril_sim_settings *sim_settings;
	struct ril_slot_config config;
};

struct ril_oem_raw;
struct ril_oem_raw *ril_oem_raw_new(struct ril_modem *modem,
						const char *log_prefix);
void ril_oem_raw_free(struct ril_oem_raw *raw);

struct ril_modem *ril_modem_create(GRilIoChannel *io, const char *log_prefix,
		const char *path, const char *imei, const char *imeisv,
		const char *ecclist_file, const struct ril_slot_config *config,
		struct ril_radio *radio, struct ril_network *network,
		struct ril_sim_card *card, struct ril_data *data,
		struct ril_sim_settings *settings,
		struct sailfish_cell_info *cell_info);
void ril_modem_delete(struct ril_modem *modem);
struct ofono_sim *ril_modem_ofono_sim(struct ril_modem *modem);
struct ofono_gprs *ril_modem_ofono_gprs(struct ril_modem *modem);
struct ofono_netreg *ril_modem_ofono_netreg(struct ril_modem *modem);

#define ril_modem_get_path(modem) ofono_modem_get_path((modem)->ofono)
#define ril_modem_4g_enabled(modem) ((modem)->config.enable_4g)
#define ril_modem_slot(modem) ((modem)->config.slot)
#define ril_modem_io(modem) ((modem)->io)

int ril_sim_app_type(struct ofono_sim *sim);
int ril_netreg_check_if_really_roaming(struct ofono_netreg *reg, gint status);

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
extern const struct ofono_phonebook_driver ril_phonebook_driver;
extern const struct ofono_radio_settings_driver ril_radio_settings_driver;
extern const struct ofono_sim_driver ril_sim_driver;
extern const struct ofono_sms_driver ril_sms_driver;
extern const struct ofono_stk_driver ril_stk_driver;
extern const struct ofono_ussd_driver ril_ussd_driver;
extern const struct ofono_voicecall_driver ril_voicecall_driver;
extern const struct ofono_netmon_driver ril_netmon_driver;

#endif /* RIL_PLUGIN_H */

/*
 * Local Variables:
 * mode: C
 * c-basic-offset: 8
 * indent-tabs-mode: t
 * End:
 */
