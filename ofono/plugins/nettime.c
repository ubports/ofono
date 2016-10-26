/*
 *  oFono - Open Source Telephony
 *
 *  Copyright (C) 2012-2016  Jolla Ltd.
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

#include <string.h>
#include <glib.h>
#include <gdbus.h>
#include <stdlib.h>

#define OFONO_API_SUBJECT_TO_CHANGE
#include <ofono/plugin.h>
#include <ofono/log.h>
#include <ofono/nettime.h>

#include "ofono.h"

#define NTD_WAIT_TIMEOUT (500) /* ms */

/*
 * There are 3 distinct states here:
 *
 * 1. !valid
 *
 *     Initial state, no time/timezone information whatsoever.
 *
 * 2. valid && !mccmnc
 *
 *    Time/timezone information has been received from the network,
 *    but no MCC and MNC yet.
 *
 * 3. valid && mccmnc
 *
 *    Time/timezone information is fully available.
 *
 */
struct nt_data {
	struct ofono_modem *modem;
	struct ofono_netreg *netreg;
	unsigned int netreg_watch_id;
	unsigned int netreg_status_watch_id;
	guint mccmnc_wait_id;

	gboolean time_available;
	gboolean valid;

	time_t nw_time_utc;
	time_t received;

	int dst;
	int time_zone;

	char *mcc;
	char *mnc;
	DBusConnection *conn;
};

