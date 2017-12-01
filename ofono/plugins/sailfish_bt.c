/*
 *  oFono - Open Source Telephony
 *
 *  Copyright (C) 2017  Jolla Ltd. All rights reserved.
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

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <stdint.h>
#include <glib.h>
#include <ofono.h>
#include <gdbus.h>

#define OFONO_API_SUBJECT_TO_CHANGE
#include <ofono/plugin.h>
#include <ofono/log.h>

#define SFOS_BT_DBUS_CV_INTERFACE "org.nemomobile.ofono.bluetooth.CallVolume"
#define HFP_CALL_VOLUME_MAX 15

struct sfos_bt {
	unsigned int emu_watch;
	struct ofono_modem *modem;
	GSList* ems;
	unsigned char speaker_volume;
	unsigned char microphone_volume;
};

static GSList *modems;
static guint modemwatch_id;

static void sfos_bt_send_unsolicited(struct sfos_bt *bt,
				const char *format, ...) G_GNUC_PRINTF(2, 3);

static void sfos_bt_send_unsolicited(struct sfos_bt *bt,
				const char *format, ...)
{
	if (bt->ems) {
		GSList *l;
		GString* buf = g_string_sized_new(15);

		va_list va;
		va_start(va, format);
		g_string_vprintf(buf, format, va);
		va_end(va);

		for (l = bt->ems; l; l = l->next) {
			ofono_emulator_send_unsolicited(l->data, buf->str);
		}

		g_string_free(buf, TRUE);
	}
}

static void set_hfp_microphone_volume(struct sfos_bt *sfos_bt,
					unsigned char gain)
{
	sfos_bt_send_unsolicited(sfos_bt, "+VGM:%d", (int) gain);
}

static void set_hfp_speaker_volume(struct sfos_bt *sfos_bt,
					unsigned char gain)
{
	sfos_bt_send_unsolicited(sfos_bt, "+VGS:%d", (int) gain);
}

static DBusMessage *cv_set_property(DBusConnection *conn, DBusMessage *msg,
					void *data)
{
	struct sfos_bt *sfos_bt = data;
	DBusMessageIter iter;
	DBusMessageIter var;
	const char *property;

	if (!dbus_message_iter_init(msg, &iter))
		return __ofono_error_invalid_args(msg);

	if (dbus_message_iter_get_arg_type(&iter) != DBUS_TYPE_STRING)
		return __ofono_error_invalid_args(msg);

	dbus_message_iter_get_basic(&iter, &property);
	dbus_message_iter_next(&iter);

	if (dbus_message_iter_get_arg_type(&iter) != DBUS_TYPE_VARIANT)
		return __ofono_error_invalid_args(msg);

	dbus_message_iter_recurse(&iter, &var);

	if (g_str_equal(property, "SpeakerVolume") == TRUE) {
		unsigned char gain;

		if (dbus_message_iter_get_arg_type(&var) != DBUS_TYPE_BYTE)
			return __ofono_error_invalid_args(msg);

		dbus_message_iter_get_basic(&var, &gain);

		if (gain > HFP_CALL_VOLUME_MAX)
			return __ofono_error_invalid_format(msg);

		if (gain == sfos_bt->speaker_volume)
			return dbus_message_new_method_return(msg);

		DBG("SpeakerVolume:%d", gain);
		sfos_bt->speaker_volume = gain;
		set_hfp_speaker_volume(sfos_bt, gain);

		return dbus_message_new_method_return(msg);

	} else if (g_str_equal(property, "MicrophoneVolume") == TRUE) {
		unsigned char gain;

		if (dbus_message_iter_get_arg_type(&var) != DBUS_TYPE_BYTE)
			return __ofono_error_invalid_args(msg);

		dbus_message_iter_get_basic(&var, &gain);

		if (gain > HFP_CALL_VOLUME_MAX)
			return __ofono_error_invalid_format(msg);

		if (gain == sfos_bt->microphone_volume)
			return dbus_message_new_method_return(msg);

		DBG("MicrophoneVolume:%d", gain);
		sfos_bt->microphone_volume = gain;
		set_hfp_microphone_volume(sfos_bt, gain);

		return dbus_message_new_method_return(msg);

	} else if (g_str_equal(property, "Muted") == TRUE) {
		unsigned char gain;
		dbus_bool_t muted;

		/*Remove when supported*/
		return __ofono_error_not_implemented(msg);

		if (dbus_message_iter_get_arg_type(&var) != DBUS_TYPE_BOOLEAN)
			return __ofono_error_invalid_args(msg);

		dbus_message_iter_get_basic(&var, &muted);

		if (muted)
			gain = 0;
		else
			gain = 7;/* rather gain = sfos->old_mic_vol */

		if (gain == sfos_bt->microphone_volume)
			return dbus_message_new_method_return(msg);

		sfos_bt->microphone_volume = gain;
		set_hfp_microphone_volume(sfos_bt, gain);

		return dbus_message_new_method_return(msg);
	}

	return __ofono_error_invalid_args(msg);
}

