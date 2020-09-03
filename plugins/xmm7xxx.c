/*
 *
 *  oFono - Open Source Telephony
 *
 *  Copyright (C) 2017  Intel Corporation. All rights reserved.
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
#include <stdio.h>
#include <unistd.h>
#include <fcntl.h>
#include <termios.h>
#include <string.h>
#include <sys/socket.h>

#include <glib.h>
#include <gatchat.h>
#include <gattty.h>

#define OFONO_API_SUBJECT_TO_CHANGE
#include <ofono/plugin.h>
#include <ofono/log.h>
#include <ofono/modem.h>
#include <ofono/devinfo.h>
#include <ofono/netreg.h>
#include <ofono/sim.h>
#include <ofono/gprs.h>
#include <ofono/radio-settings.h>
#include <ofono/gprs-context.h>
#include <ofono/stk.h>
#include <ofono/lte.h>
#include <ofono/ims.h>
#include <ofono/sim-auth.h>
#include <ofono/sms.h>
#include <ofono/phonebook.h>
#include <ofono/netmon.h>

#include <drivers/atmodem/atutil.h>
#include <drivers/atmodem/vendor.h>

#include "ofono.h"
#include "gdbus.h"
#include "util.h"

#define OFONO_COEX_INTERFACE OFONO_SERVICE ".intel.LteCoexistence"
#define OFONO_COEX_AGENT_INTERFACE OFONO_SERVICE ".intel.LteCoexistenceAgent"
#define OFONO_EUICC_LPA_INTERFACE OFONO_SERVICE ".intel.EuiccLpa"

#define NET_BAND_LTE_INVALID 0
#define NET_BAND_LTE_1 101
#define NET_BAND_LTE_43 143
#define BAND_LEN 20
#define MAX_BT_SAFE_VECTOR 15
#define MAX_WL_SAFE_VECTOR 13

static const char *none_prefix[] = { NULL };
static const char *xsimstate_prefix[] = { "+XSIMSTATE:", NULL };
static const char *xnvmplmn_prefix[] = { "+XNVMPLMN:", NULL };
static const char *ccho_prefix[] = { "+CCHO:", NULL };
static const char *cgla_prefix[] = { "+CGLA:", NULL };

struct bt_coex_info {
	int safe_tx_min;
	int safe_tx_max;
	int safe_rx_min;
	int safe_rx_max;
	int safe_vector[MAX_BT_SAFE_VECTOR];
	int num_safe_vector;
};

struct wl_coex_info {
	int safe_tx_min;
	int safe_tx_max;
	int safe_rx_min;
	int safe_rx_max;
	int safe_vector[MAX_BT_SAFE_VECTOR];
	int num_safe_vector;
};

struct coex_agent {
	char *path;
	char *bus;
	guint disconnect_watch;
	ofono_bool_t remove_on_terminate;
	ofono_destroy_func removed_cb;
	void *removed_data;
	DBusMessage *msg;
};

struct xmm7xxx_data {
	GAtChat *chat;		/* AT chat */
	struct ofono_sim *sim;
	ofono_bool_t have_sim;
	unsigned int netreg_watch;
	int xsim_status;
	ofono_bool_t stk_enable;
	ofono_bool_t enable_euicc;
};

/* eUICC Implementation */
#define EUICC_EID_CMD "80e2910006BF3E035C015A00"
#define EUICC_ISDR_AID "A0000005591010FFFFFFFF8900000100"

struct xmm7xxx_euicc {
	GAtChat *chat;
	struct ofono_modem *modem;
	char *eid;
	int channel;
	char *command;
	int length;
	DBusMessage *pending;
	ofono_bool_t is_registered;
};

static void euicc_cleanup(void *data)
{
	struct xmm7xxx_euicc *euicc = data;

	g_free(euicc->command);
	g_free(euicc->eid);

	if (euicc->pending)
		dbus_message_unref(euicc->pending);

	g_free(euicc);
}

static void euicc_release_isdr(struct xmm7xxx_euicc *euicc)
{
	char buff[20];

	snprintf(buff, sizeof(buff), "AT+CCHC=%u", euicc->channel);

	g_at_chat_send(euicc->chat, buff, none_prefix, NULL, NULL, NULL);

	euicc->channel = -1;
	g_free(euicc->command);
	euicc->command = NULL;
	euicc->length = 0;
}

static void euicc_pending_reply(struct xmm7xxx_euicc *euicc,
							const char *resp)
{
	DBusMessage *reply;
	DBusMessageIter iter, array;
	unsigned char *response = NULL;
	long length;
	int bufferBytesSize = strlen(resp) / 2;

	reply = dbus_message_new_method_return(euicc->pending);
	if (reply == NULL)
		goto done;

	response = g_new0(unsigned char, bufferBytesSize);
	decode_hex_own_buf(resp, strlen(resp),  &length, '\0', response );

	dbus_message_iter_init_append(reply, &iter);
	dbus_message_iter_open_container(&iter, DBUS_TYPE_ARRAY,
				DBUS_TYPE_BYTE_AS_STRING, &array);
	dbus_message_iter_append_fixed_array(&array, DBUS_TYPE_BYTE,
					&response, length);
	dbus_message_iter_close_container(&iter, &array);

	g_free(response);
done:
	__ofono_dbus_pending_reply(&euicc->pending, reply);
}

static DBusMessage *euicc_get_properties(DBusConnection *conn,
						DBusMessage *msg, void *data)
{
	struct xmm7xxx_euicc *euicc = data;
	DBusMessage *reply;
	DBusMessageIter iter;
	DBusMessageIter dict;
	const char *eid = NULL;

	reply = dbus_message_new_method_return(msg);
	if (reply == NULL)
		return NULL;

	dbus_message_iter_init_append(reply, &iter);

	dbus_message_iter_open_container(&iter, DBUS_TYPE_ARRAY,
					OFONO_PROPERTIES_ARRAY_SIGNATURE,
					&dict);

	eid = euicc->eid;
	ofono_dbus_dict_append(&dict, "EID", DBUS_TYPE_STRING, &eid);

	dbus_message_iter_close_container(&iter, &dict);

	return reply;
}

static DBusMessage *euicc_transmit_pdu(DBusConnection *conn,
					DBusMessage *msg, void *data);
static DBusMessage *euicc_select_isdr_req(DBusConnection *conn,
						DBusMessage *msg, void *data);
static DBusMessage *euicc_release_isdr_req(DBusConnection *conn,
						DBusMessage *msg, void *data);

static const GDBusMethodTable euicc_methods[] = {
	{ GDBUS_ASYNC_METHOD("TransmitLpaApdu",
			GDBUS_ARGS({ "pdu", "ay" }),
			GDBUS_ARGS({ "pdu", "ay" }),
			euicc_transmit_pdu) },
	{ GDBUS_ASYNC_METHOD("SelectISDR",
			NULL, NULL, euicc_select_isdr_req) },
	{ GDBUS_ASYNC_METHOD("ReleaseISDR",
			NULL, NULL, euicc_release_isdr_req) },
	{ GDBUS_ASYNC_METHOD("GetProperties",
			NULL, GDBUS_ARGS({ "properties", "a{sv}" }),
			euicc_get_properties) },
	{ }
};