#define DBG_(ntd,fmt,args...) \
	DBG("%s " fmt, ofono_modem_get_path((ntd)->modem), ##args)

static gboolean nettime_encode_time_format(struct tm *tm,
					const struct ofono_network_time *time)
{
	if (time->year < 0)
		return FALSE;

	memset(tm, 0, sizeof(struct tm));
	tm->tm_year = time->year - 1900;
	tm->tm_mon = time->mon - 1;
	tm->tm_mday = time->mday;
	tm->tm_hour = time->hour;
	tm->tm_min = time->min;
	tm->tm_sec = time->sec;
	tm->tm_gmtoff = time->utcoff;
	tm->tm_isdst = time->dst;

	return TRUE;
}

static time_t nettime_get_monotonic_time()
{
	struct timespec ts;
	memset(&ts, 0, sizeof(struct timespec));
#if defined(CLOCK_BOOTTIME)
	if (clock_gettime(CLOCK_BOOTTIME, &ts) < 0)
		clock_gettime(CLOCK_MONOTONIC, &ts);
#else
		clock_gettime(CLOCK_MONOTONIC, &ts);
#endif
	return ts.tv_sec;
}

static int nettime_fill_time_notification(DBusMessage *msg, struct nt_data *ntd)
{
	DBusMessageIter iter, iter_array;
	dbus_int64_t utc_long, received;
	dbus_int32_t dst, timezone;
	dbus_message_iter_init_append(msg, &iter);
	dbus_message_iter_open_container(&iter,	DBUS_TYPE_ARRAY,
						"{sv}",
						&iter_array);
	if (ntd->valid && ntd->mcc && ntd->mnc) {
		if (ntd->time_available) {
			utc_long = (dbus_int64_t) ntd->nw_time_utc;
			ofono_dbus_dict_append(&iter_array,
						"UTC",
						DBUS_TYPE_INT64,
						&utc_long);
			dst = (dbus_int32_t) ntd->dst;
			ofono_dbus_dict_append(&iter_array,
						"DST",
						DBUS_TYPE_UINT32,
						&dst);
			received = (dbus_int64_t) ntd->received;
			ofono_dbus_dict_append(&iter_array,
						"Received",
						DBUS_TYPE_INT64,
						&received);
		}

		timezone = (dbus_int32_t) ntd->time_zone;
		ofono_dbus_dict_append(&iter_array,
					"Timezone",
					DBUS_TYPE_INT32,
					&timezone);

		ofono_dbus_dict_append(&iter_array,
					"MobileCountryCode",
					DBUS_TYPE_STRING,
					&ntd->mcc);

		ofono_dbus_dict_append(&iter_array,
					"MobileNetworkCode",
					DBUS_TYPE_STRING,
					&ntd->mnc);
	} else {
		DBG_(ntd, "time not available");
	}

	dbus_message_iter_close_container(&iter, &iter_array);
	return 0;
}

static DBusMessage *nettime_get_network_time(DBusConnection *conn,
						DBusMessage *msg, void *data)
{
	struct nt_data *ntd = data;
	DBusMessage *reply = dbus_message_new_method_return(msg);

	if (reply) {
		nettime_fill_time_notification(reply, ntd);
	}
	return reply;
}

static const GDBusMethodTable nettime_methods[] = {
	{ GDBUS_METHOD("GetNetworkTime",
			NULL, GDBUS_ARGS({ "time", "a{sv}" }),
			nettime_get_network_time) },
	{ }
};

static const GDBusSignalTable nettime_signals[] = {
	{ GDBUS_SIGNAL("NetworkTimeChanged",
			GDBUS_ARGS({ "time", "a{sv}" })) },
	{ }
};

static void nettime_send_signal(struct nt_data *ntd)
{
	DBusMessage *signal =
		dbus_message_new_signal(ofono_modem_get_path(ntd->modem),
			OFONO_NETWORK_TIME_INTERFACE, "NetworkTimeChanged");

	DBG_(ntd, "");
	nettime_fill_time_notification(signal, ntd);
	g_dbus_send_message(ntd->conn, signal);
}

static void nettime_set_mcc_mnc(struct nt_data *ntd, const char *mcc,
							const char *mnc)
{
	if (g_strcmp0(ntd->mcc, mcc)) {
		g_free(ntd->mcc);
		ntd->mcc = g_strdup(mcc);
	}

	if (g_strcmp0(ntd->mnc, mnc)) {
		g_free(ntd->mnc);
		ntd->mnc = g_strdup(mnc);
	}

	if (ntd->mcc && ntd->mnc) {
		DBG_(ntd, "MCC: %s, MNC: %s", ntd->mcc, ntd->mnc);

		if (ntd->mccmnc_wait_id) {
			/* We have been waiting for MCC and MNC */
			g_source_remove(ntd->mccmnc_wait_id);
			ntd->mccmnc_wait_id = 0;
			nettime_send_signal(ntd);
		}
	}
}

static void nettime_netreg_status_watch_cb(int status, int lac, int ci,
			int tech, const char *mcc, const char *mnc,
			void *userdata)
{
	nettime_set_mcc_mnc(userdata, mcc, mnc);
}

static void nettime_netreg_status_watch_done(void *userdata)
{
	struct nt_data *ntd = userdata;

	DBG_(ntd, "");
	ntd->netreg_status_watch_id = 0;
}

static void nettime_set_netreg(struct nt_data *ntd,
						struct ofono_netreg *netreg)
{
	if (ntd->netreg != netreg) {
		ntd->valid = FALSE;
		ntd->netreg = netreg;
		if (netreg) {
			nettime_set_mcc_mnc(ntd,
					ofono_netreg_get_mcc(netreg),
					ofono_netreg_get_mnc(netreg));
			ntd->netreg_status_watch_id =
				__ofono_netreg_add_status_watch(netreg,
					nettime_netreg_status_watch_cb, ntd,
					nettime_netreg_status_watch_done);
		} else {
			g_free(ntd->mcc);
			g_free(ntd->mnc);
			ntd->mcc = NULL;
			ntd->mnc = NULL;
			ntd->netreg_status_watch_id = 0;
		}
	}
}

static void nettime_netreg_watch_cb(struct ofono_atom *atom,
			enum ofono_atom_watch_condition cond, void *userdata)
{
	struct nt_data *ntd = userdata;

	if (cond == OFONO_ATOM_WATCH_CONDITION_REGISTERED) {
		DBG_(ntd, "registered");
		nettime_set_netreg(ntd, __ofono_atom_get_data(atom));
	} else if (cond == OFONO_ATOM_WATCH_CONDITION_UNREGISTERED) {
		DBG_(ntd, "unregistered");
		nettime_set_netreg(ntd, NULL);
	}
}

static void nettime_netreg_watch_done(void *userdata)
{
	struct nt_data *ntd = userdata;

	DBG_(ntd, "");
	ntd->netreg_watch_id = 0;
}

static gboolean nettime_timeout_cb(gpointer data)
{
	struct nt_data *ntd = data;

	DBG_(ntd, "timed out");
	ntd->mccmnc_wait_id = 0;
	ntd->valid = FALSE;
	return G_SOURCE_REMOVE;
}

static struct nt_data *nettime_new(struct ofono_modem *modem)
{
	struct nt_data *ntd = g_new0(struct nt_data, 1);	
	struct ofono_atom *netreg_atom = __ofono_modem_find_atom(modem,
						OFONO_ATOM_TYPE_NETREG);

	ntd->modem = modem;
	ntd->conn = dbus_connection_ref(ofono_dbus_get_connection());
	ntd->netreg_watch_id = __ofono_modem_add_atom_watch(modem,
			OFONO_ATOM_TYPE_NETREG, nettime_netreg_watch_cb,
			ntd, nettime_netreg_watch_done);

	if (netreg_atom) {
		nettime_set_netreg(ntd, __ofono_atom_get_data(netreg_atom));
	}

	return ntd;
}

static void nettime_free(struct nt_data *ntd)
{
	if (ntd->mccmnc_wait_id)
		g_source_remove(ntd->mccmnc_wait_id);

	if (ntd->netreg_status_watch_id)
		__ofono_netreg_remove_status_watch(ntd->netreg,
					ntd->netreg_status_watch_id);

	if (ntd->netreg_watch_id)
		__ofono_modem_remove_atom_watch(ntd->modem,
					ntd->netreg_watch_id);

	dbus_connection_unref(ntd->conn);
	g_free(ntd->mcc);
	g_free(ntd->mnc);
	g_free(ntd);
}

static void nettime_info_received(struct ofono_nettime_context *context,
					struct ofono_network_time *info)
{
	struct nt_data *ntd = context->data;
	struct tm t;

	if (!ntd)
		return;

	ntd->received = nettime_get_monotonic_time();
	ntd->valid = TRUE;
	ntd->dst = info->dst;
	ntd->time_zone = info->utcoff;
	ntd->time_available = nettime_encode_time_format(&t, info);
	if (ntd->time_available) {
		ntd->nw_time_utc = timegm(&t);
	}

	DBG_(ntd, "time: %04d-%02d-%02d %02d:%02d:%02d%c%02d:%02d (DST=%d)",
		info->year, info->mon, info->mday, info->hour,
		info->min, info->sec, info->utcoff > 0 ? '+' : '-',
		abs(info->utcoff) / 3600, (abs(info->utcoff) % 3600) / 60,
		info->dst);
	DBG_(ntd, "UTC timestamp: %li, Received (monotonic time): %li",
		ntd->nw_time_utc, ntd->received);

	if (ntd->mcc && ntd->mnc) {
		DBG_(ntd, "MCC: %s, MNC: %s", ntd->mcc, ntd->mnc);
		nettime_send_signal(ntd);
	} else {
		DBG_(ntd, "no MCC/MNC yet");

		if (ntd->mccmnc_wait_id)
			g_source_remove(ntd->mccmnc_wait_id);

		ntd->mccmnc_wait_id = g_timeout_add(NTD_WAIT_TIMEOUT,
						nettime_timeout_cb, ntd);
	}
}

static int nettime_probe(struct ofono_nettime_context *context)
{
	struct nt_data *ntd = nettime_new(context->modem);
	const char *path = ofono_modem_get_path(context->modem);

	DBG("Network time probe for modem: %p (%s)", context->modem, path);
	if (g_dbus_register_interface(ntd->conn, path,
			OFONO_NETWORK_TIME_INTERFACE, nettime_methods,
			nettime_signals, NULL, ntd, NULL)) {
		context->data = ntd;
		ofono_info("Registered interface %s, path %s",
				OFONO_NETWORK_TIME_INTERFACE, path);
		ofono_modem_add_interface(context->modem,
					OFONO_NETWORK_TIME_INTERFACE);
		return 0;
	} else {
		ofono_error("Could not register interface %s, path %s",
				OFONO_NETWORK_TIME_INTERFACE, path);
		nettime_free(ntd);
		return 1;
	}
}

static void nettime_remove(struct ofono_nettime_context *context)
{
	struct nt_data *ntd = context->data;
	const char *path = ofono_modem_get_path(context->modem);

	DBG("Network time remove for modem: %p (%s)", context->modem, path);
	ofono_modem_remove_interface(context->modem,
					OFONO_NETWORK_TIME_INTERFACE);
	g_dbus_unregister_interface(ntd->conn, path,
					OFONO_NETWORK_TIME_INTERFACE);
	nettime_free(ntd);
}

static struct ofono_nettime_driver driver = {
	.name		= "Network Time",
	.probe		= nettime_probe,
	.remove		= nettime_remove,
	.info_received	= nettime_info_received,
};

static int nettime_init(void)
{
	return ofono_nettime_driver_register(&driver);
}

static void nettime_exit(void)
{
	ofono_nettime_driver_unregister(&driver);
}

OFONO_PLUGIN_DEFINE(nettime, "Network Time Plugin",
			VERSION, OFONO_PLUGIN_PRIORITY_DEFAULT,
			nettime_init, nettime_exit)