static const GDBusMethodTable cv_methods[] = {
	{ GDBUS_METHOD("SetProperty",
			GDBUS_ARGS({ "property", "s" }, { "value", "v" }),
			NULL, cv_set_property) },
	{ }
};

static const GDBusSignalTable cv_signals[] = {
	{ GDBUS_SIGNAL("PropertyChanged",
			GDBUS_ARGS({ "property", "s" }, { "value", "v" })) },
	{ }
};

static int sfos_bt_call_volume_set(struct ofono_modem *modem,
				unsigned char volume, const char *gain)
{
	DBusConnection *conn = ofono_dbus_get_connection();
	const char *path = ofono_modem_get_path(modem);

	return ofono_dbus_signal_property_changed(conn, path,
						SFOS_BT_DBUS_CV_INTERFACE,
						gain,
						DBUS_TYPE_BYTE, &volume);
}

static void set_gain(struct ofono_emulator *em,
			struct ofono_emulator_request *req,
			void *userdata, const char *gain)
{
	struct sfos_bt *sfos_bt = userdata;
	struct ofono_modem *modem = sfos_bt->modem;
	struct ofono_error result;
	unsigned char volume;
	int val;
	result.error = 0;

	switch (ofono_emulator_request_get_type(req)) {
	case OFONO_EMULATOR_REQUEST_TYPE_SET:
		if (ofono_emulator_request_next_number(req, &val) == FALSE)
			goto fail;

		if (val < 0 || val > 0xffff || val > HFP_CALL_VOLUME_MAX)
			goto fail;

		DBG("gain:%d", val);

		volume = (unsigned char) val;
		if (sfos_bt_call_volume_set(modem, volume, gain)<= 0)
			goto fail;

		if (!g_strcmp0(gain, "SpeakerVolume"))
			sfos_bt->speaker_volume = volume;
		else
			sfos_bt->microphone_volume = volume;

		result.type = OFONO_ERROR_TYPE_NO_ERROR;
		ofono_emulator_send_final(em, &result);
		break;

	default:
fail:
		result.type = OFONO_ERROR_TYPE_FAILURE;
		ofono_emulator_send_final(em, &result);
		break;
	}
}

static void sfos_bt_vgm_cb(struct ofono_emulator *em,
			struct ofono_emulator_request *req, void *userdata)
{
	const char *gain = "MicrophoneVolume";
	set_gain(em, req, userdata, gain);
}

static void sfos_bt_vgs_cb(struct ofono_emulator *em,
			struct ofono_emulator_request *req, void *userdata)
{
	const char *gain = "SpeakerVolume";
	set_gain(em, req, userdata, gain);
}

static void sfos_bt_cv_dbus_new(struct sfos_bt *sfos_bt)
{
	DBusConnection *conn = ofono_dbus_get_connection();
	struct ofono_modem *modem = sfos_bt->modem;
	const char *path = ofono_modem_get_path(modem);

	if (g_dbus_register_interface(conn, path,
			SFOS_BT_DBUS_CV_INTERFACE, cv_methods,
			cv_signals, NULL, sfos_bt, NULL)){
		ofono_modem_add_interface(modem,SFOS_BT_DBUS_CV_INTERFACE);
		return;
	}

	ofono_error("D-Bus register failed");
}

