/*
 *
 *  oFono - Open Source Telephony
 *
 *  Copyright (C) 2017 Vincent Cesson. All rights reserved.
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License version 2 as
 *  published by the Free Software Foundation.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
 *
 */

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <errno.h>
#include <stdlib.h>
#include <stdint.h>

#include <glib.h>
#include <gatchat.h>
#include <gattty.h>
#include <gdbus.h>

#include "ofono.h"

#define OFONO_API_SUBJECT_TO_CHANGE
#include <ofono/dbus.h>
#include <ofono/plugin.h>
#include <ofono/log.h>
#include <ofono/modem.h>
#include <ofono/devinfo.h>
#include <ofono/netreg.h>
#include <ofono/phonebook.h>
#include <ofono/sim.h>
#include <ofono/sms.h>
#include <ofono/gprs.h>
#include <ofono/gprs-context.h>
#include <ofono/location-reporting.h>

#include <drivers/atmodem/atutil.h>
#include <drivers/atmodem/vendor.h>

#define HARDWARE_MONITOR_INTERFACE OFONO_SERVICE ".cinterion.HardwareMonitor"

static const char *none_prefix[] = { NULL };
static const char *sctm_prefix[] = { "^SCTM:", NULL };
static const char *sbv_prefix[] = { "^SBV:", NULL };

struct gemalto_hardware_monitor {
	DBusMessage *msg;
	int32_t temperature;
	int32_t voltage;
};

struct gemalto_data {
	GAtChat *app;
	GAtChat *mdm;
	struct ofono_sim *sim;
	gboolean have_sim;
	struct at_util_sim_state_query *sim_state_query;
	struct gemalto_hardware_monitor *hm;
};

static int gemalto_probe(struct ofono_modem *modem)
{
	struct gemalto_data *data;

	data = g_try_new0(struct gemalto_data, 1);
	if (data == NULL)
		return -ENOMEM;

	ofono_modem_set_data(modem, data);

	return 0;
}

static void gemalto_remove(struct ofono_modem *modem)
{
	struct gemalto_data *data = ofono_modem_get_data(modem);

	/* Cleanup potential SIM state polling */
	at_util_sim_state_query_free(data->sim_state_query);
	ofono_modem_set_data(modem, NULL);
	g_free(data);
}

static void gemalto_debug(const char *str, void *user_data)
{
	const char *prefix = user_data;

	ofono_info("%s%s", prefix, str);
}

static GAtChat *open_device(const char *device)
{
	GAtSyntax *syntax;
	GIOChannel *channel;
	GAtChat *chat;

	DBG("Opening device %s", device);

	channel = g_at_tty_open(device, NULL);
	if (channel == NULL)
		return NULL;

	syntax = g_at_syntax_new_gsm_permissive();
	chat = g_at_chat_new(channel, syntax);
	g_at_syntax_unref(syntax);
	g_io_channel_unref(channel);

	if (chat == NULL)
		return NULL;

	return chat;
}

static void sim_state_cb(gboolean present, gpointer user_data)
{
	struct ofono_modem *modem = user_data;
	struct gemalto_data *data = ofono_modem_get_data(modem);

	at_util_sim_state_query_free(data->sim_state_query);
	data->sim_state_query = NULL;

	data->have_sim = present;
	ofono_modem_set_powered(modem, TRUE);
}

static void cfun_enable(gboolean ok, GAtResult *result, gpointer user_data)
{
	struct ofono_modem *modem = user_data;
	struct gemalto_data *data = ofono_modem_get_data(modem);

	if (!ok) {
		g_at_chat_unref(data->app);
		data->app = NULL;

		g_at_chat_unref(data->mdm);
		data->mdm = NULL;

		ofono_modem_set_powered(modem, FALSE);
		return;
	}

	data->sim_state_query = at_util_sim_state_query_new(data->app,
						2, 20, sim_state_cb, modem,
						NULL);
}

static void gemalto_sctm_cb(gboolean ok, GAtResult *result, gpointer user_data)
{
	struct gemalto_data *data = user_data;
	DBusMessage *reply;
	GAtResultIter iter;
	DBusMessageIter dbus_iter;
	DBusMessageIter dbus_dict;

	if (data->hm->msg == NULL)
		return;

	if (!ok)
		goto error;

	g_at_result_iter_init(&iter, result);

	if (!g_at_result_iter_next(&iter, "^SCTM:"))
		goto error;

	if (!g_at_result_iter_skip_next(&iter))
		goto error;

	if (!g_at_result_iter_skip_next(&iter))
		goto error;

	if (!g_at_result_iter_next_number(&iter, &data->hm->temperature))
		goto error;

	reply = dbus_message_new_method_return(data->hm->msg);

	dbus_message_iter_init_append(reply, &dbus_iter);

	dbus_message_iter_open_container(&dbus_iter, DBUS_TYPE_ARRAY,
			OFONO_PROPERTIES_ARRAY_SIGNATURE,
			&dbus_dict);

	ofono_dbus_dict_append(&dbus_dict, "Temperature",
			DBUS_TYPE_INT32, &data->hm->temperature);

	ofono_dbus_dict_append(&dbus_dict, "Voltage",
			DBUS_TYPE_UINT32, &data->hm->voltage);

	dbus_message_iter_close_container(&dbus_iter, &dbus_dict);

	__ofono_dbus_pending_reply(&data->hm->msg, reply);

	return;

error:
	__ofono_dbus_pending_reply(&data->hm->msg,
			__ofono_error_failed(data->hm->msg));
}

