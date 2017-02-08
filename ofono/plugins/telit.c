/*
 *
 *  oFono - Open Source Telephony
 *
 *  Copyright (C) 2008-2014  Intel Corporation. All rights reserved.
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
#include <ofono/call-barring.h>
#include <ofono/call-forwarding.h>
#include <ofono/call-meter.h>
#include <ofono/call-settings.h>
#include <ofono/devinfo.h>
#include <ofono/message-waiting.h>
#include <ofono/location-reporting.h>
#include <ofono/netreg.h>
#include <ofono/phonebook.h>
#include <ofono/sim.h>
#include <ofono/gprs.h>
#include <ofono/gprs-context.h>
#include <ofono/sms.h>
#include <ofono/ussd.h>
#include <ofono/voicecall.h>

#include <drivers/atmodem/atutil.h>
#include <drivers/atmodem/vendor.h>

static const char *none_prefix[] = { NULL };
static const char *qss_prefix[] = { "#QSS:", NULL };

enum modem_model {
	HE910 = 1,
	UE910,
	LE910,
	UC864,
	UE866,
};

static struct {
	enum modem_model model;
	const char *variant;
	gboolean has_voice;
	gboolean has_gps;
} variants_list[] = {
	{ HE910,	NULL,	FALSE,	FALSE },
	{ HE910,	"G",	TRUE,	TRUE },
	{ HE910,	"GL",	TRUE,	FALSE },
	{ HE910,	"EUR",	TRUE,	FALSE },
	{ HE910,	"NAR",	TRUE,	FALSE },
	{ HE910,	"DG",	FALSE,	TRUE },
	{ HE910,	"EUG",	FALSE,	TRUE },
	{ HE910,	"NAG",	FALSE,	TRUE },
	{ UE910,	NULL,	FALSE,	FALSE },
	{ UE910,	"EUR",	TRUE,	FALSE },
	{ UE910,	"NAR",	TRUE,	FALSE },
	{ LE910,	NULL,	FALSE,	FALSE },
	{ UC864,	NULL,	TRUE,	FALSE },
	{ UC864,	"G",	TRUE,	TRUE },
	{ UC864,	"WD",	FALSE,	FALSE },
	{ UE866,	NULL,	FALSE,	FALSE },
	{ }
};

struct telit_data {
	GAtChat *chat;		/* AT chat */
	GAtChat *modem;		/* Data port */
	struct ofono_sim *sim;
	ofono_bool_t have_sim;
	ofono_bool_t sms_phonebook_added;
	enum modem_model model;
	gboolean has_voice;
	gboolean has_gps;
};

static void telit_debug(const char *str, void *user_data)
{
	const char *prefix = user_data;

	ofono_info("%s%s", prefix, str);
}

static GAtChat *open_device(struct ofono_modem *modem,
				const char *key, char *debug)
{
	const char *device;
	GAtSyntax *syntax;
	GIOChannel *channel;
	GAtChat *chat;
	GHashTable *options;

	device = ofono_modem_get_string(modem, key);
	if (device == NULL)
		return NULL;

	DBG("%s %s", key, device);

	options = g_hash_table_new(g_str_hash, g_str_equal);
	if (options == NULL)
		return NULL;

	g_hash_table_insert(options, "Baud", "115200");
	channel = g_at_tty_open(device, options);
	g_hash_table_destroy(options);

	if (channel == NULL)
		return NULL;

	syntax = g_at_syntax_new_gsm_permissive();
	chat = g_at_chat_new(channel, syntax);
	g_at_syntax_unref(syntax);
	g_io_channel_unref(channel);

	if (chat == NULL)
		return NULL;

	if (getenv("OFONO_AT_DEBUG"))
		g_at_chat_set_debug(chat, telit_debug, debug);

	return chat;
}

