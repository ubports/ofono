/*
 *
 *  oFono - Open Source Telephony - RIL-based devices
 *
 *  Copyright (C) 2008-2011  Intel Corporation. All rights reserved.
 *  Copyright (C) 2012 Canonical Ltd.
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
#include <glib.h>
#include <gril.h>
#include <parcel.h>
#include <gdbus.h>
#include <linux/capability.h>
#include <sys/types.h>
#include <unistd.h>
#include <sys/syscall.h>
#include <sys/prctl.h>

#define OFONO_API_SUBJECT_TO_CHANGE
#include <ofono/plugin.h>
#include <ofono/log.h>
#include <ofono/modem.h>
#include <ofono/devinfo.h>
#include <ofono/phonebook.h>
#include <ofono/netreg.h>
#include <ofono/voicecall.h>
#include <ofono/sms.h>
#include <ofono/cbs.h>
#include <ofono/sim.h>
#include <ofono/ussd.h>
#include <ofono/call-forwarding.h>
#include <ofono/call-settings.h>
#include <ofono/call-barring.h>
#include <ofono/call-meter.h>
#include <ofono/call-volume.h>
#include <ofono/radio-settings.h>
#include <ofono/gprs.h>
#include <ofono/gprs-context.h>
#include <ofono/audio-settings.h>
#include <ofono/types.h>
#include <ofono/message-waiting.h>
#include <ofono/oemraw.h>
#include <ofono/stk.h>

#include "rildev.h"
#include "drivers/rilmodem/rilmodem.h"

#define MAX_POWER_ON_RETRIES	5
#define MAX_SIM_STATUS_RETRIES	15
#define RADIO_ID		1001
#define MAX_PDP_CONTEXTS	2

/* MCE definitions */
#define MCE_SERVICE			"com.nokia.mce"
#define MCE_SIGNAL_IF			"com.nokia.mce.signal"

/* MCE signal definitions */
#define MCE_DISPLAY_SIG			"display_status_ind"

#define MCE_DISPLAY_ON_STRING		"on"

/* transitional state between ON and OFF (3 seconds) */
#define MCE_DISPLAY_DIM_STRING		"dimmed"
#define MCE_DISPLAY_OFF_STRING		"off"

#define RILMODEM_CONF_FILE		"/etc/ofono/ril_subscription.conf"
#define RILSOCK_CONF_GROUP		"cmdsocket"
#define RILSOCK_CONF_PATH		"path"
#define DEFAULT_CMD_SOCK		"/dev/socket/rild"

struct ril_data {
	GRil *modem;
	int power_on_retries;
	int sim_status_retries;
	ofono_bool_t connected;
	ofono_bool_t online;
	ofono_bool_t reported;
	guint timer_id;
};

static guint mce_daemon_watch;
static guint signal_watch;
static DBusConnection *connection;

static int ril_init(void);
static void ril_exit(void);
static int send_get_sim_status(struct ofono_modem *modem);

static void ril_debug(const char *str, void *user_data)
{
	const char *prefix = user_data;

	ofono_info("%s%s", prefix, str);
}

static void sim_status_cb(struct ril_msg *message, gpointer user_data)
{
	DBG("error=%d", message->error);
	struct ofono_modem *modem = user_data;
	struct ril_data *ril = ofono_modem_get_data(modem);
	struct sim_status status;
	struct sim_app *apps[MAX_UICC_APPS];

	/*
	 * ril.h claims this should NEVER fail!
	 * However this isn't quite true.  So,
	 * on anything other than SUCCESS, we
	 * log an error, and schedule another
	 * GET_SIM_STATUS request.
	 */

	if (message->error != RIL_E_SUCCESS) {
		ril->sim_status_retries++;

		ofono_error("GET_SIM_STATUS reques failed: %d; retries: %d",
				message->error, ril->sim_status_retries);

		if (ril->sim_status_retries < MAX_SIM_STATUS_RETRIES)
			ril->timer_id = g_timeout_add_seconds(2, (GSourceFunc)
							send_get_sim_status,
							(gpointer) modem);
		else
			ofono_error("Max retries for GET_SIM_STATUS exceeded!");
	} else {
		/* Returns TRUE if cardstate == PRESENT */
		if (ril_util_parse_sim_status(ril->modem, message,
						&status, apps)) {

			if (status.num_apps)
				ril_util_free_sim_apps(apps, status.num_apps);
		} else {
			ofono_warn("No SIM card present.");
		}

		/*
		 * We cannot power on modem, but we need to get
		 * certain interfaces up to be able to make emergency calls
		 * in offline mode and without SIM
		 */
		ofono_modem_set_powered(modem, TRUE);
	}
}

