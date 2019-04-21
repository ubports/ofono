/*
 *
 *  oFono - Open Source Telephony
 *
 *
 *  Copyright (C) 2008-2016  Intel Corporation. All rights reserved.
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

#include <string.h>
#include <stdio.h>
#include <stdint.h>
#include <stdarg.h>
#include <errno.h>

#include <glib.h>
#include <gdbus.h>

#include "ofono.h"
#include "netmonagent.h"

#define CELL_INFO_DICT_APPEND(p_dict, key, info, type, dbus_type)	do { \
	type value; \
	if (info < 0) \
		break; \
	value = (type) info; \
	ofono_dbus_dict_append(p_dict, key, dbus_type, &value); \
} while (0)

static GSList *g_drivers = NULL;

struct ofono_netmon {
	const struct ofono_netmon_driver *driver;
	DBusMessage *pending;
	DBusMessage *reply;
	void *driver_data;
	struct ofono_atom *atom;
	struct netmon_agent *agent;
};

static const char *cell_type_to_tech_name(enum ofono_netmon_cell_type type)
{
	switch (type) {
	case OFONO_NETMON_CELL_TYPE_GSM:
		return "gsm";
	case OFONO_NETMON_CELL_TYPE_UMTS:
		return "umts";
	case OFONO_NETMON_CELL_TYPE_LTE:
		return "lte";
	}

	return NULL;
}

void ofono_netmon_serving_cell_notify(struct ofono_netmon *netmon,
					enum ofono_netmon_cell_type type,
					int info_type, ...)
{
	va_list arglist;
	DBusMessage *agent_notify = NULL;
	DBusMessageIter iter;
	DBusMessageIter dict;
	enum ofono_netmon_info next_info_type = info_type;
	const char *technology = cell_type_to_tech_name(type);
	char *mcc;
	char *mnc;
	int intval;

	if (netmon->pending != NULL) {
		netmon->reply = dbus_message_new_method_return(netmon->pending);
		dbus_message_iter_init_append(netmon->reply, &iter);
	} else if (netmon->agent != NULL) {
		agent_notify = netmon_agent_new_method_call(netmon->agent,
					"ServingCellInformationChanged");

		dbus_message_iter_init_append(agent_notify, &iter);
	} else
		return;

	dbus_message_iter_open_container(&iter, DBUS_TYPE_ARRAY,
					OFONO_PROPERTIES_ARRAY_SIGNATURE,
					&dict);

	va_start(arglist, info_type);

	if (technology == NULL)
		goto done;

	ofono_dbus_dict_append(&dict, "Technology",
						DBUS_TYPE_STRING, &technology);

	while (next_info_type != OFONO_NETMON_INFO_INVALID) {
		switch (next_info_type) {
		case OFONO_NETMON_INFO_MCC:
			mcc = va_arg(arglist, char *);

			if (mcc && strlen(mcc))
				ofono_dbus_dict_append(&dict,
						"MobileCountryCode",
						DBUS_TYPE_STRING, &mcc);
			break;

		case OFONO_NETMON_INFO_MNC:
			mnc = va_arg(arglist, char *);

			if (mnc && strlen(mnc))
				ofono_dbus_dict_append(&dict,
						"MobileNetworkCode",
						DBUS_TYPE_STRING, &mnc);
			break;

		case OFONO_NETMON_INFO_LAC:
			intval = va_arg(arglist, int);

			CELL_INFO_DICT_APPEND(&dict, "LocationAreaCode",
					intval, uint16_t, DBUS_TYPE_UINT16);
			break;

		case OFONO_NETMON_INFO_CI:
			intval = va_arg(arglist, int);

			CELL_INFO_DICT_APPEND(&dict, "CellId",
					intval, uint32_t, DBUS_TYPE_UINT32);
			break;

		case OFONO_NETMON_INFO_ARFCN:
			intval = va_arg(arglist, int);

			CELL_INFO_DICT_APPEND(&dict, "ARFCN",
					intval, uint16_t, DBUS_TYPE_UINT16);
			break;

		case OFONO_NETMON_INFO_BSIC:
			intval = va_arg(arglist, int);

			CELL_INFO_DICT_APPEND(&dict, "BSIC",
					intval, uint8_t, DBUS_TYPE_BYTE);
			break;

		case OFONO_NETMON_INFO_RXLEV:
			intval = va_arg(arglist, int);

			CELL_INFO_DICT_APPEND(&dict, "ReceivedSignalStrength",
					intval, uint8_t, DBUS_TYPE_BYTE);
			break;

		case OFONO_NETMON_INFO_TIMING_ADVANCE:
			intval = va_arg(arglist, int);

			CELL_INFO_DICT_APPEND(&dict, "TimingAdvance",
					intval, uint8_t, DBUS_TYPE_BYTE);
			break;

		case OFONO_NETMON_INFO_PSC:
			intval = va_arg(arglist, int);

			CELL_INFO_DICT_APPEND(&dict, "PrimaryScramblingCode",
					intval, uint16_t, DBUS_TYPE_UINT16);
			break;

		case OFONO_NETMON_INFO_BER:
			intval = va_arg(arglist, int);

			CELL_INFO_DICT_APPEND(&dict, "BitErrorRate",
					intval, uint8_t, DBUS_TYPE_BYTE);
			break;

		case OFONO_NETMON_INFO_RSSI:
			intval = va_arg(arglist, int);

			CELL_INFO_DICT_APPEND(&dict, "Strength",
					intval, uint8_t, DBUS_TYPE_BYTE);
			break;

		case OFONO_NETMON_INFO_RSCP:
			intval = va_arg(arglist, int);

			CELL_INFO_DICT_APPEND(&dict, "ReceivedSignalCodePower",
					intval, uint8_t, DBUS_TYPE_BYTE);
			break;

		case OFONO_NETMON_INFO_ECN0:
			intval = va_arg(arglist, int);

			CELL_INFO_DICT_APPEND(&dict, "ReceivedEnergyRatio",
					intval, uint8_t, DBUS_TYPE_BYTE);
			break;

		case OFONO_NETMON_INFO_RSRQ:
			intval = va_arg(arglist, int);

			CELL_INFO_DICT_APPEND(&dict,
					"ReferenceSignalReceivedQuality",
					intval, uint8_t, DBUS_TYPE_BYTE);
			break;

		case OFONO_NETMON_INFO_RSRP:
			intval = va_arg(arglist, int);

			CELL_INFO_DICT_APPEND(&dict,
					"ReferenceSignalReceivedPower",
					intval, uint8_t, DBUS_TYPE_BYTE);
			break;

		case OFONO_NETMON_INFO_EARFCN:
			intval = va_arg(arglist, int);

			CELL_INFO_DICT_APPEND(&dict, "EARFCN",
					intval, uint16_t, DBUS_TYPE_UINT16);
			break;

		case OFONO_NETMON_INFO_EBAND:
			intval = va_arg(arglist, int);

			CELL_INFO_DICT_APPEND(&dict, "EBand",
					intval, uint8_t, DBUS_TYPE_BYTE);
			break;

		case OFONO_NETMON_INFO_CQI:
			intval = va_arg(arglist, int);

			CELL_INFO_DICT_APPEND(&dict, "ChannelQualityIndicator",
					intval, uint8_t, DBUS_TYPE_BYTE);
			break;

		case OFONO_NETMON_INFO_INVALID:
			break;
		}

		next_info_type = va_arg(arglist, int);
	}

done:
	va_end(arglist);

	dbus_message_iter_close_container(&iter, &dict);

	if (agent_notify)
		netmon_agent_send_no_reply(netmon->agent, agent_notify);
}

static void serving_cell_info_callback(const struct ofono_error *error,
		void *data)
{
	struct ofono_netmon *netmon = data;
	DBusMessage *reply = netmon->reply;

	if (error->type != OFONO_ERROR_TYPE_NO_ERROR) {
		if (reply)
			dbus_message_unref(reply);

		reply = __ofono_error_failed(netmon->pending);
        } else if (!reply) {
		DBusMessageIter iter;
		DBusMessageIter dict;

		reply = dbus_message_new_method_return(netmon->pending);
		dbus_message_iter_init_append(reply, &iter);
		dbus_message_iter_open_container(&iter, DBUS_TYPE_ARRAY,
					OFONO_PROPERTIES_ARRAY_SIGNATURE,
					&dict);
		dbus_message_iter_close_container(&iter, &dict);
	}

	netmon->reply = NULL;
	__ofono_dbus_pending_reply(&netmon->pending, reply);
}

static DBusMessage *netmon_get_serving_cell_info(DBusConnection *conn,
			DBusMessage *msg, void *data)
{
	struct ofono_netmon *netmon = data;

	if (!netmon->driver && !netmon->driver->request_update)
		return __ofono_error_not_implemented(msg);

	if (netmon->pending)
		return __ofono_error_busy(msg);

	netmon->pending = dbus_message_ref(msg);

	netmon->driver->request_update(netmon,
					serving_cell_info_callback, netmon);

	return NULL;
}

static void periodic_updates_enabled_cb(const struct ofono_error *error,
					void *data)
{
	struct ofono_netmon *netmon = data;

	if (error->type != OFONO_ERROR_TYPE_NO_ERROR) {
		ofono_error("Error enabling periodic updates");

		netmon_agent_free(netmon->agent);
		return;
	}
}

static void periodic_updates_disabled_cb(const struct ofono_error *error,
					void *data)
{
	if (error->type != OFONO_ERROR_TYPE_NO_ERROR)
		ofono_error("Error disabling periodic updates");
}

static void agent_removed_cb(gpointer user_data)
{
	struct ofono_netmon *netmon = user_data;

	netmon->agent = NULL;

	netmon->driver->enable_periodic_update(netmon, 0, 0,
						periodic_updates_disabled_cb,
						NULL);
}

static DBusMessage *netmon_register_agent(DBusConnection *conn,
				DBusMessage *msg, void *data)
{
	struct ofono_netmon *netmon = data;
	const char *agent_path;
	const unsigned int enable = 1;
	unsigned int period;

	if (netmon->agent)
		return __ofono_error_busy(msg);

	if (!netmon->driver->enable_periodic_update)
		return __ofono_error_not_implemented(msg);

	if (dbus_message_get_args(msg, NULL,
				DBUS_TYPE_OBJECT_PATH, &agent_path,
				DBUS_TYPE_UINT32, &period,
				DBUS_TYPE_INVALID) == FALSE)
		return __ofono_error_invalid_args(msg);

	if (!dbus_validate_path(agent_path, NULL))
		return __ofono_error_invalid_format(msg);

	if (!period)
		return __ofono_error_invalid_args(msg);

	/* minimum period is 5 seconds, to avoid frequent updates*/
	if (period < 5)
		period = 5;

	netmon->agent = netmon_agent_new(agent_path,
					dbus_message_get_sender(msg));

	if (netmon->agent == NULL)
		return __ofono_error_failed(msg);

	netmon_agent_set_removed_notify(netmon->agent, agent_removed_cb, netmon);

	netmon->driver->enable_periodic_update(netmon, enable, period,
					periodic_updates_enabled_cb, netmon);

	return dbus_message_new_method_return(msg);
}

