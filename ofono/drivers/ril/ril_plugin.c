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

#include "ril_plugin.h"
#include "ril_mce.h"
#include "ril_constants.h"
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

#define RILMODEM_CONF_FILE          "/etc/ofono/ril_subscription.conf"
#define RILMODEM_DEFAULT_SOCK       "/dev/socket/rild"
#define RILMODEM_DEFAULT_SOCK2      "/dev/socket/rild2"
#define RILMODEM_DEFAULT_SUB        "SUB1"
#define RILMODEM_DEFAULT_4G         TRUE /* 4G is on by default */
#define RILMODEM_DEFAULT_SLOT       0xffffffff
#define RILMODEM_DEFAULT_TIMEOUT    0 /* No timeout */

#define RILCONF_DEV_PREFIX          "ril_"
#define RILCONF_PATH_PREFIX         "/" RILCONF_DEV_PREFIX
#define RILCONF_NAME                "name"
#define RILCONF_SOCKET              "socket"
#define RILCONF_SLOT                "slot"
#define RILCONF_SUB                 "sub"
#define RILCONF_TIMEOUT             "timeout"
#define RILCONF_4G                  "enable4G"

#define RIL_STORE                   "ril"
#define RIL_STORE_GROUP             "Settings"
#define RIL_STORE_ENABLED_SLOTS     "EnabledSlots"
#define RIL_STORE_DEFAULT_VOICE_SIM "DefaultVoiceSim"
#define RIL_STORE_DEFAULT_DATA_SIM  "DefaultDataSim"
#define RIL_STORE_SLOTS_SEP         ","

struct ril_plugin_priv {
	struct ril_plugin pub;
	struct ril_plugin_dbus *dbus;
	GSList *slots;
	struct ril_modem *data_modem;
	char *default_voice_imsi;
	char *default_data_imsi;
	char *default_voice_path;
	char *default_data_path;
	GKeyFile *storage;
};

struct ril_slot {
	char *path;
	char *name;
	char *sockpath;
	char *sub;
	gint timeout;           /* RIL timeout, in seconds */
	struct ril_modem_config config;

	struct ril_plugin_priv *plugin;
	struct ril_sim_dbus *sim_dbus;
	struct ril_modem *modem;
	struct ril_mce *mce;
	struct ofono_sim *sim;
	GRilIoChannel *io;
	guint trace_id;
	guint dump_id;
	guint error_id;
	guint eof_id;
	guint retry_id;
	guint sim_watch_id;
	guint sim_state_watch_id;
	enum ofono_sim_state sim_state;
};

static void ril_debug_trace_notify(struct ofono_debug_desc *desc);
static void ril_debug_dump_notify(struct ofono_debug_desc *desc);
static void ril_plugin_create_modem(struct ril_slot *slot);

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

