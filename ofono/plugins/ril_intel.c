/*
 *
 *  oFono - Open Source Telephony - RIL-based devices
 *
 *  Copyright (C) 2008-2015  Intel Corporation. All rights reserved.
 *  Copyright (C) 2012-2014 Canonical Ltd.
 *  Copyright (C) 2013 Jolla Ltd.
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

#include <stdio.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>

#include <unistd.h>
#include <glib.h>

#define OFONO_API_SUBJECT_TO_CHANGE
#include <ofono/plugin.h>
#include <ofono/log.h>
#include <ofono/types.h>
#include <ofono/modem.h>
#include <ofono/devinfo.h>
#include <ofono/netreg.h>
#include <ofono/sms.h>
#include <ofono/sim.h>
#include <ofono/gprs.h>
#include <ofono/gprs-context.h>
#include <ofono/radio-settings.h>
#include <ofono/ussd.h>
#include <ofono/netmon.h>

#include <gril/gril.h>

#include "drivers/rilmodem/rilmodem.h"
#include "drivers/rilmodem/vendor.h"
#include "gdbus.h"

#include "ofono.h"

#define THERMAL_MANAGEMENT_INTERFACE OFONO_SERVICE ".sofia3gr.ThermalManagement"

struct ril_data {
	GRil *ril;
};

struct ril_thermal_management {
	DBusMessage *pending;
	struct ofono_modem *modem;
	dbus_bool_t throttling;
};

static int ril_send_power(GRil *ril, ofono_bool_t online,
				GRilResponseFunc func,
				gpointer user_data,
				GDestroyNotify destroy)
{
	struct parcel rilp;

	DBG("%d", online);

	parcel_init(&rilp);
	parcel_w_int32(&rilp, 1);
	parcel_w_int32(&rilp, online);

	g_ril_append_print_buf(ril, "(%d)", online);

	return g_ril_send(ril, RIL_REQUEST_RADIO_POWER, &rilp,
						func, user_data, destroy);
}

static void ril_debug(const char *str, void *user_data)
{
	const char *prefix = user_data;

	ofono_info("%s%s", prefix, str);
}

static void ril_radio_state_changed(struct ril_msg *message,
							gpointer user_data)
{
	struct ofono_modem *modem = user_data;
	struct ril_data *rd = ofono_modem_get_data(modem);
	struct parcel rilp;
	int radio_state;

	g_ril_init_parcel(message, &rilp);
	radio_state = parcel_r_int32(&rilp);

	if (rilp.malformed) {
		ofono_error("%s: malformed parcel received", __func__);
		ofono_modem_set_powered(modem, FALSE);
		return;
	}

	g_ril_append_print_buf(rd->ril, "(state: %s)",
				ril_radio_state_to_string(radio_state));
	g_ril_print_unsol(rd->ril, message);

	switch (radio_state) {
	case RADIO_STATE_ON:
		break;
	case RADIO_STATE_UNAVAILABLE:
		ofono_modem_set_powered(modem, FALSE);
		break;
	case RADIO_STATE_OFF:
		break;
	}
}

static int ril_probe(struct ofono_modem *modem)
{
	struct ril_data *rd;
	ofono_bool_t lte_cap;

	DBG("");

	rd = g_new0(struct ril_data, 1);

	lte_cap = getenv("OFONO_RIL_RAT_LTE") ? TRUE : FALSE;
	ofono_modem_set_boolean(modem, MODEM_PROP_LTE_CAPABLE, lte_cap);

	ofono_modem_set_data(modem, rd);

	return 0;
}

static void ril_remove(struct ofono_modem *modem)
{
	DBusConnection *conn = ofono_dbus_get_connection();
	struct ril_data *rd = ofono_modem_get_data(modem);
	const char *path = ofono_modem_get_path(modem);

	if (g_dbus_unregister_interface(conn, path,
					THERMAL_MANAGEMENT_INTERFACE))
		ofono_modem_remove_interface(modem,
						THERMAL_MANAGEMENT_INTERFACE);

	ofono_modem_set_data(modem, NULL);

	g_ril_unref(rd->ril);
	g_free(rd);
}

static void set_rf_power_status_cb(struct ril_msg *message, gpointer user_data)
{
	DBusConnection *conn = ofono_dbus_get_connection();
	struct ril_thermal_management *tm = user_data;
	struct ril_data *rd = ofono_modem_get_data(tm->modem);
	const char *path = ofono_modem_get_path(tm->modem);

	DBG("");

	if (message->error != RIL_E_SUCCESS) {
		ofono_error("%s RILD reply failure: %s",
			g_ril_request_id_to_string(rd->ril, message->req),
			ril_error_to_string(message->error));

		__ofono_dbus_pending_reply(&tm->pending,
					__ofono_error_failed(tm->pending));
		return;
	}

	/* Change the throttling state */
	tm->throttling = tm->throttling ? false : true;

	__ofono_dbus_pending_reply(&tm->pending,
				dbus_message_new_method_return(tm->pending));

	ofono_dbus_signal_property_changed(conn, path,
					THERMAL_MANAGEMENT_INTERFACE,
					"TransmitPowerThrottling",
					DBUS_TYPE_BOOLEAN,
					&tm->throttling);
}