static const GDBusSignalTable euicc_signals[] = {
	{ GDBUS_SIGNAL("PropertyChanged",
			GDBUS_ARGS({ "name", "s" }, { "value", "v" })) },
	{ }
};

static void euicc_register(struct xmm7xxx_euicc *euicc)
{
	DBusConnection *conn = ofono_dbus_get_connection();
	const char *path = ofono_modem_get_path(euicc->modem);

	DBG("euicc_register");

	if (!g_dbus_register_interface(conn, path, OFONO_EUICC_LPA_INTERFACE,
					euicc_methods,
					euicc_signals,
					NULL, euicc, euicc_cleanup)) {
		ofono_error("Could not register %s interface under %s",
					OFONO_EUICC_LPA_INTERFACE, path);
		return;
	}

	ofono_modem_add_interface(euicc->modem, OFONO_EUICC_LPA_INTERFACE);
	euicc->is_registered = TRUE;

	ofono_dbus_signal_property_changed(conn, path,
			OFONO_EUICC_LPA_INTERFACE, "EID",
			DBUS_TYPE_STRING, &euicc->eid);
}

static void euicc_send_cmd_cb(gboolean ok, GAtResult *result,
					gpointer user_data)
{
	struct xmm7xxx_euicc *euicc = user_data;
	GAtResultIter iter;
	int length;
	const char *resp;

	DBG("ok %d", ok);

	if (!ok) {
		g_free(euicc->command);

		if (!euicc->is_registered) {
			g_free(euicc->eid);
			g_free(euicc);
		}

		return;
	}

	DBG("Success");

	g_at_result_iter_init(&iter, result);
	DBG("Iter init");

	if (!g_at_result_iter_next(&iter, "+CGLA:"))
		return;

	DBG("CGLA");

	if (!g_at_result_iter_next_number(&iter, &length))
		return;

	DBG("length = %d", length);

	if (!g_at_result_iter_next_string(&iter, &resp))
		return;

	DBG("resp = %s", resp);

	if (!euicc->is_registered) {
		g_free(euicc->eid);
		euicc->eid = g_strdup(resp+10);
		euicc_release_isdr(euicc);

		/* eid is present register interface*/
		euicc_register(euicc);
	}

	DBG("pending = %p", euicc->pending);

	if (euicc->pending)
		euicc_pending_reply(euicc, resp);
}

static void euicc_send_cmd(struct xmm7xxx_euicc *euicc)
{
	char *buff = g_new0(char, euicc->length + 20);

	sprintf(buff, "AT+CGLA=%u,%u,\"%s\"",
		euicc->channel, euicc->length, euicc->command);

	g_at_chat_send(euicc->chat, buff, cgla_prefix,
			euicc_send_cmd_cb, euicc, NULL);

	g_free(buff);
}

static void euicc_select_isdr_cb(gboolean ok, GAtResult *result,
							gpointer user_data)
{
	struct xmm7xxx_euicc *euicc = user_data;
	GAtResultIter iter;

	DBG("ok %d", ok);

	if (!ok) {
		g_free (euicc->command);

		if (!euicc->is_registered) {
			g_free(euicc->eid);
			g_free(euicc);
		}

		return;
	}

	g_at_result_iter_init(&iter, result);

	if (!g_at_result_iter_next(&iter, "+CCHO:"))
		return;

	g_at_result_iter_next_number(&iter, &euicc->channel);

	if (!euicc->is_registered)
		euicc_send_cmd(euicc);

	if (euicc->pending)
		__ofono_dbus_pending_reply(&euicc->pending,
				dbus_message_new_method_return(euicc->pending));
}

static void euicc_select_isdr(struct xmm7xxx_euicc *euicc)
{
	char buff[50];

	snprintf(buff, sizeof(buff), "AT+CCHO=\"%s\"", EUICC_ISDR_AID);

	g_at_chat_send(euicc->chat, buff, ccho_prefix,
			euicc_select_isdr_cb, euicc, NULL);
}

static DBusMessage *euicc_transmit_pdu(DBusConnection *conn,
					DBusMessage *msg, void *data)
{
	struct xmm7xxx_euicc *euicc = data;
	DBusMessageIter iter, array;
	const unsigned char *command;
	int length;

	DBG("euicc_transmit_pdu");

	if (euicc->pending)
		return __ofono_error_busy(msg);

	if (euicc->channel < 0)
		return __ofono_error_not_available(msg);

	if (!dbus_message_iter_init(msg, &iter))
		return __ofono_error_invalid_args(msg);

	if (dbus_message_iter_get_arg_type(&iter) != DBUS_TYPE_ARRAY)
		return __ofono_error_invalid_args(msg);

	dbus_message_iter_recurse(&iter, &array);

	if (dbus_message_iter_get_arg_type(&array) != DBUS_TYPE_BYTE)
		return __ofono_error_invalid_args(msg);

	dbus_message_iter_get_fixed_array(&array, &command, &length);

	g_free(euicc->command);
	euicc->length = length * 2;
	euicc->command = g_new0(char, euicc->length + 1);
	encode_hex_own_buf(command,(long)length,0, euicc->command);
	euicc->pending = dbus_message_ref(msg);

	euicc_send_cmd(euicc);

	return NULL;
}

static DBusMessage *euicc_select_isdr_req(DBusConnection *conn,
					DBusMessage *msg, void *data)
{
	struct xmm7xxx_euicc *euicc = data;

	DBG("euicc_select_isdr_req");

	if (euicc->pending)
		return __ofono_error_busy(msg);

	euicc_select_isdr(euicc);

	euicc->pending = dbus_message_ref(msg);

	return NULL;
}

static DBusMessage *euicc_release_isdr_req(DBusConnection *conn,
						DBusMessage *msg, void *data)
{
	struct xmm7xxx_euicc *euicc = data;

	DBG("euicc_release_isdr_req");

	if (euicc->pending)
		return __ofono_error_busy(msg);

	euicc_release_isdr(euicc);

	return dbus_message_new_method_return(msg);
}

static void euicc_update_eid(struct xmm7xxx_euicc *euicc)
{
	g_free(euicc->command);
	euicc->command = g_strdup(EUICC_EID_CMD);
	euicc->length = sizeof(EUICC_EID_CMD) - 1;

	euicc_select_isdr(euicc);
}

static void xmm_euicc_enable(struct ofono_modem *modem, void *data)
{
	struct xmm7xxx_euicc *euicc = g_new0(struct xmm7xxx_euicc, 1);

	DBG("euicc enable");

	euicc->chat = data;
	euicc->modem = modem;
	euicc->eid = g_strdup("INVALID");
	euicc->channel = -1;
	euicc->command = NULL;
	euicc->pending = NULL;
	euicc->is_registered = FALSE;

	euicc_update_eid(euicc);
}

