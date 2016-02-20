/*
 *  oFono - Open Source Telephony - RIL-based devices
 *
 *  Copyright (C) 2015-2016 Jolla Ltd.
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

#include "ril_plugin.h"
#include "ril_sim_card.h"
#include "ril_sim_info.h"
#include "ril_network.h"
#include "ril_radio.h"
#include "ril_data.h"
#include "ril_mce.h"
#include "ril_util.h"
#include "ril_log.h"

#include <gdbus.h>
#include <gutil_strv.h>
#include <linux/capability.h>
#include <sys/types.h>
#include <sys/syscall.h>
#include <sys/prctl.h>

#define OFONO_API_SUBJECT_TO_CHANGE
#include <ofono/plugin.h>
#include "ofono.h"
#include "storage.h"

#define RADIO_GID                   1001
#define RADIO_UID                   1001
#define RIL_SUB_SIZE                4

#define RILMODEM_CONF_FILE          CONFIGDIR "/ril_subscription.conf"
#define RILMODEM_DEFAULT_SOCK       "/dev/socket/rild"
#define RILMODEM_DEFAULT_SOCK2      "/dev/socket/rild2"
#define RILMODEM_DEFAULT_SUB        "SUB1"
#define RILMODEM_DEFAULT_4G         TRUE /* 4G is on by default */
#define RILMODEM_DEFAULT_SLOT       0xffffffff
#define RILMODEM_DEFAULT_TIMEOUT    0 /* No timeout */
#define RILMODEM_DEFAULT_SIM_FLAGS  RIL_SIM_CARD_V9_UICC_SUBSCRIPTION_WORKAROUND

#define RILCONF_DEV_PREFIX          "ril_"
#define RILCONF_PATH_PREFIX         "/" RILCONF_DEV_PREFIX
#define RILCONF_NAME                "name"
#define RILCONF_SOCKET              "socket"
#define RILCONF_SLOT                "slot"
#define RILCONF_SUB                 "sub"
#define RILCONF_TIMEOUT             "timeout"
#define RILCONF_4G                  "enable4G"
#define RILCONF_UICC_WORKAROUND     "uiccWorkaround"

#define RIL_STORE                   "ril"
#define RIL_STORE_GROUP             "Settings"
#define RIL_STORE_ENABLED_SLOTS     "EnabledSlots"
#define RIL_STORE_DEFAULT_VOICE_SIM "DefaultVoiceSim"
#define RIL_STORE_DEFAULT_DATA_SIM  "DefaultDataSim"
#define RIL_STORE_SLOTS_SEP         ","

enum ril_plugin_io_events {
	IO_EVENT_CONNECTED,
	IO_EVENT_ERROR,
	IO_EVENT_EOF,
	IO_EVENT_RADIO_STATE_CHANGED,
	IO_EVENT_COUNT
};

struct ril_plugin_priv {
	struct ril_plugin pub;
	struct ril_plugin_dbus *dbus;
	struct ril_data_manager *data_manager;
	GSList *slots;
	ril_slot_info_ptr *slots_info;
	struct ril_slot *voice_slot;
	struct ril_slot *data_slot;
	struct ril_slot *mms_slot;
	char *default_voice_imsi;
	char *default_data_imsi;
	char *mms_imsi;
	GKeyFile *storage;
};

struct ril_slot {
	struct ril_slot_info pub;
	char *path;
	char *imei;
	char *name;
	char *sockpath;
	char *sub;
	gint timeout;           /* RIL timeout, in milliseconds */
	int index;
	int sim_flags;
	struct ril_slot_config config;
	struct ril_plugin_priv *plugin;
	struct ril_modem *modem;
	struct ril_mce *mce;
	struct ofono_sim *sim;
	struct ril_radio *radio;
	struct ril_network *network;
	struct ril_sim_card *sim_card;
	struct ril_sim_info *sim_info;
	struct ril_sim_info_dbus *sim_info_dbus;
	struct ril_data *data;
	GRilIoChannel *io;
	gulong io_event_id[IO_EVENT_COUNT];
	gulong imei_req_id;
	gulong sim_card_state_event_id;
	guint trace_id;
	guint dump_id;
	guint retry_id;
	guint sim_watch_id;
	guint sim_state_watch_id;
	enum ofono_sim_state sim_state;
};

static void ril_debug_trace_notify(struct ofono_debug_desc *desc);
static void ril_debug_dump_notify(struct ofono_debug_desc *desc);
static void ril_debug_grilio_notify(struct ofono_debug_desc *desc);
static void ril_plugin_retry_init_io(struct ril_slot *slot);

GLOG_MODULE_DEFINE("rilmodem");

static struct ofono_debug_desc ril_debug_trace OFONO_DEBUG_ATTR = {
	.name = "ril_trace",
	.flags = OFONO_DEBUG_FLAG_DEFAULT,
	.notify = ril_debug_trace_notify
};

static struct ofono_debug_desc ril_debug_dump OFONO_DEBUG_ATTR = {
	.name = "ril_dump",
	.flags = OFONO_DEBUG_FLAG_DEFAULT,
	.notify = ril_debug_dump_notify
};

static struct ofono_debug_desc grilio_debug OFONO_DEBUG_ATTR = {
	.name = "grilio",
	.flags = OFONO_DEBUG_FLAG_DEFAULT,
	.notify = ril_debug_grilio_notify
};

static struct ril_plugin_priv *ril_plugin_cast(struct ril_plugin *pub)
{
	return G_CAST(pub, struct ril_plugin_priv, pub);
}

static gboolean ril_plugin_multisim(struct ril_plugin_priv *plugin)
{
	return plugin->slots && plugin->slots->next;
}

static void ril_plugin_foreach_slot_proc(gpointer data, gpointer user_data)
{
	void (*fn)(struct ril_slot *) = user_data;
	fn((struct ril_slot *)data);
}

static void ril_plugin_foreach_slot(struct ril_plugin_priv *plugin,
					void (*fn)(struct ril_slot *))
{
	g_slist_foreach(plugin->slots, ril_plugin_foreach_slot_proc, fn);
}

static void ril_plugin_remove_slot_handler(struct ril_slot *slot, int id)
{
	GASSERT(id >= 0 && id<IO_EVENT_COUNT);
	if (slot->io_event_id[id]) {
		grilio_channel_remove_handler(slot->io, slot->io_event_id[id]);
		slot->io_event_id[id] = 0;
	}
}