static DBusMessage *set_rf_power_status(DBusMessage *msg,
					dbus_bool_t enable,
					void *data)
{
	struct ril_thermal_management *tm = data;
	struct ril_data *rd = ofono_modem_get_data(tm->modem);
	struct parcel rilp;

	int cmd_id;
	char buf[4];

	DBG("");

	if (tm->pending)
		return __ofono_error_busy(msg);

	parcel_init(&rilp);
	parcel_w_int32(&rilp, 2);
	/* RIL_OEM_HOOK_STRING_SET_RF_POWER_STATUS = 0x000000AC */
	cmd_id = 0x000000AC;
	sprintf(buf, "%d", cmd_id);
	parcel_w_string(&rilp, buf);

	memset(buf, 0, sizeof(buf));
	sprintf(buf, "%d", enable ? 1 : 0);
	parcel_w_string(&rilp, buf);

	g_ril_append_print_buf(rd->ril, "{cmd_id=0x%02X,arg=%s}", cmd_id, buf);

	if (g_ril_send(rd->ril, RIL_REQUEST_OEM_HOOK_STRINGS, &rilp,
			set_rf_power_status_cb, tm, NULL) == 0)
		return __ofono_error_failed(msg);

	tm->pending = dbus_message_ref(msg);

	return NULL;
}

static DBusMessage *thermal_management_set_property(DBusConnection *conn,
							DBusMessage *msg,
							void *data)
{
	struct ril_thermal_management *tm = data;
	DBusMessageIter iter;
	DBusMessageIter var;
	const char *name;
	dbus_bool_t throttling;

	DBG("");

	if (!ofono_modem_get_online(tm->modem))
		return __ofono_error_not_available(msg);

	if (!dbus_message_iter_init(msg, &iter))
		return __ofono_error_invalid_args(msg);

	if (dbus_message_iter_get_arg_type(&iter) != DBUS_TYPE_STRING)
		return __ofono_error_invalid_args(msg);

	dbus_message_iter_get_basic(&iter, &name);

	if (!strcmp(name, "TransmitPowerThrottling")) {
		dbus_message_iter_next(&iter);

		if (dbus_message_iter_get_arg_type(&iter) != DBUS_TYPE_VARIANT)
			return __ofono_error_invalid_args(msg);

		dbus_message_iter_recurse(&iter, &var);

		if (dbus_message_iter_get_arg_type(&var) != DBUS_TYPE_BOOLEAN)
			return __ofono_error_invalid_args(msg);

		dbus_message_iter_get_basic(&var, &throttling);

		if (tm->throttling == throttling)
			/* Ignore set request if new state == current state */
			return dbus_message_new_method_return(msg);

		return set_rf_power_status(msg, throttling, tm);
	}

	return __ofono_error_invalid_args(msg);
}