static void xmm_euicc_disable(struct ofono_modem *modem)
{
	DBusConnection *conn = ofono_dbus_get_connection();
	const char *path = ofono_modem_get_path(modem);

	if (g_dbus_unregister_interface(conn, path, OFONO_EUICC_LPA_INTERFACE))
		ofono_modem_remove_interface(modem, OFONO_EUICC_LPA_INTERFACE);
}
/* eUICC Implementation Ends */

/* Coex Implementation */
enum wlan_bw {
	WLAN_BW_UNSUPPORTED = -1,
	WLAN_BW_20MHZ = 0,
	WLAN_BW_40MHZ = 1,
	WLAN_BW_80MHZ = 2,
};

struct plmn_hist {
	unsigned short mnc;
	unsigned short mcc;
	unsigned long tdd;
	unsigned long fdd;
	unsigned char bw;
};

struct xmm7xxx_coex {
	GAtChat *chat;
	struct ofono_modem *modem;

	DBusMessage *pending;
	ofono_bool_t bt_active;
	ofono_bool_t wlan_active;
	enum wlan_bw wlan_bw;
	char *lte_band;

	ofono_bool_t pending_bt_active;
	ofono_bool_t pending_wlan_active;
	enum wlan_bw pending_wlan_bw;

	struct coex_agent *session_agent;
};

static ofono_bool_t coex_agent_matches(struct coex_agent *agent,
			const char *path, const char *sender)
{
	return !strcmp(agent->path, path) && !strcmp(agent->bus, sender);
}

static void coex_agent_set_removed_notify(struct coex_agent *agent,
					ofono_destroy_func destroy,
					void *user_data)
{
	agent->removed_cb = destroy;
	agent->removed_data = user_data;
}

static void coex_agent_send_noreply(struct coex_agent *agent,
							const char *method)
{
	DBusConnection *conn = ofono_dbus_get_connection();
	DBusMessage *message;

	message = dbus_message_new_method_call(agent->bus, agent->path,
						OFONO_COEX_INTERFACE,
						method);
	if (message == NULL)
		return;

	dbus_message_set_no_reply(message, TRUE);
	g_dbus_send_message(conn, message);
}

static void coex_agent_send_release(struct coex_agent *agent)
{
	coex_agent_send_noreply(agent, "Release");
}

static void coex_agent_free(struct coex_agent *agent)
{
	DBusConnection *conn = ofono_dbus_get_connection();

	if (agent->disconnect_watch) {
		coex_agent_send_release(agent);

		g_dbus_remove_watch(conn, agent->disconnect_watch);
		agent->disconnect_watch = 0;
	}

	if (agent->removed_cb)
		agent->removed_cb(agent->removed_data);

	g_free(agent->path);
	g_free(agent->bus);
	g_free(agent);
}

static void coex_agent_disconnect_cb(DBusConnection *conn, void *user_data)
{
	struct coex_agent *agent = user_data;

	ofono_debug("Agent exited without calling Unregister");

	agent->disconnect_watch = 0;

	coex_agent_free(agent);
}

static struct coex_agent *coex_agent_new(const char *path, const char *sender,
					ofono_bool_t remove_on_terminate)
{
	struct coex_agent *agent = g_try_new0(struct coex_agent, 1);
	DBusConnection *conn = ofono_dbus_get_connection();

	DBG("");
	if (agent == NULL)
		return NULL;

	agent->path = g_strdup(path);
	agent->bus = g_strdup(sender);

	agent->remove_on_terminate = remove_on_terminate;

	agent->disconnect_watch = g_dbus_add_disconnect_watch(conn, sender,
						coex_agent_disconnect_cb,
						agent, NULL);

	return agent;
}

static int coex_agent_coex_wlan_notify(struct coex_agent *agent,
					const struct wl_coex_info wlan_info)
{
	DBusConnection *conn = ofono_dbus_get_connection();
	DBusMessageIter wl_args, wl_dict, wl_array;
	const dbus_int32_t *pwl_array = wlan_info.safe_vector;
	dbus_int32_t value;

	agent->msg = dbus_message_new_method_call(agent->bus, agent->path,
						OFONO_COEX_AGENT_INTERFACE,
						"ReceiveWiFiNotification");
	if (agent->msg == NULL)
		return -ENOMEM;

	dbus_message_iter_init_append(agent->msg, &wl_args);

	dbus_message_iter_open_container(&wl_args, DBUS_TYPE_ARRAY,
				DBUS_TYPE_INT32_AS_STRING, &wl_array);
	dbus_message_iter_append_fixed_array(&wl_array, DBUS_TYPE_INT32,
					&pwl_array, MAX_WL_SAFE_VECTOR);

	dbus_message_iter_close_container(&wl_args, &wl_array);

	dbus_message_iter_open_container(&wl_args, DBUS_TYPE_ARRAY,
							"{sv}", &wl_dict);

	value = wlan_info.safe_tx_min;
	ofono_dbus_dict_append(&wl_dict, "SafeTxMin", DBUS_TYPE_UINT32, &value);
	value = wlan_info.safe_tx_max;
	ofono_dbus_dict_append(&wl_dict, "SafeTxMax", DBUS_TYPE_UINT32, &value);
	value = wlan_info.safe_rx_min;
	ofono_dbus_dict_append(&wl_dict, "SafeRxMin", DBUS_TYPE_UINT32, &value);
	value = wlan_info.safe_rx_max;
	ofono_dbus_dict_append(&wl_dict, "SafeRxMax", DBUS_TYPE_UINT32, &value);
	value = wlan_info.num_safe_vector;
	ofono_dbus_dict_append(&wl_dict, "NumSafeVector",
					DBUS_TYPE_UINT32, &value);

	dbus_message_iter_close_container(&wl_args, &wl_dict);
	dbus_message_set_no_reply(agent->msg, TRUE);

	if (dbus_connection_send(conn, agent->msg, NULL) == FALSE)
		return -EIO;

	dbus_message_unref(agent->msg);

	return 0;
}

static int coex_agent_coex_bt_notify(struct coex_agent *agent,
					const struct bt_coex_info bt_info)
{
	DBusConnection *conn = ofono_dbus_get_connection();
	DBusMessageIter bt_args, bt_dict, bt_array;
	const dbus_int32_t *pbt_array = bt_info.safe_vector;
	int len = MAX_BT_SAFE_VECTOR;
	dbus_int32_t value;

	agent->msg = dbus_message_new_method_call(agent->bus, agent->path,
						OFONO_COEX_AGENT_INTERFACE,
						"ReceiveBTNotification");

	if (agent->msg == NULL)
		return -ENOMEM;

	pbt_array = bt_info.safe_vector;

	dbus_message_iter_init_append(agent->msg, &bt_args);

	dbus_message_iter_open_container(&bt_args, DBUS_TYPE_ARRAY,
					DBUS_TYPE_INT32_AS_STRING, &bt_array);

	dbus_message_iter_append_fixed_array(&bt_array, DBUS_TYPE_INT32,
					&pbt_array, len);

	dbus_message_iter_close_container(&bt_args, &bt_array);

	dbus_message_iter_open_container(&bt_args,
					DBUS_TYPE_ARRAY, "{sv}", &bt_dict);

	value = bt_info.safe_tx_min;
	DBG("value = %d", value);
	ofono_dbus_dict_append(&bt_dict, "SafeTxMin", DBUS_TYPE_UINT32, &value);

	value = bt_info.safe_tx_max;
	DBG("value = %d", value);
	ofono_dbus_dict_append(&bt_dict, "SafeTxMax", DBUS_TYPE_UINT32, &value);

	value = bt_info.safe_rx_min;
	DBG("value = %d", value);
	ofono_dbus_dict_append(&bt_dict, "SafeRxMin", DBUS_TYPE_UINT32, &value);

	value = bt_info.safe_rx_max;
	DBG("value = %d", value);
	ofono_dbus_dict_append(&bt_dict, "SafeRxMax", DBUS_TYPE_UINT32, &value);

	value = bt_info.num_safe_vector;
	DBG("value = %d", value);
	ofono_dbus_dict_append(&bt_dict, "NumSafeVector",
						DBUS_TYPE_UINT32, &value);

	dbus_message_iter_close_container(&bt_args, &bt_dict);

	if (dbus_connection_send(conn, agent->msg, NULL) == FALSE)
		return -EIO;

	dbus_message_unref(agent->msg);

	return 0;
}