static inline struct ril_plugin_priv *ril_plugin_cast(struct ril_plugin *pub)
{
	return G_CAST(pub, struct ril_plugin_priv, pub);
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

static gboolean ril_plugin_retry(gpointer data)
{
	struct ril_slot *slot = data;

	slot->retry_id = 0;
	ril_plugin_create_modem(slot);
	return FALSE;
}

static void ril_plugin_shutdown_slot(struct ril_slot *slot, gboolean kill_io)
{
	if (slot->sim) {
		if (slot->sim_state_watch_id) {
			ofono_sim_remove_state_watch(slot->sim,
						slot->sim_state_watch_id);
		}
		slot->sim = NULL;
	}

	if (slot->modem) {
		struct ofono_modem *m = ril_modem_ofono_modem(slot->modem);

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

		if (slot->io) {
			grilio_channel_remove_logger(slot->io, slot->trace_id);
			grilio_channel_remove_logger(slot->io, slot->dump_id);
			slot->trace_id = 0;
			slot->dump_id = 0;

			grilio_channel_remove_handler(slot->io, slot->error_id);
			grilio_channel_remove_handler(slot->io, slot->eof_id);
			slot->error_id = 0;
			slot->eof_id = 0;

			grilio_channel_shutdown(slot->io, FALSE);
			grilio_channel_unref(slot->io);
			slot->io = NULL;
		}
	}
}

static void ril_pligin_set_config_string(struct ril_plugin_priv *plugin,
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
	struct ril_slot *voice = ril_plugin_find_slot_imsi(plugin->slots,
						plugin->default_voice_imsi);
	struct ril_slot *data = ril_plugin_find_slot_imsi(plugin->slots,
						plugin->default_data_imsi);

	if (!voice) {
		/* If there's no default voice SIM, find any SIM instead.
		 * One should always be able to make and receive a phone call
		 * if there's a working SIM in the phone. However if the
		 * previously selected voice SIM is inserted, we will switch
		 * back to it. */
		voice = ril_plugin_find_slot_imsi(plugin->slots, NULL);
	}

	if (voice) {
		if (g_strcmp0(plugin->default_voice_path, voice->path)) {
			DBG("Default voice SIM at %s", voice->path);
			g_free(plugin->default_voice_path);
			plugin->default_voice_path = g_strdup(voice->path);
			mask |= RIL_PLUGIN_SIGNAL_VOICE_PATH;
		}
	} else if (plugin->default_voice_path) {
		DBG("No default voice SIM");
		g_free(plugin->default_voice_path);
		plugin->default_voice_path = NULL;
		mask |= RIL_PLUGIN_SIGNAL_VOICE_PATH;
	}

	if (data) {
		if (g_strcmp0(plugin->default_data_path, data->path)) {
			DBG("Default data SIM at %s", data->path);
			g_free(plugin->default_data_path);
			plugin->default_data_path = g_strdup(data->path);
			mask |= RIL_PLUGIN_SIGNAL_DATA_PATH;
		}
		if (plugin->data_modem != data->modem) {
			plugin->data_modem = data->modem;
			ril_modem_allow_data(data->modem);
		}
	} else if (plugin->default_data_path) {
		DBG("No default data SIM");
		g_free(plugin->default_data_path);
		plugin->default_data_path = NULL;
		mask |= RIL_PLUGIN_SIGNAL_DATA_PATH;
	}

	plugin->pub.default_voice_path = plugin->default_voice_path;
	plugin->pub.default_data_path = plugin->default_data_path;
	return mask;
}

static void ril_plugin_check_sim_state(struct ril_slot *slot)
{
	const char *slot_imsi = ofono_sim_get_imsi(slot->sim);
	const char *dbus_imsi = ril_sim_dbus_imsi(slot->sim_dbus);

	if (!slot_imsi) {
		if (slot->sim_dbus) {
			ril_sim_dbus_free(slot->sim_dbus);
			slot->sim_dbus = NULL;
		}
	} else if (g_strcmp0(slot_imsi, dbus_imsi)) {
		ril_sim_dbus_free(slot->sim_dbus);
		slot->sim_dbus = ril_sim_dbus_new(slot->modem);
	}
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

	DBG("%s sim state %d", slot->path + 1, new_state);
	slot->sim_state = new_state;
	ril_plugin_check_sim_state(slot);
	ril_plugin_dbus_signal(slot->plugin->dbus,
				ril_plugin_update_modem_paths(slot->plugin));
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
		slot->sim = NULL;
	}

	ril_plugin_check_sim_state(slot);
	ril_plugin_dbus_signal(slot->plugin->dbus,
				ril_plugin_update_modem_paths(slot->plugin));
}

static void ril_plugin_handle_error(struct ril_slot *slot)
{
	ril_plugin_shutdown_slot(slot, TRUE);
	GASSERT(!slot->retry_id);
	slot->retry_id = g_timeout_add_seconds(RIL_RETRY_SECS,
						ril_plugin_retry, slot);
}

static void ril_plugin_error(GRilIoChannel *io, const GError *error, void *data)
{
	ril_plugin_handle_error((struct ril_slot *)data);
}

static void ril_plugin_disconnect(GRilIoChannel *io, void *data)
{
	ril_plugin_handle_error((struct ril_slot *)data);
}

static void ril_plugin_modem_error(struct ril_modem *modem, void *data)
{
	ril_plugin_handle_error((struct ril_slot *)data);
}

static void ril_plugin_modem_removed(struct ril_modem *modem, void *data)
{
	struct ril_slot *slot = data;

	DBG("");
	GASSERT(slot->modem);
	GASSERT(slot->modem == modem);

	if (slot->sim_dbus) {
		ril_sim_dbus_free(slot->sim_dbus);
		slot->sim_dbus = NULL;
	}

	slot->modem = NULL;
	if (slot->plugin->data_modem == modem) {
		slot->plugin->data_modem = NULL;
	}
}