static DBusMessage *thermal_management_get_properties(DBusConnection *conn,
							DBusMessage *msg,
							void *data)
{
	struct ril_thermal_management *tm = data;
	DBusMessage *reply;
	DBusMessageIter iter;
	DBusMessageIter dict;

	DBG("");

	reply = dbus_message_new_method_return(msg);
	if (reply == NULL)
		return NULL;

	dbus_message_iter_init_append(reply, &iter);

	dbus_message_iter_open_container(&iter, DBUS_TYPE_ARRAY,
					OFONO_PROPERTIES_ARRAY_SIGNATURE,
					&dict);

	ofono_dbus_dict_append(&dict, "TransmitPowerThrottling",
				DBUS_TYPE_BOOLEAN,
				&tm->throttling);

	dbus_message_iter_close_container(&iter, &dict);

	return reply;
}

static const GDBusMethodTable thermal_management_methods[] = {
	{ GDBUS_METHOD("GetProperties",
			NULL, GDBUS_ARGS({ "properties", "a{sv}" }),
			thermal_management_get_properties) },
	{ GDBUS_ASYNC_METHOD("SetProperty",
			GDBUS_ARGS({ "property", "s" }, { "value", "v" }),
			NULL, thermal_management_set_property) },
	{}
};

static const GDBusSignalTable thermal_management_signals[] = {
	{ GDBUS_SIGNAL("PropertyChanged",
			GDBUS_ARGS({ "name", "s" }, { "value", "v" })) },
	{ }
};

static void thermal_management_cleanup(void *data)
{
	struct ril_thermal_management *tm = data;

	if (tm->pending)
		__ofono_dbus_pending_reply(&tm->pending,
					__ofono_error_canceled(tm->pending));

	g_free(tm);
}

static void get_rf_power_status_cb(struct ril_msg *message, gpointer user_data)
{
	struct ofono_modem *modem = user_data;
	struct ril_data *rd = ofono_modem_get_data(modem);
	struct ril_thermal_management *tm;
	DBusConnection *conn = ofono_dbus_get_connection();
	struct parcel rilp;
	gint numstr;
	gchar *power_status;
	char *endptr;
	int enabled;
	const char *path = ofono_modem_get_path(modem);

	DBG("");

	if (message->error != RIL_E_SUCCESS) {
		ofono_error("%s RILD reply failure: %s",
			g_ril_request_id_to_string(rd->ril, message->req),
			ril_error_to_string(message->error));
		return;
	}

	g_ril_init_parcel(message, &rilp);

	numstr = parcel_r_int32(&rilp);
	if (numstr < 1) {
		ofono_error("RILD reply empty !");
		return;
	}

	power_status = parcel_r_string(&rilp);
	if (power_status == NULL || *power_status == '\0')
		return;

	enabled = strtol(power_status, &endptr, 10);
	/*
	 * power_status == endptr => conversion error
	 * *endptr != '\0' => partial conversion
	 */
	if (power_status == endptr || *endptr != '\0')
		return;

	tm = g_try_new0(struct ril_thermal_management, 1);
	if (tm == NULL)
		return;

	tm->modem = modem;
	tm->throttling = (enabled > 0) ? true : false;


	if (!g_dbus_register_interface(conn, path, THERMAL_MANAGEMENT_INTERFACE,
					thermal_management_methods,
					thermal_management_signals,
					NULL, tm, thermal_management_cleanup)) {
		ofono_error("Could not register %s interface under %s",
					THERMAL_MANAGEMENT_INTERFACE, path);
		g_free(tm);
		return;
	}

	ofono_modem_add_interface(modem, THERMAL_MANAGEMENT_INTERFACE);
}