static void ril_plugin_shutdown_slot(struct ril_slot *slot, gboolean kill_io)
{
	if (slot->sim) {
		if (slot->sim_state_watch_id) {
			ofono_sim_remove_state_watch(slot->sim,
						slot->sim_state_watch_id);
		}
		ril_sim_info_set_ofono_sim(slot->sim_info, NULL);
		slot->sim = NULL;
	}

	if (slot->modem) {
		struct ofono_modem *m = slot->modem->ofono;

		if (m && slot->sim_watch_id) {
			__ofono_modem_remove_atom_watch(m, slot->sim_watch_id);
		}

		ril_modem_delete(slot->modem);
		/* The above call is expected to result in
		 * ril_plugin_modem_removed getting called
		 * which will set slot->modem to NULL */
		GASSERT(!slot->modem);
	}

	/* All watches have to be unregistered by now */
	GASSERT(!slot->sim_state_watch_id);
	GASSERT(!slot->sim_watch_id);

	if (kill_io) {
		if (slot->mce) {
			ril_mce_free(slot->mce);
			slot->mce = NULL;
		}

		if (slot->retry_id) {
			g_source_remove(slot->retry_id);
			slot->retry_id = 0;
		}

		if (slot->data) {
			ril_data_allow(slot->data, FALSE);
			ril_data_unref(slot->data);
			slot->data = NULL;
		}

		if (slot->radio) {
			ril_radio_unref(slot->radio);
			slot->radio = NULL;
		}

		if (slot->network) {
			ril_network_unref(slot->network);
			slot->network = NULL;
		}

		if (slot->sim_card) {
			ril_sim_card_remove_handler(slot->sim_card,
						slot->sim_card_state_event_id);
			ril_sim_card_unref(slot->sim_card);
			slot->sim_card_state_event_id = 0;
			slot->sim_card = NULL;
		}

		if (slot->io) {
			int i;

			grilio_channel_remove_logger(slot->io, slot->trace_id);
			grilio_channel_remove_logger(slot->io, slot->dump_id);
			slot->trace_id = 0;
			slot->dump_id = 0;

			grilio_channel_cancel_request(slot->io,
						slot->imei_req_id, FALSE);
			slot->imei_req_id = 0;

			for (i=0; i<IO_EVENT_COUNT; i++) {
				ril_plugin_remove_slot_handler(slot, i);
			}

			grilio_channel_shutdown(slot->io, FALSE);
			grilio_channel_unref(slot->io);
			slot->io = NULL;
		}
	}
}

static void ril_plugin_set_config_string(struct ril_plugin_priv *plugin,
			const char *key, const char *value, gboolean sync)
{
	if (value) {
		g_key_file_set_string(plugin->storage, RIL_STORE_GROUP, key,
									value);
	} else {
		g_key_file_remove_key(plugin->storage, RIL_STORE_GROUP, key,
									NULL);
	}
	if (sync) {
		storage_sync(NULL, RIL_STORE, plugin->storage);
	}
}

static struct ril_slot *ril_plugin_find_slot_imsi(GSList *slots,
						const char *imsi)
{
	struct ril_slot *default_slot = NULL;

	while (slots) {
		struct ril_slot *slot = slots->data;
		const char *slot_imsi = ofono_sim_get_imsi(slot->sim);
		if (slot_imsi) {
			if (imsi) {
				/* We are looking for the specific sim */
				if (!strcmp(imsi, slot_imsi)) {
					return slot;
				}
			} else {
				/* We are looking for any slot with a sim */
				if (!default_slot) {
					default_slot = slot;
				}
			}
		}
		slots = slots->next;
	}

	return default_slot;
}

static struct ril_slot *ril_plugin_find_slot_number(GSList *slots, guint number)
{
	while (slots) {
		struct ril_slot *slot = slots->data;
		if (slot->config.slot == number) {
			return slot;
		}
		slots = slots->next;
	}

	return NULL;
}

/* Returns the event mask to be passed to ril_plugin_dbus_signal.
 * The caller has a chance to OR it with other bits */
static int ril_plugin_update_modem_paths(struct ril_plugin_priv *plugin)
{
	int mask = 0;
	struct ril_slot *slot = NULL;
	struct ril_slot *mms_slot = NULL;
	struct ril_slot *old_data_slot = NULL;
	struct ril_slot *new_data_slot = NULL;

	/* Voice */
	if (plugin->default_voice_imsi) {
		slot = ril_plugin_find_slot_imsi(plugin->slots,
				plugin->default_voice_imsi);
	} else if (plugin->voice_slot) {
		/* Make sure that the slot is enabled and SIM is in */
		slot = ril_plugin_find_slot_imsi(plugin->slots,
				plugin->voice_slot->modem ?
				ofono_sim_get_imsi(plugin->voice_slot->sim) :
				NULL);
	}

	/*
	 * If there's no default voice SIM, we will find any SIM instead.
	 * One should always be able to make and receive a phone call
	 * if there's a working SIM in the phone. However if the
	 * previously selected voice SIM is inserted, we will switch
	 * back to it.
	 *
	 * There is no such fallback for the data.
	 */
	if (!slot) {
		slot = ril_plugin_find_slot_imsi(plugin->slots, NULL);
	}

	if (plugin->voice_slot != slot) {
		mask |= RIL_PLUGIN_SIGNAL_VOICE_PATH;
		plugin->voice_slot = slot;
		if (slot) {
			DBG("Default voice SIM at %s", slot->path);
			plugin->pub.default_voice_path = slot->path;
		} else {
			DBG("No default voice SIM");
			plugin->pub.default_voice_path = NULL;
		}
	}

	/* Data */
	if (plugin->default_data_imsi) {
		slot = ril_plugin_find_slot_imsi(plugin->slots,
				plugin->default_data_imsi);
	} else if (plugin->data_slot) {
		/* Make sure that the slot is enabled and SIM is in */
		slot = ril_plugin_find_slot_imsi(plugin->slots,
				plugin->data_slot->modem ?
				ofono_sim_get_imsi(plugin->data_slot->sim) :
				NULL);
	} else {
		slot = ril_plugin_find_slot_imsi(plugin->slots, NULL);
	}

	if (slot && !slot->radio->online) {
		slot = NULL;
	}

	if (plugin->mms_imsi) {
		mms_slot = ril_plugin_find_slot_imsi(plugin->slots,
						plugin->mms_imsi);
	}

	if (mms_slot && mms_slot != slot) {
		/*
		 * Reset default data SIM if another SIM is
		 * temporarily selected for MMS.
		 */
		slot = NULL;
	}

	/* Are we actually switching data SIMs? */
	old_data_slot = plugin->mms_slot ? plugin->mms_slot : plugin->data_slot;
	new_data_slot = mms_slot ? mms_slot : slot;

	if (plugin->data_slot != slot) {
		mask |= RIL_PLUGIN_SIGNAL_DATA_PATH;
		plugin->data_slot = slot;
		if (slot) {
			DBG("Default data SIM at %s", slot->path);
			plugin->pub.default_data_path = slot->path;
		} else {
			DBG("No default data SIM");
			plugin->pub.default_data_path = NULL;
		}
	}

	if (plugin->mms_slot != mms_slot) {
		mask |= RIL_PLUGIN_SIGNAL_MMS_PATH;
		plugin->mms_slot = mms_slot;
		if (mms_slot) {
			DBG("MMS data SIM at %s", mms_slot->path);
			plugin->pub.mms_path = mms_slot->path;
		} else {
			DBG("No MMS data SIM");
			plugin->pub.mms_path = NULL;
		}
	}

	if (old_data_slot != new_data_slot) {
		/* Yes we are switching data SIMs */
		if (old_data_slot) {
			ril_data_allow(old_data_slot->data, FALSE);
		}
		if (new_data_slot) {
			ril_data_allow(new_data_slot->data, TRUE);
		}
	}

	return mask;
}

