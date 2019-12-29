/*
 *
 *  oFono - Open Source Telephony
 *
 *  Copyright (C) 2014  Philip Paeps. All rights reserved.
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

#include <glib.h>
#include <gatchat.h>
#include <gattty.h>
#include <ell/ell.h>

#define OFONO_API_SUBJECT_TO_CHANGE
#include <ofono/plugin.h>
#include <ofono/modem.h>
#include <ofono/devinfo.h>
#include <ofono/netreg.h>
#include <ofono/sim.h>
#include <ofono/gprs.h>
#include <ofono/gprs-context.h>
#include <ofono/netmon.h>
#include <ofono/lte.h>
#include <ofono/sms.h>
#include <ofono/voicecall.h>
#include <ofono/call-forwarding.h>
#include <ofono/call-settings.h>
#include <ofono/call-meter.h>
#include <ofono/call-barring.h>
#include <ofono/message-waiting.h>
#include <ofono/ussd.h>

#include <drivers/atmodem/vendor.h>
#include <drivers/ubloxmodem/ubloxmodem.h>

static const char *uusbconf_prefix[] = { "+UUSBCONF:", NULL };
static const char *none_prefix[] = { NULL };

enum ublox_device_flags {
	UBLOX_DEVICE_F_HIGH_THROUGHPUT_MODE	= (1 << 0),
};

struct ublox_data {
	GAtChat *modem;
	GAtChat *aux;
	enum ofono_vendor vendor_family;

	const struct ublox_model *model;
	int flags;

	struct l_timeout *init_timeout;
	int init_count;
	guint init_cmd;
};

static void ublox_debug(const char *str, void *user_data)
{
	const char *prefix = user_data;

	ofono_info("%s%s", prefix, str);
}

static int ublox_probe(struct ofono_modem *modem)
{
	struct ublox_data *data;

	DBG("%p", modem);

	data = g_try_new0(struct ublox_data, 1);
	if (data == NULL)
		return -ENOMEM;

	ofono_modem_set_data(modem, data);

	return 0;
}

static void ublox_remove(struct ofono_modem *modem)
{
	struct ublox_data *data = ofono_modem_get_data(modem);

	DBG("%p", modem);

	ofono_modem_set_data(modem, NULL);
	g_at_chat_unref(data->aux);
	g_at_chat_unref(data->modem);
	g_free(data);
}

static void close_devices(struct ofono_modem* modem)
{
	struct ublox_data * data = ofono_modem_get_data(modem);

	g_at_chat_unref(data->aux);
	data->aux = NULL;
	g_at_chat_unref(data->modem);
	data->modem = NULL;
	ofono_modem_set_powered(modem, FALSE);
}

static void cfun_enable(gboolean ok, GAtResult *result, gpointer user_data)
{
	struct ofono_modem *modem = user_data;

	DBG("ok %d", ok);

	if (!ok) {
		close_devices(modem);
		return;
	}

	ofono_modem_set_powered(modem, TRUE);
}

static void query_usbconf_cb(gboolean ok,
				GAtResult *result, gpointer user_data)
{
	struct ofono_modem *modem = user_data;
	struct ublox_data *data = ofono_modem_get_data(modem);
	GAtResultIter iter;
	int profile;

	if (!ok) {
		ofono_error("Unable to query USB configuration");
		goto error;
	}

	g_at_result_iter_init(&iter, result);

retry:
	if (!g_at_result_iter_next(&iter, "+UUSBCONF:")) {
		ofono_error("Unable to query USB configuration");
		goto error;
	}

	if (!g_at_result_iter_next_number(&iter, &profile))
		goto retry;

	switch (profile) {
	case 0: /* Fairly back compatible */
	case 1: /* Fairly back compatible plus audio */
		break;
	case 2: /* Low/medium throughput */
		ofono_error("Medium throughput mode not supported");
		goto error;
	case 3: /* High throughput mode */
		data->flags |= UBLOX_DEVICE_F_HIGH_THROUGHPUT_MODE;
		break;
	default:
		ofono_error("Unexpected USB profile: %d", profile);
		goto error;
	}

	if (g_at_chat_send(data->aux, "AT+CFUN=4", none_prefix,
					cfun_enable, modem, NULL))
		return;

error:
	close_devices(modem);
}