static void sfos_bt_remove_handler(struct ofono_emulator *em)
{
	ofono_emulator_remove_handler(em, "+VGS");
	ofono_emulator_remove_handler(em, "+VGM");
}

static void sfos_bt_cv_dbus_free(struct sfos_bt *sfos_bt)
{
	DBusConnection *conn = ofono_dbus_get_connection();
	struct ofono_modem *modem = sfos_bt->modem;
	const char *path = ofono_modem_get_path(modem);
	ofono_modem_remove_interface(modem, SFOS_BT_DBUS_CV_INTERFACE);
	g_dbus_unregister_interface(conn, path,
					SFOS_BT_DBUS_CV_INTERFACE);
}

static void sfos_bt_emu_watch_cb(struct ofono_atom *atom,
				enum ofono_atom_watch_condition cond,
				void *data)
{
	struct sfos_bt *bt = data;
	struct ofono_emulator *em = __ofono_atom_get_data(atom);

	if (cond == OFONO_ATOM_WATCH_CONDITION_REGISTERED){
		if (!bt->ems)
			sfos_bt_cv_dbus_new(bt);

		bt->ems = g_slist_append(bt->ems, em);
		ofono_emulator_add_handler(em, "+VGS", sfos_bt_vgs_cb, bt,
									NULL);
		ofono_emulator_add_handler(em, "+VGM", sfos_bt_vgm_cb, bt,
									NULL);
	} else {
		sfos_bt_remove_handler(em);
		bt->ems = g_slist_remove(bt->ems, em);

		if (!bt->ems)
			sfos_bt_cv_dbus_free(bt);
	}
}

static void sfos_bt_emu_watch_destroy(void *data)
{
	struct sfos_bt *sfos_bt = data;

	sfos_bt->emu_watch = 0;
}

static void sfos_bt_free_em(gpointer data)
{
	sfos_bt_remove_handler((struct ofono_emulator*)data);
}

static void sfos_bt_free(void *data)
{
	struct sfos_bt *bt = data;

	if (bt->emu_watch)
		__ofono_modem_remove_atom_watch(bt->modem, bt->emu_watch);

	if (bt->ems) {
		sfos_bt_cv_dbus_free(bt);
		g_slist_free_full(bt->ems, sfos_bt_free_em);
		bt->ems = NULL;
	}

	g_free(bt);
}

static gint sfos_bt_find_modem(gconstpointer listdata, gconstpointer modem)
{
	const struct sfos_bt *sfos_bt = listdata;

	return (sfos_bt->modem != modem);
}

static void modem_watch(struct ofono_modem *modem, gboolean added, void *user)
{
	struct sfos_bt *sfos_bt;
	DBG("modem: %p, added: %d", modem, added);

	if (added) {
		sfos_bt = g_new0(struct sfos_bt, 1);
		modems = g_slist_append(modems, sfos_bt);
		sfos_bt->emu_watch = __ofono_modem_add_atom_watch(modem,
			OFONO_ATOM_TYPE_EMULATOR_HFP, sfos_bt_emu_watch_cb,
			sfos_bt, sfos_bt_emu_watch_destroy);
		sfos_bt->modem = modem;
	} else {
		GSList *link = g_slist_find_custom(modems, modem,
						sfos_bt_find_modem);
		if (link) {
			sfos_bt_free(link->data);
			modems = g_slist_delete_link(modems, link);
		}
	}
}

static void call_modemwatch(struct ofono_modem *modem, void *user)
{
	modem_watch(modem, TRUE, user);
}

static int sfos_bt_init(void)
{
	modemwatch_id = __ofono_modemwatch_add(modem_watch, NULL, NULL);
	__ofono_modem_foreach(call_modemwatch, NULL);

	return 0;
}

static void sfos_bt_exit(void)
{
	DBG("");
	__ofono_modemwatch_remove(modemwatch_id);
	g_slist_free_full(modems, sfos_bt_free);
}

OFONO_PLUGIN_DEFINE(sfos_bt, "Sailfish OS Bluetooth Plugin", VERSION,
			OFONO_PLUGIN_PRIORITY_DEFAULT,
			sfos_bt_init, sfos_bt_exit)