static int send_get_sim_status(struct ofono_modem *modem)
{
	struct ril_data *ril = ofono_modem_get_data(modem);
	int request = RIL_REQUEST_GET_SIM_STATUS;
	guint ret;

	ril->timer_id = 0;

	ret = g_ril_send(ril->modem, request,
				NULL, 0, sim_status_cb, modem, NULL);

	g_ril_print_request_no_args(ril->modem, ret, request);

	/*
	 * This function is used as a callback function for
	 * g_timeout_add_seconds therefore we must always return FALSE.
	 * The other place where this is called is from ril_connected but it
	 * doesn't even check the return value.
	 */
	return FALSE;
}

static int ril_probe(struct ofono_modem *modem)
{
	DBG("modem: %p", modem);
	struct ril_data *ril = NULL;

	ril = g_try_new0(struct ril_data, 1);
	if (ril == NULL) {
		errno = ENOMEM;
		goto error;
	}

	ril->modem = NULL;

	ofono_modem_set_data(modem, ril);

	return 0;

error:
	g_free(ril);

	return -errno;
}

static void ril_remove(struct ofono_modem *modem)
{
	struct ril_data *ril = ofono_modem_get_data(modem);
	DBG("modem: %p ril: %p", modem, ril);

	ofono_modem_set_data(modem, NULL);

	if (!ril)
		return;

	if (ril->timer_id > 0)
		g_source_remove(ril->timer_id);

	g_ril_unref(ril->modem);

	g_free(ril);

	g_dbus_remove_watch(connection, mce_daemon_watch);

	if (signal_watch > 0)
		g_dbus_remove_watch(connection, signal_watch);
}

static void ril_pre_sim(struct ofono_modem *modem)
{
	DBG("");
	struct ril_data *ril = ofono_modem_get_data(modem);
	ofono_sim_create(modem, 0, "rilmodem", ril->modem);
	ofono_voicecall_create(modem, 0, "rilmodem", ril->modem);
}

static void ril_post_sim(struct ofono_modem *modem)
{
	DBG("");
	struct ril_data *ril = ofono_modem_get_data(modem);
	struct ofono_gprs *gprs;
	struct ofono_gprs_context *gc;
	struct ofono_message_waiting *mw;
	int i;
	/* TODO: this function should setup:
	 *  - stk ( SIM toolkit )
	 */
	ofono_sms_create(modem, 0, "rilmodem", ril->modem);

	gprs = ofono_gprs_create(modem, 0, "rilmodem", ril->modem);
	if (gprs) {
		for (i = 0; i < MAX_PDP_CONTEXTS; i++) {
			gc = ofono_gprs_context_create(modem, 0, "rilmodem",
					ril->modem);
			if (gc == NULL)
				break;

			ofono_gprs_add_context(gprs, gc);
		}
	}

	ofono_radio_settings_create(modem, 0, "rilmodem", ril->modem);
	ofono_phonebook_create(modem, 0, "rilmodem", ril->modem);
	ofono_call_forwarding_create(modem, 0, "rilmodem", ril->modem);
	ofono_call_barring_create(modem, 0, "rilmodem", ril->modem);
	ofono_stk_create(modem, 0, "rilmodem", ril->modem);

	mw = ofono_message_waiting_create(modem);
	if (mw)
		ofono_message_waiting_register(mw);
}