/* Update modem paths and emit D-Bus signal if necessary */
static void ril_plugin_update_modem_paths_full(struct ril_plugin_priv *plugin)
{
	ril_plugin_dbus_signal(plugin->dbus,
					ril_plugin_update_modem_paths(plugin));
}

static void ril_plugin_update_ready(struct ril_plugin_priv *plugin)
{
	GSList *link;
	gboolean ready = TRUE;

	for (link = plugin->slots; link; link = link->next) {
		struct ril_slot *slot = link->data;

		if (!slot->imei || !slot->sim_card || !slot->sim_card->status) {
			ready = FALSE;
			break;
		}
	}

	if (plugin->pub.ready != ready) {
		DBG("%sready", ready ? "" : "not ");
		plugin->pub.ready = ready;
		ril_plugin_dbus_signal(plugin->dbus, RIL_PLUGIN_SIGNAL_READY);
	}
}

static void ril_plugin_sim_state_changed(struct ril_sim_card *card, void *data)
{
	struct ril_slot *slot = data;
	struct ril_plugin_priv *plugin = slot->plugin;
	gboolean present;

	if (card && card->status &&
			card->status->card_state == RIL_CARDSTATE_PRESENT) {
		DBG("SIM found in slot %u", slot->config.slot);
		present = TRUE;
	} else {
		DBG("No SIM in slot %u", slot->config.slot);
		present = FALSE;
	}

	if (slot->pub.sim_present != present) {
		slot->pub.sim_present = present;
		ril_plugin_dbus_signal_sim(plugin->dbus, slot->index, present);
	}

	ril_plugin_update_ready(plugin);
}

static void ril_plugin_sim_watch_done(void *data)
{
	struct ril_slot *slot = data;

	slot->sim_watch_id = 0;
}

static void ril_plugin_sim_state_watch_done(void *data)
{
	struct ril_slot *slot = data;

	slot->sim_state_watch_id = 0;
}

static void ril_plugin_sim_state_watch(enum ofono_sim_state new_state,
								void *data)
{
	struct ril_slot *slot = data;
	struct ril_plugin_priv *plugin = slot->plugin;

	DBG("%s sim state %d", slot->path + 1, new_state);
	slot->sim_state = new_state;
	if (new_state == OFONO_SIM_STATE_READY) {
		struct ril_slot *voice_slot = plugin->voice_slot;
		struct ril_slot *data_slot = plugin->data_slot;
		int signal_mask;

		/*
		 * OFONO_SIM_STATE_READY means that pin code has been
		 * entered (if necessary) and IMSI has been obtained.
		 *
		 * We want the first slot to be selected by default.
		 * However, things may become available in pretty much
		 * any order, so reset the slot pointers to NULL and let
		 * ril_plugin_update_modem_paths() to pick them again.
		 *
		 * Only affects the very first boot and first boot after
		 * the default voice SIM has been removed.
		 */
		plugin->voice_slot = NULL;
		plugin->data_slot = NULL;
		signal_mask = ril_plugin_update_modem_paths(plugin);
		if (voice_slot != plugin->voice_slot) {
			DBG("Voice slot changed");
			signal_mask |= RIL_PLUGIN_SIGNAL_VOICE_PATH;
		}
		if (data_slot != plugin->data_slot) {
			DBG("Data slot changed");
			signal_mask |= RIL_PLUGIN_SIGNAL_DATA_PATH;
		}
		ril_plugin_dbus_signal(plugin->dbus, signal_mask);
	} else {
		ril_plugin_update_modem_paths_full(plugin);
	}
}

static void ril_plugin_register_sim(struct ril_slot *slot, struct ofono_sim *sim)
{
	GASSERT(sim);
	GASSERT(!slot->sim);
	GASSERT(slot->sim_watch_id);
	GASSERT(!slot->sim_state_watch_id);

	slot->sim = sim;
	slot->sim_state = ofono_sim_get_state(sim);
	slot->sim_state_watch_id = ofono_sim_add_state_watch(sim,
					ril_plugin_sim_state_watch, slot,
					ril_plugin_sim_state_watch_done);
	ril_sim_info_set_ofono_sim(slot->sim_info, sim);
}

static void ril_plugin_sim_watch(struct ofono_atom *atom,
			enum ofono_atom_watch_condition cond, void *data)
{
	struct ril_slot *slot = data;

	if (cond == OFONO_ATOM_WATCH_CONDITION_REGISTERED) {
		DBG("%s sim registered", slot->path + 1);
		ril_plugin_register_sim(slot, __ofono_atom_get_data(atom));
	} else if (cond == OFONO_ATOM_WATCH_CONDITION_UNREGISTERED) {
		DBG("%s sim unregistered", slot->path + 1);
		ril_sim_info_set_ofono_sim(slot->sim_info, NULL);
		slot->sim = NULL;
	}

	ril_plugin_update_modem_paths_full(slot->plugin);
}

static void ril_plugin_handle_error(struct ril_slot *slot)
{
	ril_plugin_shutdown_slot(slot, TRUE);
	ril_plugin_update_modem_paths_full(slot->plugin);
	ril_plugin_retry_init_io(slot);
}

static void ril_plugin_slot_error(GRilIoChannel *io, const GError *error,
								void *data)
{
	ril_plugin_handle_error((struct ril_slot *)data);
}

static void ril_plugin_slot_disconnected(GRilIoChannel *io, void *data)
{
	ril_plugin_handle_error((struct ril_slot *)data);
}

static void ril_plugin_modem_online(struct ril_modem *modem, gboolean online,
								void *data)
{
	struct ril_slot *slot = data;

	DBG("%s %d", slot->path + 1, online);
	GASSERT(slot->modem);
	GASSERT(slot->modem == modem);

	ril_radio_set_online(slot->radio, online);
	ril_plugin_update_modem_paths_full(slot->plugin);
}

static void ril_plugin_modem_removed(struct ril_modem *modem, void *data)
{
	struct ril_slot *slot = data;

	DBG("");
	GASSERT(slot->modem);
	GASSERT(slot->modem == modem);

	if (slot->sim_info_dbus) {
		ril_sim_info_dbus_free(slot->sim_info_dbus);
		slot->sim_info_dbus = NULL;
	}

	slot->modem = NULL;
	ril_radio_set_online(slot->radio, FALSE);
	ril_data_allow(slot->data, FALSE);
	ril_plugin_update_modem_paths_full(slot->plugin);
	ril_sim_info_set_ofono_sim(slot->sim_info, NULL);
}