static gboolean coex_wlan_bw_from_string(const char *str,
							enum wlan_bw *band)
{
	if (g_str_equal(str, "20")) {
		*band = WLAN_BW_20MHZ;
		return TRUE;
	} else if (g_str_equal(str, "40")) {
		*band = WLAN_BW_40MHZ;
		return TRUE;
	} else if (g_str_equal(str, "80")) {
		*band = WLAN_BW_80MHZ;
		return TRUE;
	} else
		*band = WLAN_BW_UNSUPPORTED;

	return FALSE;
}

static const char *wlan_bw_to_string(int band)
{
	switch (band) {
	case WLAN_BW_20MHZ:
		return "20MHz";
	case WLAN_BW_40MHZ:
		return "40MHz";
	case WLAN_BW_80MHZ:
		return "80MHz";
	case WLAN_BW_UNSUPPORTED:
		return "UnSupported";
	}

	return "";
}

static void xmm_get_band_string(int lte_band, char *band)
{
	int band_lte;

	band_lte = lte_band - NET_BAND_LTE_1 + 1;

	if (lte_band >= NET_BAND_LTE_1 && lte_band <= NET_BAND_LTE_43)
		sprintf(band, "BAND_LTE_%d", band_lte);
	else
		sprintf(band, "INVALID");
}

static DBusMessage *coex_get_properties(DBusConnection *conn,
					DBusMessage *msg, void *data)
{
	struct xmm7xxx_coex *coex = data;
	DBusMessage *reply;
	DBusMessageIter iter;
	DBusMessageIter dict;
	dbus_bool_t value;
	const char *band = NULL;

	reply = dbus_message_new_method_return(msg);
	if (reply == NULL)
		return NULL;

	dbus_message_iter_init_append(reply, &iter);

	dbus_message_iter_open_container(&iter, DBUS_TYPE_ARRAY,
					OFONO_PROPERTIES_ARRAY_SIGNATURE,
					&dict);

	value = coex->bt_active;
	ofono_dbus_dict_append(&dict, "BTActive",
				DBUS_TYPE_BOOLEAN, &value);

	value = coex->wlan_active;
	ofono_dbus_dict_append(&dict, "WLANActive",
				DBUS_TYPE_BOOLEAN, &value);

	band = wlan_bw_to_string(coex->wlan_bw);
	ofono_dbus_dict_append(&dict, "WLANBandwidth",
				DBUS_TYPE_STRING, &band);

	band = coex->lte_band;
	ofono_dbus_dict_append(&dict, "Band", DBUS_TYPE_STRING, &band);

	dbus_message_iter_close_container(&iter, &dict);

	return reply;
}

static void coex_set_params_cb(gboolean ok, GAtResult *result,
					gpointer user_data)
{
	struct xmm7xxx_coex *coex = user_data;
	DBusMessage *reply;
	DBusConnection *conn = ofono_dbus_get_connection();
	const char *path = ofono_modem_get_path(coex->modem);

	DBG("ok %d", ok);

	if (!ok) {
		coex->pending_bt_active = coex->bt_active;
		coex->pending_wlan_active = coex->wlan_active;
		coex->pending_wlan_bw = coex->wlan_bw;
		reply = __ofono_error_failed(coex->pending);
		__ofono_dbus_pending_reply(&coex->pending, reply);
		return;
	}

	reply = dbus_message_new_method_return(coex->pending);
	__ofono_dbus_pending_reply(&coex->pending, reply);

	if (coex->bt_active != coex->pending_bt_active) {
		coex->bt_active = coex->pending_bt_active;
		ofono_dbus_signal_property_changed(conn, path,
				OFONO_COEX_INTERFACE, "BTActive",
				DBUS_TYPE_BOOLEAN, &coex->bt_active);
	}

	if (coex->wlan_active != coex->pending_wlan_active) {
		coex->wlan_active = coex->pending_wlan_active;
		ofono_dbus_signal_property_changed(conn, path,
				OFONO_COEX_INTERFACE, "WLANActive",
				DBUS_TYPE_BOOLEAN, &coex->wlan_active);
	}

	if (coex->wlan_bw != coex->pending_wlan_bw) {
		const char *str_band = wlan_bw_to_string(coex->wlan_bw);

		coex->wlan_bw = coex->pending_wlan_bw;
		ofono_dbus_signal_property_changed(conn, path,
				OFONO_COEX_INTERFACE, "WLANBandwidth",
				DBUS_TYPE_STRING, &str_band);
	}
}

static void coex_set_params(struct xmm7xxx_coex *coex, ofono_bool_t bt_active,
					ofono_bool_t wlan_active, int wlan_bw)
{
	char buf[64];
	DBusMessage *reply;

	DBG("");
	sprintf(buf, "AT+XNRTCWS=65535,%u,%u,%u", (int)wlan_active,
					wlan_bw, bt_active);

	if (g_at_chat_send(coex->chat, buf, none_prefix,
				coex_set_params_cb, coex, NULL) > 0)
		return;

	coex->pending_bt_active = coex->bt_active;
	coex->pending_wlan_active = coex->wlan_active;
	coex->pending_wlan_bw = coex->wlan_bw;
	reply = __ofono_error_failed(coex->pending);
	__ofono_dbus_pending_reply(&coex->pending, reply);
}