static DBusMessage *netmon_unregister_agent(DBusConnection *conn,
					DBusMessage *msg, void *data)
{
	struct ofono_netmon *netmon = data;
	const char *agent_path;
	const char *agent_bus = dbus_message_get_sender(msg);

	if (!netmon->driver->enable_periodic_update)
		return __ofono_error_not_implemented(msg);

	if (dbus_message_get_args(msg, NULL,
					DBUS_TYPE_OBJECT_PATH, &agent_path,
					DBUS_TYPE_INVALID) == FALSE)
		return __ofono_error_invalid_args(msg);

	if (netmon->agent == NULL)
		return __ofono_error_failed(msg);

	if (!netmon_agent_matches(netmon->agent, agent_path, agent_bus))
		return __ofono_error_access_denied(msg);

	netmon_agent_free(netmon->agent);

	return dbus_message_new_method_return(msg);
}

static const GDBusMethodTable netmon_methods[] = {
	{ GDBUS_ASYNC_METHOD("GetServingCellInformation",
			NULL, GDBUS_ARGS({ "cellinfo", "a{sv}" }),
			netmon_get_serving_cell_info) },
	{ GDBUS_METHOD("RegisterAgent",
			GDBUS_ARGS({ "path", "o"}, { "period", "u"}), NULL,
			netmon_register_agent) },
	{ GDBUS_METHOD("UnregisterAgent",
			GDBUS_ARGS({ "agent", "o" }), NULL,
			netmon_unregister_agent) },
	{ }
};