static void query_model_cb(gboolean ok, GAtResult *result, gpointer user_data)
{
	struct ofono_modem *modem = user_data;
	struct ublox_data *data = ofono_modem_get_data(modem);
	struct ofono_error error;
	const char *model;
	const struct ublox_model *m;

	decode_at_error(&error, g_at_result_final_response(result));

	if (!ok)
		goto fail;

	if (at_util_parse_attr(result, "", &model) == FALSE) {
		ofono_error("Failed to query modem model");
		goto fail;
	}

	m = ublox_model_from_name(model);
	if (!m) {
		ofono_error("Unrecognized model: %s", model);
		goto fail;
	}

	data->model = m;

	DBG("Model: %s", data->model->name);

	data->vendor_family = OFONO_VENDOR_UBLOX;

	if (data->model->flags & UBLOX_F_HAVE_USBCONF) {
		if (g_at_chat_send(data->aux, "AT+UUSBCONF?", uusbconf_prefix,
					query_usbconf_cb, modem, NULL))
			return;

		ofono_error("Unable to query USB configuration");
		goto fail;
	}

	if (g_at_chat_send(data->aux, "AT+CFUN=4", none_prefix,
					cfun_enable, modem, NULL))
		return;

fail:
	close_devices(modem);
}

static void init_cmd_cb(gboolean ok, GAtResult *result, void *user_data)
{
	struct ofono_modem *modem = user_data;
	struct ublox_data *data = ofono_modem_get_data(modem);

	DBG("%p", modem);

	if (!ok)
		goto fail;

	/* When the 'init command' succeeds, we insert an additional
	 * delay of 1 second before proceeding with the actual
	 * intialization of the device.  We reuse the init_timeout
	 * instance for this, just clearing the command to indicate
	 * that additional retries aren't necessary.
	 */
	data->init_cmd = 0;
	data->init_count = 0;
	l_timeout_modify_ms(data->init_timeout, 1000);

	return;

fail:
	l_timeout_remove(data->init_timeout);
	data->init_timeout = NULL;

	close_devices(modem);
}

static void init_timeout_cb(struct l_timeout *timeout, void *user_data)
{
	struct ofono_modem *modem = user_data;
	struct ublox_data *data = ofono_modem_get_data(modem);

	DBG("%p", modem);

	/* As long as init_cmd is set we need to either keep retrying
	 * or fail everything after excessive retries
	 */
	if (data->init_cmd && data->init_count++ < 20) {
		g_at_chat_retry(data->aux, data->init_cmd);
		l_timeout_modify_ms(timeout, 1000);
		return;
	}

	l_timeout_remove(data->init_timeout);
	data->init_timeout = NULL;

	if (data->init_cmd) {
		ofono_error("failed to init modem after 20 attempts");
		goto fail;
	}

	g_at_chat_send(data->aux, "ATE0", none_prefix,
					NULL, NULL, NULL);
	g_at_chat_send(data->aux, "AT+CMEE=1", none_prefix,
					NULL, NULL, NULL);

	if (g_at_chat_send(data->aux, "AT+CGMM", NULL,
				query_model_cb, modem, NULL) > 0)
		return;

fail:
	close_devices(modem);
}

static int ublox_enable(struct ofono_modem *modem)
{
	struct ublox_data *data = ofono_modem_get_data(modem);

	DBG("%p", modem);

	data->aux = at_util_open_device(modem, "Aux",
					ublox_debug, "Aux: ", NULL);
	/* If this is a serial modem then the device may be behind
	 * the 'Device' attribute instead...
	 */
	if (data->aux == NULL) {
		data->aux = at_util_open_device(modem, "Device",
						ublox_debug, "Aux: ", NULL);
		if (data->aux == NULL)
			return -EINVAL;
	}

	data->modem = at_util_open_device(modem, "Modem",
						ublox_debug, "Modem: ", NULL);
	if (data->modem) {
		g_at_chat_set_slave(data->modem, data->aux);
		g_at_chat_send(data->modem, "ATE0 +CMEE=1", none_prefix,
						NULL, NULL, NULL);
		g_at_chat_send(data->modem, "AT&C0", NULL, NULL, NULL, NULL);
	}

	/*
	 * uBlox devices present their USB interfaces well before those
	 * interfaces are actually ready to use.  The specs say to monitor
	 * the 'greeting text' to detect whether the device is ready to use;
	 * unfortunately, other than for the TOBY L4, the greeting text is
	 * not actually specified.
	 *
	 * What has been determined experimentally to work is to probe with
	 * an 'AT' command until it responds and then wait an additional
	 * second before continuing with device initialization.  Even for
	 * the TOBY L4 where one should wait for the '+AT: READY' URC
	 * before intialization, this seems to be sufficient; the 'READY'
	 * indication always arrives within this time.
	 *
	 * (It would be more rigorous to actually wait for the 'READY'
	 * indication, but that would require knowing the device model
	 * before the device model is actually queried.  Do-able via
	 * USB Product ID, but overkill when the above seems to work
	 * reliably.)
	 */

	data->init_count = 0;
	data->init_cmd = g_at_chat_send(data->aux, "AT", none_prefix,
					init_cmd_cb, modem, NULL);
	data->init_timeout = l_timeout_create_ms(500, init_timeout_cb, modem,
							NULL);

	return -EINPROGRESS;
}

static void cfun_disable(gboolean ok, GAtResult *result, gpointer user_data)
{
	struct ofono_modem *modem = user_data;

	DBG("");

	if (!ok) {
		ofono_error("Failed to disable modem");
		return;
	}

	close_devices(modem);
}