static DBusMessage *coex_set_property(DBusConnection *conn,
					DBusMessage *msg, void *data)
{
	struct xmm7xxx_coex *coex = data;
	DBusMessageIter iter;
	DBusMessageIter var;
	const char *property;
	dbus_bool_t value;

	if (coex->pending)
		return __ofono_error_busy(msg);

	if (!dbus_message_iter_init(msg, &iter))
		return __ofono_error_invalid_args(msg);

	if (dbus_message_iter_get_arg_type(&iter) != DBUS_TYPE_STRING)
		return __ofono_error_invalid_args(msg);

	dbus_message_iter_get_basic(&iter, &property);
	dbus_message_iter_next(&iter);

	if (dbus_message_iter_get_arg_type(&iter) != DBUS_TYPE_VARIANT)
		return __ofono_error_invalid_args(msg);

	dbus_message_iter_recurse(&iter, &var);

	if (!strcmp(property, "BTActive")) {
		if (dbus_message_iter_get_arg_type(&var) != DBUS_TYPE_BOOLEAN)
			return __ofono_error_invalid_args(msg);

		dbus_message_iter_get_basic(&var, &value);

		if (coex->bt_active == (ofono_bool_t) value)
			return dbus_message_new_method_return(msg);

		coex->pending_bt_active = value;
		coex->pending = dbus_message_ref(msg);

		coex_set_params(coex, value, coex->wlan_active, coex->wlan_bw);
		return NULL;
	} else if (!strcmp(property, "WLANActive")) {
		if (dbus_message_iter_get_arg_type(&var) != DBUS_TYPE_BOOLEAN)
			return __ofono_error_invalid_args(msg);

		dbus_message_iter_get_basic(&var, &value);

		if (coex->wlan_active == (ofono_bool_t) value)
			return dbus_message_new_method_return(msg);

		coex->pending_wlan_active = value;
		coex->pending = dbus_message_ref(msg);

		coex_set_params(coex, coex->bt_active, value, coex->wlan_bw);
		return NULL;
	} else if (g_strcmp0(property, "WLANBandwidth") == 0) {
		const char *value;
		enum wlan_bw band;

		if (dbus_message_iter_get_arg_type(&var) != DBUS_TYPE_STRING)
			return __ofono_error_invalid_args(msg);

		dbus_message_iter_get_basic(&var, &value);
		if (coex_wlan_bw_from_string(value, &band) == FALSE)
			return __ofono_error_invalid_args(msg);

		if (coex->wlan_bw == band)
			return dbus_message_new_method_return(msg);

		coex->pending_wlan_bw = band;
		coex->pending = dbus_message_ref(msg);

		coex_set_params(coex, coex->bt_active, coex->wlan_active, band);
		return NULL;
	} else {
		return __ofono_error_invalid_args(msg);
	}
}

static void coex_default_agent_notify(gpointer user_data)
{
	struct xmm7xxx_coex *coex = user_data;

	g_at_chat_send(coex->chat, "AT+XNRTCWS=0", none_prefix,
				NULL, NULL, NULL);

	coex->session_agent = NULL;
}

static DBusMessage *coex_register_agent(DBusConnection *conn,
					DBusMessage *msg, void *data)
{
	struct xmm7xxx_coex *coex = data;
	const char *agent_path;

	if (coex->session_agent) {
		DBG("Coexistence agent already registered");
		return __ofono_error_busy(msg);
	}

	if (dbus_message_get_args(msg, NULL,
				DBUS_TYPE_OBJECT_PATH, &agent_path,
				DBUS_TYPE_INVALID) == FALSE)
		return __ofono_error_invalid_args(msg);

	if (!dbus_validate_path(agent_path, NULL))
		return __ofono_error_invalid_format(msg);

	coex->session_agent = coex_agent_new(agent_path,
					dbus_message_get_sender(msg),
					FALSE);

	if (coex->session_agent == NULL)
		return __ofono_error_failed(msg);

	coex_agent_set_removed_notify(coex->session_agent,
				coex_default_agent_notify, coex);

	return dbus_message_new_method_return(msg);
}

static DBusMessage *coex_unregister_agent(DBusConnection *conn,
						DBusMessage *msg, void *data)
{
	struct xmm7xxx_coex *coex = data;
	const char *agent_path;
	const char *agent_bus = dbus_message_get_sender(msg);

	if (dbus_message_get_args(msg, NULL,
					DBUS_TYPE_OBJECT_PATH, &agent_path,
					DBUS_TYPE_INVALID) == FALSE)
		return __ofono_error_invalid_args(msg);

	if (coex->session_agent == NULL)
		return __ofono_error_failed(msg);

	if (!coex_agent_matches(coex->session_agent, agent_path, agent_bus))
		return __ofono_error_failed(msg);

	coex_agent_send_release(coex->session_agent);
	coex_agent_free(coex->session_agent);

	g_at_chat_send(coex->chat, "AT+XNRTCWS=0", none_prefix,
				NULL, NULL, NULL);

	return dbus_message_new_method_return(msg);
}

static void append_plmn_properties(struct plmn_hist *list,
					DBusMessageIter *dict)
{
	ofono_dbus_dict_append(dict, "MobileCountryCode",
			DBUS_TYPE_UINT16, &list->mcc);
	ofono_dbus_dict_append(dict, "MobileNetworkCode",
			DBUS_TYPE_UINT16, &list->mnc);
	ofono_dbus_dict_append(dict, "LteBandsFDD",
			DBUS_TYPE_UINT32, &list->fdd);
	ofono_dbus_dict_append(dict, "LteBandsTDD",
			DBUS_TYPE_UINT32, &list->tdd);
	ofono_dbus_dict_append(dict, "ChannelBandwidth",
			DBUS_TYPE_UINT32, &list->bw);
}

static void append_plmn_history_struct_list(struct plmn_hist *list,
						DBusMessageIter *arr)
{
	DBusMessageIter iter;
	DBusMessageIter dict;

	dbus_message_iter_open_container(arr, DBUS_TYPE_STRUCT, NULL, &iter);

	dbus_message_iter_open_container(&iter, DBUS_TYPE_ARRAY,
					OFONO_PROPERTIES_ARRAY_SIGNATURE,
					&dict);

	append_plmn_properties(list, &dict);

	dbus_message_iter_close_container(&iter, &dict);

	dbus_message_iter_close_container(arr, &iter);
}

static void coex_get_plmn_history_cb(gboolean ok, GAtResult *result,
					gpointer user_data)
{
	struct xmm7xxx_coex *coex = user_data;
	struct plmn_hist *list = NULL;
	GAtResultIter iter;
	int list_size = 0, count;
	DBusMessage *reply;
	DBusMessageIter itr, arr;
	int value;

	DBG("ok %d", ok);

	if (!ok) {
		__ofono_dbus_pending_reply(&coex->pending,
				__ofono_error_failed(coex->pending));
		return;
	}

	g_at_result_iter_init(&iter, result);

	while (g_at_result_iter_next(&iter, "+XNVMPLMN:")) {
		if (!list_size)
			list = g_new0(struct plmn_hist, ++list_size);
		else
			list = g_renew(struct plmn_hist, list, ++list_size);

		g_at_result_iter_next_number(&iter, &value);
		list[list_size - 1].mcc = value;
		g_at_result_iter_next_number(&iter, &value);
		list[list_size - 1].mnc = value;
		g_at_result_iter_next_number(&iter, &value);
		list[list_size - 1].fdd = value;
		g_at_result_iter_next_number(&iter, &value);
		list[list_size - 1].tdd = value;
		g_at_result_iter_next_number(&iter, &value);
		list[list_size - 1].bw = value;

		DBG("list_size = %d", list_size);
	}

	reply = dbus_message_new_method_return(coex->pending);
	dbus_message_iter_init_append(reply, &itr);

	dbus_message_iter_open_container(&itr, DBUS_TYPE_ARRAY,
				DBUS_STRUCT_BEGIN_CHAR_AS_STRING
				DBUS_TYPE_ARRAY_AS_STRING
				DBUS_DICT_ENTRY_BEGIN_CHAR_AS_STRING
				DBUS_TYPE_STRING_AS_STRING
				DBUS_TYPE_VARIANT_AS_STRING
				DBUS_DICT_ENTRY_END_CHAR_AS_STRING
				DBUS_STRUCT_END_CHAR_AS_STRING,
				&arr);

	for (count = 0; count < list_size; count++)
		append_plmn_history_struct_list(list, &arr);

	dbus_message_iter_close_container(&itr, &arr);

	reply = dbus_message_new_method_return(coex->pending);
	__ofono_dbus_pending_reply(&coex->pending, reply);

	g_free(list);
}