static void ril_post_online(struct ofono_modem *modem)
{
	DBG("");
	struct ril_data *ril = ofono_modem_get_data(modem);

	ofono_call_volume_create(modem, 0, "rilmodem", ril->modem);

	ofono_netreg_create(modem, 0, "rilmodem", ril->modem);
	ofono_ussd_create(modem, 0, "rilmodem", ril->modem);
	ofono_call_settings_create(modem, 0, "rilmodem", ril->modem);
	ofono_oem_raw_create(modem, 0, "rilmodem", ril->modem);
}

static void ril_set_online_cb(struct ril_msg *message, gpointer user_data)
{
	DBG("");
	struct cb_data *cbd = user_data;
	ofono_modem_online_cb_t cb = cbd->cb;

	if (message->error == RIL_E_SUCCESS)
		CALLBACK_WITH_SUCCESS(cb, cbd->data);
	else
		CALLBACK_WITH_FAILURE(cb, cbd->data);
}

static void ril_set_online(struct ofono_modem *modem, ofono_bool_t online,
				ofono_modem_online_cb_t callback, void *data)
{
	DBG("Set online state (online = 1, offline = 0)): %i", online);
	struct ril_data *ril = ofono_modem_get_data(modem);
	struct cb_data *cbd = cb_data_new(callback, data);
	struct parcel rilp;
	int ret = 0;

	parcel_init(&rilp);
	parcel_w_int32(&rilp, 1);	/* Number of params */
	parcel_w_int32(&rilp, online);	/* Radio ON = 1, Radio OFF = 0 */

	ofono_info("%s: RIL_REQUEST_RADIO_POWER %d", __func__, online);
	ret = g_ril_send(ril->modem, RIL_REQUEST_RADIO_POWER, rilp.data,
				rilp.size, ril_set_online_cb, cbd, g_free);

	parcel_free(&rilp);
	DBG("RIL_REQUEST_RADIO_POWER done");
	if (ret <= 0) {
		g_free(cbd);
		CALLBACK_WITH_FAILURE(callback, data);
	} else {
		if (online)
			current_online_state = RIL_ONLINE_PREF;
		else
			current_online_state = RIL_OFFLINE;
	}
}

static int ril_screen_state(struct ofono_modem *modem, ofono_bool_t state)
{
	struct ril_data *ril = ofono_modem_get_data(modem);
	struct parcel rilp;
	int request = RIL_REQUEST_SCREEN_STATE;
	guint ret;

	parcel_init(&rilp);
	parcel_w_int32(&rilp, 1);		/* size of array */
	parcel_w_int32(&rilp, state);	/* screen on/off */

	/* fire and forget i.e. not waiting for the callback*/
	ret = g_ril_send(ril->modem, request, rilp.data,
			 rilp.size, NULL, NULL, NULL);

	g_ril_append_print_buf(ril->modem, "(0)");
	g_ril_print_request(ril->modem, ret, request);

	parcel_free(&rilp);

	return 0;
}

static gboolean display_changed(DBusConnection *conn,
					DBusMessage *message, void *user_data)
{
	DBG("");
	struct ofono_modem *modem = user_data;
	DBusMessageIter iter;
	const char *value;

	if (!dbus_message_iter_init(message, &iter))
		return TRUE;

	dbus_message_iter_get_basic(&iter, &value);
	DBG("Screen state: %s", value);

	if (g_strcmp0(value, MCE_DISPLAY_ON_STRING) == 0)
		ril_screen_state(modem, TRUE);
	else if (g_strcmp0(value, MCE_DISPLAY_OFF_STRING) == 0)
		ril_screen_state(modem, FALSE);
	else
		ril_screen_state(modem, TRUE);	/* Dimmed, interpreted as ON */

	return TRUE;
}