static int ril_thermal_management_enable(struct ofono_modem *modem)
{
	struct ril_data *rd = ofono_modem_get_data(modem);
	struct parcel rilp;

	int cmd_id;
	char buf[4];

	DBG("");

	parcel_init(&rilp);
	parcel_w_int32(&rilp, 1);
	/* RIL_OEM_HOOK_STRING_GET_RF_POWER_STATUS = 0x000000AB */
	cmd_id = 0x000000AB;
	sprintf(buf, "%d", cmd_id);
	parcel_w_string(&rilp, buf);

	g_ril_append_print_buf(rd->ril, "{cmd_id=0x%02X}", cmd_id);

	if (g_ril_send(rd->ril, RIL_REQUEST_OEM_HOOK_STRINGS, &rilp,
			get_rf_power_status_cb, modem, NULL) > 0)
		return 0;

	/* Error path */

	return -EIO;
}

static void ril_pre_sim(struct ofono_modem *modem)
{
	struct ril_data *rd = ofono_modem_get_data(modem);

	DBG("");

	ofono_devinfo_create(modem, 0, "rilmodem", rd->ril);
	ofono_sim_create(modem, 0, "rilmodem", rd->ril);
	ril_thermal_management_enable(modem);
}

static void ril_post_sim(struct ofono_modem *modem)
{
	struct ril_data *rd = ofono_modem_get_data(modem);
	struct ofono_gprs *gprs;
	struct ofono_gprs_context *gc;

	if (ofono_modem_get_boolean(modem, MODEM_PROP_LTE_CAPABLE))
		ofono_sms_create(modem, 0, "rilmodem", rd->ril);
	else
		ofono_sms_create(modem, OFONO_RIL_VENDOR_IMC_SOFIA3GR,
					"rilmodem", rd->ril);

	gprs = ofono_gprs_create(modem, 0, "rilmodem", rd->ril);
	gc = ofono_gprs_context_create(modem, 0, "rilmodem", rd->ril);

	if (gc) {
		ofono_gprs_context_set_type(gc,
					OFONO_GPRS_CONTEXT_TYPE_INTERNET);
		ofono_gprs_add_context(gprs, gc);
	}

	if (ofono_modem_get_boolean(modem, MODEM_PROP_LTE_CAPABLE))
		ofono_lte_create(modem, 0, "rilmodem", rd->ril);

	ofono_stk_create(modem, 0, "rilmodem", rd->ril);
}

static void ril_post_online(struct ofono_modem *modem)
{
	struct ril_data *rd = ofono_modem_get_data(modem);

	ofono_netreg_create(modem, 0, "rilmodem", rd->ril);

	if (ofono_modem_get_boolean(modem, MODEM_PROP_LTE_CAPABLE))
		ofono_radio_settings_create(modem, 0, "rilmodem", rd->ril);
	else
		ofono_radio_settings_create(modem, OFONO_RIL_VENDOR_IMC_SOFIA3GR,
						"rilmodem", rd->ril);

	ofono_ussd_create(modem, 0, "rilmodem", rd->ril);
	ofono_netmon_create(modem, 0, "rilmodem", rd->ril);
}

static void ril_set_online_cb(struct ril_msg *message, gpointer user_data)
{
	struct cb_data *cbd = user_data;
	ofono_modem_online_cb_t cb = cbd->cb;

	DBG("%d", message->error);

	if (message->error == RIL_E_SUCCESS)
		CALLBACK_WITH_SUCCESS(cb, cbd->data);
	else
		CALLBACK_WITH_FAILURE(cb, cbd->data);
}

static void ril_set_online(struct ofono_modem *modem, ofono_bool_t online,
			ofono_modem_online_cb_t cb, void *data)
{
	struct ril_data *rd = ofono_modem_get_data(modem);
	struct cb_data *cbd = cb_data_new(cb, data, rd);

	if (ril_send_power(rd->ril, online, ril_set_online_cb, cbd, g_free) > 0)
		return;

	CALLBACK_WITH_FAILURE(cb, cbd->data);
	g_free(cbd);
}