static void ril_plugin_trace(GRilIoChannel *io, GRILIO_PACKET_TYPE type,
	guint id, guint code, const void *data, guint data_len, void *user_data)
{
	/* Use log sub-module to turn prefix off */
	static GLOG_MODULE_DEFINE2_(log_module, NULL, GLOG_MODULE_NAME);
	const char *prefix = io->name ? io->name : "";
	const char dir = (type == GRILIO_PACKET_REQ) ? '<' : '>';
	const char *scode;

	switch (type) {
	case GRILIO_PACKET_REQ:
		if (io->ril_version <= 9 &&
				code == RIL_REQUEST_V9_SET_UICC_SUBSCRIPTION) {
			scode = "V9_SET_UICC_SUBSCRIPTION";
		} else {
			scode = ril_request_to_string(code);
		}
		gutil_log(&log_module, GLOG_LEVEL_INFO, "%s%c [%08x] %s",
				prefix, dir, id, scode);
		break;
	case GRILIO_PACKET_RESP:
		gutil_log(&log_module, GLOG_LEVEL_INFO, "%s%c [%08x] %s",
				prefix, dir, id, ril_error_to_string(code));
		break;
	case GRILIO_PACKET_UNSOL:
		gutil_log(&log_module, GLOG_LEVEL_INFO, "%s%c %s",
				prefix, dir, ril_unsol_event_to_string(code));
		break;
	}
}

static void ril_debug_dump_update_slot(struct ril_slot *slot)
{
	if (slot->io) {
		if (ril_debug_dump.flags & OFONO_DEBUG_FLAG_PRINT) {
			if (!slot->dump_id) {
				slot->dump_id =
					grilio_channel_add_default_logger(
						slot->io, GLOG_LEVEL_INFO);
			}
		} else if (slot->dump_id) {
			grilio_channel_remove_logger(slot->io, slot->dump_id);
			slot->dump_id = 0;
		}
	}
}

static void ril_debug_trace_update_slot(struct ril_slot *slot)
{
	if (slot->io) {
		if (ril_debug_trace.flags & OFONO_DEBUG_FLAG_PRINT) {
			if (!slot->trace_id) {
				slot->trace_id =
					grilio_channel_add_logger(slot->io,
						ril_plugin_trace, slot);
				/*
				 * Loggers are invoked in the order they have
				 * been registered. Make sure that dump logger
				 * is invoked after ril_plugin_trace.
				 */
				if (slot->dump_id) {
					grilio_channel_remove_logger(slot->io,
								slot->dump_id);
					slot->dump_id = 0;
				}
				ril_debug_dump_update_slot(slot);
			}
		} else if (slot->trace_id) {
			grilio_channel_remove_logger(slot->io, slot->trace_id);
			slot->trace_id = 0;
		}
	}
}

static gboolean ril_plugin_can_create_modem(struct ril_slot *slot)
{
	return slot->pub.enabled && slot->io && slot->io->connected;
}

static void ril_plugin_create_modem(struct ril_slot *slot)
{
	struct ril_modem *modem;

	DBG("%s", slot->path);
	GASSERT(slot->io && slot->io->connected);
	GASSERT(!slot->modem);

	modem = ril_modem_create(slot->io, &slot->pub, slot->radio,
				 slot->network, slot->sim_card, slot->data);

	if (modem) {
		struct ofono_sim *sim = ril_modem_ofono_sim(modem);

		slot->modem = modem;
		slot->sim_watch_id = __ofono_modem_add_atom_watch(modem->ofono,
				OFONO_ATOM_TYPE_SIM, ril_plugin_sim_watch,
				slot, ril_plugin_sim_watch_done);
		if (sim) {
			ril_plugin_register_sim(slot, sim);
		}

		slot->sim_info_dbus = ril_sim_info_dbus_new(slot->modem,
							slot->sim_info);

		ril_modem_set_removed_cb(modem, ril_plugin_modem_removed, slot);
		ril_modem_set_online_cb(modem, ril_plugin_modem_online, slot);
	} else {
		ril_plugin_shutdown_slot(slot, TRUE);
	}

	ril_plugin_update_modem_paths_full(slot->plugin);
}

static void ril_plugin_imei_cb(GRilIoChannel *io, int status,
			const void *data, guint len, void *user_data)
{
	struct ril_slot *slot = user_data;
	struct ril_plugin_priv *plugin = slot->plugin;
	gboolean all_done = TRUE;
	GSList *link;

	GASSERT(!slot->imei);
	GASSERT(slot->imei_req_id);
	slot->imei_req_id = 0;

	if (status == RIL_E_SUCCESS) {
		GRilIoParser rilp;
		grilio_parser_init(&rilp, data, len);
		slot->pub.imei = slot->imei = grilio_parser_get_utf8(&rilp);
		DBG("%s", slot->imei);
		if (slot->modem) {
			ril_modem_set_imei(slot->modem, slot->imei);
		}
		ril_plugin_update_ready(plugin);
	} else {
		ofono_error("Slot %u IMEI query error: %s", slot->config.slot,
						ril_error_to_string(status));
	}

	for (link = plugin->slots; link && all_done; link = link->next) {
		if (((struct ril_slot *)link->data)->imei_req_id) {
			all_done = FALSE;
		}
	}

	if (all_done) {
		DBG("all done");
		ril_plugin_dbus_block_imei_requests(plugin->dbus, FALSE);
	}
}

/*
 * It seems to be necessary to kick (with RIL_REQUEST_RADIO_POWER) the
 * modems with power on after one of the modems has been powered off.
 * Otherwise bad things may happen (like the modem never registering
 * on the network).
 */
static void ril_plugin_power_check(struct ril_slot *slot)
{
	ril_radio_confirm_power_on(slot->radio);
}

static void ril_plugin_radio_state_changed(GRilIoChannel *io, guint code,
				const void *data, guint len, void *user_data)
{
	if (ril_radio_state_parse(data, len) == RADIO_STATE_OFF) {
		struct ril_slot *slot = user_data;

		DBG("power off for slot %u", slot->config.slot);
		ril_plugin_foreach_slot(slot->plugin, ril_plugin_power_check);
	}
}