static void switch_sim_state_status(struct ofono_modem *modem, int status)
{
	struct telit_data *data = ofono_modem_get_data(modem);

	DBG("%p, SIM status: %d", modem, status);

	switch (status) {
	case 0:	/* SIM not inserted */
		if (data->have_sim == TRUE) {
			ofono_sim_inserted_notify(data->sim, FALSE);
			data->have_sim = FALSE;
			data->sms_phonebook_added = FALSE;
		}
		break;
	case 1:	/* SIM inserted */
	case 2:	/* SIM inserted and PIN unlocked */
		if (data->have_sim == FALSE) {
			ofono_sim_inserted_notify(data->sim, TRUE);
			data->have_sim = TRUE;
		}
		break;
	case 3:	/* SIM inserted, SMS and phonebook ready */
		if (data->have_sim == FALSE) {
			ofono_sim_inserted_notify(data->sim, TRUE);
			data->have_sim = TRUE;
		}

		if (data->sms_phonebook_added == FALSE) {
			ofono_phonebook_create(modem, 0, "atmodem", data->chat);
			ofono_sms_create(modem, 0, "atmodem", data->chat);
			data->sms_phonebook_added = TRUE;
		}
		break;
	default:
		ofono_warn("Unknown SIM state %d received", status);
		break;
	}
}

static void telit_qss_notify(GAtResult *result, gpointer user_data)
{
	struct ofono_modem *modem = user_data;
	int status;
	GAtResultIter iter;

	DBG("%p", modem);

	g_at_result_iter_init(&iter, result);

	if (!g_at_result_iter_next(&iter, "#QSS:"))
		return;

	g_at_result_iter_next_number(&iter, &status);

	switch_sim_state_status(modem, status);
}

static void qss_query_cb(gboolean ok, GAtResult *result, gpointer user_data)
{
	struct ofono_modem *modem = user_data;
	int status, mode;
	GAtResultIter iter;

	DBG("%p", modem);

	if (!ok)
		return;

	g_at_result_iter_init(&iter, result);

	if (!g_at_result_iter_next(&iter, "#QSS:"))
		return;

	if (!g_at_result_iter_next_number(&iter, &mode))
		return;

	if (!g_at_result_iter_next_number(&iter, &status))
		return;

	switch_sim_state_status(modem, status);
}

static void cfun_enable_cb(gboolean ok, GAtResult *result, gpointer user_data)
{
	struct ofono_modem *modem = user_data;
	struct telit_data *data = ofono_modem_get_data(modem);

	DBG("%p", modem);

	if (!ok) {
		g_at_chat_unref(data->chat);
		data->chat = NULL;

		g_at_chat_unref(data->modem);
		data->modem = NULL;

		ofono_modem_set_powered(modem, FALSE);
		return;
	}

	/*
	 * Switch data carrier detect signal off.
	 * When the DCD is disabled the modem does not hangup anymore
	 * after the data connection. We need to do that on both channels.
	 */
	g_at_chat_send(data->chat, "AT&C0", NULL, NULL, NULL, NULL);
	g_at_chat_send(data->modem, "AT&C0", NULL, NULL, NULL, NULL);

	data->have_sim = FALSE;
	data->sms_phonebook_added = FALSE;

	ofono_modem_set_powered(modem, TRUE);

	/*
	 * Tell the modem not to automatically initiate auto-attach
	 * proceedures on its own.
	 */
	g_at_chat_send(data->chat, "AT#AUTOATT=0", none_prefix,
				NULL, NULL, NULL);

	/* Follow sim state */
	g_at_chat_register(data->chat, "#QSS:", telit_qss_notify,
				FALSE, modem, NULL);

	/* Enable sim state notification */
	g_at_chat_send(data->chat, "AT#QSS=2", none_prefix, NULL, NULL, NULL);

	g_at_chat_send(data->chat, "AT#QSS?", qss_prefix,
			qss_query_cb, modem, NULL);
}