static DBusMessage *coex_get_plmn_history(DBusConnection *conn,
			DBusMessage *msg, void *data)
{
	struct xmm7xxx_coex *coex = data;

	if (coex->pending)
		return __ofono_error_busy(msg);

	if (!g_at_chat_send(coex->chat, "AT+XNVMPLMN=2,2", xnvmplmn_prefix,
				coex_get_plmn_history_cb, coex, NULL))
		return __ofono_error_failed(msg);

	coex->pending = dbus_message_ref(msg);
	return NULL;
}

static const GDBusMethodTable coex_methods[] = {
	{ GDBUS_METHOD("GetProperties",
			NULL, GDBUS_ARGS({ "properties", "a{sv}" }),
			coex_get_properties) },
	{ GDBUS_METHOD("SetProperty",
			GDBUS_ARGS({ "property", "s" }, { "value", "v" }),
			NULL, coex_set_property) },
	{ GDBUS_METHOD("RegisterAgent",
			GDBUS_ARGS({ "path", "o" }), NULL,
			coex_register_agent) },
	{ GDBUS_METHOD("UnregisterAgent",
			GDBUS_ARGS({ "path", "o" }), NULL,
			coex_unregister_agent) },
	{ GDBUS_ASYNC_METHOD("GetPlmnHistory",
			NULL, GDBUS_ARGS({ "plmnhistory", "a(a{sv})" }),
			coex_get_plmn_history) },
	{ }
};

static const GDBusSignalTable coex_signals[] = {
	{ GDBUS_SIGNAL("PropertyChanged",
			GDBUS_ARGS({ "name", "s" }, { "value", "v" })) },
	{ }
};

static void xmm_coex_w_notify(GAtResult *result, gpointer user_data)
{
	struct xmm7xxx_coex *coex = user_data;
	GAtResultIter iter;
	int count;
	struct wl_coex_info wlan;

	g_at_result_iter_init(&iter, result);

	if (!g_at_result_iter_next(&iter, "+XNRTCWSW:"))
		return;

	g_at_result_iter_next_number(&iter, &wlan.safe_rx_min);
	g_at_result_iter_skip_next(&iter);
	g_at_result_iter_skip_next(&iter);
	g_at_result_iter_next_number(&iter, &wlan.safe_rx_max);
	g_at_result_iter_skip_next(&iter);
	g_at_result_iter_skip_next(&iter);
	g_at_result_iter_next_number(&iter, &wlan.safe_tx_min);
	g_at_result_iter_skip_next(&iter);
	g_at_result_iter_skip_next(&iter);
	g_at_result_iter_next_number(&iter, &wlan.safe_tx_max);
	g_at_result_iter_skip_next(&iter);
	g_at_result_iter_skip_next(&iter);
	g_at_result_iter_skip_next(&iter);
	g_at_result_iter_next_number(&iter, &wlan.num_safe_vector);

	for (count = 0; count < wlan.num_safe_vector; count++)
		g_at_result_iter_next_number(&iter, &wlan.safe_vector[count]);

	DBG("WLAN notification");

	if (coex->session_agent)
		coex_agent_coex_wlan_notify(coex->session_agent, wlan);
}

static void xmm_coex_b_notify(GAtResult *result, gpointer user_data)
{
	struct xmm7xxx_coex *coex = user_data;
	GAtResultIter iter;
	struct bt_coex_info bt;
	int count;

	g_at_result_iter_init(&iter, result);

	if (!g_at_result_iter_next(&iter, "+XNRTCWSB:"))
		return;

	g_at_result_iter_next_number(&iter, &bt.safe_rx_min);
	g_at_result_iter_next_number(&iter, &bt.safe_rx_max);
	g_at_result_iter_next_number(&iter, &bt.safe_tx_min);
	g_at_result_iter_next_number(&iter, &bt.safe_tx_max);
	g_at_result_iter_next_number(&iter, &bt.num_safe_vector);

	for (count = 0; count < bt.num_safe_vector; count++)
		g_at_result_iter_next_number(&iter, &bt.safe_vector[count]);

	DBG("BT notification");

	if (coex->session_agent)
		coex_agent_coex_bt_notify(coex->session_agent, bt);
}

static void xmm_lte_band_notify(GAtResult *result, gpointer user_data)
{
	struct xmm7xxx_coex *coex = user_data;
	GAtResultIter iter;
	int lte_band;
	char band[BAND_LEN];
	const char *path = ofono_modem_get_path(coex->modem);
	DBusConnection *conn = ofono_dbus_get_connection();

	g_at_result_iter_init(&iter, result);

	if (!g_at_result_iter_next(&iter, "+XCCINFO:"))
		return;

	g_at_result_iter_skip_next(&iter);
	g_at_result_iter_skip_next(&iter);
	g_at_result_iter_skip_next(&iter);
	g_at_result_iter_skip_next(&iter);

	if (!g_at_result_iter_next_number(&iter, &lte_band))
		return;

	xmm_get_band_string(lte_band, band);
	DBG("band %s", band);

	if (!strcmp(band, coex->lte_band))
		return;

	g_free(coex->lte_band);
	coex->lte_band = g_strdup(band);

	if (coex->lte_band == NULL)
		return;

	ofono_dbus_signal_property_changed(conn, path,
				OFONO_COEX_INTERFACE,
				"Band", DBUS_TYPE_STRING, &coex->lte_band);
}

static void coex_cleanup(void *data)
{
	struct xmm7xxx_coex *coex = data;

	if (coex->pending)
		__ofono_dbus_pending_reply(&coex->pending,
				__ofono_error_canceled(coex->pending));

	if (coex->session_agent) {
		coex_agent_free(coex->session_agent);

		g_at_chat_send(coex->chat, "AT+XNRTCWS=0", none_prefix,
				NULL, NULL, NULL);
	}

	g_free(coex->lte_band);
	g_free(coex);
}