static void ril_plugin_slot_connected(struct ril_slot *slot)
{
	struct ril_plugin_priv *plugin = slot->plugin;

	ofono_debug("%s version %u", (slot->name && slot->name[0]) ?
				slot->name : "RIL", slot->io->ril_version);

	GASSERT(slot->io->connected);
	GASSERT(!slot->io_event_id[IO_EVENT_CONNECTED]);

	GASSERT(!slot->mce);
	slot->mce = ril_mce_new(slot->io);

	GASSERT(!slot->imei_req_id);
	slot->imei_req_id = grilio_channel_send_request_full(slot->io, NULL,
			RIL_REQUEST_GET_IMEI, ril_plugin_imei_cb, NULL, slot);

	GASSERT(!slot->radio);
	slot->radio = ril_radio_new(slot->io);
	slot->network = ril_network_new(slot->io, slot->radio);

	GASSERT(!slot->io_event_id[IO_EVENT_RADIO_STATE_CHANGED]);
	slot->io_event_id[IO_EVENT_RADIO_STATE_CHANGED] =
		grilio_channel_add_unsol_event_handler(slot->io,
				ril_plugin_radio_state_changed,
				RIL_UNSOL_RESPONSE_RADIO_STATE_CHANGED, slot);

	GASSERT(!slot->sim_card);
	slot->sim_card = ril_sim_card_new(slot->io, slot->config.slot,
							slot->sim_flags);
	slot->sim_card_state_event_id = ril_sim_card_add_state_changed_handler(
			slot->sim_card, ril_plugin_sim_state_changed, slot);

	GASSERT(!slot->data);
	slot->data = ril_data_new(plugin->data_manager, slot->radio,
						slot->network, slot->io);

	if (ril_plugin_multisim(plugin)) {
		ril_data_set_name(slot->data, slot->path + 1);
	}

	if (ril_plugin_can_create_modem(slot) && !slot->modem) {
		ril_plugin_create_modem(slot);
	}

	ril_plugin_update_ready(plugin);
}

static void ril_plugin_slot_connected_cb(GRilIoChannel *io, void *user_data)
{
	struct ril_slot *slot = user_data;

	ril_plugin_remove_slot_handler(slot, IO_EVENT_CONNECTED);
	ril_plugin_slot_connected(slot);
}

static void ril_plugin_init_io(struct ril_slot *slot)
{
	if (!slot->io) {
		DBG("%s %s", slot->sockpath, slot->sub);
		slot->io = grilio_channel_new_socket(slot->sockpath, slot->sub);
		if (slot->io) {
			ril_debug_trace_update_slot(slot);
			ril_debug_dump_update_slot(slot);

			if (slot->name) {
				grilio_channel_set_name(slot->io, slot->name);
			}

			grilio_channel_set_timeout(slot->io, slot->timeout);
			slot->io_event_id[IO_EVENT_ERROR] =
				grilio_channel_add_error_handler(slot->io,
					ril_plugin_slot_error, slot);
			slot->io_event_id[IO_EVENT_EOF] =
				grilio_channel_add_disconnected_handler(slot->io,
					ril_plugin_slot_disconnected, slot);

			if (slot->io->connected) {
				ril_plugin_slot_connected(slot);
			} else {
				slot->io_event_id[IO_EVENT_CONNECTED] =
					grilio_channel_add_connected_handler(
						slot->io,
						ril_plugin_slot_connected_cb,
						slot);
			}
		}
	}

	if (!slot->io) {
		ril_plugin_retry_init_io(slot);
	}
}

static gboolean ril_plugin_retry_init_io_cb(gpointer data)
{
	struct ril_slot *slot = data;

	GASSERT(slot->retry_id);
	slot->retry_id = 0;
	ril_plugin_init_io(slot);

	return G_SOURCE_REMOVE;
}

static void ril_plugin_retry_init_io(struct ril_slot *slot)
{
	if (slot->retry_id) {
		g_source_remove(slot->retry_id);
	}

	DBG("%s %s", slot->sockpath, slot->sub);
	slot->retry_id = g_timeout_add_seconds(RIL_RETRY_SECS,
					ril_plugin_retry_init_io_cb, slot);
}

static GSList *ril_plugin_create_default_config()
{
	GSList *list = NULL;

	if (g_file_test(RILMODEM_DEFAULT_SOCK, G_FILE_TEST_EXISTS)) {
		struct ril_slot *slot;

		if (g_file_test(RILMODEM_DEFAULT_SOCK2, G_FILE_TEST_EXISTS)) {
			DBG("Falling back to default 2-SIM config");

			slot = g_new0(struct ril_slot, 1);
			slot->path = g_strdup(RILCONF_PATH_PREFIX "0");
			slot->sockpath = g_strdup(RILMODEM_DEFAULT_SOCK);
			slot->name = g_strdup("RIL1");
			slot->config.enable_4g = RILMODEM_DEFAULT_4G;
			slot->timeout = RILMODEM_DEFAULT_TIMEOUT;
			slot->sim_flags = RILMODEM_DEFAULT_SIM_FLAGS;
			list = g_slist_append(list, slot);

			slot = g_new0(struct ril_slot, 1);
			slot->path = g_strdup(RILCONF_PATH_PREFIX "1");
			slot->sockpath = g_strdup(RILMODEM_DEFAULT_SOCK2);
			slot->name = g_strdup("RIL2");
			slot->config.enable_4g = RILMODEM_DEFAULT_4G;
			slot->timeout = RILMODEM_DEFAULT_TIMEOUT;
			slot->sim_flags = RILMODEM_DEFAULT_SIM_FLAGS;
			slot->config.slot = 1;
			list = g_slist_append(list, slot);
		} else {
			DBG("Falling back to default Jolla1 config");

			slot = g_new0(struct ril_slot, 1);
			slot->path = g_strdup(RILCONF_PATH_PREFIX "0");
			slot->sockpath = g_strdup(RILMODEM_DEFAULT_SOCK);
			slot->sub = g_strdup(RILMODEM_DEFAULT_SUB);
			slot->name = g_strdup("");
			slot->config.enable_4g = RILMODEM_DEFAULT_4G;
			slot->timeout = RILMODEM_DEFAULT_TIMEOUT;
			slot->sim_flags = RILMODEM_DEFAULT_SIM_FLAGS;
			list = g_slist_append(list, slot);
		}
	} else {
		DBG("No default config");
	}

	return list;
}

static void ril_plugin_read_config_flag(GKeyFile *file, const char *group,
					const char *key, int flag, int *flags)
{
	GError *err = NULL;

	if (g_key_file_get_boolean(file, group, key, &err)) {
		*flags |= flag;
	} else if (!err) {
		*flags &= ~flag;
	} else {
		g_error_free(err);
	}
}