static void ril_plugin_trace(GRilIoChannel *io, GRILIO_PACKET_TYPE type,
	guint id, guint code, const void *data, guint data_len, void *user_data)
{
	/* Use log sub-module to turn prefix off */
	static GLOG_MODULE_DEFINE2_(log_module, NULL, GLOG_MODULE_NAME);
        const char *prefix = io->name ? io->name : "";
        const char dir = (type == GRILIO_PACKET_REQ) ? '<' : '>';
	switch (type) {
	case GRILIO_PACKET_REQ:
		gutil_log(&log_module, GLOG_LEVEL_INFO, "%s%c [%08x] %s",
				prefix, dir, id, ril_request_to_string(code));
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

static void ril_plugin_create_modem(struct ril_slot *slot)
{
	DBG("%s %s", slot->sockpath, slot->sub);
	if (!slot->io) {
		slot->io = grilio_channel_new_socket(slot->sockpath, slot->sub);
		if (slot->io) {
			ril_debug_trace_update_slot(slot);
			ril_debug_dump_update_slot(slot);

			if (slot->name) {
				grilio_channel_set_name(slot->io, slot->name);
			}

			grilio_channel_set_timeout(slot->io, slot->timeout);
			slot->error_id =
				grilio_channel_add_error_handler(slot->io,
						ril_plugin_error, slot);
			slot->eof_id =
				grilio_channel_add_disconnected_handler(slot->io,
						ril_plugin_disconnect, slot);
		}
	}

	if (slot->io) {
		GASSERT(!slot->modem);
		slot->modem = ril_modem_create(slot->io, slot->path + 1,
						&slot->config);
		if (slot->modem) {
			struct ofono_sim *sim = ril_modem_ofono_sim(slot->modem);

			slot->sim_watch_id = __ofono_modem_add_atom_watch(
				ril_modem_ofono_modem(slot->modem),
				OFONO_ATOM_TYPE_SIM, ril_plugin_sim_watch,
				slot, ril_plugin_sim_watch_done);
			if (sim) {
				ril_plugin_register_sim(slot, sim);
			}
			ril_modem_set_error_cb(slot->modem,
					ril_plugin_modem_error, slot);
			ril_modem_set_removed_cb(slot->modem,
					ril_plugin_modem_removed, slot);
			if (!slot->mce) {
				GASSERT(!slot->mce);
				slot->mce = ril_mce_new(slot->io);
			}
		} else {
			ril_plugin_shutdown_slot(slot, TRUE);
		}
	}

	if (!slot->modem) {
		GASSERT(!slot->retry_id);
		slot->retry_id = g_timeout_add_seconds(RIL_RETRY_SECS,
						ril_plugin_retry, slot);
	}
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
			list = g_slist_append(list, slot);

			slot = g_new0(struct ril_slot, 1);
			slot->path = g_strdup(RILCONF_PATH_PREFIX "1");
			slot->sockpath = g_strdup(RILMODEM_DEFAULT_SOCK2);
			slot->name = g_strdup("RIL2");
			slot->config.enable_4g = RILMODEM_DEFAULT_4G;
			slot->timeout = RILMODEM_DEFAULT_TIMEOUT;
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
			list = g_slist_append(list, slot);
		}
	} else {
		DBG("No default config");
	}

	return list;
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
	} else {
		DBG("no socket path in %s", group);
	}

	return slot;
}