static gboolean find_model_variant(struct ofono_modem *modem,
						const char * model_variant)
{
	struct telit_data *data = ofono_modem_get_data(modem);
	char model[32];
	char variant[32];
	gchar **tokens;
	int i;

	if (!model_variant || model_variant[0] == '\0')
		return FALSE;

	DBG("%s", model_variant);

	tokens = g_strsplit(model_variant, "-", 2);

	if (!tokens || !tokens[0] || !tokens[1])
		return FALSE;

	g_strlcpy(model, tokens[0], sizeof(model));
	g_strlcpy(variant, tokens[1], sizeof(variant));
	g_strfreev(tokens);

	if (g_str_equal(model, "HE910"))
		data->model = HE910;
	else if (g_str_equal(model, "UE910"))
		data->model = UE910;
	else if (g_str_equal(model, "LE910"))
		data->model = LE910;
	else if (g_str_equal(model, "UC864"))
		data->model = UC864;
	else if (g_str_equal(model, "UE866"))
		data->model = UE866;
	else
		return FALSE;

	DBG("Model: %s", model);

	for (i = 0; variants_list[i].model; i++) {
		if (variants_list[i].model != data->model)
			continue;

		/* Set model defaults */
		if (variants_list[i].variant == NULL) {
			data->has_voice = variants_list[i].has_voice;
			data->has_gps = variants_list[i].has_gps;
			continue;
		}

		/* Specific variant match */
		if (g_str_equal(variant, variants_list[i].variant)) {
			DBG("Variant: %s", variant);
			data->has_voice = variants_list[i].has_voice;
			data->has_gps = variants_list[i].has_gps;
		}
	}

	return TRUE;
}

static void cfun_gmm_cb(gboolean ok, GAtResult *result, gpointer user_data)
{
	struct ofono_modem *modem = user_data;
	struct telit_data *data = ofono_modem_get_data(modem);
	const char * model_variant;

	DBG("%p", modem);

	if (!ok)
		goto error;

	/* Get +GMM response */
	if (!at_util_parse_attr(result, "", &model_variant))
		goto error;

	/* Try to find modem model and variant */
	if (!find_model_variant(modem, model_variant)) {
		ofono_info("Unknown xE910 model/variant %s", model_variant);
		goto error;
	}

	/* Set phone functionality */
	if (g_at_chat_send(data->chat, "AT+CFUN=1", none_prefix,
				cfun_enable_cb, modem, NULL) > 0)
		return;

error:
	g_at_chat_unref(data->chat);
	data->chat = NULL;

	g_at_chat_unref(data->modem);
	data->modem = NULL;

	ofono_modem_set_powered(modem, FALSE);
}

static int telit_enable(struct ofono_modem *modem)
{
	struct telit_data *data = ofono_modem_get_data(modem);

	DBG("%p", modem);

	data->modem = open_device(modem, "Modem", "Modem: ");
	if (data->modem == NULL)
		return -EINVAL;

	data->chat = open_device(modem, "Aux", "Aux: ");
	if (data->chat == NULL) {
		g_at_chat_unref(data->modem);
		data->modem = NULL;
		return -EIO;
	}

	g_at_chat_set_slave(data->modem, data->chat);

	/*
	 * Disable command echo and
	 * enable the Extended Error Result Codes
	 */
	g_at_chat_send(data->chat, "ATE0 +CMEE=1", none_prefix,
				NULL, NULL, NULL);
	g_at_chat_send(data->modem, "ATE0", none_prefix,
				NULL, NULL, NULL);


	/* Get modem model and variant */
	g_at_chat_send(data->chat, "AT+GMM", NULL,
				cfun_gmm_cb, modem, NULL);


	return -EINPROGRESS;
}

static void cfun_disable_cb(gboolean ok, GAtResult *result, gpointer user_data)
{
	struct ofono_modem *modem = user_data;
	struct telit_data *data = ofono_modem_get_data(modem);

	DBG("%p", modem);

	g_at_chat_unref(data->chat);
	data->chat = NULL;

	if (ok)
		ofono_modem_set_powered(modem, FALSE);
}