static int xmm_coex_enable(struct ofono_modem *modem, void *data)
{
	struct xmm7xxx_coex *coex = g_new0(struct xmm7xxx_coex, 1);
	DBusConnection *conn = ofono_dbus_get_connection();
	const char *path = ofono_modem_get_path(modem);

	DBG("coex enable");

	coex->chat = data;
	coex->modem = modem;
	coex->bt_active = 0;
	coex->wlan_active = 0;
	coex->wlan_bw = WLAN_BW_20MHZ;
	coex->lte_band = g_strdup("INVALID");
	coex->session_agent = NULL;

	if (!g_at_chat_send(coex->chat, "AT+XCCINFO=1", none_prefix,
				NULL, NULL, NULL))
		goto out;

	if (!g_at_chat_send(coex->chat, "AT+XNRTCWS=7", none_prefix,
				NULL, NULL, NULL))
		goto out;

	if (!g_dbus_register_interface(conn, path, OFONO_COEX_INTERFACE,
					coex_methods,
					coex_signals,
					NULL, coex, coex_cleanup)) {
		ofono_error("Could not register %s interface under %s",
					OFONO_COEX_INTERFACE, path);
		goto out;
	}

	ofono_modem_add_interface(modem, OFONO_COEX_INTERFACE);

	g_at_chat_register(coex->chat, "+XNRTCWSW:", xmm_coex_w_notify,
					FALSE, coex, NULL);
	g_at_chat_register(coex->chat, "+XNRTCWSB:", xmm_coex_b_notify,
					FALSE, coex, NULL);
	g_at_chat_register(coex->chat, "+XCCINFO:", xmm_lte_band_notify,
					FALSE, coex, NULL);
	return 0;

out:
	g_free(coex->lte_band);
	g_free(coex);
	return -EIO;
}

/* Coex Implementation Ends*/

static void xmm7xxx_debug(const char *str, void *user_data)
{
	const char *prefix = user_data;

	ofono_info("%s%s", prefix, str);
}

static GAtChat *open_device(struct ofono_modem *modem,
				const char *key, char *debug)
{
	return at_util_open_device(modem, key, xmm7xxx_debug, debug,
					"Baud", "115200",
					NULL);
}

static void switch_sim_state_status(struct ofono_modem *modem, int status)
{
	struct xmm7xxx_data *data = ofono_modem_get_data(modem);

	DBG("%p, SIM status: %d", modem, status);

	switch (status) {
	case 0:	/* SIM not inserted */
	case 9:	/* SIM removed */
		if (data->have_sim == TRUE) {
			ofono_sim_inserted_notify(data->sim, FALSE);
			data->have_sim = FALSE;
		}
		break;
	case 1: /* SIM inserted, PIN verification needed */
	case 4: /* SIM inserted, PUK verification needed */
		if (data->have_sim == FALSE) {
			ofono_sim_inserted_notify(data->sim, TRUE);
			data->have_sim = TRUE;
		}
		break;
	case 2:	/* SIM inserted, PIN verification not needed - READY */
	case 3:	/* SIM inserted, PIN verified - READY */
	case 7: /* SIM inserted, Ready for ATTACH - READY */
		if (data->have_sim == FALSE) {
			ofono_sim_inserted_notify(data->sim, TRUE);
			data->have_sim = TRUE;
		}

		ofono_sim_initialized_notify(data->sim);
		break;
	case 18:
		data->enable_euicc = TRUE;
		break;
	default:
		ofono_warn("Unknown SIM state %d received", status);
		break;
	}

	data->xsim_status = status;
}

static void xsimstate_notify(GAtResult *result, gpointer user_data)
{
	struct ofono_modem *modem = user_data;
	struct xmm7xxx_data *data = ofono_modem_get_data(modem);
	int status;
	GAtResultIter iter;

	DBG("%p", modem);

	g_at_result_iter_init(&iter, result);

	if (!g_at_result_iter_next(&iter, "+XSIM:"))
		return;

	g_at_result_iter_next_number(&iter, &status);

	DBG("status=%d\n", status);

	if (data->xsim_status != status)
		switch_sim_state_status(modem, status);
}

static void xsimstate_query_cb(gboolean ok, GAtResult *result,
						gpointer user_data)
{
	struct ofono_modem *modem = user_data;
	int status, mode;
	GAtResultIter iter;

	DBG("%p", modem);

	if (!ok)
		return;

	g_at_result_iter_init(&iter, result);

	if (!g_at_result_iter_next(&iter, "+XSIMSTATE:"))
		return;

	if (!g_at_result_iter_next_number(&iter, &mode))
		return;

	if (!g_at_result_iter_next_number(&iter, &status))
		return;

	DBG("mode=%d, status=%d\n", mode, status);

	switch_sim_state_status(modem, status);
}

static void cfun_enable_cb(gboolean ok, GAtResult *result, gpointer user_data)
{
	struct ofono_modem *modem = user_data;
	struct xmm7xxx_data *data = ofono_modem_get_data(modem);

	DBG("%p", modem);

	if (!ok) {
		g_at_chat_unref(data->chat);
		data->chat = NULL;

		ofono_modem_set_powered(modem, FALSE);
		return;
	}

	/*
	 * Switch data carrier detect signal off.
	 * When the DCD is disabled the modem does not hangup anymore
	 * after the data connection.
	 */
	g_at_chat_send(data->chat, "AT&C0", NULL, NULL, NULL, NULL);

	data->have_sim = FALSE;
	data->xsim_status = -1;

	ofono_modem_set_powered(modem, TRUE);

	g_at_chat_register(data->chat, "+XSIM:", xsimstate_notify,
				FALSE, modem, NULL);

	g_at_chat_send(data->chat, "AT+XSIMSTATE=1", none_prefix,
			NULL, NULL, NULL);
	g_at_chat_send(data->chat, "AT+XSIMSTATE?", xsimstate_prefix,
			xsimstate_query_cb, modem, NULL);
}

static void netreg_watch(struct ofono_atom *atom,
				enum ofono_atom_watch_condition cond,
				void *data)
{
	struct ofono_modem *modem = data;
	struct xmm7xxx_data *modem_data = ofono_modem_get_data(modem);
	DBusConnection *conn = ofono_dbus_get_connection();
	const char *path = ofono_modem_get_path(modem);

	if (cond == OFONO_ATOM_WATCH_CONDITION_UNREGISTERED) {
		if (g_dbus_unregister_interface(conn, path,
						OFONO_COEX_INTERFACE))
			ofono_modem_remove_interface(modem,
							OFONO_COEX_INTERFACE);
		return;
	}

	if (cond == OFONO_ATOM_WATCH_CONDITION_REGISTERED) {
		xmm_coex_enable(modem, modem_data->chat);
		return;
	}
}

static int xmm7xxx_enable(struct ofono_modem *modem)
{
	struct xmm7xxx_data *data = ofono_modem_get_data(modem);

	DBG("%p", modem);

	data->chat = open_device(modem, "Modem", "Modem: ");
	if (data->chat == NULL)
		return -EIO;

	/*
	 * Disable command echo and
	 * enable the Extended Error Result Codes
	 */
	g_at_chat_send(data->chat, "ATE0 +CMEE=1", none_prefix,
				NULL, NULL, NULL);

	/* Set phone functionality */
	g_at_chat_send(data->chat, "AT+CFUN=4", none_prefix,
				cfun_enable_cb, modem, NULL);

	data->netreg_watch = __ofono_modem_add_atom_watch(modem,
					OFONO_ATOM_TYPE_NETREG,
					netreg_watch, modem, NULL);

	return -EINPROGRESS;
}