static void gemalto_sbv_cb(gboolean ok, GAtResult *result, gpointer user_data)
{
	struct gemalto_data *data = user_data;
	GAtResultIter iter;

	if (!ok)
		goto error;

	g_at_result_iter_init(&iter, result);

	if (!g_at_result_iter_next(&iter, "^SBV:"))
		goto error;

	if (!g_at_result_iter_next_number(&iter, &data->hm->voltage))
		goto error;

	if (g_at_chat_send(data->app, "AT^SCTM?", sctm_prefix, gemalto_sctm_cb,
				data, NULL) > 0)
		return;

error:
	__ofono_dbus_pending_reply(&data->hm->msg,
			__ofono_error_failed(data->hm->msg));
}

static DBusMessage *hardware_monitor_get_statistics(DBusConnection *conn,
							DBusMessage *msg,
							void *user_data)
{
	struct gemalto_data *data = user_data;

	DBG("");

	if (data->hm->msg != NULL)
		return __ofono_error_busy(msg);

	if (!g_at_chat_send(data->app, "AT^SBV", sbv_prefix, gemalto_sbv_cb,
			data, NULL))
		return __ofono_error_failed(msg);

	data->hm->msg = dbus_message_ref(msg);

	return NULL;
}

static const GDBusMethodTable hardware_monitor_methods[] = {
	{ GDBUS_ASYNC_METHOD("GetStatistics",
			NULL, GDBUS_ARGS({ "Statistics", "a{sv}" }),
			hardware_monitor_get_statistics) },
	{}
};

static void hardware_monitor_cleanup(void *user_data)
{
	struct gemalto_data *data = user_data;
	struct gemalto_hardware_monitor *hm = data->hm;

	g_free(hm);
}

static int gemalto_hardware_monitor_enable(struct ofono_modem *modem)
{
	struct gemalto_data *data = ofono_modem_get_data(modem);
	DBusConnection *conn = ofono_dbus_get_connection();
	const char *path = ofono_modem_get_path(modem);

	DBG("");

	/* Enable temperature output */
	g_at_chat_send(data->app, "AT^SCTM=0,1", none_prefix, NULL, NULL, NULL);

	/* Create Hardware Monitor DBus interface */
	data->hm = g_try_new0(struct gemalto_hardware_monitor, 1);
	if (data->hm == NULL)
		return -EIO;

	if (!g_dbus_register_interface(conn, path, HARDWARE_MONITOR_INTERFACE,
					hardware_monitor_methods, NULL, NULL,
					data, hardware_monitor_cleanup)) {
		ofono_error("Could not register %s interface under %s",
					HARDWARE_MONITOR_INTERFACE, path);
		g_free(data->hm);
		return -EIO;
	}

	ofono_modem_add_interface(modem, HARDWARE_MONITOR_INTERFACE);
	return 0;
}

static int gemalto_enable(struct ofono_modem *modem)
{
	struct gemalto_data *data = ofono_modem_get_data(modem);
	const char *app, *mdm;

	DBG("%p", modem);

	app = ofono_modem_get_string(modem, "Application");
	mdm = ofono_modem_get_string(modem, "Modem");

	if (app == NULL || mdm == NULL)
		return -EINVAL;

	/* Open devices */
	data->app = open_device(app);
	if (data->app == NULL)
		return -EINVAL;

	data->mdm = open_device(mdm);
	if (data->mdm == NULL) {
		g_at_chat_unref(data->app);
		data->app = NULL;
		return -EINVAL;
	}

	if (getenv("OFONO_AT_DEBUG")) {
		g_at_chat_set_debug(data->app, gemalto_debug, "App");
		g_at_chat_set_debug(data->mdm, gemalto_debug, "Mdm");
	}

	g_at_chat_send(data->mdm, "ATE0", none_prefix, NULL, NULL, NULL);
	g_at_chat_send(data->app, "ATE0 +CMEE=1", none_prefix,
			NULL, NULL, NULL);
	g_at_chat_send(data->mdm, "AT&C0", none_prefix, NULL, NULL, NULL);
	g_at_chat_send(data->app, "AT&C0", none_prefix, NULL, NULL, NULL);

	g_at_chat_send(data->app, "AT+CFUN=4", none_prefix,
			cfun_enable, modem, NULL);

	gemalto_hardware_monitor_enable(modem);

	return -EINPROGRESS;
}