static struct ril_slot *ril_plugin_parse_config_group(GKeyFile *file,
							const char *group)
{
	struct ril_slot *slot = NULL;
	char *sock = g_key_file_get_string(file, group, RILCONF_SOCKET, NULL);
	if (sock) {
		int value;
		GError *err = NULL;
		char *sub = g_key_file_get_string(file, group, RILCONF_SUB,
									NULL);

		slot = g_new0(struct ril_slot, 1);
		slot->sockpath = sock;
		slot->path = g_strconcat("/", group, NULL);
		slot->name = g_key_file_get_string(file, group, RILCONF_NAME,
									NULL);
		slot->sim_flags = RILMODEM_DEFAULT_SIM_FLAGS;

		if (sub && strlen(sub) == RIL_SUB_SIZE) {
			DBG("%s: %s:%s", group, sock, sub);
			slot->sub = sub;
		} else {
			DBG("%s: %s", group, sock);
			g_free(sub);
		}

		value = g_key_file_get_integer(file, group, RILCONF_SLOT, &err);
		if (!err && value >= 0) {
			slot->config.slot = value;
			DBG("%s: slot %u", group, slot->config.slot);
		} else {
			slot->config.slot = RILMODEM_DEFAULT_SLOT;
			if (err) {
				g_error_free(err);
				err = NULL;
			}
		}

		value = g_key_file_get_integer(file, group, RILCONF_TIMEOUT,
									&err);
		if (!err) {
			slot->timeout = value;
			DBG("%s: timeout %d", group, slot->timeout);
		} else {
			slot->timeout = RILMODEM_DEFAULT_TIMEOUT;
			if (err) {
				g_error_free(err);
				err = NULL;
			}
		}

		slot->config.enable_4g = g_key_file_get_boolean(file, group,
							RILCONF_4G, &err);
		if (err) {
			/* Set to default */
			slot->config.enable_4g = RILMODEM_DEFAULT_4G;
			g_error_free(err);
			err = NULL;
		}
		DBG("%s: 4G %s", group, slot->config.enable_4g ? "on" : "off");

		ril_plugin_read_config_flag(file, group,
			RILCONF_UICC_WORKAROUND,
			RIL_SIM_CARD_V9_UICC_SUBSCRIPTION_WORKAROUND,
			&slot->sim_flags);
		DBG("%s: UICC workaround %s", group, (slot->sim_flags &
				RIL_SIM_CARD_V9_UICC_SUBSCRIPTION_WORKAROUND) ?
								"on" : "off");
	} else {
		DBG("no socket path in %s", group);
	}

	return slot;
}

static void ril_plugin_delete_slot(struct ril_slot *slot)
{
	ril_plugin_shutdown_slot(slot, TRUE);
	ril_sim_info_unref(slot->sim_info);
	g_free(slot->path);
	g_free(slot->imei);
	g_free(slot->name);
	g_free(slot->sockpath);
	g_free(slot->sub);
	g_free(slot);
}

static GSList *ril_plugin_add_slot(GSList *slots, struct ril_slot *new_slot)
{
	GSList *link = slots;

	/* Slot numbers and paths must be unique */
	while (link) {
		GSList *next = link->next;
		struct ril_slot *slot = link->data;
		gboolean delete_this_slot = FALSE;

		if (!strcmp(slot->path, new_slot->path)) {
			ofono_error("Duplicate modem path '%s'", slot->path);
			delete_this_slot = TRUE;
		} else if (slot->config.slot != RILMODEM_DEFAULT_SLOT &&
				slot->config.slot == new_slot->config.slot) {
			ofono_error("Duplicate RIL slot %u", slot->config.slot);
			delete_this_slot = TRUE;
		}

		if (delete_this_slot) {
			slots = g_slist_delete_link(slots, link);
			ril_plugin_delete_slot(slot);
		}

		link = next;
	}

	return g_slist_append(slots, new_slot);
}

static guint ril_plugin_find_unused_slot(GSList *slots)
{
	guint number;

	for (number = 0; ril_plugin_find_slot_number(slots, number); number++);
	return number;
}

static GSList *ril_plugin_parse_config_file(GKeyFile *file)
{
	GSList *list = NULL;
	GSList *link;
	gsize i, n = 0;
	gchar **groups = g_key_file_get_groups(file, &n);

	for (i=0; i<n; i++) {
		if (g_str_has_prefix(groups[i], RILCONF_DEV_PREFIX)) {
			struct ril_slot *slot =
				ril_plugin_parse_config_group(file, groups[i]);

			if (slot) {
				list = ril_plugin_add_slot(list, slot);
			}
		}
	}

	/* Automatically assign slot numbers */
	link = list;
	while (link) {
		struct ril_slot *slot = link->data;
		if (slot->config.slot == RILMODEM_DEFAULT_SLOT) {
			slot->config.slot = ril_plugin_find_unused_slot(list);
		}
		link = link->next;
	}

	g_strfreev(groups);
	return list;
}

static GSList *ril_plugin_load_config(const char *path)
{
	GError *err = NULL;
	GSList *list = NULL;
	GKeyFile *file = g_key_file_new();

	if (g_key_file_load_from_file(file, path, 0, &err)) {
		DBG("loading %s", path);
		list = ril_plugin_parse_config_file(file);
	} else {
		DBG("conf load result: %s", err->message);
		g_error_free(err);
	}

	if (!list) {
		list = ril_plugin_create_default_config();
	}

	g_key_file_free(file);
	return list;
}

static void ril_plugin_destroy_slot(gpointer data)
{
	ril_plugin_delete_slot((struct ril_slot *)data);
}

/* RIL expects user radio */
static void ril_plugin_switch_user()
{
	if (prctl(PR_SET_KEEPCAPS, 1, 0, 0, 0) < 0) {
		ofono_error("prctl(PR_SET_KEEPCAPS) failed: %s",
							strerror(errno));
	} else if (setgid(RADIO_GID) < 0) {
		ofono_error("setgid(%d) failed: %s", RADIO_GID,
							strerror(errno));
	} else if (setuid(RADIO_UID) < 0) {
		ofono_error("setuid(%d) failed: %s", RADIO_UID,
							strerror(errno));
	} else {
		struct __user_cap_header_struct header;
		struct __user_cap_data_struct cap;

		memset(&header, 0, sizeof(header));
		memset(&cap, 0, sizeof(cap));

		header.version = _LINUX_CAPABILITY_VERSION;
		cap.effective = cap.permitted = (1 << CAP_NET_ADMIN) |
							(1 << CAP_NET_RAW);

		if (syscall(SYS_capset, &header, &cap) < 0) {
			ofono_error("syscall(SYS_capset) failed: %s",
							strerror(errno));
		}
	}
}

static void ril_plugin_update_enabled_slot(struct ril_slot *slot)
{
	if (slot->pub.enabled) {
		DBG("%s enabled", slot->path + 1);
		if (ril_plugin_can_create_modem(slot) && !slot->modem) {
			ril_plugin_create_modem(slot);
		}
	}
}

static void ril_plugin_update_disabled_slot(struct ril_slot *slot)
{
	if (!slot->pub.enabled) {
		DBG("%s disabled", slot->path + 1);
		ril_plugin_shutdown_slot(slot, FALSE);
		ril_plugin_update_modem_paths_full(slot->plugin);
	}
}