static void mce_connect(DBusConnection *conn, void *user_data)
{
	DBG("");
	signal_watch = g_dbus_add_signal_watch(conn,
						MCE_SERVICE, NULL,
						MCE_SIGNAL_IF,
						MCE_DISPLAY_SIG,
						display_changed,
						user_data, NULL);
}

static void mce_disconnect(DBusConnection *conn, void *user_data)
{
	DBG("");
	g_dbus_remove_watch(conn, signal_watch);
	signal_watch = 0;
}

static void ril_connected(struct ril_msg *message, gpointer user_data)
{
	DBG("");

	struct ofono_modem *modem = (struct ofono_modem *) user_data;
	struct ril_data *ril = ofono_modem_get_data(modem);
	int ril_version = 0;
	struct parcel rilp;

	ril_util_init_parcel(message, &rilp);
	ril_version = parcel_r_int32(&rilp);
	ofono_debug("%s: [UNSOL]< %s, RIL_VERSION %d",
			__func__, ril_unsol_request_to_string(message->req),
			ril_version);

	ril->connected = TRUE;

	send_get_sim_status(modem);

	connection = ofono_dbus_get_connection();
	mce_daemon_watch = g_dbus_add_service_watch(connection, MCE_SERVICE,
				mce_connect, mce_disconnect, modem, NULL);
}

static int create_gril(struct ofono_modem *modem);

static gboolean connect_rild(gpointer user_data)

{
	struct ofono_modem *modem = (struct ofono_modem *) user_data;

	ofono_info("%s: Connecting %p to rild...", __func__, modem);

	if (create_gril(modem) < 0) {
		DBG("Connecting %p to rild failed, retry timer continues...",
				modem);
		return TRUE;
	}


	return FALSE;
}

/* RIL socket callback from g_io channel */
static void gril_disconnected(gpointer user_data)
{
	struct ofono_modem *modem = user_data;
	ofono_error("%s: modem: %p", __func__, modem);
	DBusConnection *conn = ofono_dbus_get_connection();

	if (ofono_modem_is_registered(modem)) {
		mce_disconnect(conn, user_data);
		ril_modem_remove(modem);
	}

}

void ril_switchUser()
{
	if (prctl(PR_SET_KEEPCAPS, 1, 0, 0, 0) < 0)
		ofono_error("%s: prctl(PR_SET_KEEPCAPS) failed:%s,%d",
				__func__, strerror(errno), errno);

	if (setgid(RADIO_ID) < 0)
		ofono_error("%s: setgid(%d) failed:%s,%d",
				__func__, RADIO_ID, strerror(errno), errno);
	if (setuid(RADIO_ID) < 0)
		ofono_error("%s: setuid(%d) failed:%s,%d",
				__func__, RADIO_ID, strerror(errno), errno);

	struct __user_cap_header_struct header;
	struct __user_cap_data_struct cap;
	header.version = _LINUX_CAPABILITY_VERSION;
	header.pid = 0;
	cap.effective = cap.permitted = (1 << CAP_NET_ADMIN)
						| (1 << CAP_NET_RAW);
	cap.inheritable = 0;

	if (syscall(SYS_capset, &header, &cap) < 0)
		ofono_error("%s: syscall(SYS_capset) failed:%s,%d",
				__func__, strerror(errno), errno);

}

/* TODO: Reading RILD socket path by for now from rilmodem .conf file,
 * 	but change this later to StateFs when plans are more concrete.
 * return: Null-terminated path string. Ownership transferred.
 * */