static void gemalto_smso_cb(gboolean ok, GAtResult *result, gpointer user_data)
{
	struct ofono_modem *modem = user_data;
	struct gemalto_data *data = ofono_modem_get_data(modem);

	DBG("");

	g_at_chat_unref(data->mdm);
	data->mdm = NULL;
	g_at_chat_unref(data->app);
	data->app = NULL;

	if (ok)
		ofono_modem_set_powered(modem, FALSE);
}

static int gemalto_disable(struct ofono_modem *modem)
{
	struct gemalto_data *data = ofono_modem_get_data(modem);
	DBusConnection *conn = ofono_dbus_get_connection();
	const char *path = ofono_modem_get_path(modem);

	DBG("%p", modem);

	g_at_chat_cancel_all(data->app);
	g_at_chat_unregister_all(data->app);

	if (g_dbus_unregister_interface(conn, path,
				HARDWARE_MONITOR_INTERFACE))
		ofono_modem_remove_interface(modem,
				HARDWARE_MONITOR_INTERFACE);

	/* Shutdown the modem */
	g_at_chat_send(data->app, "AT^SMSO", none_prefix, gemalto_smso_cb,
			modem, NULL);

	return -EINPROGRESS;
}

static void set_online_cb(gboolean ok, GAtResult *result, gpointer user_data)
{
	struct cb_data *cbd = user_data;
	ofono_modem_online_cb_t cb = cbd->cb;
	struct ofono_error error;

	decode_at_error(&error, g_at_result_final_response(result));

	cb(&error, cbd->data);
}

static void gemalto_set_online(struct ofono_modem *modem, ofono_bool_t online,
		ofono_modem_online_cb_t cb, void *user_data)
{
	struct gemalto_data *data = ofono_modem_get_data(modem);
	struct cb_data *cbd = cb_data_new(cb, user_data);
	char const *command = online ? "AT+CFUN=1" : "AT+CFUN=4";

	DBG("modem %p %s", modem, online ? "online" : "offline");

	if (g_at_chat_send(data->app, command, NULL, set_online_cb, cbd, g_free))
		return;

	CALLBACK_WITH_FAILURE(cb, cbd->data);

	g_free(cbd);
}

static void gemalto_pre_sim(struct ofono_modem *modem)
{
	struct gemalto_data *data = ofono_modem_get_data(modem);
	struct ofono_sim *sim;

	DBG("%p", modem);

	ofono_devinfo_create(modem, 0, "atmodem", data->app);
	ofono_location_reporting_create(modem, 0, "gemaltomodem", data->app);
	sim = ofono_sim_create(modem, OFONO_VENDOR_CINTERION, "atmodem",
						data->app);

	if (sim && data->have_sim == TRUE)
		ofono_sim_inserted_notify(sim, TRUE);
}

static void gemalto_post_sim(struct ofono_modem *modem)
{
	struct gemalto_data *data = ofono_modem_get_data(modem);
	struct ofono_gprs *gprs;
	struct ofono_gprs_context *gc;

	DBG("%p", modem);

	ofono_phonebook_create(modem, 0, "atmodem", data->app);

	ofono_sms_create(modem, OFONO_VENDOR_CINTERION, "atmodem", data->app);

	gprs = ofono_gprs_create(modem, 0, "atmodem", data->app);
	gc = ofono_gprs_context_create(modem, 0, "atmodem", data->mdm);

	if (gprs && gc)
		ofono_gprs_add_context(gprs, gc);
}

static void gemalto_post_online(struct ofono_modem *modem)
{
	struct gemalto_data *data = ofono_modem_get_data(modem);

	DBG("%p", modem);

	ofono_netreg_create(modem, OFONO_VENDOR_CINTERION, "atmodem", data->app);
}

static struct ofono_modem_driver gemalto_driver = {
	.name		= "gemalto",
	.probe		= gemalto_probe,
	.remove		= gemalto_remove,
	.enable		= gemalto_enable,
	.disable	= gemalto_disable,
	.set_online	= gemalto_set_online,
	.pre_sim	= gemalto_pre_sim,
	.post_sim	= gemalto_post_sim,
	.post_online	= gemalto_post_online,
};

static int gemalto_init(void)
{
	return ofono_modem_driver_register(&gemalto_driver);
}

static void gemalto_exit(void)
{
	ofono_modem_driver_unregister(&gemalto_driver);
}

OFONO_PLUGIN_DEFINE(gemalto, "Gemalto modem plugin", VERSION,
		OFONO_PLUGIN_PRIORITY_DEFAULT, gemalto_init, gemalto_exit)