static void ril_plugin_update_slots(struct ril_plugin_priv *plugin)
{
	ril_plugin_foreach_slot(plugin, ril_plugin_update_disabled_slot);
	ril_plugin_foreach_slot(plugin, ril_plugin_update_enabled_slot);
	ril_plugin_update_modem_paths_full(plugin);
}

struct ril_plugin_set_enabled_slots_data {
	gchar * const * enabled;
	gboolean all_enabled;
	gboolean changed;
};

static void ril_plugin_enabled_slots_proc(gpointer data, gpointer user_data)
{
	struct ril_slot *slot = data;
	if (slot->pub.enabled) {
		char ***list = user_data;
		*list = gutil_strv_add(*list, slot->path);
	}
}

static void ril_plugin_set_enabled_slots_proc(gpointer data, gpointer user_data)
{
	struct ril_slot *slot = data;
	struct ril_plugin_set_enabled_slots_data *context = user_data;
	const gboolean was_enabled = slot->pub.enabled;

	slot->pub.enabled = gutil_strv_contains(context->enabled, slot->path);

	if ((was_enabled && !slot->pub.enabled) ||
				(!was_enabled && slot->pub.enabled)) {
		context->changed = TRUE;
	}

	if (!slot->pub.enabled) {
		context->all_enabled = FALSE;
	}
}

void ril_plugin_set_enabled_slots(struct ril_plugin *pub, gchar **slots)
{
	struct ril_plugin_priv *plugin = ril_plugin_cast(pub);
	struct ril_plugin_set_enabled_slots_data context;

	context.enabled = slots;
	context.changed = FALSE;
	context.all_enabled = TRUE;
	g_slist_foreach(plugin->slots, ril_plugin_set_enabled_slots_proc,
								&context);
	if (context.changed) {
		char **new_slots = NULL;

		g_slist_foreach(plugin->slots, ril_plugin_enabled_slots_proc,
								&new_slots);

		/* Save the new config value. If it exactly matches the list
		 * of available modems, delete the setting because that's the
		 * default behavior. */
		if (context.all_enabled) {
			ril_plugin_set_config_string(plugin,
					RIL_STORE_ENABLED_SLOTS, NULL, TRUE);
		} else {
			const char *value;
			char *tmp;

			if (new_slots) {
				tmp = g_strjoinv(RIL_STORE_SLOTS_SEP, new_slots);
				value = tmp;
			} else {
				tmp = NULL;
				value = "";
			}

			ril_plugin_set_config_string(plugin,
					RIL_STORE_ENABLED_SLOTS, value, TRUE);
			g_free(tmp);
		}
		g_strfreev(new_slots);
		ril_plugin_dbus_signal(plugin->dbus,
					RIL_PLUGIN_SIGNAL_ENABLED_SLOTS);

		/* Add and remove modems */
		ril_plugin_update_slots(plugin);
	}
}

void ril_plugin_set_default_voice_imsi(struct ril_plugin *pub, const char *imsi)
{
	struct ril_plugin_priv *plugin = ril_plugin_cast(pub);

	if (g_strcmp0(plugin->default_voice_imsi, imsi)) {
		DBG("Default voice sim set to %s", imsi ? imsi : "(auto)");
		g_free(plugin->default_voice_imsi);
		pub->default_voice_imsi =
		plugin->default_voice_imsi = g_strdup(imsi);
		ril_plugin_set_config_string(plugin, RIL_STORE_DEFAULT_VOICE_SIM,
								imsi, TRUE);
		ril_plugin_dbus_signal(plugin->dbus,
				RIL_PLUGIN_SIGNAL_VOICE_IMSI |
				ril_plugin_update_modem_paths(plugin));
	}
}

void ril_plugin_set_default_data_imsi(struct ril_plugin *pub, const char *imsi)
{
	struct ril_plugin_priv *plugin = ril_plugin_cast(pub);

	if (g_strcmp0(plugin->default_data_imsi, imsi)) {
		DBG("Default data sim set to %s", imsi ? imsi : "(auto)");
		g_free(plugin->default_data_imsi);
		pub->default_data_imsi =
		plugin->default_data_imsi = g_strdup(imsi);
		ril_plugin_set_config_string(plugin, RIL_STORE_DEFAULT_DATA_SIM,
								imsi, TRUE);
		ril_plugin_dbus_signal(plugin->dbus,
				RIL_PLUGIN_SIGNAL_DATA_IMSI |
				ril_plugin_update_modem_paths(plugin));
	}
}

gboolean ril_plugin_set_mms_imsi(struct ril_plugin *pub, const char *imsi)
{
	struct ril_plugin_priv *plugin = ril_plugin_cast(pub);

	if (imsi && imsi[0]) {
		if (g_strcmp0(plugin->mms_imsi, imsi)) {
			if (ril_plugin_find_slot_imsi(plugin->slots, imsi)) {
				DBG("MMS sim %s", imsi);
				g_free(plugin->mms_imsi);
				pub->mms_imsi = plugin->mms_imsi =
					g_strdup(imsi);
				ril_plugin_dbus_signal(plugin->dbus,
					RIL_PLUGIN_SIGNAL_MMS_IMSI |
					ril_plugin_update_modem_paths(plugin));
			} else {
				DBG("IMSI not found: %s", imsi);
				return FALSE;
			}
		}
	} else {
		if (plugin->mms_imsi) {
			DBG("No MMS sim");
			g_free(plugin->mms_imsi);
			pub->mms_imsi = plugin->mms_imsi = NULL;
			ril_plugin_dbus_signal(plugin->dbus,
					RIL_PLUGIN_SIGNAL_MMS_IMSI |
					ril_plugin_update_modem_paths(plugin));
		}
	}

	return TRUE;
}

static void ril_plugin_init_slots(struct ril_plugin_priv *plugin)
{
	int i;
	GSList *link;
	ril_slot_info_ptr *info = g_new0(ril_slot_info_ptr,
			g_slist_length(plugin->slots) + 1);

	plugin->pub.slots = plugin->slots_info = info;
	for (i = 0, link = plugin->slots; link; link = link->next, i++) {
		struct ril_slot *slot = link->data;

		*info++ = &slot->pub;
		slot->index = i;
		slot->plugin = plugin;
		slot->pub.path = slot->path;
		slot->pub.config = &slot->config;
		slot->sim_info = ril_sim_info_new(NULL);
	}

	*info = NULL;
}

static void ril_plugin_enable_disable_slot(gpointer data, gpointer user_data)
{
	struct ril_slot *slot = data;
	slot->pub.enabled = gutil_strv_contains(user_data, slot->path);
}

static void ril_plugin_enable_slot(struct ril_slot *slot)
{
	slot->pub.enabled = TRUE;
}

struct ril_plugin_priv *ril_plugin = NULL;

static void ril_debug_trace_notify(struct ofono_debug_desc *desc)
{
	if (ril_plugin) {
		ril_plugin_foreach_slot(ril_plugin, ril_debug_trace_update_slot);
	}
}