static int ublox_disable(struct ofono_modem *modem)
{
	struct ublox_data *data = ofono_modem_get_data(modem);

	DBG("%p", modem);

	l_timeout_remove(data->init_timeout);

	g_at_chat_cancel_all(data->modem);
	g_at_chat_unregister_all(data->modem);

	g_at_chat_cancel_all(data->aux);
	g_at_chat_unregister_all(data->aux);

	g_at_chat_send(data->aux, "AT+CFUN=0", none_prefix,
					cfun_disable, modem, NULL);

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

static void ublox_set_online(struct ofono_modem *modem, ofono_bool_t online,
				ofono_modem_online_cb_t cb, void *user_data)
{
	struct ublox_data *data = ofono_modem_get_data(modem);
	struct cb_data *cbd = cb_data_new(cb, user_data);
	char const *command = online ? "AT+CFUN=1" : "AT+CFUN=4";

	DBG("modem %p %s", modem, online ? "online" : "offline");

	if (g_at_chat_send(data->aux, command, none_prefix, set_online_cb,
					cbd, g_free) > 0)
		return;

	CALLBACK_WITH_FAILURE(cb, cbd->data);

	g_free(cbd);
}

static void ublox_pre_sim(struct ofono_modem *modem)
{
	struct ublox_data *data = ofono_modem_get_data(modem);
	struct ofono_sim *sim;

	DBG("%p", modem);

	ofono_devinfo_create(modem, 0, "atmodem", data->aux);
	/*
	 * Call support is technically possible only after sim insertion
	 * with the module online. However the EMERGENCY_SETUP procedure of
	 * the 3GPP TS_24.008 is triggered by the same AT command,
	 * and namely 'ATD112;' and 'ATD911;'. Therefore it makes sense to
	 * add the voice support as soon as possible.
	 */
	ofono_voicecall_create(modem, 0, "atmodem", data->aux);
	sim = ofono_sim_create(modem, data->vendor_family, "atmodem",
					data->aux);

	if (sim)
		ofono_sim_inserted_notify(sim, TRUE);
}

static void ublox_post_sim(struct ofono_modem *modem)
{
	struct ublox_data *data = ofono_modem_get_data(modem);
	struct ofono_gprs *gprs;
	struct ofono_gprs_context *gc;
	GAtChat *chat = data->modem ? data->modem : data->aux;
	struct ofono_message_waiting *mw;
	const char *driver;
	const char *iface;
	int variant;

	DBG("%p", modem);

	gprs = ofono_gprs_create(modem, data->vendor_family, "atmodem",
					data->aux);

	iface = ofono_modem_get_string(modem, "NetworkInterface");
	if (iface) {
		driver = "ubloxmodem";
		variant = ublox_model_to_id(data->model);
	} else {
		driver = "atmodem";
		variant = OFONO_VENDOR_UBLOX;
	}

	gc = ofono_gprs_context_create(modem, variant, driver, chat);
	if (gprs && gc)
		ofono_gprs_add_context(gprs, gc);

	ofono_lte_create(modem,
		ublox_model_to_id(data->model), "ubloxmodem", data->aux);

	ofono_sms_create(modem, 0, "atmodem", data->aux);

	ofono_ussd_create(modem, 0, "atmodem", data->aux);
	ofono_call_forwarding_create(modem, 0, "atmodem", data->aux);
	ofono_call_settings_create(modem, 0, "atmodem", data->aux);
	ofono_call_meter_create(modem, 0, "atmodem", data->aux);
	ofono_call_barring_create(modem, 0, "atmodem", data->aux);

	mw = ofono_message_waiting_create(modem);
	if (mw)
		ofono_message_waiting_register(mw);
}

static void ublox_post_online(struct ofono_modem *modem)
{
	struct ublox_data *data = ofono_modem_get_data(modem);

	ofono_netreg_create(modem,
		ublox_model_to_id(data->model), "ubloxmodem", data->aux);

	ofono_netmon_create(modem, data->vendor_family, "ubloxmodem", data->aux);
}

static struct ofono_modem_driver ublox_driver = {
	.name		= "ublox",
	.probe		= ublox_probe,
	.remove		= ublox_remove,
	.enable		= ublox_enable,
	.disable	= ublox_disable,
	.set_online	= ublox_set_online,
	.pre_sim	= ublox_pre_sim,
	.post_sim	= ublox_post_sim,
	.post_online	= ublox_post_online,
};

static int ublox_init(void)
{
	return ofono_modem_driver_register(&ublox_driver);
}

static void ublox_exit(void)
{
	ofono_modem_driver_unregister(&ublox_driver);
}

OFONO_PLUGIN_DEFINE(ublox, "u-blox modem driver", VERSION,
		OFONO_PLUGIN_PRIORITY_DEFAULT, ublox_init, ublox_exit)