static char *ril_socket_path()
{
	GError *err = NULL;
	GKeyFile *keyfile = NULL;
	char *res = NULL;

	keyfile = g_key_file_new();
	g_key_file_set_list_separator(keyfile, ',');

	if (!g_key_file_load_from_file(keyfile, RILMODEM_CONF_FILE, 0, &err)) {
		if (err) {
			DBG("conf load result: %s", err->message);
			g_error_free(err);
		}
	} else {
		if (g_key_file_has_group(keyfile, RILSOCK_CONF_GROUP)) {
			res = g_key_file_get_string(
				keyfile, RILSOCK_CONF_GROUP, RILSOCK_CONF_PATH, &err);
			if (err) {
				DBG("conf get result: %s", err->message);
				g_error_free(err);
			}
		}
	}

	g_key_file_free(keyfile);

	if (!res) {
		DBG("Falling back to default cmd sock path");
		res = g_strdup(DEFAULT_CMD_SOCK);
	}

	return res;
}

static int create_gril(struct ofono_modem *modem)
{
	DBG(" modem: %p", modem);
	struct ril_data *ril = ofono_modem_get_data(modem);

	/* RIL expects user radio */
	ril_switchUser();

	char *path = ril_socket_path();
	ril->modem = g_ril_new(path);
	g_free(path);
	path = NULL;

	g_ril_set_disconnect_function(ril->modem, gril_disconnected, modem);

	/* NOTE: Since AT modems open a tty, and then call
	 * g_at_chat_new(), they're able to return -EIO if
	 * the first fails, and -ENOMEM if the second fails.
	 * in our case, we already return -EIO if the ril_new
	 * fails.  If this is important, we can create a ril_socket
	 * abstraction... ( probaby not a bad idea ).
	 */

	if (ril->modem == NULL) {
		DBG("g_ril_new() failed to create modem!");
		return -EIO;
	}

	if (getenv("OFONO_RIL_TRACE"))
		g_ril_set_trace(ril->modem, TRUE);

	if (getenv("OFONO_RIL_HEX_TRACE"))
		g_ril_set_debugf(ril->modem, ril_debug, "Device: ");

	g_ril_register(ril->modem, RIL_UNSOL_RIL_CONNECTED,
			ril_connected, modem);

	ofono_devinfo_create(modem, 0, "rilmodem", ril->modem);

	return 0;
}


static int ril_enable(struct ofono_modem *modem)
{
	int ret;
	DBG("");

	ret = create_gril(modem);
	if (ret < 0) {
		DBG("create gril: %d, queue reconnect", ret);
		g_timeout_add_seconds(2,
			connect_rild, modem);
	}

	return -EINPROGRESS;
}

static int ril_disable(struct ofono_modem *modem)
{
	DBG("%p", modem);

	struct ril_data *ril = ofono_modem_get_data(modem);
	struct parcel rilp;
	int request = RIL_REQUEST_RADIO_POWER;
	guint ret;

	parcel_init(&rilp);
	parcel_w_int32(&rilp, 1); /* size of array */
	parcel_w_int32(&rilp, 0); /* POWER=OFF */

	ofono_info("%s: RIL_REQUEST_RADIO_POWER OFF", __func__);
	/* fire and forget i.e. not waiting for the callback*/
	ret = g_ril_send(ril->modem, request, rilp.data,
			 rilp.size, NULL, NULL, NULL);

	g_ril_append_print_buf(ril->modem, "(0)");
	g_ril_print_request(ril->modem, ret, request);

	parcel_free(&rilp);

	/* this will trigger the cleanup of g_io_channel */
	g_ril_unref(ril->modem);
	ril->modem = NULL;

	return 0;
}

static struct ofono_modem_driver ril_driver = {
	.name = "ril",
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
	int retval = ofono_modem_driver_register(&ril_driver);
	if (retval)
		DBG("ofono_modem_driver_register returned: %d", retval);

	return retval;
}

static void ril_exit(void)
{
	ofono_modem_driver_unregister(&ril_driver);
}

OFONO_PLUGIN_DEFINE(ril, "RIL modem plugin", VERSION,
			OFONO_PLUGIN_PRIORITY_DEFAULT, ril_init, ril_exit)