static void ril_debug_dump_notify(struct ofono_debug_desc *desc)
{
	if (ril_plugin) {
		ril_plugin_foreach_slot(ril_plugin, ril_debug_dump_update_slot);
	}
}

static void ril_debug_grilio_notify(struct ofono_debug_desc *desc)
{
	if (desc->flags & OFONO_DEBUG_FLAG_PRINT) {
		grilio_log.level = GLOG_LEVEL_VERBOSE;
	} else {
		grilio_log.level = GLOG_LEVEL_INHERIT;
	}
}

static int ril_plugin_init(void)
{
	char *enabled_slots;

	DBG("");
	GASSERT(!ril_plugin);

	/* ofono core calls openlog() */
	gutil_log_func = gutil_log_syslog;

	ril_plugin_switch_user();

	ril_plugin = g_new0(struct ril_plugin_priv, 1);
	ril_plugin->slots = ril_plugin_load_config(RILMODEM_CONF_FILE);
	ril_plugin_init_slots(ril_plugin);
	ril_plugin->dbus = ril_plugin_dbus_new(&ril_plugin->pub);
	ril_plugin->data_manager = ril_data_manager_new();

	if (ril_plugin->slots) {
		/*
		 * Since IMEI query is asynchronous, we need to hold IMEI
		 * related requests until all queries complete.
		 */
		ril_plugin_dbus_block_imei_requests(ril_plugin->dbus, TRUE);
	}

	/* Load settings */
	ril_plugin->storage = storage_open(NULL, RIL_STORE);
	enabled_slots = g_key_file_get_string(ril_plugin->storage,
				RIL_STORE_GROUP, RIL_STORE_ENABLED_SLOTS, NULL);
	if (enabled_slots) {
		char **strv = g_strsplit(enabled_slots, RIL_STORE_SLOTS_SEP, 0);

		DBG("Enabled slots: %s", enabled_slots);
		g_slist_foreach(ril_plugin->slots,
					ril_plugin_enable_disable_slot, strv);
		g_strfreev(strv);
		g_free(enabled_slots);
	} else {
		/* Let all slots be enabled by default */
		ril_plugin_foreach_slot(ril_plugin, ril_plugin_enable_slot);
	}

	ril_plugin->pub.default_voice_imsi =
	ril_plugin->default_voice_imsi =
		g_key_file_get_string(ril_plugin->storage, RIL_STORE_GROUP,
					RIL_STORE_DEFAULT_VOICE_SIM, NULL);
	ril_plugin->pub.default_data_imsi =
	ril_plugin->default_data_imsi =
		g_key_file_get_string(ril_plugin->storage, RIL_STORE_GROUP,
					RIL_STORE_DEFAULT_DATA_SIM, NULL);

	DBG("Default voice sim is %s",  ril_plugin->default_voice_imsi ?
				ril_plugin->default_voice_imsi : "(auto)");
	DBG("Default data sim is %s",  ril_plugin->default_data_imsi ?
				ril_plugin->default_data_imsi : "(auto)");

	ofono_modem_driver_register(&ril_modem_driver);
	ofono_sim_driver_register(&ril_sim_driver);
	ofono_sms_driver_register(&ril_sms_driver);
	ofono_netreg_driver_register(&ril_netreg_driver);
	ofono_devinfo_driver_register(&ril_devinfo_driver);
	ofono_voicecall_driver_register(&ril_voicecall_driver);
	ofono_call_barring_driver_register(&ril_call_barring_driver);
	ofono_call_forwarding_driver_register(&ril_call_forwarding_driver);
	ofono_call_settings_driver_register(&ril_call_settings_driver);
	ofono_call_volume_driver_register(&ril_call_volume_driver);
	ofono_radio_settings_driver_register(&ril_radio_settings_driver);
	ofono_gprs_driver_register(&ril_gprs_driver);
	ofono_gprs_context_driver_register(&ril_gprs_context_driver);
	ofono_phonebook_driver_register(&ril_phonebook_driver);
	ofono_ussd_driver_register(&ril_ussd_driver);
	ofono_cbs_driver_register(&ril_cbs_driver);
	ofono_oem_raw_driver_register(&ril_oem_raw_driver);
	ofono_stk_driver_register(&ril_stk_driver);

	/* This will create the modems (those that are enabled) */
	ril_plugin_update_slots(ril_plugin);

	/*
	 * Init RIL I/O for disabled slots as well so that we can receive
	 * SIM insertion/removal notifications
	 */
	ril_plugin_foreach_slot(ril_plugin, ril_plugin_init_io);
	return 0;
}

static void ril_plugin_exit(void)
{
	DBG("");
	GASSERT(ril_plugin);

	ofono_modem_driver_unregister(&ril_modem_driver);
	ofono_sim_driver_unregister(&ril_sim_driver);
	ofono_sms_driver_unregister(&ril_sms_driver);
	ofono_devinfo_driver_unregister(&ril_devinfo_driver);
	ofono_netreg_driver_unregister(&ril_netreg_driver);
	ofono_voicecall_driver_unregister(&ril_voicecall_driver);
	ofono_call_barring_driver_unregister(&ril_call_barring_driver);
	ofono_call_forwarding_driver_unregister(&ril_call_forwarding_driver);
	ofono_call_settings_driver_unregister(&ril_call_settings_driver);
	ofono_call_volume_driver_unregister(&ril_call_volume_driver);
	ofono_radio_settings_driver_unregister(&ril_radio_settings_driver);
	ofono_gprs_driver_unregister(&ril_gprs_driver);
	ofono_gprs_context_driver_unregister(&ril_gprs_context_driver);
	ofono_phonebook_driver_unregister(&ril_phonebook_driver);
	ofono_ussd_driver_unregister(&ril_ussd_driver);
	ofono_cbs_driver_unregister(&ril_cbs_driver);
	ofono_oem_raw_driver_unregister(&ril_oem_raw_driver);
	ofono_stk_driver_unregister(&ril_stk_driver);

	if (ril_plugin) {
		g_slist_free_full(ril_plugin->slots, ril_plugin_destroy_slot);
		ril_plugin_dbus_free(ril_plugin->dbus);
		ril_data_manager_unref(ril_plugin->data_manager);
		g_key_file_free(ril_plugin->storage);
		g_free(ril_plugin->slots_info);
		g_free(ril_plugin->default_voice_imsi);
		g_free(ril_plugin->default_data_imsi);
		g_free(ril_plugin->mms_imsi);
		g_free(ril_plugin);
		ril_plugin = NULL;
	}
}

OFONO_PLUGIN_DEFINE(ril, "RIL driver", VERSION,
	OFONO_PLUGIN_PRIORITY_DEFAULT, ril_plugin_init, ril_plugin_exit)

/*
 * Local Variables:
 * mode: C
 * c-basic-offset: 8
 * indent-tabs-mode: t
 * End:
 */