static int telit_disable(struct ofono_modem *modem)
{
	struct telit_data *data = ofono_modem_get_data(modem);

	DBG("%p", modem);

	g_at_chat_cancel_all(data->modem);
	g_at_chat_unregister_all(data->modem);
	g_at_chat_unref(data->modem);
	data->modem = NULL;

	g_at_chat_cancel_all(data->chat);
	g_at_chat_unregister_all(data->chat);

	/* Power down modem */
	g_at_chat_send(data->chat, "AT+CFUN=4", none_prefix,
				cfun_disable_cb, modem, NULL);

	return -EINPROGRESS;
}

static void telit_pre_sim(struct ofono_modem *modem)
{
	struct telit_data *data = ofono_modem_get_data(modem);

	DBG("%p", modem);

	ofono_devinfo_create(modem, 0, "atmodem", data->chat);
	data->sim = ofono_sim_create(modem, OFONO_VENDOR_TELIT, "atmodem",
					data->chat);

	if (data->has_gps)
		ofono_location_reporting_create(modem, 0, "telitmodem",
								data->chat);
}

static void telit_post_online(struct ofono_modem *modem)
{
	struct telit_data *data = ofono_modem_get_data(modem);
	struct ofono_gprs *gprs;
	struct ofono_gprs_context *gc;

	DBG("%p", modem);

	ofono_netreg_create(modem, OFONO_VENDOR_TELIT, "atmodem", data->chat);

	if (data->has_voice) {
		struct ofono_message_waiting *mw;

		ofono_voicecall_create(modem, 0, "atmodem", data->chat);
		ofono_ussd_create(modem, 0, "atmodem", data->chat);
		ofono_call_forwarding_create(modem, 0, "atmodem", data->chat);
		ofono_call_settings_create(modem, 0, "atmodem", data->chat);
		ofono_call_meter_create(modem, 0, "atmodem", data->chat);
		ofono_call_barring_create(modem, 0, "atmodem", data->chat);

		mw = ofono_message_waiting_create(modem);
		if (mw)
			ofono_message_waiting_register(mw);
	}

	gprs = ofono_gprs_create(modem, OFONO_VENDOR_TELIT, "atmodem",
					data->chat);

	if (data->model == LE910)
		gc = ofono_gprs_context_create(modem, OFONO_VENDOR_TELIT,
						"telitncmmodem", data->modem);
	else
		gc = ofono_gprs_context_create(modem, 0, "atmodem",
						data->modem);

	if (gprs && gc)
		ofono_gprs_add_context(gprs, gc);
}

static int telit_probe(struct ofono_modem *modem)
{
	struct telit_data *data;

	DBG("%p", modem);

	data = g_try_new0(struct telit_data, 1);
	if (data == NULL)
		return -ENOMEM;

	ofono_modem_set_data(modem, data);

	return 0;
}

static void telit_remove(struct ofono_modem *modem)
{
	struct telit_data *data = ofono_modem_get_data(modem);

	DBG("%p", modem);

	ofono_modem_set_data(modem, NULL);

	/* Cleanup after hot-unplug */
	g_at_chat_unref(data->chat);
	g_at_chat_unref(data->modem);

	g_free(data);
}

static struct ofono_modem_driver telit_driver = {
	.name		= "telit",
	.probe		= telit_probe,
	.remove		= telit_remove,
	.enable		= telit_enable,
	.disable	= telit_disable,
	.pre_sim	= telit_pre_sim,
	.post_online	= telit_post_online,
};

static int telit_init(void)
{
	DBG("");

	return ofono_modem_driver_register(&telit_driver);
}

static void telit_exit(void)
{
	ofono_modem_driver_unregister(&telit_driver);
}

OFONO_PLUGIN_DEFINE(telit, "Telit driver", VERSION,
		OFONO_PLUGIN_PRIORITY_DEFAULT, telit_init, telit_exit)