static void ril_plugin_delete_slot(struct ril_slot *slot)
{
	ril_plugin_shutdown_slot(slot, TRUE);
	g_free(slot->path);
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

static void ril_plugin_init_slot(gpointer data, gpointer user_data)
{
	struct ril_plugin_priv *plugin = user_data;
	struct ril_slot *slot = data;

	slot->plugin = plugin;
	GASSERT(!gutil_strv_contains(plugin->pub.available_slots, slot->path));
	plugin->pub.available_slots = gutil_strv_add(plugin->pub.available_slots,
							slot->path);
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

/* Removes unavailable and duplicate slots from the string array,
 * reorders them to match the order of available slots */
static char **ril_plugin_clean_slots(struct ril_plugin *plugin, char **slots)
{
	guint i;
	const guint len1 = gutil_strv_length(plugin->available_slots);
	const guint len2 = gutil_strv_length(slots);
	const guint len = MIN(len1, len2);
	char **clean_slots = g_new0(char*, len + 1);
	char **ptr = clean_slots;

	*ptr = NULL;
	for (i = 0; i < len1; i++) {
		const char *slot = plugin->available_slots[i];
		if (gutil_strv_contains(slots, slot)) {
			GASSERT(!gutil_strv_contains(clean_slots, slot));
			*ptr++ = g_strdup(slot);
			*ptr = NULL;
		}
	}

	GASSERT(gutil_strv_length(clean_slots) <= len);
	return clean_slots;
}

static gboolean ril_plugin_slot_enabled(struct ril_slot *slot)
{
	char **enabled = slot->plugin->pub.enabled_slots;

	if (enabled) {
		while (*enabled) {
			if (!strcmp(*enabled, slot->path)) {
				return TRUE;
			}
			enabled++;
		}
	}

	return FALSE;
}

static void ril_plugin_update_enabled_slot(struct ril_slot *slot)
{
	if (ril_plugin_slot_enabled(slot)) {
		DBG("%s enabled", slot->path + 1);
		if (!slot->modem) {
			ril_plugin_create_modem(slot);
		}
	}
}

static void ril_plugin_update_disabled_slot(struct ril_slot *slot)
{
	if (!ril_plugin_slot_enabled(slot)) {
		DBG("%s disabled", slot->path + 1);
		ril_plugin_shutdown_slot(slot, FALSE);
	}
}

static void ril_plugin_update_slots(struct ril_plugin_priv *plugin)
{
	ril_plugin_foreach_slot(plugin, ril_plugin_update_disabled_slot);
	ril_plugin_foreach_slot(plugin, ril_plugin_update_enabled_slot);
	ril_plugin_dbus_signal(plugin->dbus,
				ril_plugin_update_modem_paths(plugin));
}

void ril_plugin_set_enabled_slots(struct ril_plugin *pub, gchar **slots)
{
	char **new_slots = ril_plugin_clean_slots(pub, slots);

	if (!gutil_strv_equal(pub->enabled_slots, new_slots)) {
		struct ril_plugin_priv *plugin = ril_plugin_cast(pub);

		/* Save the new config value. If it exactly matches the list
		 * of available modems, delete the setting because that's the
		 * default behavior. */
		if (gutil_strv_equal(pub->enabled_slots, new_slots)) {
			ril_pligin_set_config_string(plugin,
					RIL_STORE_ENABLED_SLOTS, NULL, TRUE);
		} else {
			char *value = g_strjoinv(RIL_STORE_SLOTS_SEP, new_slots);
			ril_pligin_set_config_string(plugin,
					RIL_STORE_ENABLED_SLOTS, value, TRUE);
			g_free(value);
		}
		g_strfreev(pub->enabled_slots);
		pub->enabled_slots = new_slots;
		ril_plugin_dbus_signal(plugin->dbus,
					RIL_PLUGIN_SIGNAL_ENABLED_SLOTS);

		/* Add and remove modems */
		ril_plugin_update_slots(plugin);
	} else {
		g_strfreev(new_slots);
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
		ril_pligin_set_config_string(plugin, RIL_STORE_DEFAULT_VOICE_SIM,
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
		ril_pligin_set_config_string(plugin, RIL_STORE_DEFAULT_DATA_SIM,
								imsi, TRUE);
		ril_plugin_dbus_signal(plugin->dbus,
				RIL_PLUGIN_SIGNAL_DATA_IMSI |
				ril_plugin_update_modem_paths(plugin));
	}
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

static int ril_plugin_init(void)
{
	char *enabled_slots;

	DBG("");
	GASSERT(!ril_plugin);

	gutil_log_func = gutil_log_syslog;

	ril_plugin_switch_user();

	ril_plugin = g_new0(struct ril_plugin_priv, 1);
	ril_plugin->slots = ril_plugin_load_config(RILMODEM_CONF_FILE);
	g_slist_foreach(ril_plugin->slots, ril_plugin_init_slot, ril_plugin);
	ril_plugin->dbus = ril_plugin_dbus_new(&ril_plugin->pub);

	/* Load settings */
	ril_plugin->storage = storage_open(NULL, RIL_STORE);
	enabled_slots = g_key_file_get_string(ril_plugin->storage,
				RIL_STORE_GROUP, RIL_STORE_ENABLED_SLOTS, NULL);
	if (enabled_slots) {
		char **strv = g_strsplit(enabled_slots, RIL_STORE_SLOTS_SEP, 0);

		DBG("Enabled slots: %s", enabled_slots);
		ril_plugin->pub.enabled_slots =
			ril_plugin_clean_slots(&ril_plugin->pub, strv);
		g_strfreev(strv);
		g_free(enabled_slots);
	} else {
		/* Let all slots be enabled by default */
		ril_plugin->pub.enabled_slots =
			g_strdupv(ril_plugin->pub.available_slots);
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
		g_key_file_free(ril_plugin->storage);
		g_strfreev(ril_plugin->pub.available_slots);
		g_strfreev(ril_plugin->pub.enabled_slots);
		g_free(ril_plugin->default_voice_imsi);
		g_free(ril_plugin->default_data_imsi);
		g_free(ril_plugin->default_voice_path);
		g_free(ril_plugin->default_data_path);
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