static void ril_init_power(struct ril_msg *message, gpointer user_data)
{
	struct ofono_modem *modem = user_data;
	ofono_bool_t powered;

	DBG("%d", message->error);

	powered = message->error != RIL_E_SUCCESS ? FALSE : TRUE;
	ofono_modem_set_powered(modem, powered);
}

static void ril_connected(struct ril_msg *message, gpointer user_data)
{
	struct ofono_modem *modem = user_data;
	struct ril_data *rd = ofono_modem_get_data(modem);

	DBG("");

	/* Set Modem Offline */
	if (ril_send_power(rd->ril, FALSE, ril_init_power, modem, NULL) > 0)
		return;

	ofono_modem_set_powered(modem, FALSE);
}

static int ril_enable(struct ofono_modem *modem)
{
	struct ril_data *rd = ofono_modem_get_data(modem);

	DBG("");

	rd->ril = g_ril_new("/tmp/rild", OFONO_RIL_VENDOR_AOSP);
	if (rd->ril == NULL) {
		ofono_error("g_ril_new() failed to create modem!");
		return -EIO;
	}

	if (getenv("OFONO_RIL_TRACE"))
		g_ril_set_trace(rd->ril, TRUE);

	if (getenv("OFONO_RIL_HEX_TRACE"))
		g_ril_set_debugf(rd->ril, ril_debug, "IntelModem:");

	g_ril_register(rd->ril, RIL_UNSOL_RIL_CONNECTED,
						ril_connected, modem);

	g_ril_register(rd->ril, RIL_UNSOL_RESPONSE_RADIO_STATE_CHANGED,
					ril_radio_state_changed, modem);

	return -EINPROGRESS;
}

static void ril_send_power_off_cb(struct ril_msg *message, gpointer user_data)
{
	struct ofono_modem *modem = (struct ofono_modem *) user_data;
	struct ril_data *rd = ofono_modem_get_data(modem);

	g_ril_unref(rd->ril);

	ofono_modem_set_powered(modem, FALSE);
}

static int ril_disable(struct ofono_modem *modem)
{
	DBusConnection *conn = ofono_dbus_get_connection();
	struct ril_data *rd = ofono_modem_get_data(modem);
	const char *path = ofono_modem_get_path(modem);
	struct parcel rilp;
	int cmd_id;
	char buf[4];

	DBG("%p", modem);

	if (g_dbus_unregister_interface(conn, path,
					THERMAL_MANAGEMENT_INTERFACE))
		ofono_modem_remove_interface(modem,
						THERMAL_MANAGEMENT_INTERFACE);

	/* RIL_OEM_HOOK_STRING_SET_MODEM_OFF = 0x000000CF */
	cmd_id = 0x000000CF;
	sprintf(buf, "%d", cmd_id);
	parcel_init(&rilp);
	parcel_w_int32(&rilp, 1);
	parcel_w_string(&rilp, buf);

	g_ril_append_print_buf(rd->ril, "{cmd_id=0x%02X}", cmd_id);

	g_ril_send(rd->ril, RIL_REQUEST_OEM_HOOK_STRINGS, &rilp,
					ril_send_power_off_cb, modem, NULL);

	return -EINPROGRESS;
}

static struct ofono_modem_driver ril_driver = {
	.name = "ril_intel",
	.probe = ril_probe,
	.remove = ril_remove,
	.enable = ril_enable,
	.disable = ril_disable,
	.pre_sim = ril_pre_sim,
	.post_sim = ril_post_sim,
	.post_online = ril_post_online,
	.set_online = ril_set_online,
};

static int ril_init(void)
{
	return ofono_modem_driver_register(&ril_driver);
}

static void ril_exit(void)
{
	ofono_modem_driver_unregister(&ril_driver);
}

OFONO_PLUGIN_DEFINE(ril_intel, "Intel RIL-based modem driver", VERSION,
			OFONO_PLUGIN_PRIORITY_DEFAULT, ril_init, ril_exit)