int ofono_netmon_driver_register(const struct ofono_netmon_driver *d)
{
	DBG("driver: %p, name: %s", d, d->name);

	if (d->probe == NULL)
		return -EINVAL;

	g_drivers = g_slist_prepend(g_drivers, (void *) d);

	return 0;
}

void ofono_netmon_driver_unregister(const struct ofono_netmon_driver *d)
{
	DBG("driver: %p, name: %s", d, d->name);

	g_drivers = g_slist_remove(g_drivers, (void *) d);
}

static void netmon_unregister(struct ofono_atom *atom)
{
	DBusConnection *conn = ofono_dbus_get_connection();
	struct ofono_modem *modem = __ofono_atom_get_modem(atom);
	const char *path = __ofono_atom_get_path(atom);

	ofono_modem_remove_interface(modem, OFONO_NETMON_INTERFACE);
	g_dbus_unregister_interface(conn, path, OFONO_NETMON_INTERFACE);
}

static void netmon_remove(struct ofono_atom *atom)
{
	struct ofono_netmon *netmon = __ofono_atom_get_data(atom);

	if (netmon == NULL)
		return;

	if (netmon->driver && netmon->driver->remove)
		netmon->driver->remove(netmon);

	g_free(netmon);
}

struct ofono_netmon *ofono_netmon_create(struct ofono_modem *modem,
			unsigned int vendor, const char *driver, void *data)
{
	struct ofono_netmon *netmon;
	GSList *l;

	if (driver == NULL)
		return NULL;

	netmon = g_try_new0(struct ofono_netmon, 1);

	if (netmon == NULL)
		return NULL;

	netmon->atom = __ofono_modem_add_atom(modem, OFONO_ATOM_TYPE_NETMON,
						netmon_remove, netmon);

	for (l = g_drivers; l; l = l->next) {
		const struct ofono_netmon_driver *drv = l->data;

		if (g_strcmp0(drv->name, driver))
			continue;

		if (drv->probe(netmon, vendor, data) < 0)
			continue;

		netmon->driver = drv;
		break;
	}

	return netmon;
}

void ofono_netmon_register(struct ofono_netmon *netmon)
{
	DBusConnection *conn = ofono_dbus_get_connection();
	struct ofono_modem *modem = __ofono_atom_get_modem(netmon->atom);
	const char *path = __ofono_atom_get_path(netmon->atom);

	if (!g_dbus_register_interface(conn, path,
				OFONO_NETMON_INTERFACE,
				netmon_methods, NULL, NULL,
				netmon, NULL)) {
		ofono_error("Could not create %s interface",
				OFONO_NETMON_INTERFACE);
		return;
	}

	ofono_modem_add_interface(modem, OFONO_NETMON_INTERFACE);

	__ofono_atom_register(netmon->atom, netmon_unregister);
}

void ofono_netmon_remove(struct ofono_netmon *netmon)
{
	__ofono_atom_free(netmon->atom);
}

void ofono_netmon_set_data(struct ofono_netmon *netmon, void *data)
{
	netmon->driver_data = data;
}

void *ofono_netmon_get_data(struct ofono_netmon *netmon)
{
	return netmon->driver_data;
}