static void cfun_disable_cb(gboolean ok, GAtResult *result, gpointer user_data)
{
	struct ofono_modem *modem = user_data;
	struct xmm7xxx_data *data = ofono_modem_get_data(modem);

	DBG("%p", modem);

	g_at_chat_unref(data->chat);
	data->chat = NULL;

	if (ok)
		ofono_modem_set_powered(modem, FALSE);
}

static int xmm7xxx_disable(struct ofono_modem *modem)
{
	struct xmm7xxx_data *data = ofono_modem_get_data(modem);

	DBG("%p", modem);

	g_at_chat_cancel_all(data->chat);
	g_at_chat_unregister_all(data->chat);

	/* Power down modem */
	g_at_chat_send(data->chat, "AT+CFUN=0", none_prefix,
				cfun_disable_cb, modem, NULL);

	if (data->netreg_watch) {
		__ofono_modem_remove_atom_watch(modem, data->netreg_watch);
		data->netreg_watch = 0;
	}

	xmm_euicc_disable(modem);
	return -EINPROGRESS;
}

static void xmm7xxx_pre_sim(struct ofono_modem *modem)
{
	struct xmm7xxx_data *data = ofono_modem_get_data(modem);

	DBG("%p", modem);

	ofono_devinfo_create(modem, OFONO_VENDOR_IFX, "atmodem", data->chat);
	data->sim = ofono_sim_create(modem, OFONO_VENDOR_XMM, "atmodem",
					data->chat);
	xmm_euicc_enable(modem, data->chat);
	ofono_stk_create(modem, 0, "atmodem", data->chat);
}

static void set_online_cb(gboolean ok, GAtResult *result, gpointer user_data)
{
	struct cb_data *cbd = user_data;
	ofono_modem_online_cb_t cb = cbd->cb;
	struct ofono_error error;
	struct ofono_modem *modem = cbd->data;
	struct xmm7xxx_data *data = ofono_modem_get_data(modem);

	decode_at_error(&error, g_at_result_final_response(result));

	if (data->enable_euicc == TRUE && data->stk_enable == TRUE)
		g_at_chat_send(data->chat, "AT+CFUN=16", none_prefix,
							NULL, NULL, NULL);

	cb(&error, cbd->data);
}

static void xmm7xxx_set_online(struct ofono_modem *modem, ofono_bool_t online,
				ofono_modem_online_cb_t cb, void *user_data)
{
	struct xmm7xxx_data *data = ofono_modem_get_data(modem);
	struct cb_data *cbd = cb_data_new(cb, user_data);
	char const *command = online ? "AT+CFUN=1" : "AT+CFUN=4";

	DBG("modem %p %s", modem, online ? "online" : "offline");
	data->stk_enable = online;

	if (g_at_chat_send(data->chat, command, none_prefix,
					set_online_cb, cbd, g_free) > 0)
		return;

	CALLBACK_WITH_FAILURE(cb, cbd->data);

	g_free(cbd);
}

static void xmm7xxx_post_sim(struct ofono_modem *modem)
{
	struct xmm7xxx_data *data = ofono_modem_get_data(modem);

	ofono_lte_create(modem, 0, "atmodem", data->chat);
	ofono_radio_settings_create(modem, 0, "xmm7modem", data->chat);
	ofono_sim_auth_create(modem);
}

static void xmm7xxx_post_online(struct ofono_modem *modem)
{
	struct xmm7xxx_data *data = ofono_modem_get_data(modem);
	struct ofono_gprs *gprs;
	struct ofono_gprs_context *gc;
	const char *interface = NULL;

	DBG("%p", modem);

	ofono_phonebook_create(modem, 0, "atmodem", data->chat);
	ofono_sms_create(modem, 0, "atmodem", data->chat);

	ofono_netreg_create(modem, OFONO_VENDOR_IFX, "atmodem", data->chat);

	gprs = ofono_gprs_create(modem, OFONO_VENDOR_IFX, "atmodem",
					data->chat);

	interface = ofono_modem_get_string(modem, "NetworkInterface");
	gc = ofono_gprs_context_create(modem, OFONO_VENDOR_XMM, "ifxmodem",
					data->chat);

	if (gprs && gc) {
		ofono_gprs_add_context(gprs, gc);
		ofono_gprs_context_set_interface(gc, interface);
	}

	interface = ofono_modem_get_string(modem, "NetworkInterface2");

	if (interface) {
		gc = ofono_gprs_context_create(modem, OFONO_VENDOR_XMM,
						"ifxmodem", data->chat);

		if (gprs && gc) {
			ofono_gprs_add_context(gprs, gc);
			ofono_gprs_context_set_interface(gc, interface);
		}
	}

	interface = ofono_modem_get_string(modem, "NetworkInterface3");

	if (interface) {
		gc = ofono_gprs_context_create(modem, OFONO_VENDOR_XMM,
						"ifxmodem", data->chat);

		if (gprs && gc) {
			ofono_gprs_add_context(gprs, gc);
			ofono_gprs_context_set_interface(gc, interface);
		}
	}

	ofono_ims_create(modem, "xmm7modem", data->chat);
	ofono_netmon_create(modem, 0, "xmm7modem", data->chat);
}

static int xmm7xxx_probe(struct ofono_modem *modem)
{
	struct xmm7xxx_data *data;

	DBG("%p", modem);

	data = g_try_new0(struct xmm7xxx_data, 1);
	if (data == NULL)
		return -ENOMEM;

	ofono_modem_set_data(modem, data);

	return 0;
}

static void xmm7xxx_remove(struct ofono_modem *modem)
{
	struct xmm7xxx_data *data = ofono_modem_get_data(modem);

	DBG("%p", modem);

	ofono_modem_set_data(modem, NULL);

	/* Cleanup after hot-unplug */
	g_at_chat_unref(data->chat);

	g_free(data);
}

static struct ofono_modem_driver xmm7xxx_driver = {
	.name		= "xmm7xxx",
	.probe		= xmm7xxx_probe,
	.remove		= xmm7xxx_remove,
	.enable		= xmm7xxx_enable,
	.disable	= xmm7xxx_disable,
	.set_online	= xmm7xxx_set_online,
	.pre_sim	= xmm7xxx_pre_sim,
	.post_sim	= xmm7xxx_post_sim,
	.post_online	= xmm7xxx_post_online,
};

static int xmm7xxx_init(void)
{
	DBG("");

	return ofono_modem_driver_register(&xmm7xxx_driver);
}

static void xmm7xxx_exit(void)
{
	ofono_modem_driver_unregister(&xmm7xxx_driver);
}

OFONO_PLUGIN_DEFINE(xmm7xxx, "Intel XMM7xxx driver", VERSION,
		OFONO_PLUGIN_PRIORITY_DEFAULT, xmm7xxx_init, xmm7xxx_exit)
